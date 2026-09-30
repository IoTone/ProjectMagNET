#!/usr/bin/env python3
"""ota_push.py — mesh OTA: have a node send an OTA package to a peer.

    python3 tools/ota_push.py <sender-port> <target-ml-eid> node.mnpkg

The package (.mnpkg, docs/OTA-PACKAGE.md — build it with tools/mnpkg.py)
travels as one Type 6 transfer over Thread with meta "mnpkg:1". The sender is
ANY Hanasu node on a host link. The target checks the header as soon as chunk
0 lands and everything else at the end; !XFER_SENT therefore means "staged
and verified", and a rejection comes back as "refused:<n>" (n = the spec §4
check). Applying is separate and privileged — on the target: FORTH, then
`ota-apply` (swaps in TEST mode; confirms once healthy, rolls back otherwise).

Host-link rules: a C6 resets on open (this waits for its READY); a MG24's
SAMD11 bridge only forwards while DTR is asserted (pyserial's default) and
does not reset. Long lines are written paced and never flush()ed. Sending to
the sender's OWN ML-EID is a valid loopback test (Type 6 allows it).
"""
import argparse
import base64
import re
import sys
import threading
import time

import serial

CHUNK, WINDOW = 336, 32
CODES = {1: "E_PKG_FORMAT", 2: "E_PKG_KEY", 3: "E_PKG_SIG", 4: "E_PKG_TARGET",
         5: "E_PKG_REPARTITION", 6: "E_PKG_SIZE", 7: "E_PKG_SHA", 8: "E_PKG_IMAGE",
         9: "E_PKG_MIN"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("target")
    ap.add_argument("image", help=".mnpkg package")
    ap.add_argument("--verbose", action="store_true", help="print everything the sender port said")
    a = ap.parse_args()

    img = open(a.image, "rb").read()
    if img[:4] != b"MNPK":
        sys.exit("not an OTA package (magic MNPK) — build one with tools/mnpkg.py")
    meta = "mnpkg:1"
    v = img[16:24]
    print(f"# package v{v[0]}.{v[1]}.{int.from_bytes(v[2:4], 'big')}+{int.from_bytes(v[4:8], 'big')}"
          f"  chip 0x{img[8:10].hex()} board 0x{img[10:12].hex()}  sha128 {img[36:52].hex()}")
    n = (len(img) + CHUNK - 1) // CHUNK

    s = serial.Serial(a.port, 115200, timeout=0.1)   # DTR asserted: MG24 bridge forwards
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

    time.sleep(3)
    if log and b"MagNET Hanasu" in log and not wait(rb"!STATE READY", 60):
        sys.exit("sender rebooted on open but never became READY")
    time.sleep(1)
    send("")
    send("SUB xfer")
    send(f"XFER BEGIN {a.target} {len(img)} {meta}")
    if not wait(rb"\+OK xid=", 10):
        if a.verbose:
            print(bytes(log).decode(errors="replace"))
        sys.exit("sender refused XFER BEGIN")
    print(f"# {len(img)} B, {n} chunks, meta {meta}")
    t0 = time.time()
    for i in range(n):
        # One line in flight: wait for this DATA line's reply before the next.
        # A MG24's UART has no flow control, so an unpaced host overruns the
        # node's receive ring the moment the node is busy (flash erase, radio).
        mark = len(log)
        send("XFER DATA " + base64.b64encode(img[i * CHUNK:(i + 1) * CHUNK]).decode())
        t1 = time.time()
        while time.time() - t1 < 5:
            tail = bytes(log[mark:])
            if b"+OK" in tail or b"-ERR" in tail:
                break
            time.sleep(0.002)
        if b"-ERR" in bytes(log[mark:]) and b"window full" not in bytes(log[mark:]):
            print(f"\n# node rejected DATA line {i}: "
                  + bytes(log[mark:]).decode(errors="replace").strip()[:120])
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
    res = m.group(0).decode() if m else "no result"
    r = re.search(r"refused:(\d+)", res)
    if r:
        res += f"  = {CODES.get(int(r.group(1)), '?')}"
    print(f"\n# sender: {res} in {time.time() - t0:.0f}s")
    time.sleep(1)
    if a.verbose:
        print(bytes(log).decode(errors="replace"))
    for l in bytes(log).decode(errors="replace").splitlines():   # loopback: target lines too
        if l.startswith("!OTA") or "refused" in l:
            print("# node:  ", l.strip())
    sys.exit(0 if m and m.group(1) == b"SENT" else 1)


if __name__ == "__main__":
    main()
