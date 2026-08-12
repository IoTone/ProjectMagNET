#!/usr/bin/env python3
"""xfer — Type 6 extended-transfer host reference (docs/EXTENDED-TRANSFER.md).

    xfer.py send <port> <peer-ipv6> <file>    # push a file to a peer
    xfer.py recv <port> <out-file>            # wait for one inbound transfer
    xfer.py loopback <port> [<bytes>]         # single-node self-test: sends to
                                              # the node's own ML-EID and checks
                                              # the received bytes hash-identical

The node is a modem — this script holds the file. Sending feeds one 32-chunk
window of base64 `XFER DATA` lines, then waits for `!XFER_NEXT`; receiving
reassembles `!XFER` chunk events by index (order on the air doesn't matter).
"""
import base64
import hashlib
import os
import queue
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "host-sdk", "python"))
from magnet_hcp import MagnetNode  # noqa: E402

CHUNK, WINDOW = 336, 32


def send_file(node, peer, data, meta):
    evq = queue.Queue()
    for name in ("xfer_next", "xfer_sent", "xfer_fail"):
        node.on(name, evq.put)
    total = len(data)
    n_chunks = (total + CHUNK - 1) // CHUNK
    print(f"send: {total} B = {n_chunks} chunks → {peer}")
    print("  " + node.command(f"XFER BEGIN {peer} {total} {meta}"))
    base, t0 = 0, time.time()
    while True:
        n = min(WINDOW, n_chunks - base)
        for i in range(n):
            raw = data[(base + i) * CHUNK:(base + i + 1) * CHUNK]
            node.command("XFER DATA " + base64.b64encode(raw).decode())
        print(f"  window {base}..{base + n - 1} fed, waiting for ack…")
        ev = evq.get(timeout=90)
        if ev.name == "xfer_fail":
            raise RuntimeError(f"transfer failed: {ev.raw}")
        if ev.name == "xfer_sent":
            dt = time.time() - t0
            print(f"  SENT {total} B in {dt:.1f} s ({total / dt / 1024:.2f} KiB/s)")
            return
        base = int(ev.fields[1])                      # !XFER_NEXT <xid> <base>


class Receiver:
    """Collects one inbound transfer from !XFER_* events."""

    def __init__(self, node):
        self.chunks = {}
        self.done = queue.Queue()
        node.on("xfer_begin", self._begin)
        node.on("xfer", self._chunk)
        node.on("xfer_done", self.done.put)
        node.on("xfer_fail", self.done.put)

    def _begin(self, ev):
        print(f"recv: from={ev.fields[0]} xid={ev.fields[1]} "
              f"len={ev.fields[2]} chunks={ev.fields[3]} meta={ev.fields[5]}")

    def _chunk(self, ev):
        idx = int(ev.fields[2].split("/")[0])
        self.chunks[idx] = base64.b64decode(ev.fields[3])

    def wait(self, timeout=600):
        ev = self.done.get(timeout=timeout)
        if ev.name == "xfer_fail":
            raise RuntimeError(f"transfer failed: {ev.raw}")
        data = b"".join(self.chunks[i] for i in range(len(self.chunks)))
        print(f"recv: DONE {len(data)} B in {len(self.chunks)} chunks")
        return data


def own_eid(node):
    node.command("MESH")
    for line in node.comments:
        if "ml-eid=" in line:
            return line.split("ml-eid=")[1].split()[0]
    raise RuntimeError("no ML-EID (radio down?)")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    mode, port = sys.argv[1], sys.argv[2]
    with MagnetNode(port, timeout=15.0, keep_comments=True) as node:
        node.command("SUB xfer")
        if mode == "send":
            peer, path = sys.argv[3], sys.argv[4]
            data = open(path, "rb").read()
            send_file(node, peer, data, os.path.basename(path)[:64])
        elif mode == "recv":
            out = sys.argv[3]
            rx = Receiver(node)
            data = rx.wait()
            open(out, "wb").write(data)
            print(f"wrote {out} sha256={hashlib.sha256(data).hexdigest()[:16]}…")
        elif mode == "loopback":
            size = int(sys.argv[3]) if len(sys.argv) > 3 else 50 * 1024
            data = os.urandom(size)
            eid = own_eid(node)
            print(f"loopback via {eid}")
            rx = Receiver(node)
            send_file(node, eid, data, "loopback.bin")
            got = rx.wait(timeout=120)
            a = hashlib.sha256(data).hexdigest()
            b = hashlib.sha256(got).hexdigest()
            print(f"sha256 sent={a[:16]}… recv={b[:16]}…")
            if a != b:
                print("MISMATCH")
                return 1
            print("loopback PASS")
        else:
            print(__doc__)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
