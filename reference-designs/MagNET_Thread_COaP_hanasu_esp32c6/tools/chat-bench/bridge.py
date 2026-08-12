#!/usr/bin/env python3
"""chat-bench bridge — two Hanasu nodes ⇄ one browser page.

Wraps a MagnetNode (host-sdk) around each serial port and exposes:

    GET  /            the SolidJS chat page (index.html, same dir)
    GET  /events      SSE stream: every ! event from both nodes + periodic
                      STATUS/CHANNEL/WHOAMI snapshots, all stamped with the
                      bridge clock (one clock ⇒ honest cross-node latency)
    POST /send        {"node": 0|1, "text": "..."} → CHAT on that node;
                      responds {"ok": true, "ts": <bridge time before write>}

    python3 bridge.py [port0 port1] [--http 8642]

Opening a serial port resets the node (DTR/RTS toggle) — both boards reboot
when the bridge starts and re-attach in ~30 s; the page shows the state live.
Don't run hcp.py against the same ports while the bridge holds them.

Stdlib only (plus pyserial via the SDK). No build step, no dependencies.
"""
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


def broadcast(obj):
    data = json.dumps(obj)
    with clients_lock:
        for q in clients:
            q.put(data)


def attach_node(i, port):
    n = MagnetNode(port, timeout=10.0)

    def on_any(ev):
        broadcast({"kind": "event", "node": i, "name": ev.name,
                   "fields": ev.fields, "text": ev.text, "raw": ev.raw,
                   "ts": time.time()})

    n.on("*", on_any)
    return n


def snapshot(i, n):
    info = {"kind": "info", "node": i, "port": n._t.port, "ts": time.time()}
    try:
        info.update(n.channel())         # channel-show: name selector mcast …
        info.update(n.status())          # state role channel peers … (channel key wins)
        info.update(n.whoami())          # id name fw — node display name wins over
                                         # the channel's name= from CHANNEL SHOW
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
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    ports = args if len(args) == 2 else DEFAULT_PORTS
    http_port = 8642
    for a in sys.argv[1:]:
        if a.startswith("--http"):
            http_port = int(a.split("=")[1] if "=" in a else sys.argv[sys.argv.index(a) + 1])

    print(f"attaching {ports[0]} + {ports[1]} (boards reset on open, ~30 s to READY)")
    for i, p in enumerate(ports):
        nodes.append(attach_node(i, p))
    threading.Thread(target=poll_loop, daemon=True).start()

    srv = ThreadingHTTPServer(("127.0.0.1", http_port), Handler)
    print(f"chat bench: http://127.0.0.1:{http_port}/")
    srv.serve_forever()


if __name__ == "__main__":
    main()
