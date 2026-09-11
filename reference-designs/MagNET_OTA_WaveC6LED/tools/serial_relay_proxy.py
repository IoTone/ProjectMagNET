#!/usr/bin/env python3
"""
serial_relay_proxy.py — the bench host for R4, on a laptop with pyserial.

Does what the web console's /device-serial page does, from a terminal: opens
the board's USB port, attaches ("!A", re-sent every 20 s as a keep-alive),
turns every "!R<base64>" request line into an HTTP call, and writes the
answer back as "!R<base64>" lines. Everything that is NOT a relay line is the
device's own console, printed through — and you can type console commands
(`checkin`, `relay`, `show`) at the same prompt, which is the whole point of
the pipe riding the console instead of owning the port.

    .venv/bin/pip install pyserial
    python tools/serial_relay_proxy.py /dev/cu.usbmodemXXXX --server http://10.0.0.116:8000
    python tools/serial_relay_proxy.py /dev/cu.usbmodemXXXX --server ... --cmd checkin --for 30

Frames are the same 10-byte header + CRC as relay_frame.c; the codec is
imported from ble_relay_proxy.py so the two bench tools cannot drift. Frame
size is capped at 144 B under the console's 200-char line reader.

Plan §5.3 (a): the device's dvc_ token crosses this script in clear text.
"""
import argparse
import base64
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ble_relay_proxy import T_ERR, T_REQ, T_RESP, chunk, decode  # noqa: E402

try:
    import serial
except ImportError:
    sys.exit("pyserial is not installed: pip install pyserial")

MAX_FRAME = 144
KEEPALIVE_S = 20


class SerialHost:
    def __init__(self, port: str, server: str, timeout: float):
        self.ser = serial.Serial(port, 115200, timeout=0.1)
        self.server = server.rstrip("/")
        self.timeout = timeout
        self.cur = None
        self.lock = threading.Lock()
        self.n_req = self.n_in = self.n_out = 0
        self.log = []

    def write_line(self, line: str):
        with self.lock:
            self.ser.write((line + "\n").encode())
            self.ser.flush()

    def attach(self):
        self.write_line("!A")

    def on_line(self, line: str):
        if line.startswith("!R"):
            self.n_in += 1
            try:
                f = decode(base64.b64decode(line[2:]))
            except Exception:
                f = None
            if f is None:
                print("  ! bad frame")
                return
            t, seq, total, idx, payload = f
            if t != T_REQ:
                return
            if idx == 0:
                self.cur = [seq, total, 0, []]
            elif self.cur is None or seq != self.cur[0] or idx != self.cur[2]:
                print(f"  ! frame seq={seq} idx={idx} out of order — dropping")
                self.cur = None
                return
            self.cur[2] += 1
            self.cur[3].append(payload)
            if self.cur[2] >= total:
                msg = b"".join(self.cur[3])
                self.cur = None
                threading.Thread(target=self.handle, args=(seq, msg), daemon=True).start()
        elif line == "!K":
            pass                                   # keep-alive ack
        elif line.strip():
            print(f"  dev| {line}")

    def handle(self, seq: int, msg: bytes):
        self.n_req += 1
        nl = msg.find(b"\n")
        try:
            head = json.loads(msg[:nl].decode()) if nl >= 0 else None
        except ValueError:
            head = None
        if head is None:
            self.reply(seq, T_ERR, 0, b"bad request header")
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
        except Exception as e:  # noqa: BLE001
            print(f"  {method} {path} -> ERR {e}")
            self.reply(seq, T_ERR, 0, str(e).encode()[:200])
            return
        self.reply(seq, T_RESP, status, rbody)
        print(f"  {method} {path} -> {status} ({len(rbody)} B) in {time.time() - t0:.2f}s, seq {seq}")
        self.log.append((method, path, status, len(rbody)))

    def reply(self, seq: int, t: int, status: int, body: bytes):
        import struct
        msg = struct.pack("<H", status) + body
        for frame in chunk(t, seq, msg, MAX_FRAME):
            self.write_line("!R" + base64.b64encode(frame).decode())
            self.n_out += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--server", required=True, help="base URL when the device sends none")
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("--cmd", action="append", default=[], help="console command to send after attaching (repeatable)")
    ap.add_argument("--for", dest="duration", type=float, default=0, help="run this many seconds then exit (0 = until Ctrl-C)")
    args = ap.parse_args()

    host = SerialHost(args.port, args.server, args.timeout)
    time.sleep(0.3)
    host.ser.reset_input_buffer()
    host.attach()
    print(f"attached on {args.port}; relaying to {args.server}")
    for c in args.cmd:
        time.sleep(0.5)
        print(f"  >>> {c}")
        host.write_line(c)

    t0 = time.time()
    last_ka = time.time()
    buf = b""
    try:
        while not args.duration or time.time() - t0 < args.duration:
            d = host.ser.read(4096)
            if d:
                buf += d
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    host.on_line(line.decode("utf-8", "replace").rstrip("\r"))
            if time.time() - last_ka > KEEPALIVE_S:
                host.attach()
                last_ka = time.time()
    except KeyboardInterrupt:
        pass
    finally:
        host.write_line("!D")
        print(f"detached: {host.n_req} requests, {host.n_in} frames in, {host.n_out} out")


if __name__ == "__main__":
    main()
