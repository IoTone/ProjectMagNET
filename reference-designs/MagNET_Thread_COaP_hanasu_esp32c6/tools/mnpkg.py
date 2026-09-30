#!/usr/bin/env python3
"""mnpkg.py — build, inspect and verify MagNET OTA packages (.mnpkg).

Normative format: docs/OTA-PACKAGE.md (wire format 1, document rev 1.1).
Needs the `cryptography` package (the Zephyr venv has it:
~/zephyrproject/.venv/bin/python).

    mnpkg.py keygen  --alg ed25519 -o release.pem           # prints key_id + public key
    mnpkg.py build   --chip esp32c6 --board xiao-esp32c6 --variant thread \\
                     --version 0.8.0+1 --fw-version 0.8.0-zf \\
                     --key release.pem firmware.bin -o node.mnpkg
    mnpkg.py inspect node.mnpkg
    mnpkg.py verify  node.mnpkg --pubkey release.pem         # checks 1-8, offline
    mnpkg.py c-keys  key1.pem [key2.pem]                     # release-key store for firmware

The DEV release keys in tools/keys/ are for the bench only: `build` refuses
them unless the variant starts with "dev" (spec §6).
"""
import argparse
import hashlib
import struct
import sys
import time
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, ed25519
from cryptography.hazmat.primitives.asymmetric.utils import (
    decode_dss_signature, encode_dss_signature)
from cryptography.exceptions import InvalidSignature

MAGIC = b"MNPK"
HDR_LEN = 256
SIGNED_LEN = 192
SIG_P256, SIG_ED25519 = 1, 2
FLAG_REQUIRES_REPARTITION, FLAG_CLEARS_BUNDLES = 0x01, 0x02
FMT_ESP_IDF, FMT_MCUBOOT = 1, 2

# ---- registry (spec §3.1) — append-only, keep in step with the spec ----
CHIPS = {                         # name: (id, image_format)
    "esp32c6":   (0x0001, FMT_ESP_IDF),
    "efr32mg24": (0x0002, FMT_MCUBOOT),
    "esp32":     (0x0003, FMT_ESP_IDF),
    "esp32s3":   (0x0004, FMT_ESP_IDF),
    "esp32c3":   (0x0005, FMT_ESP_IDF),
    "nrf52840":  (0x0006, FMT_MCUBOOT),
}
BOARDS = {
    "any": 0x0000,
    "m5nanoc6": 0x0101, "xiao-esp32c6": 0x0102, "esp32c6-devkitc-1": 0x0103,
    "waveshare-c6-lcd-1.47": 0x0104, "waveshare-c6-touch-lcd-1.47": 0x0105,
    "mr60bha2": 0x0106,
    "xiao-mg24": 0x0201,
    "esp32-cam": 0x0301, "atom-matrix": 0x0302, "atom-echo": 0x0303,
    "m5camera-x": 0x0304, "m5stickc-plus": 0x0305,
    "m5dial": 0x0401, "m5capsule": 0x0402, "atoms3": 0x0403, "atoms3r-m12": 0x0404,
    "stamps3": 0x0405, "unit-cams3": 0x0406, "respeaker-lite": 0x0407,
    "m5stamp-c3u": 0x0501, "xiao-esp32c3": 0x0502,
    "xiao-nrf52840": 0x0601,
}
CHIP_NAME = {v[0]: k for k, v in CHIPS.items()}
BOARD_NAME = {v: k for k, v in BOARDS.items()}

KEYS_DIR = Path(__file__).parent / "keys"


class PkgError(Exception):
    """Carries the spec §4 failure code."""
    def __init__(self, code, detail):
        super().__init__(f"{code}: {detail}")
        self.code = code


# ---- versions: "maj.min.rev+build" <-> 8 bytes ----
def parse_version(s):
    main, _, build = s.partition("+")
    parts = [int(x) for x in main.split(".")]
    if len(parts) != 3:
        raise ValueError(f"version must be MAJOR.MINOR.REV[+BUILD]: {s!r}")
    return (parts[0], parts[1], parts[2], int(build or 0))


def pack_version(v):
    maj, mnr, rev, bld = v
    return struct.pack(">BBHI", maj, mnr, rev, bld)


def unpack_version(b):
    return struct.unpack(">BBHI", b)


def fmt_version(v):
    return f"{v[0]}.{v[1]}.{v[2]}+{v[3]}"


# ---- keys ----
def load_private(path):
    return serialization.load_pem_private_key(Path(path).read_bytes(), password=None)


