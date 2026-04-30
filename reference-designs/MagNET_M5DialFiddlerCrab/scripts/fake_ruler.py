#!/usr/bin/env python3
"""
fake_ruler.py — laptop-side test ruler for MagNET hive protocol v1.

Advertises `_magnet-ruler._tcp` via mDNS on the local LAN, listens on TCP
port 7447, validates HMAC-SHA256 on incoming messages, and auto-accepts any
valid HELLO with a fresh session id. See
`../docs/MagNET-HiveProtocol-v1.md` for the on-wire spec.

Usage:
    python fake_ruler.py                             # defaults
    python fake_ruler.py --hive lab-test --port 7447
    python fake_ruler.py --secret-hex <64hex>

Install deps:
    python -m venv .venv && source .venv/bin/activate
    pip install -r requirements.txt

macOS note:
    If the script runs but no ESP32 nodes connect, check macOS Privacy
    settings: System Settings → Privacy & Security → Local Network →
    enable Terminal and Python. Also check the macOS firewall. See
    ../docs/macOS-LAN-networking.md for the full troubleshooting guide.
"""

import argparse
import hashlib
import hmac
import json
import secrets
import socket
import struct
import sys
import threading
import time
import uuid
from datetime import datetime

from zeroconf import ServiceInfo, Zeroconf

# Must match CRAW_HIVE_DEV_SECRET in components/craw_hive/craw_hive.h.
DEFAULT_SECRET_HEX = (
    "A08F19C34B55D7E1F20A778899AABBCC"
    "DDEEFF112233445566778899AABBCCDD"
)
DEFAULT_HIVE     = "beehive-1"
DEFAULT_PORT     = 7447
PROTO_VERSION    = 1
HEARTBEAT_SEC    = 30
TS_SKEW_SEC      = 30
MAX_FRAME        = 4096

# Set from --log-hmac. When true, print the exact HMAC-signed string for
# every send and every receive so canonical-JSON drift between C and Python
# can be diagnosed (alphabetical-key order, number formatting, escaping).
LOG_HMAC = False


def log(msg):
    ts = datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def canonical(obj) -> str:
    """Produce the same canonical JSON as craw_hive_proto.c canonicalize_object:
    alphabetically-sorted keys, no whitespace, standard JSON escaping.
    """
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def compute_hmac(key: bytes, msg_type: str, nonce: str, ts: int, payload: dict) -> str:
    """Must match sign_input() in craw_hive_proto.c exactly:
    "<type>|<nonce>|<ts>|<canonical-payload>"
    """
    to_sign = f"{msg_type}|{nonce}|{ts}|{canonical(payload)}"
    return hmac.new(key, to_sign.encode("utf-8"), hashlib.sha256).hexdigest()


def make_nonce() -> str:
    """16 bytes, hex-encoded = 32 chars. Matches CRAW_HIVE_NONCE_BYTES."""
    return secrets.token_hex(16)


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


