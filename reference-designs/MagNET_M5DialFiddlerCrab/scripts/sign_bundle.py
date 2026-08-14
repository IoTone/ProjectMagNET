#!/usr/bin/env python3
"""
sign_bundle.py — author + sign a MagNET role bundle.

Reads a Forth source file, computes CRC-32, base64-encodes, signs, and writes
a JSON envelope to stdout (or --out). Two signature algorithms:

  hmac-sha256 (v1, default) — shared hive secret; proves hive membership only.
  ed25519 (v2)              — per-author private key; proves the author.
                              The dev seed lives in dev_ed25519.key next to
                              this script; its public half is baked into the
                              firmware's keys.h.

Usage:
    python sign_bundle.py spy.forth \\
        --name spy --version 1.0.0 --author iotone-dev \\
        --caps-req camera,jpeg \\
        > spy.json

    # v2 — Ed25519 (dev key picked up automatically):
    python sign_bundle.py spy.forth --alg ed25519 \\
        --name spy --version 1.0.0 --author iotone-dev > spy.json

Ed25519 uses the 'cryptography' package when importable (the ESP-IDF python
env has it) and otherwise falls back to a bundled pure-Python RFC 8032
implementation — slower (tens of ms) but dependency-free.

    # send to a Scribe via the laptop fake-ruler's KV table (or via a real Scribe):
    # (out of band — the bundle JSON is just KV data once signed)

The signing input format MUST match craw_role_bundle's
craw_role_bundle_signing_input() exactly:

    "<name>|<version>|<min_proto>|<author>|<crc32_hex>|<src_b64>"

If a node ever rejects with BUNDLE_ERR_SIG, run with --verbose to see the
exact bytes signed and compare against the C side's diagnostics.
"""

import argparse
import base64
import binascii
import hashlib
import hmac
import json
import sys
import zlib
from pathlib import Path

# Same bytes as CRAW_HIVE_DEV_SECRET / CRAW_ROLE_BUNDLE_DEV_HMAC_KEY.
DEFAULT_SECRET_HEX = (
    "A08F19C34B55D7E1F20A778899AABBCC"
    "DDEEFF112233445566778899AABBCCDD"
)

# Dev Ed25519 seed (public half baked into keys.h as
# CRAW_ROLE_BUNDLE_DEV_ED25519_PUB). Production authors pass --key-file.
DEFAULT_ED25519_KEY_FILE = Path(__file__).parent / "dev_ed25519.key"


