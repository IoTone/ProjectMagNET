#!/usr/bin/env python3
"""ear_rx — ATVV v1.0 host for the MagNET Ear (macOS / Linux-BlueZ, via bleak).

Finds an Ear (ATVV service UUID or MagNET-Ear-* name), subscribes, GET_CAPS,
then either waits for the thumb button (hold-to-talk) or opens the mic itself.

  ear_rx.py                         # listen (host daemon, reconnects forever):
                                    #   every button hold -> ear_NNN.wav
  ear_rx.py --open 5                # host-initiated: MIC_OPEN for 5 s
  ear_rx.py --pcm - | aplay -t raw -f S16_LE -r 16000 -c 1 -D hw:Loopback,0
                                    # Linux, no PipeWire: stream into snd-aloop

Pairing: press side button B on the Ear first (screen: PAIRING m:ss). The
audio characteristics need an authenticated bond; macOS pops a passkey
dialog on first use — type the 6 digits shown on the Stick. On Linux,
pair once with bluetoothctl (`agent KeyboardDisplay`, `pair <addr>`).
"""
import argparse, asyncio, struct, sys, time, wave

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

SVC = "ab5e0001-5a21-4f05-bc7d-af01f617b664"
TX  = "ab5e0002-5a21-4f05-bc7d-af01f617b664"
RX  = "ab5e0003-5a21-4f05-bc7d-af01f617b664"
CTL = "ab5e0004-5a21-4f05-bc7d-af01f617b664"

GET_CAPS = bytes([0x0A, 0x01, 0x00, 0x00, 0x03, 0x03])   # v1.0, 8k|16k, PTT|HTT
MIC_OPEN = bytes([0x0C, 0x00])                           # playback mode
MIC_CLOSE = bytes([0x0D, 0x00])
MIC_EXTEND = bytes([0x0E, 0x00])

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]


def log(*a):
    print(*a, file=sys.stderr, flush=True)