def send_frame(sock: socket.socket, env: dict) -> None:
    data = json.dumps(env, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    sock.sendall(struct.pack(">I", len(data)) + data)


def verify(key: bytes, raw: bytes) -> tuple[dict | None, str | None]:
    try:
        env = json.loads(raw.decode("utf-8"))
    except Exception:
        if LOG_HMAC:
            log(f"  [hmac rx] parse fail, raw = {raw!r}")
        return None, "parse"
    required = ("type", "from", "to", "nonce", "ts", "payload", "auth")
    if not all(k in env for k in required):
        return None, "schema"
    now = int(time.time())
    ts = int(env["ts"])
    if abs(ts - now) > TS_SKEW_SEC:
        if LOG_HMAC:
            log(f"  [hmac rx] ts_skew: got {ts}, local now {now}, diff {ts - now}s")
        return None, "ts_skew"
    to_sign  = f"{env['type']}|{env['nonce']}|{ts}|{canonical(env['payload'])}"
    expected = hmac.new(key, to_sign.encode("utf-8"), hashlib.sha256).hexdigest()
    got      = env["auth"]
    if LOG_HMAC:
        log(f"  [hmac rx] signed  = {to_sign!r}")
        log(f"  [hmac rx] expect  = {expected}")
        log(f"  [hmac rx] got     = {got}")
    if not hmac.compare_digest(expected, got):
        if not LOG_HMAC:
            # Always surface the mismatch detail once, even without the flag —
            # it's the single most common debug case.
            log(f"  [hmac rx] MISMATCH")
            log(f"  [hmac rx] signed  = {to_sign!r}")
            log(f"  [hmac rx] expect  = {expected}")
            log(f"  [hmac rx] got     = {got}")
        return None, "auth"
    return env, None


class Ruler:
    def __init__(self, hive: str, secret: bytes, ruler_id: str):
        self.hive = hive
        self.secret = secret
        self.ruler_id = ruler_id
        # Milestone C step 1 — in-memory KV table shared across all peer
        # connections. Mirrors the C ruler's behavior; lock-free since the
        # GIL serializes Python thread access for our small ops.
        self.kv: dict[str, str] = {}

    def send(self, sock: socket.socket, mtype: str, to: str, payload: dict) -> None:
        nonce = make_nonce()
        ts = int(time.time())
        to_sign = f"{mtype}|{nonce}|{ts}|{canonical(payload)}"
        auth = hmac.new(self.secret, to_sign.encode("utf-8"), hashlib.sha256).hexdigest()
        if LOG_HMAC:
            log(f"  [hmac tx] signed  = {to_sign!r}")
            log(f"  [hmac tx] auth    = {auth}")
        env = {
            "type":    mtype,
            "from":    self.ruler_id,
            "to":      to,
            "nonce":   nonce,
            "ts":      ts,
            "payload": payload,
            "auth":    auth,
        }
        send_frame(sock, env)

    def reject(self, sock: socket.socket, to: str, reason: str) -> None:
        self.send(sock, "REJECT", to, {"reason": reason})

    def handle(self, sock: socket.socket, addr: tuple[str, int]) -> None:
        peer = f"{addr[0]}:{addr[1]}"
        log(f"→ TCP connect from {peer}")
        node_id = "?"
        role = "spawn"
        try:
            # First frame must be HELLO.
            raw = recv_frame(sock)
            if not raw:
                log(f"  {peer} closed before HELLO")
                return
            env, err = verify(self.secret, raw)
            if err:
                log(f"  REJECT ({err}) from {peer}")
                self.reject(sock, "*", err)
                return
            if env["type"] != "HELLO":
                log(f"  expected HELLO from {peer}, got {env['type']}")
                return

            node_id = env["from"]
            payload = env["payload"]
            hive    = payload.get("hive", "")
            role    = payload.get("role_requested", "spawn")
            caps    = payload.get("caps", [])
            chip    = payload.get("chip", "?")
            fw      = payload.get("fw", "?")

            if hive != self.hive:
                log(f"  REJECT (hive_mismatch: got '{hive}', want '{self.hive}') from {node_id}")
                self.reject(sock, node_id, "hive_mismatch")
                return

            log(f"  HELLO {node_id} chip={chip} fw={fw} role={role} caps={caps}")
            session_id = str(uuid.uuid4())
            self.send(sock, "WELCOME", node_id, {
                "session_id": session_id,
                "role":       role,
                "heartbeat":  HEARTBEAT_SEC,
            })
            log(f"  ← WELCOME {node_id} session={session_id[:8]} role={role}")

            # Stay connected: echo PING, log role changes + unknown types.
            while True:
                raw = recv_frame(sock)
                if not raw:
                    log(f"  {node_id} disconnected")
                    return
                env, err = verify(self.secret, raw)
                if err:
                    log(f"  {node_id} bad frame ({err})")
                    continue
                mtype = env["type"]
                if mtype == "PING":
                    self.send(sock, "PING", node_id, {})
                    log(f"  ♥ PING {node_id}")
                elif mtype == "ROLE_REQUEST":
                    new_role = env["payload"].get("role_requested", role)
                    log(f"  ROLE_REQUEST {node_id}: {role} → {new_role}")
                    role = new_role
                    self.send(sock, "ROLE_GRANT", node_id, {
                        "role":       new_role,
                        "bundle_url": None,
                        "bundle_sig": None,
                    })
                elif mtype == "KV_GET":
                    key = env["payload"].get("key", "")
                    if key in self.kv:
                        log(f"  KV_GET {node_id}: '{key}' → '{self.kv[key]}'")
                        self.send(sock, "KV_DATA", node_id,
                                  {"key": key, "value": self.kv[key]})
                    else:
                        log(f"  KV_GET {node_id}: '{key}' → not_found")
                        self.send(sock, "KV_NOT_FOUND", node_id, {"key": key})
                elif mtype == "KV_PUT":
                    key = env["payload"].get("key", "")
                    val = env["payload"].get("value", "")
                    if key:
                        self.kv[key] = val
                        log(f"  KV_PUT {node_id}: '{key}' = '{val[:60]}'"
                            f"{'…' if len(val) > 60 else ''}")
                else:
                    log(f"  {mtype} {node_id}: {env['payload']}")
        except OSError as e:
            log(f"  {node_id} socket error: {e}")
        finally:
            sock.close()


def detect_local_ip() -> str:
    """UDP-connect trick: resolves which interface the OS would use for LAN
    traffic without sending anything."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()


def main() -> int:
    ap = argparse.ArgumentParser(description="MagNET fake ruler (dev/test).")
    ap.add_argument("--hive",       default=DEFAULT_HIVE,       help="hive id TXT value")
    ap.add_argument("--port",       type=int, default=DEFAULT_PORT, help="TCP port")
    ap.add_argument("--secret-hex", default=DEFAULT_SECRET_HEX, help="32-byte shared secret in hex")
    ap.add_argument("--bind",       default="0.0.0.0",          help="TCP bind address")
    ap.add_argument("--log-hmac",   action="store_true",
                    help="print the HMAC-signed string for every frame (sent + received); "
                         "on mismatch, the signed string + expected/got are always printed")
    args = ap.parse_args()

    global LOG_HMAC
    LOG_HMAC = args.log_hmac

    try:
        secret = bytes.fromhex(args.secret_hex)
    except ValueError as e:
        print(f"bad --secret-hex: {e}", file=sys.stderr)
        return 2
    if len(secret) != 32:
        print(f"secret must be 32 bytes (got {len(secret)})", file=sys.stderr)
        return 2

    local_ip = detect_local_ip()
    hostname = socket.gethostname().split(".")[0]
    short    = hostname[:8].replace(" ", "-") or "ruler"
    ruler_id = f"MagNET-ruler-{short}"

    # mDNS advertisement
    zc = Zeroconf()
    service_name = f"{short}._magnet-ruler._tcp.local."
    info = ServiceInfo(
        type_="_magnet-ruler._tcp.local.",
        name=service_name,
        addresses=[socket.inet_aton(local_ip)],
        port=args.port,
        properties={"ver": str(PROTO_VERSION), "hive": args.hive},
        server=f"{short}-ruler.local.",
    )
    zc.register_service(info)

    log(f"listening on {local_ip}:{args.port} (bind {args.bind})")
    log(f"hive = {args.hive}  ruler_id = {ruler_id}")
    log(f"mDNS service = {service_name}  TXT ver={PROTO_VERSION} hive={args.hive}")

    # TCP accept loop
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(8)

    ruler = Ruler(args.hive, secret, ruler_id)

    try:
        while True:
            sock, addr = srv.accept()
            t = threading.Thread(target=ruler.handle, args=(sock, addr), daemon=True)
            t.start()
    except KeyboardInterrupt:
        log("shutting down")
    finally:
        zc.unregister_service(info)
        zc.close()
        srv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
