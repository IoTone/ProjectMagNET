#!/usr/bin/env python3
"""chat-bench bridge — N Hanasu nodes (2+) ⇄ one browser page.

Wraps a MagnetNode (host-sdk) around each serial port and exposes:

    GET  /            the SolidJS chat page (index.html, same dir)
    GET  /nodes       {"count": N, "ports": [...]} — pane bootstrap
    GET  /events      SSE stream: every ! event from every node + periodic
                      STATUS/CHANNEL/WHOAMI snapshots, all stamped with the
                      bridge clock (one clock ⇒ honest cross-node latency)
    POST /send        {"node": i, "text": "..."} → CHAT on that node;
                      responds {"ok": true, "ts": <bridge time before write>}
    POST /cmd         {"node": i, "line": "<verb …>"} → run one HCP command
                      (LED, NAME, STATS, …); responds {"ok", "body", "ts"}.
                      This is the driver-facing surface: hosts like the
                      catbot own NO serial — the bridge is the sole owner.
    POST /sendphoto   {"node": i, "to": j, "name": "x.jpg", "data": "<b64>"} →
                      Type 6 transfer to node j's ML-EID ("to" may be omitted
                      on a two-node bench: the other node is implied). Responds
                      immediately; progress/photo events arrive over SSE:
                        {kind:"xfer", node, dir:"out|in", phase:"begin|
                         progress|sent|fail", pct, name, secs, reason}
                        {kind:"photo", node, from, name, size, secs,
                         data:"data:image/...;base64,..."}   (receiver side)
                      Raw !XFER_* lines are consumed here, never forwarded
                      (a 448-char b64 line per chunk would swamp the page).

    python3 bridge.py [port0 port1 ...] [--http 8642]

Opening a serial port resets the node (DTR/RTS toggle) — both boards reboot
when the bridge starts and re-attach in ~30 s; the page shows the state live.
Don't run hcp.py against the same ports while the bridge holds them.

Stdlib only (plus pyserial via the SDK). No build step, no dependencies.
"""
import base64
import json
import os
import queue
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "host-sdk", "python"))
from magnet_hcp import MagnetNode, HCPError  # noqa: E402

DEFAULT_PORTS = ["/dev/ttyACM0", "/dev/ttyACM1"]

clients: list = []                      # queue.Queue per connected SSE client
clients_lock = threading.Lock()
nodes: list = []
eids: list = []                         # each node's ML-EID (poll fills it in)
CHUNK, WINDOW = 336, 32


def broadcast(obj):
    data = json.dumps(obj)
    with clients_lock:
        for q in clients:
            q.put(data)


XFER_EVENTS = ("xfer", "xfer_begin", "xfer_done", "xfer_fail",
               "xfer_next", "xfer_sent")


class XferManager:
    """Per-node transfer plumbing: reassembles inbound transfers into a
    single {kind:"photo"} SSE event, and routes the sender-flow events
    (!XFER_NEXT/!XFER_SENT/!XFER_FAIL) to the /sendphoto worker thread."""

    def __init__(self, i):
        self.i = i
        self.inb = None                  # inbound: xid name from total nchunks chunks t0
        self.evq = queue.Queue()         # outbound flow events for the worker
        self.out_busy = threading.Lock()

    def handle(self, ev, ts):
        if ev.name == "xfer_begin":
            self.inb = {"xid": ev.fields[1], "from": ev.fields[0],
                        "total": int(ev.fields[2]), "nchunks": int(ev.fields[3]),
                        "name": ev.fields[5], "chunks": {}, "t0": ts, "pct": -10}
            broadcast({"kind": "xfer", "node": self.i, "dir": "in",
                       "phase": "begin", "name": self.inb["name"],
                       "from": self.inb["from"], "total": self.inb["total"]})
        elif (ev.name == "xfer" and self.inb is not None
              and ev.fields[1] == self.inb["xid"]):
            idx = int(ev.fields[2].split("/")[0])
            self.inb["chunks"][idx] = base64.b64decode(ev.fields[3])
            pct = 100 * len(self.inb["chunks"]) // self.inb["nchunks"]
            if pct >= self.inb["pct"] + 10:      # throttle progress events
                self.inb["pct"] = pct
                broadcast({"kind": "xfer", "node": self.i, "dir": "in",
                           "phase": "progress", "pct": pct})
        elif (ev.name == "xfer_done" and self.inb is not None
              and ev.fields[1] == self.inb["xid"]):
            if len(self.inb["chunks"]) != self.inb["nchunks"]:
                # a chunk event line was lost on the serial link — fail
                # loudly instead of KeyError-ing (SDK would swallow it)
                broadcast({"kind": "xfer", "node": self.i, "dir": "in",
                           "phase": "fail", "reason": "chunk event lost on serial"})
                self.inb = None
                return
            data = b"".join(self.inb["chunks"][k]
                            for k in range(self.inb["nchunks"]))
            mime = ("image/png" if self.inb["name"].lower().endswith(".png")
                    else "image/jpeg")
            broadcast({"kind": "photo", "node": self.i, "from": self.inb["from"],
                       "name": self.inb["name"], "size": len(data),
                       "secs": round(ts - self.inb["t0"], 1),
                       "data": "data:%s;base64,%s"
                               % (mime, base64.b64encode(data).decode())})
            self.inb = None
        elif ev.name == "xfer_fail":
            if self.inb is not None and ev.fields[0] == self.inb["xid"]:
                broadcast({"kind": "xfer", "node": self.i, "dir": "in",
                           "phase": "fail", "reason": ev.raw})
                self.inb = None
            else:
                self.evq.put(ev)         # an outbound worker may be waiting
        elif ev.name in ("xfer_next", "xfer_sent"):
            self.evq.put(ev)


