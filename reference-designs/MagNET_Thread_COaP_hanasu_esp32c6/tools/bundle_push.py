#!/usr/bin/env python3
"""bundle_push.py — install a signed role bundle on a Hanasu node over HCP.

    python3 tools/bundle_push.py /dev/cu.usbmodemXXXX bundle.json [--list]

Sends BUNDLE BEGIN, the minified envelope as BUNDLE ADD chunks, then
BUNDLE COMMIT, and prints the node's responses. Works for any Hanasu node
(ESP32-C6 or the Zephyr/XIAO MG24 build).

Link notes (MG24 / SAMD11 bridge):
  * DTR must be asserted — the bridge only forwards while it is (pyserial's
    default). A C6 resets on open regardless; wait for its !STATE READY.
  * Long lines are written paced (32 B / 5 ms) and never flush()ed: tcdrain
    can block forever under the bridge's backpressure.
"""
import argparse
import json
import sys
import time

import serial

CHUNK = 200           # well under the 512-byte HCP line cap, even with "BUNDLE ADD "


def send(s, line):
    b = (line + "\r\n").encode()
    for i in range(0, len(b), 32):
        s.write(b[i:i + 32])
        time.sleep(0.005)


def await_reply(s, timeout):
    """Return the first +OK/-ERR line (events and comments are echoed)."""
    buf = b""
    t0 = time.time()
    while time.time() - t0 < timeout:
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode(errors="replace").strip()
            if not text:
                continue
            if text.startswith(("+", "-ERR")):
                return text
            print("  " + text)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("bundle")
    ap.add_argument("--list", action="store_true", help="BUNDLE LIST afterwards")
    a = ap.parse_args()

    env = json.dumps(json.load(open(a.bundle)), separators=(",", ":"))
    s = serial.Serial(a.port, 115200, timeout=0.1)
    time.sleep(0.3)
    s.read(65536)                       # drop any boot noise
    send(s, "")                         # flush the bridge's open-time NUL

    steps = [("BUNDLE BEGIN", 3)]
    steps += [("BUNDLE ADD " + env[i:i + CHUNK], 3) for i in range(0, len(env), CHUNK)]
    steps += [("BUNDLE COMMIT", 15)]
    if a.list:
        steps += [("BUNDLE LIST", 3)]

    print(f"# {a.bundle}: {len(env)} B minified, {len(steps) - 2 - a.list} chunk(s)")
    for line, tmo in steps:
        send(s, line)
        r = await_reply(s, tmo)
        shown = line if len(line) < 60 else line[:57] + "..."
        print(f"> {shown}\n< {r}")
        if r is None or r.startswith("-ERR"):
            sys.exit(1)
    # role-init output (e.g. a mn-chat) and events arrive just after COMMIT
    t0 = time.time()
    while time.time() - t0 < 2:
        out = s.read(4096)
        if out:
            sys.stdout.write(out.decode(errors="replace"))


if __name__ == "__main__":
    main()
