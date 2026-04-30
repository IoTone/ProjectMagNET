#!/usr/bin/env python3
"""
sign_bundle.py — author + sign a MagNET role bundle.

Reads a Forth source file, computes CRC-32, base64-encodes, signs with
HMAC-SHA256 (must match the firmware's keys.h dev key for v1), and writes
a JSON envelope to stdout (or --out).

Usage:
    python sign_bundle.py spy.forth \\
        --name spy --version 1.0.0 --author iotone-dev \\
        --caps-req camera,jpeg \\
        > spy.json

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


def main() -> int:
    ap = argparse.ArgumentParser(description="Sign a MagNET role bundle.")
    ap.add_argument("source",       help="Path to a .forth source file")
    ap.add_argument("--name",       required=True, help="Role name (e.g. 'spy')")
    ap.add_argument("--version",    required=True, help="Semver e.g. 1.0.0")
    ap.add_argument("--author",     required=True, help="Author tag matching trust-store entry")
    ap.add_argument("--caps-req",   default="",    help="Comma-separated caps required, e.g. camera,jpeg")
    ap.add_argument("--deps",       default="",    help="Comma-separated dependency names (unused in v1)")
    ap.add_argument("--min-proto",  type=int, default=1)
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
        "sig_alg":   "hmac-sha256",
        "sig":       sig,
        "src_b64":   src_b64,
    }
    out_text = json.dumps(envelope, indent=2)

    if args.out == "-":
        print(out_text)
    else:
        Path(args.out).write_text(out_text)
        print(f"wrote {args.out} ({len(out_text)} bytes)", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