xfers: list = []


def send_photo_worker(i, dest_i, name, data):
    """Drive one outbound Type 6 transfer; progress goes out over SSE."""
    xm, n = xfers[i], nodes[i]
    if not xfers[i].out_busy.acquire(blocking=False):
        broadcast({"kind": "xfer", "node": i, "dir": "out", "phase": "fail",
                   "reason": "a photo send is already running on this node"})
        return
    try:
        dest = eids[dest_i]
        if not dest:
            broadcast({"kind": "xfer", "node": i, "dir": "out", "phase": "fail",
                       "reason": "peer ML-EID not known yet (node still attaching?)"})
            return
        t0 = time.time()
        body = n.command(f"XFER BEGIN {dest} {len(data)} {name}")
        xid = body.split("xid=")[1].split()[0]
        n_chunks = (len(data) + CHUNK - 1) // CHUNK
        broadcast({"kind": "xfer", "node": i, "dir": "out", "phase": "begin",
                   "name": name, "total": len(data)})
        base = 0
        while True:
            nw = min(WINDOW, n_chunks - base)
            for k in range(nw):
                n.command("XFER DATA " + base64.b64encode(
                    data[(base + k) * CHUNK:(base + k + 1) * CHUNK]).decode())
            broadcast({"kind": "xfer", "node": i, "dir": "out",
                       "phase": "progress",
                       "pct": min(99, 100 * (base + nw) // n_chunks)})
            while True:
                ev = xm.evq.get(timeout=120)
                if ev.fields and ev.fields[0] == xid:
                    break                # ignore stale/other-transfer events
            if ev.name == "xfer_fail":
                broadcast({"kind": "xfer", "node": i, "dir": "out",
                           "phase": "fail", "reason": ev.raw})
                return
            if ev.name == "xfer_sent":
                broadcast({"kind": "xfer", "node": i, "dir": "out",
                           "phase": "sent", "name": name,
                           "secs": round(time.time() - t0, 1)})
                return
            base = int(ev.fields[1])     # !XFER_NEXT <xid> <base>
    except Exception as e:
        broadcast({"kind": "xfer", "node": i, "dir": "out", "phase": "fail",
                   "reason": str(e)})
    finally:
        xm.out_busy.release()


def attach_node(i, port):
    n = MagnetNode(port, timeout=20.0, keep_comments=True)

    def on_any(ev):
        ts = time.time()
        if ev.name in XFER_EVENTS:       # consumed here, never forwarded raw
            xfers[i].handle(ev, ts)
            return
        broadcast({"kind": "event", "node": i, "name": ev.name,
                   "fields": ev.fields, "text": ev.text, "raw": ev.raw,
                   "ts": ts})

    n.on("*", on_any)
    return n


def node_eid(n):
    n.command("MESH")
    for c in n.comments:
        if "ml-eid=" in c:
            return c.split("ml-eid=")[1].split()[0]
    return None


def snapshot(i, n):
    info = {"kind": "info", "node": i, "port": n._t.port, "ts": time.time()}
    try:
        info.update(n.channel())         # channel-show: name selector mcast …
        info.update(n.status())          # state role channel peers … (channel key wins)
        info.update(n.whoami())          # id name fw — node display name wins over
                                         # the channel's name= from CHANNEL SHOW
        if eids[i] is None and info.get("state") == "READY":
            eids[i] = node_eid(n)        # photo dest = the OTHER node's EID
        info["eid"] = eids[i]
    except Exception as e:               # node mid-reboot etc. — report, retry next round
        info["error"] = str(e)
    broadcast(info)


def poll_loop():
    while True:
        for i, n in enumerate(nodes):
            snapshot(i, n)
        time.sleep(5)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):           # quiet
        pass

    def do_GET(self):
        if self.path == "/nodes":
            body = json.dumps({"count": len(nodes),
                               "ports": [n._t.port for n in nodes]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path in ("/", "/index.html"):
            with open(os.path.join(HERE, "index.html"), "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            q = queue.Queue()
            with clients_lock:
                clients.append(q)
            # fresh snapshots for the newcomer (goes to everyone; harmless)
            for i, n in enumerate(nodes):
                threading.Thread(target=snapshot, args=(i, n), daemon=True).start()
            try:
                while True:
                    try:
                        data = q.get(timeout=15)
                        self.wfile.write(f"data: {data}\n\n".encode())
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                with clients_lock:
                    if q in clients:
                        clients.remove(q)
            return
        self.send_error(404)

    def do_POST(self):
        if self.path == "/sendphoto":
            n = int(self.headers.get("Content-Length", 0))
            req = json.loads(self.rfile.read(n))
            i = int(req["node"])
            # "to" is implied on a two-node bench, required beyond that
            dest_i = req.get("to", 1 - i if len(nodes) == 2 else None)
            if (dest_i is None or not 0 <= int(dest_i) < len(nodes)
                    or int(dest_i) == i):
                body = json.dumps({"ok": False,
                                   "err": "need 'to': a different node index"}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            # spaces would split the firmware's !XFER_BEGIN meta field
            name = (os.path.basename(str(req.get("name", "photo.jpg")))
                    .replace(" ", "_")[:64] or "photo.jpg")
            data = base64.b64decode(req["data"])
            threading.Thread(target=send_photo_worker,
                             args=(i, int(dest_i), name, data),
                             daemon=True).start()
            body = json.dumps({"ok": True, "size": len(data)}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/cmd":
            n = int(self.headers.get("Content-Length", 0))
            req = json.loads(self.rfile.read(n))
            i, line = int(req["node"]), str(req["line"])[:400]
            ts = time.time()
            try:
                body = nodes[i].command(line)
                rsp = {"ok": True, "body": body, "ts": ts}
            except HCPError as e:
                rsp = {"ok": False, "err": e.code, "ts": ts}
            except Exception as e:
                rsp = {"ok": False, "err": str(e), "ts": ts}
            out = json.dumps(rsp).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(out)))
            self.end_headers()
            self.wfile.write(out)
            return
        if self.path != "/send":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length", 0))
        req = json.loads(self.rfile.read(n))
        i, text = int(req["node"]), str(req["text"])[:400]
        ts = time.time()                 # before the serial write: true e2e start
        try:
            nodes[i].chat(text)
            rsp = {"ok": True, "ts": ts}
        except HCPError as e:
            rsp = {"ok": False, "err": e.code, "ts": ts}
        except Exception as e:
            rsp = {"ok": False, "err": str(e), "ts": ts}
        body = json.dumps(rsp).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    # `--http N` consumes its value, so it can't be mistaken for a port name
    # (the old split dropped explicit ports whenever --http was also given).
    argv, ports, http_port = sys.argv[1:], [], 8642
    i = 0
    while i < len(argv):
        a = argv[i]
        if a.startswith("--http"):
            if "=" in a:
                http_port = int(a.split("=", 1)[1])
            else:
                i += 1
                http_port = int(argv[i])
        else:
            ports.append(a)
        i += 1
    if ports and len(ports) < 2:
        print(f"need at least two serial ports (got {len(ports)}): {' '.join(ports)}")
        return 1
    ports = ports or DEFAULT_PORTS

    print(f"attaching {' + '.join(ports)} (boards reset on open, ~30 s to READY)")
    for i, p in enumerate(ports):
        eids.append(None)
        xfers.append(XferManager(i))
        nodes.append(attach_node(i, p))
    threading.Thread(target=poll_loop, daemon=True).start()

    srv = ThreadingHTTPServer(("127.0.0.1", http_port), Handler)
    print(f"chat bench: http://127.0.0.1:{http_port}/")
    srv.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