class Session:
    def __init__(self, args):
        self.args = args
        self.frame = 0           # bytes per frame from CAPS_RESP
        self.caps = asyncio.Event()
        self.live = False
        self.pred = self.idx = 0
        self.expect = 0          # next frame number
        self.synced = False
        self.pcm = []
        self.frames = self.gaps = self.unsynced = 0
        self.t0 = 0.0
        self.n = 0
        self.done = asyncio.Event()

    # -- codec
    def decode(self, data):
        out, pred, idx = [], self.pred, self.idx
        for b in data:
            for nib in (b >> 4, b & 0x0F):
                step = STEP[idx]
                diff = step >> 3
                if nib & 1: diff += step >> 2
                if nib & 2: diff += step >> 1
                if nib & 4: diff += step
                pred = max(-32768, min(32767, pred - diff if nib & 8 else pred + diff))
                idx = max(0, min(88, idx + IDX[nib & 7]))
                out.append(pred)
        self.pred, self.idx = pred, idx
        return out

    def emit(self, samples):
        if self.args.pcm:
            sys.stdout.buffer.write(struct.pack(f"<{len(samples)}h", *samples))
            sys.stdout.buffer.flush()
        else:
            self.pcm.extend(samples)

    # -- notifications
    def on_ctl(self, _h, data: bytearray):
        op = data[0]
        if op == 0x0B and len(data) >= 7:
            ver = (data[1] << 8) | data[2]
            self.frame = (data[5] << 8) | data[6]
            log(f"CAPS_RESP v{ver >> 8}.{ver & 0xFF} codecs=0x{data[3]:02x} "
                f"model=0x{data[4]:02x} frame={self.frame} B")
            self.caps.set()
        elif op == 0x04 and len(data) >= 4:
            why = {0x00: "MIC_OPEN", 0x01: "PTT", 0x03: "HOLD-TO-TALK"}.get(data[1], hex(data[1]))
            log(f"AUDIO_START reason={why} codec=0x{data[2]:02x} stream={data[3]}")
            self.live, self.synced, self.pcm = True, False, []
            self.frames = self.gaps = self.unsynced = 0
            self.t0 = time.monotonic()
        elif op == 0x0A and len(data) >= 7:
            fno = (data[2] << 8) | data[3]
            if self.synced and fno > self.expect:          # frames lost before this sync
                miss = fno - self.expect
                self.gaps += miss
                self.emit([0] * (miss * 2 * self.frame))     # conceal with silence
            self.pred = struct.unpack(">h", bytes(data[4:6]))[0]
            self.idx = data[6]
            self.expect, self.synced = fno, True
        elif op == 0x00 and not self.live:
            log(f"AUDIO_STOP 0x{data[1] if len(data) > 1 else 0:02x} (no stream open)")
        elif op == 0x00:
            reason = data[1] if len(data) > 1 else 0
            dur = time.monotonic() - self.t0
            log(f"AUDIO_STOP reason=0x{reason:02x}: {self.frames} frames "
                f"({self.frames * 2 * self.frame / 16000:.2f} s audio in {dur:.2f} s), "
                f"{self.gaps} lost, {self.unsynced} pre-sync")
            self.live = False
            self.finish()
        elif op == 0x0C:
            log(f"MIC_OPEN_ERROR 0x{data[1]:02x}{data[2]:02x}")
        else:
            log("CTL", data.hex())

    def on_rx(self, _h, data: bytearray):
        if not self.live:
            return
        if not self.synced:
            self.unsynced += 1
            return
        self.emit(self.decode(data))
        self.frames += 1
        self.expect = (self.expect + 1) & 0xFFFF

    def finish(self):
        if not self.args.pcm and self.pcm:
            name = f"{self.args.prefix}_{self.n:03d}.wav"
            with wave.open(name, "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
                w.writeframes(struct.pack(f"<{len(self.pcm)}h", *self.pcm))
            peak = max(abs(s) for s in self.pcm)
            log(f"wrote {name} ({len(self.pcm) / 16000:.2f} s, peak {peak})")
            self.n += 1
        if self.args.once or self.args.open:
            self.done.set()


def is_ear(d, a):
    return SVC in [u.lower() for u in a.service_uuids] or (a.local_name or "").startswith("MagNET-Ear")


async def find(args, wait=False):
    """Return the Ear as soon as it is seen. wait=True: keep scanning forever."""
    if args.address:
        return args.address
    log("scanning for an Ear...")
    while True:
        d = await BleakScanner.find_device_by_filter(is_ear, timeout=10.0)
        if d:
            log(f"found {d.name} {d.address}")
            return d
        if not wait:
            sys.exit("no Ear found. An unpaired Ear is invisible until you press side button B "
                     "(screen shows PAIRING); a paired one may already be connected to another host.")


async def session(s, args, dev):
    """One connection. Returns when the stream (--open/--once) is done or the link drops."""
    gone = asyncio.Event()
    async with BleakClient(dev, timeout=20.0, disconnected_callback=lambda _c: gone.set()) as c:
        log("connected; subscribing")
        await c.start_notify(CTL, s.on_ctl)
        await c.start_notify(RX, s.on_rx)
        # TX requires an authenticated bond: first write triggers pairing.
        await c.write_gatt_char(TX, GET_CAPS, response=True)
        await asyncio.wait_for(s.caps.wait(), 60)

        if args.open:
            await c.write_gatt_char(TX, MIC_OPEN, response=True)
            t_end = time.monotonic() + args.open
            while time.monotonic() < t_end and not s.done.is_set():
                await asyncio.sleep(min(5.0, max(0.05, t_end - time.monotonic())))
                if time.monotonic() < t_end:
                    await c.write_gatt_char(TX, MIC_EXTEND, response=True)
            if s.live:
                await c.write_gatt_char(TX, MIC_CLOSE, response=True)
            await asyncio.wait_for(s.done.wait(), 5)
            return
        log("ready — hold button A on the Ear and talk")
        waits = [asyncio.ensure_future(gone.wait())]
        if args.once:
            waits.append(asyncio.ensure_future(s.done.wait()))
        await asyncio.wait(waits, return_when=asyncio.FIRST_COMPLETED)
        for w in waits:
            w.cancel()
        if gone.is_set():
            log("link lost")


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--address", help="BLE address / CoreBluetooth UUID (skip scan)")
    ap.add_argument("--open", type=float, metavar="SECS", help="host-initiated MIC_OPEN for SECS")
    ap.add_argument("--once", action="store_true", help="exit after the first stream")
    ap.add_argument("--pcm", metavar="-", help="write raw s16le 16 kHz mono to stdout instead of WAVs")
    ap.add_argument("--prefix", default="ear", help="WAV filename prefix")
    ap.add_argument("--timeout", type=float, default=0, help="give up after SECS (0 = forever)")
    args = ap.parse_args()

    s = Session(args)
    listen = not args.open and not args.once      # host daemon: stay connected forever
    if args.timeout:
        try:
            await asyncio.wait_for(run(s, args, listen), args.timeout)
        except asyncio.TimeoutError:
            log("timeout")
    else:
        await run(s, args, listen)


async def run(s, args, listen):
    while True:
        dev = await find(args, wait=listen)
        try:
            await session(s, args, dev)
        except (BleakError, asyncio.TimeoutError, OSError) as e:
            log(f"connection failed: {e!r}")
            if not listen:
                raise
        if not listen or s.done.is_set() and args.once:
            return
        s.caps.clear()
        log("reconnecting...")
        await asyncio.sleep(0.5)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