def load_public(path):
    data = Path(path).read_bytes()
    try:
        return serialization.load_pem_public_key(data)
    except ValueError:
        return load_private(path).public_key()


def key_alg(key):
    if isinstance(key, (ed25519.Ed25519PrivateKey, ed25519.Ed25519PublicKey)):
        return SIG_ED25519
    if isinstance(key, (ec.EllipticCurvePrivateKey, ec.EllipticCurvePublicKey)):
        if not isinstance(key.curve, ec.SECP256R1):
            raise ValueError("ECDSA release keys must be P-256")
        return SIG_P256
    raise ValueError("release key must be Ed25519 or ECDSA P-256")


def pub_bytes(pub):
    """The key 'as stored' (spec §3): 32 raw bytes, or the 65-byte point."""
    if key_alg(pub) == SIG_ED25519:
        return pub.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return pub.public_bytes(serialization.Encoding.X962,
                            serialization.PublicFormat.UncompressedPoint)


def key_id(pub):
    return hashlib.sha256(pub_bytes(pub)).digest()[:8]


def dev_key_ids():
    ids = set()
    for p in KEYS_DIR.glob("dev_release_*.pem"):
        ids.add(key_id(load_private(p).public_key()))
    return ids


def sign(priv, msg):
    if key_alg(priv) == SIG_ED25519:
        return priv.sign(msg)
    der = priv.sign(msg, ec.ECDSA(hashes.SHA256(), deterministic_signing=True))
    r, s = decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def verify_sig(pub, sig, msg):
    try:
        if key_alg(pub) == SIG_ED25519:
            pub.verify(sig, msg)
        else:
            der = encode_dss_signature(int.from_bytes(sig[:32], "big"),
                                       int.from_bytes(sig[32:], "big"))
            pub.verify(der, msg, ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


# ---- payload self-checks (spec §5, check 8) ----
def check_payload(fmt, img):
    if fmt == FMT_ESP_IDF:
        # esp_image_header_t: magic 0xE9, segment count, …; 24-byte header
        if len(img) < 24 or img[0] != 0xE9:
            raise PkgError("E_PKG_IMAGE", "not an ESP-IDF app image (magic 0xE9)")
        return f"ESP-IDF app image, {img[1]} segments"
    if fmt == FMT_MCUBOOT:
        if len(img) < 32 or struct.unpack_from("<I", img, 0)[0] != 0x96F3B83D:
            raise PkgError("E_PKG_IMAGE", "not an MCUboot image (magic 0x96f3b83d)")
        hdr_sz = struct.unpack_from("<H", img, 8)[0]
        img_sz = struct.unpack_from("<I", img, 12)[0]
        tlv = hdr_sz + img_sz
        if tlv + 4 > len(img) or struct.unpack_from("<H", img, tlv)[0] not in (0x6907, 0x6908):
            raise PkgError("E_PKG_IMAGE", "MCUboot header sizes do not reach a TLV area")
        v = struct.unpack_from("<BBHI", img, 20)
        return f"MCUboot image v{fmt_version(v)}, header {hdr_sz} B, body {img_sz} B"
    raise PkgError("E_PKG_IMAGE", f"unknown image_format {fmt}")


# ---- header ----
def pad(s, n):
    b = s.encode("ascii")
    if len(b) > n:
        raise ValueError(f"{s!r} longer than {n} bytes")
    return b + b"\0" * (n - len(b))


def build_header(chip, board, fmt, flags, version, min_running, img, variant,
                 fw_version, build_time, priv):
    pub = priv.public_key()
    h = bytearray(HDR_LEN)
    struct.pack_into(">4sBBHHHBBH", h, 0, MAGIC, 1, key_alg(priv), HDR_LEN,
                     chip, board, fmt, flags, 0)
    h[16:24] = pack_version(version)
    h[24:32] = pack_version(min_running)
    struct.pack_into(">I", h, 32, len(img))
    h[36:68] = hashlib.sha256(img).digest()
    struct.pack_into(">q", h, 68, build_time)
    h[76:92] = pad(variant, 16)
    h[92:124] = pad(fw_version, 32)
    h[124:132] = key_id(pub)
    h[192:256] = sign(priv, bytes(h[:SIGNED_LEN]))
    return bytes(h)


def parse_header(h):
    if len(h) < HDR_LEN:
        raise PkgError("E_PKG_FORMAT", "shorter than a header")
    magic, fv, alg, hlen, chip, board, fmt, flags, res = struct.unpack_from(">4sBBHHHBBH", h, 0)
    d = dict(magic=magic, format_version=fv, sig_alg=alg, header_len=hlen, chip=chip,
             board=board, image_format=fmt, flags=flags, reserved14=res,
             version=unpack_version(h[16:24]), min_running=unpack_version(h[24:32]),
             image_len=struct.unpack_from(">I", h, 32)[0], image_sha256=h[36:68],
             build_time=struct.unpack_from(">q", h, 68)[0],
             variant=h[76:92].rstrip(b"\0").decode("ascii", "replace"),
             fw_version=h[92:124].rstrip(b"\0").decode("ascii", "replace"),
             key_id=h[124:132], reserved132=h[132:192], signature=h[192:256])
    return d


def check_structure(d):
    """Spec §4 check 1."""
    if (d["magic"] != MAGIC or d["format_version"] != 1 or d["header_len"] != HDR_LEN
            or d["sig_alg"] not in (SIG_P256, SIG_ED25519)
            or d["flags"] & ~(FLAG_REQUIRES_REPARTITION | FLAG_CLEARS_BUNDLES)
            or d["reserved14"] or any(d["reserved132"])):
        raise PkgError("E_PKG_FORMAT", "magic/version/length/alg/flags/reserved")


# ---- commands ----
def cmd_keygen(a):
    priv = (ed25519.Ed25519PrivateKey.generate() if a.alg == "ed25519"
            else ec.generate_private_key(ec.SECP256R1()))
    out = Path(a.out)
    if out.exists():
        sys.exit(f"{out} exists — refusing to overwrite a key")
    out.write_bytes(priv.private_bytes(serialization.Encoding.PEM,
                                       serialization.PrivateFormat.PKCS8,
                                       serialization.NoEncryption()))
    pub = priv.public_key()
    print(f"{out}: {a.alg}  key_id {key_id(pub).hex()}  public {pub_bytes(pub).hex()}")


def cmd_build(a):
    chip, fmt = CHIPS[a.chip]
    board = BOARDS[a.board]
    if board and board >> 8 != chip:
        sys.exit(f"board {a.board} (0x{board:04x}) is not a {a.chip} board")
    img = Path(a.image).read_bytes()
    try:
        print("# payload:", check_payload(fmt, img))
    except PkgError as e:
        sys.exit(f"refusing: {e}")
    priv = load_private(a.key)
    if key_id(priv.public_key()) in dev_key_ids() and not a.variant.startswith("dev"):
        sys.exit("refusing: DEV release key with a non-dev variant (spec §6)")
    flags = ((FLAG_REQUIRES_REPARTITION if a.requires_repartition else 0)
             | (FLAG_CLEARS_BUNDLES if a.clears_bundles else 0))
    hdr = build_header(chip, board, fmt, flags, parse_version(a.version),
                       parse_version(a.min_running), img, a.variant, a.fw_version,
                       a.build_time if a.build_time is not None else int(time.time()), priv)
    out = Path(a.out)
    out.write_bytes(hdr + img)
    d = parse_header(hdr)
    print(f"{out}: {len(hdr) + len(img)} B  {a.chip}/{a.board}  v{fmt_version(d['version'])}"
          f"  sha128 {d['image_sha256'][:16].hex()}  key_id {d['key_id'].hex()}")


def describe(d):
    alg = {SIG_P256: "ECDSA P-256", SIG_ED25519: "Ed25519"}.get(d["sig_alg"], "?")
    flags = [n for bit, n in ((FLAG_REQUIRES_REPARTITION, "REQUIRES_REPARTITION"),
                              (FLAG_CLEARS_BUNDLES, "CLEARS_BUNDLES")) if d["flags"] & bit]
    return "\n".join([
        f"format_version {d['format_version']}   sig_alg {d['sig_alg']} ({alg})   header_len {d['header_len']}",
        f"chip   0x{d['chip']:04x} {CHIP_NAME.get(d['chip'], '?')}   board 0x{d['board']:04x} "
        f"{BOARD_NAME.get(d['board'], '?')}   image_format {d['image_format']}",
        f"flags  0x{d['flags']:02x} {' '.join(flags) or '-'}",
        f"version {fmt_version(d['version'])}   min_running {fmt_version(d['min_running'])}",
        f"image  {d['image_len']} B   sha256 {d['image_sha256'].hex()}   sha128 {d['image_sha256'][:16].hex()}",
        f"built  {time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime(d['build_time']))}"
        f"   variant {d['variant']!r}   fw_version {d['fw_version']!r}",
        f"key_id {d['key_id'].hex()}",
    ])


def cmd_inspect(a):
    data = Path(a.package).read_bytes()
    d = parse_header(data)
    print(describe(d))
    try:
        check_structure(d)
        print("structure: ok (check 1)")
    except PkgError as e:
        print(f"structure: {e}")
        return 1
    return 0


def verify_package(data, pub):
    """Offline spec §4 checks 1-8 (targeting is the node's own, so check 4 is
    registry-consistency here). Raises PkgError with the spec code."""
    d = parse_header(data)
    check_structure(d)                                            # 1
    if d["key_id"] != key_id(pub):                                # 2
        raise PkgError("E_PKG_KEY", f"key_id {d['key_id'].hex()} is not this key")
    if key_alg(pub) != d["sig_alg"] or not verify_sig(pub, d["signature"], data[:SIGNED_LEN]):
        raise PkgError("E_PKG_SIG", "signature does not verify")  # 3
    chip = d["chip"]
    if chip not in CHIP_NAME or d["image_format"] != CHIPS[CHIP_NAME[chip]][1] or \
            (d["board"] and d["board"] >> 8 != chip):             # 4
        raise PkgError("E_PKG_TARGET", "chip/board/image_format inconsistent")
    if d["image_len"] != len(data) - HDR_LEN:                     # 6
        raise PkgError("E_PKG_SIZE", "image_len does not match the payload")
    img = data[HDR_LEN:]
    if hashlib.sha256(img).digest() != d["image_sha256"]:         # 7
        raise PkgError("E_PKG_SHA", "payload sha256 mismatch")
    return d, check_payload(d["image_format"], img)               # 8


def cmd_verify(a):
    data = Path(a.package).read_bytes()
    try:
        d, what = verify_package(data, load_public(a.pubkey))
    except PkgError as e:
        print(f"FAIL {e}")
        return 1
    print(f"OK  v{fmt_version(d['version'])}  {what}"
          + ("   (REQUIRES_REPARTITION: USB only)" if d["flags"] & FLAG_REQUIRES_REPARTITION else ""))
    return 0


def cmd_c_keys(a):
    """Emit the firmware's release-key store (spec §6): ≤ 2 tagged entries."""
    if not 1 <= len(a.keys) <= 2:
        sys.exit("the store holds 1 or 2 keys (current + next)")
    print("/* Generated by tools/mnpkg.py c-keys — OTA release-key store (OTA-PACKAGE §6). */")
    for i, p in enumerate(a.keys):
        pub = load_public(p)
        raw = pub_bytes(pub)
        body = ", ".join(f"0x{b:02x}" for b in raw)
        print(f"/* {Path(p).name}: {'Ed25519' if key_alg(pub) == SIG_ED25519 else 'P-256'} */")
        print(f"#define MN_PKG_KEY{i}_ALG   {key_alg(pub)}")
        print(f"#define MN_PKG_KEY{i}_ID    {{ {', '.join(f'0x{b:02x}' for b in key_id(pub))} }}")
        print(f"#define MN_PKG_KEY{i}_LEN   {len(raw)}")
        print(f"#define MN_PKG_KEY{i}_PUB   {{ {body} }}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen"); k.add_argument("--alg", choices=["ed25519", "p256"], default="ed25519")
    k.add_argument("-o", "--out", required=True)
    b = sub.add_parser("build")
    b.add_argument("image")
    b.add_argument("--chip", required=True, choices=sorted(CHIPS))
    b.add_argument("--board", required=True, choices=sorted(BOARDS, key=BOARDS.get))
    b.add_argument("--variant", required=True)
    b.add_argument("--version", required=True)
    b.add_argument("--min-running", default="0.0.0+0")
    b.add_argument("--fw-version", required=True)
    b.add_argument("--key", required=True)
    b.add_argument("--build-time", type=int, help="Unix seconds (default: now) — fix it for reproducible builds")
    b.add_argument("--requires-repartition", action="store_true")
    b.add_argument("--clears-bundles", action="store_true")
    b.add_argument("-o", "--out", required=True)
    i = sub.add_parser("inspect"); i.add_argument("package")
    v = sub.add_parser("verify"); v.add_argument("package"); v.add_argument("--pubkey", required=True)
    c = sub.add_parser("c-keys"); c.add_argument("keys", nargs="+")
    a = ap.parse_args()
    return {"keygen": cmd_keygen, "build": cmd_build, "inspect": cmd_inspect,
            "verify": cmd_verify, "c-keys": cmd_c_keys}[a.cmd](a) or 0


if __name__ == "__main__":
    sys.exit(main())
