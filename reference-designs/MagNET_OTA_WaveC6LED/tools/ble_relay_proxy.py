#!/usr/bin/env python3
"""
ble_relay_proxy.py — the bench proxy for R3, on a Mac (or Linux) with bleak.

Does exactly what the contributor phone app does, from a laptop: finds the
board advertising the relay service, subscribes to tx, and turns every framed
request into an HTTP call against the server, framing the answer back.

    python3 -m venv .venv && .venv/bin/pip install bleak
    .venv/bin/python tools/ble_relay_proxy.py --server http://10.0.0.116:8000

Why this exists next to the Flutter proxy: it proves the DEVICE side on real
hardware without a phone in the loop, and its log is the record of a run
(each relayed request with status and byte count). The framing here is the
same 12-byte header + CRC as relay_frame.c and relay_frame.dart; the CRC
check value 0x29B1 for "123456789" is asserted at start-up so a copy-paste
error in the constant cannot masquerade as a firmware bug.

Plan §5.3 (a), said plainly: the device's dvc_ token crosses this script in
clear text and is attached to the HTTP request. It is not written anywhere.
"""
import argparse
import asyncio
import json
import struct
import sys
import time
import urllib.error
import urllib.request

# bleak is imported in main(), not here: serial_relay_proxy.py imports this
# module for the codec and must work on a machine with pyserial but no bleak.

SVC  = "4d41474e-4554-0002-0000-000000000000"
TX   = "4d41474e-4554-0002-0000-000000000001"   # device -> us (notify)
RX   = "4d41474e-4554-0002-0000-000000000002"   # us -> device (write)
INFO = "4d41474e-4554-0002-0000-000000000003"

VER, T_REQ, T_RESP, T_ACK, T_ERR = 1, 1, 2, 3, 4
HDR = struct.Struct("<BBHHHH")      # version, type, seq, total, idx, len
OVERHEAD = HDR.size + 2


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


assert crc16(b"123456789") == 0x29B1, "CRC constant drifted from relay_frame.c"


def encode(t: int, seq: int, total: int, idx: int, payload: bytes) -> bytes:
    head = HDR.pack(VER, t, seq, total, idx, len(payload)) + payload
    return head + struct.pack("<H", crc16(head))


def decode(buf: bytes):
    if len(buf) < OVERHEAD or buf[0] != VER:
        return None
    ver, t, seq, total, idx, plen = HDR.unpack_from(buf)
    if OVERHEAD + plen != len(buf) or crc16(buf[:HDR.size + plen]) != struct.unpack_from("<H", buf, HDR.size + plen)[0]:
        return None
    if total == 0 or idx >= total:
        return None
    return t, seq, total, idx, buf[HDR.size:HDR.size + plen]


def chunk(t: int, seq: int, msg: bytes, max_frame: int):
    per = max_frame - OVERHEAD
    total = max(1, (len(msg) + per - 1) // per)
    for i in range(total):
        yield encode(t, seq, total, i, msg[i * per:(i + 1) * per])


class Proxy:
    def __init__(self, client, server: str, max_frame: int, timeout: float):   # client: BleakClient
        self.client, self.server, self.max_frame, self.timeout = client, server.rstrip("/"), max_frame, timeout
        self.cur = None      # (seq, total, next_idx, chunks)
        self.n_in = self.n_out = self.n_req = 0

    def on_notify(self, _handle, data: bytearray):
        self.n_in += 1
        f = decode(bytes(data))
        if f is None:
            print(f"  ! bad frame ({len(data)} B)")
            return
        t, seq, total, idx, payload = f
        if t != T_REQ:
            return
        if idx == 0:
            self.cur = [seq, total, 0, []]
        elif self.cur is None or seq != self.cur[0] or idx != self.cur[2]:
            print(f"  ! frame seq={seq} idx={idx} out of order — dropping sequence")
            self.cur = None
            return
        self.cur[2] += 1
        self.cur[3].append(payload)
        if self.cur[2] >= total:
            msg = b"".join(self.cur[3])
            self.cur = None
            asyncio.get_event_loop().create_task(self.handle(seq, msg))

    async def handle(self, seq: int, msg: bytes):
        self.n_req += 1
        nl = msg.find(b"\n")
        try:
            head = json.loads(msg[:nl].decode()) if nl >= 0 else None
        except ValueError:
            head = None
        if head is None:
            await self.reply(seq, T_ERR, 0, b"bad request header")
            return
        body = msg[nl + 1:]
        method, path = head.get("m", "GET").upper(), head.get("p", "/")
        base = head.get("s") or self.server
        url = base.rstrip("/") + path
        headers = {}
        if head.get("t"):
            headers["Authorization"] = "Bearer " + head["t"]
        if body:
            headers["Content-Type"] = "application/json"
        t0 = time.time()
        try:
            req = urllib.request.Request(url, data=body if body else None, method=method, headers=headers)
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                status, rbody = r.status, r.read()
        except urllib.error.HTTPError as e:
            status, rbody = e.code, e.read()
        except Exception as e:  # noqa: BLE001 — every failure becomes ERR for the device
            print(f"  {method} {path} -> ERR {e}")
            await self.reply(seq, T_ERR, 0, str(e).encode()[:200])
            return
        await self.reply(seq, T_RESP, status, rbody)
        print(f"  {method} {path} -> {status} ({len(rbody)} B) in {time.time() - t0:.2f}s, seq {seq}")

    async def reply(self, seq: int, t: int, status: int, body: bytes):
        msg = struct.pack("<H", status) + body
        for frame in chunk(t, seq, msg, self.max_frame):
            await self.client.write_gatt_char(RX, frame, response=True)
            self.n_out += 1


async def main():
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        sys.exit("bleak is not installed: python3 -m venv .venv && .venv/bin/pip install bleak")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", help="base URL when the device sends none (its own NVS server_url wins)")
    ap.add_argument("--name", default="ROBOTARME-", help="advertised-name prefix to accept (default ROBOTARME-)")
    ap.add_argument("--timeout", type=float, default=15.0, help="HTTP timeout seconds")
    ap.add_argument("--scan", type=float, default=15.0, help="scan seconds")
    args = ap.parse_args()

    print(f"scanning {args.scan:.0f} s for {args.name}* with service {SVC} ...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or "").startswith(args.name) or SVC in (ad.service_uuids or []),
        timeout=args.scan)
    if dev is None:
        sys.exit("no device found — is the board powered and advertising? (console: `relay`)")
    print(f"found {dev.name} [{dev.address}]")

    async with BleakClient(dev) as client:
        # macOS negotiates the MTU itself; bleak exposes the result.
        mtu = getattr(client, "mtu_size", 23) or 23
        max_frame = max(20, mtu - 3)
        try:
            info = json.loads((await client.read_gatt_char(INFO)).decode())
            print(f"device info: {info}")
        except Exception as e:  # noqa: BLE001
            print(f"(info read failed: {e})")
        proxy = Proxy(client, args.server or "", max_frame, args.timeout)
        await client.start_notify(TX, proxy.on_notify)
        print(f"ATTACHED (mtu {mtu}, frames <= {max_frame} B). Relaying; Ctrl-C to stop.")
        if not args.server:
            print("  no --server: only devices that carry their own server_url will work")
        try:
            while client.is_connected:
                await asyncio.sleep(1)
        except (KeyboardInterrupt, asyncio.CancelledError):
            pass
        finally:
            print(f"stopping: {proxy.n_req} requests, {proxy.n_in} frames in, {proxy.n_out} out")
            try:
                await client.stop_notify(TX)
            except Exception:  # noqa: BLE001
                pass


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
