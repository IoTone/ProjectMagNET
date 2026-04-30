#!/usr/bin/env python3
"""
push_bundles.py — laptop-side dev/CI helper.

Discovers a running MagNET ruler via mDNS, joins as a transient hive
client, and KV_PUTs each signed bundle .json file in the target directory.
Use case: edit a .forth file, sign it, push to a running Dial without
reflashing.

Usage:
    python push_bundles.py                              # push all bundles/
    python push_bundles.py --bundles ./my-bundles       # custom dir
    python push_bundles.py --hive lab-test              # non-default hive
    python push_bundles.py --ruler 10.0.0.101 --port 7447  # skip mDNS

CI usage:
    pip install -r requirements.txt
    python push_bundles.py --bundles bundles/ \\
        --ruler 10.0.0.101 \\
        --node-id MagNET-tools-ci

The script behaves like a regular hive node — full HELLO/WELCOME handshake,
HMAC-signed messages — so the ruler just sees another peer arriving.
"""

import argparse
import hashlib
import hmac
import json
import secrets
import socket
import struct
import sys
import time
import uuid
from datetime import datetime
from pathlib import Path

try:
    from zeroconf import ServiceBrowser, Zeroconf
except ImportError:
    print("zeroconf not installed. Run: pip install -r requirements.txt", file=sys.stderr)
    sys.exit(2)


DEFAULT_SECRET_HEX = (
    "A08F19C34B55D7E1F20A778899AABBCC"
    "DDEEFF112233445566778899AABBCCDD"
)
DEFAULT_HIVE      = "beehive-1"
DEFAULT_PORT      = 7447
DEFAULT_NODE_ID   = "MagNET-tools-laptop"
PROTO_VERSION     = 1
TS_SKEW_SEC       = 30
MAX_FRAME         = 4096


def log(msg):
    ts = datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def canonical(obj) -> str:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def make_nonce() -> str:
    return secrets.token_hex(16)


def compute_hmac(key: bytes, msg_type: str, nonce: str, ts: int, payload: dict) -> str:
    to_sign = f"{msg_type}|{nonce}|{ts}|{canonical(payload)}"
    return hmac.new(key, to_sign.encode("utf-8"), hashlib.sha256).hexdigest()