def _ed25519_sign_pure(seed: bytes, msg: bytes) -> bytes:
    """RFC 8032 Ed25519, reference-style. Fallback when 'cryptography' is
    unavailable. Slow (tens of ms) but exact."""
    q = 2**255 - 19
    l = 2**252 + 27742317777372353535851937790883648493

    def H(m): return hashlib.sha512(m).digest()
    def inv(x): return pow(x, q - 2, q)
    d = -121665 * inv(121666) % q
    I = pow(2, (q - 1) // 4, q)

    def xrecover(y):
        xx = (y * y - 1) * inv(d * y * y + 1)
        x = pow(xx, (q + 3) // 8, q)
        if (x * x - xx) % q != 0:
            x = (x * I) % q
        if x % 2 != 0:
            x = q - x
        return x

    By = 4 * inv(5) % q
    B = (xrecover(By), By)

    def edwards(P, Q):
        x1, y1 = P
        x2, y2 = Q
        x3 = (x1 * y2 + x2 * y1) * inv(1 + d * x1 * x2 * y1 * y2)
        y3 = (y1 * y2 + x1 * x2) * inv(1 - d * x1 * x2 * y1 * y2)
        return (x3 % q, y3 % q)

    def scalarmult(P, e):
        Q = (0, 1)
        while e:
            if e & 1:
                Q = edwards(Q, P)
            P = edwards(P, P)
            e >>= 1
        return Q

    def encodepoint(P):
        x, y = P
        return int.to_bytes(y | ((x & 1) << 255), 32, "little")

    h = H(seed)
    a = (2**254 | int.from_bytes(h[:32], "little") & ~(2**255 | 2**254 | 7))
    A = encodepoint(scalarmult(B, a))
    r = int.from_bytes(H(h[32:] + msg), "little") % l
    R = encodepoint(scalarmult(B, r))
    S = (r + int.from_bytes(H(R + A + msg), "little") * a) % l
    return R + int.to_bytes(S, 32, "little")


def ed25519_sign(seed: bytes, msg: bytes) -> bytes:
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        return Ed25519PrivateKey.from_private_bytes(seed).sign(msg)
    except ImportError:
        return _ed25519_sign_pure(seed, msg)


def main() -> int:
    ap = argparse.ArgumentParser(description="Sign a MagNET role bundle.")
    ap.add_argument("source",       help="Path to a .forth source file")
    ap.add_argument("--name",       required=True, help="Role name (e.g. 'spy')")
    ap.add_argument("--version",    required=True, help="Semver e.g. 1.0.0")
    ap.add_argument("--author",     required=True, help="Author tag matching trust-store entry")
    ap.add_argument("--caps-req",   default="",    help="Comma-separated caps required, e.g. camera,jpeg")
    ap.add_argument("--deps",       default="",    help="Comma-separated dependency names (unused in v1)")
    ap.add_argument("--min-proto",  type=int, default=1)
    ap.add_argument("--tick-ms",    type=int, default=None,
                    help="role-tick cadence in ms (H5 lifecycle); omitted = host default 1000")
    ap.add_argument("--alg",        choices=["hmac-sha256", "ed25519"],
                    default="hmac-sha256", help="Signature algorithm (v1 HMAC or v2 Ed25519)")
    ap.add_argument("--key-file",   default=str(DEFAULT_ED25519_KEY_FILE),
                    help="ed25519 only: file holding the 32-byte private seed as hex")
    ap.add_argument("--secret-hex", default=DEFAULT_SECRET_HEX)
    ap.add_argument("--out",        default="-",   help="Output JSON path (default stdout)")
    ap.add_argument("--verbose",    action="store_true",
                    help="Print signing input + signature on stderr")
    args = ap.parse_args()

    secret = bytes.fromhex(args.secret_hex)
    if len(secret) != 32:
        print(f"--secret-hex must be 32 bytes (got {len(secret)})", file=sys.stderr)
        return 2

    src_bytes = Path(args.source).read_bytes()
    if not src_bytes:
        print("source file is empty", file=sys.stderr)
        return 2
    if len(src_bytes) > 4096:
        print(f"source is {len(src_bytes)} bytes; firmware limit is 4096", file=sys.stderr)
        return 2

    # Standard CRC-32 (poly 0xEDB88320 reflected, init 0xFFFFFFFF, XOR-out 0xFFFFFFFF)
    # — matches esp_rom_crc32_le's expected protocol.
    crc = zlib.crc32(src_bytes) & 0xFFFFFFFF
    crc_hex = f"{crc:08x}"

    src_b64 = base64.b64encode(src_bytes).decode("ascii")

    # Canonical signing input — order MUST match craw_role_bundle_signing_input()
    signing_input = f"{args.name}|{args.version}|{args.min_proto}|{args.author}|{crc_hex}|{src_b64}"

    if args.alg == "ed25519":
        key_text = Path(args.key_file).read_text().strip()
        seed = bytes.fromhex(key_text)
        if len(seed) != 32:
            print(f"--key-file must hold a 32-byte hex seed (got {len(seed)})", file=sys.stderr)
            return 2
        sig = ed25519_sign(seed, signing_input.encode("utf-8")).hex()
    else:
        sig = hmac.new(secret, signing_input.encode("utf-8"), hashlib.sha256).hexdigest()

    if args.verbose:
        print(f"[sign] crc32={crc_hex}", file=sys.stderr)
        print(f"[sign] signing_input ({len(signing_input)} bytes):", file=sys.stderr)
        print(f"  {signing_input!r}", file=sys.stderr)
        print(f"[sign] sig={sig}", file=sys.stderr)

    caps = [c.strip() for c in args.caps_req.split(",") if c.strip()]
    deps = [d.strip() for d in args.deps.split(",")     if d.strip()]

    envelope = {
        "name":      args.name,
        "version":   args.version,
        "min_proto": args.min_proto,
        "author":    args.author,
        "caps_req":  caps,
        "deps":      deps,
        "crc32":     crc_hex,
        "sig_alg":   args.alg,
        "sig":       sig,
        "src_b64":   src_b64,
    }
    if args.tick_ms is not None:
        envelope["tick_ms"] = args.tick_ms
    out_text = json.dumps(envelope, indent=2)

    if args.out == "-":
        print(out_text)
    else:
        Path(args.out).write_text(out_text)
        print(f"wrote {args.out} ({len(out_text)} bytes)", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
