#!/usr/bin/env python3
"""ota_push.py — mesh OTA: have a node send a signed MCUboot image to a peer.

    python3 tools/ota_push.py <sender-port> <target-ml-eid> zephyr.signed.bin

The sender is ANY Hanasu node on a host link (a C6 works); the image travels
as a Type 6 transfer over Thread with meta "ota:<first 128 bits of sha256>".
A target running firmware-zephyr claims it and writes it into slot1, then
reports `!OTA staged vX.Y.Z+N`. Applying is separate and privileged — on the
target: FORTH, then `ota-apply` (swaps in TEST mode; confirms once READY,
rolls back on its own if the new image never gets there).

Host-link rules (both boards): C6 ports reset on open — this waits for READY;
long lines are written paced and never flush()ed.
"""
import argparse
import base64
import hashlib
import re
import sys
import threading
import time

import serial

CHUNK, WINDOW = 336, 32


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("target")
    ap.add_argument("image")
    a = ap.parse_args()

    img = open(a.image, "rb").read()
    if img[:4] != bytes.fromhex("3db8f396"):
        sys.exit("not an MCUboot image (bad magic) — send zephyr.signed.bin")
    meta = "ota:" + hashlib.sha256(img).hexdigest()[:32]
    n = (len(img) + CHUNK - 1) // CHUNK

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, 115200, 0.1
    s.dtr = s.rts = False          # a C6 still resets; a MG24 needs DTR -> reopen below
    s.open()
    log = bytearray()

    def rd():
        while True:
            log.extend(s.read(4096))
    threading.Thread(target=rd, daemon=True).start()

    def send(line):
        b = (line + "\r\n").encode()
        for i in range(0, len(b), 32):
            s.write(b[i:i + 32])
            time.sleep(0.004)

    def wait(pat, t):
        t0 = time.time()
        while time.time() - t0 < t:
            m = re.search(pat, bytes(log))
            if m:
                return m
            time.sleep(0.1)
        return None

    if not wait(rb"!STATE READY", 60) and not log:
        print("# no boot seen (MG24 sender?) — continuing")
    time.sleep(1)
    send("")
    send("SUB xfer")
    send(f"XFER BEGIN {a.target} {len(img)} {meta}")
    if not wait(rb"\+OK xid=", 10):
        sys.exit("sender refused XFER BEGIN")
    print(f"# {len(img)} B, {n} chunks, meta {meta}")
    t0 = time.time()
    for i in range(n):
        send("XFER DATA " + base64.b64encode(img[i * CHUNK:(i + 1) * CHUNK]).decode())
        if (i + 1) % WINDOW == 0 and i + 1 < n:
            k = (i + 1) // WINDOW
            t1 = time.time()
            while bytes(log).count(b"!XFER_NEXT") < k and time.time() - t1 < 60:
                if b"!XFER_FAIL" in log:
                    break
                time.sleep(0.05)
            if b"!XFER_FAIL" in log:
                break
            print(f"\r# {i + 1}/{n} chunks  {(i + 1) * CHUNK / (time.time() - t0) / 1024:.1f} KiB/s",
                  end="", flush=True)
    m = wait(rb"!XFER_(SENT|FAIL)[^\r\n]*", 120)
    print(f"\n# sender: {m.group(0).decode() if m else 'no result'} in {time.time() - t0:.0f}s")
    sys.exit(0 if m and m.group(1) == b"SENT" else 1)


if __name__ == "__main__":
    main()