def send_msg(sock: socket.socket, secret: bytes, mtype: str, sender: str,
             to: str, payload: dict) -> None:
    nonce = make_nonce()
    ts = int(time.time())
    env = {
        "type":    mtype,
        "from":    sender,
        "to":      to,
        "nonce":   nonce,
        "ts":      ts,
        "payload": payload,
        "auth":    compute_hmac(secret, mtype, nonce, ts, payload),
    }
    data = json.dumps(env, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    sock.sendall(struct.pack(">I", len(data)) + data)


def recv_exact(sock: socket.socket, n: int) -> bytes | None:
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def recv_frame(sock: socket.socket) -> bytes | None:
    hdr = recv_exact(sock, 4)
    if not hdr:
        return None
    (length,) = struct.unpack(">I", hdr)
    if length == 0 or length > MAX_FRAME:
        return None
    return recv_exact(sock, length)


# ---- mDNS discovery ----------------------------------------------------

class RulerListener:
    def __init__(self, hive: str):
        self.hive = hive
        self.results = []  # list of (host, port)

    def add_service(self, zc, type_, name):
        info = zc.get_service_info(type_, name, timeout=2000)
        if info is None:
            return
        # Filter by TXT hive=<value>
        txt = {k.decode("ascii"): (v.decode("ascii") if v else "")
               for k, v in (info.properties or {}).items()}
        if txt.get("hive") != self.hive:
            return
        if info.parsed_addresses():
            host = info.parsed_addresses()[0]
            self.results.append((host, info.port))

    def update_service(self, *args, **kwargs): pass
    def remove_service(self, *args, **kwargs): pass


def discover(hive: str, timeout_s: float = 3.0):
    zc = Zeroconf()
    listener = RulerListener(hive)
    ServiceBrowser(zc, "_magnet-ruler._tcp.local.", listener)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if listener.results:
            break
        time.sleep(0.1)
    zc.close()
    return listener.results


# ---- Main pipeline -----------------------------------------------------

def push_bundles(host: str, port: int, secret: bytes, hive: str,
                 node_id: str, bundle_dir: Path, dry_run: bool) -> int:
    log(f"connecting to {host}:{port} as '{node_id}' (hive={hive})")
    sock = socket.create_connection((host, port), timeout=10)
    sock.settimeout(10)

    # HELLO
    send_msg(sock, secret, "HELLO", node_id, "*", {
        "role_requested": "tools",
        "chip":           "laptop",
        "fw":             "push_bundles.py",
        "hive":           hive,
        "caps":           ["tools", "kv-publisher"],
    })

    # Read WELCOME (also discards REJECTs early)
    raw = recv_frame(sock)
    if not raw:
        log("connection closed before WELCOME")
        sock.close()
        return 1
    env = json.loads(raw.decode("utf-8"))
    if env["type"] != "WELCOME":
        log(f"got {env['type']} instead of WELCOME: {env.get('payload')}")
        sock.close()
        return 1
    log(f"WELCOME session={env['payload'].get('session_id','?')[:8]} role={env['payload'].get('role')}")

    # Push each bundle
    bundle_paths = sorted(bundle_dir.glob("*.json"))
    if not bundle_paths:
        log(f"no .json files in {bundle_dir}")
        sock.close()
        return 1

    pushed = 0
    for path in bundle_paths:
        try:
            content = path.read_text()
            envelope = json.loads(content)
            name = envelope.get("name")
            if not name:
                log(f"  skip {path.name}: missing 'name' field")
                continue
            key = f"bundle:{name}"
            if dry_run:
                log(f"  would push {key} ({len(content)} bytes)")
            else:
                send_msg(sock, secret, "KV_PUT", node_id, "*",
                         {"key": key, "value": content})
                log(f"  pushed {key} ({len(content)} bytes)")
                pushed += 1
        except Exception as e:
            log(f"  fail {path.name}: {e}")

    log(f"done — pushed {pushed} bundle(s)")
    # Be tidy: read whatever else the ruler sends for a moment, then close.
    sock.settimeout(0.5)
    try:
        sock.recv(4096)
    except (socket.timeout, OSError):
        pass
    sock.close()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bundles",    default="bundles", help="Directory containing signed *.json")
    ap.add_argument("--hive",       default=DEFAULT_HIVE)
    ap.add_argument("--ruler",      default=None,      help="Skip mDNS, connect to this host/IP directly")
    ap.add_argument("--port",       type=int, default=DEFAULT_PORT)
    ap.add_argument("--node-id",    default=DEFAULT_NODE_ID)
    ap.add_argument("--secret-hex", default=DEFAULT_SECRET_HEX)
    ap.add_argument("--dry-run",    action="store_true",
                    help="List what would be pushed; don't actually KV_PUT")
    args = ap.parse_args()

    secret = bytes.fromhex(args.secret_hex)
    if len(secret) != 32:
        print(f"--secret-hex must be 32 bytes (got {len(secret)})", file=sys.stderr)
        return 2

    bundle_dir = Path(args.bundles)
    if not bundle_dir.is_dir():
        print(f"--bundles path is not a directory: {bundle_dir}", file=sys.stderr)
        return 2

    if args.ruler:
        host, port = args.ruler, args.port
    else:
        log(f"discovering rulers via mDNS (hive={args.hive})...")
        results = discover(args.hive)
        if not results:
            print(f"no ruler found via mDNS (hive={args.hive}). "
                  f"Pass --ruler <ip> to skip discovery.", file=sys.stderr)
            return 1
        host, port = results[0]
        log(f"found ruler at {host}:{port}")

    return push_bundles(host, port, secret, args.hive, args.node_id,
                        bundle_dir, args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
