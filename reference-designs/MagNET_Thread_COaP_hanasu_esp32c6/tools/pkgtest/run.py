#!/usr/bin/env python3
"""pkgtest — the firmware's package verifier (magnet_pkg.c) against packages
built by tools/mnpkg.py: every OTA-PACKAGE §4 code, from real firmware.

    ~/zephyrproject/.venv/bin/python tools/pkgtest/run.py [c6 firmware.bin] [mg24 zephyr.signed.bin]

Compiles magnet_pkg.c + TweetNaCl unchanged with an OpenSSL shim for the
platform crypto (host_crypto.c), then runs one case per row. Exit 1 on any
mismatch. Both tools agree = the C and the Python implement the same spec.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJ = HERE.parent.parent
sys.path.insert(0, str(HERE.parent))
import mnpkg  # noqa: E402

MAG = PROJ / "firmware-idf/components/magnet"
NACL = PROJ / "firmware-idf/components/magnet_crypto"
SSL = "/opt/homebrew/opt/openssl@3"

C6_IMG = Path(sys.argv[1]) if len(sys.argv) > 1 else PROJ / "firmware-idf/.pio/build/esp32c6/firmware.bin"
MG_IMG = Path(sys.argv[2]) if len(sys.argv) > 2 else \
    Path.home() / "zephyrproject/build/mnota4/firmware-zephyr/zephyr/zephyr.signed.bin"

ED = mnpkg.load_private(HERE.parent / "keys/dev_release_ed25519.pem")
P256 = mnpkg.load_private(HERE.parent / "keys/dev_release_p256.pem")
STRANGER = mnpkg.ed25519.Ed25519PrivateKey.generate()      # not in the store

C6 = dict(chip=0x0001, board=0x0101, fmt=1, slot=0x130000)          # NanoC6, 1216 KB
MG = dict(chip=0x0002, board=0x0201, fmt=2, slot=712 * 1024 - 16384)


def build(img, chip, board, fmt, key, flags=0, version="0.8.0+1", min_running="0.0.0+0"):
    hdr = mnpkg.build_header(chip, board, fmt, flags, mnpkg.parse_version(version),
                             mnpkg.parse_version(min_running), img, "dev-test", "0.8.0-t",
                             1790000000, key)
    return bytearray(hdr + img)


def resign(pkg, key):
    pkg[192:256] = mnpkg.sign(key, bytes(pkg[:192]))
    return pkg


def compile_driver(tmp):
    exe = tmp / "test_pkg"
    objs = []
    for src, inc in ((MAG / "magnet_pkg.c", [MAG / "include"]),
                     (HERE / "host_crypto.c", [MAG / "include", Path(SSL) / "include"]),
                     (HERE / "test_pkg.c", [MAG / "include"]),
                     (NACL / "magnet_crypto.c", [NACL / "include", NACL]),     # TweetNaCl's own header
                     (NACL / "tweetnacl.c", [NACL])):
        o = tmp / (src.stem + "_" + src.parent.name + ".o")
        subprocess.run(["cc", "-std=c11", "-O1", "-Wall", "-Wno-unused-function", "-Wno-sign-compare",
                        "-c", str(src), "-o", str(o)] + [f"-I{i}" for i in inc], check=True)
        objs.append(str(o))
    subprocess.run(["cc", "-o", str(exe)] + objs + [f"-L{SSL}/lib", "-lcrypto"], check=True)
    return exe


def main():
    c6img, mgimg = C6_IMG.read_bytes(), MG_IMG.read_bytes()
    c6 = lambda **k: build(c6img, C6["chip"], C6["board"], 1, **k)   # noqa: E731
    mg = lambda **k: build(mgimg, MG["chip"], MG["board"], 2, **k)   # noqa: E731

    cases = []   # (name, pkg bytes, target profile, running, expected, transfer override)
    add = lambda n, p, t, e, run="0.7.0+0", xfer=None: cases.append((n, p, t, run, e, xfer))  # noqa: E731

    add("C6 Ed25519, valid", c6(key=ED), C6, "OK")
    add("C6 P-256, valid", c6(key=P256), C6, "OK")
    add("MG24 P-256, valid", mg(key=P256), MG, "OK")
    add("MG24 Ed25519, valid", mg(key=ED), MG, "OK")
    add("board 0 (any) on NanoC6", build(c6img, 1, 0, 1, ED), C6, "OK")
    add("CLEARS_BUNDLES accepted", c6(key=ED, flags=2), C6, "OK")

    p = c6(key=ED); p[0] = ord("X"); add("bad magic", p, C6, "E_PKG_FORMAT")
    p = c6(key=ED); p[4] = 2; add("format_version 2", resign(p, ED), C6, "E_PKG_FORMAT")
    p = c6(key=ED); p[5] = 7; add("sig_alg 7", resign(p, ED), C6, "E_PKG_FORMAT")
    p = c6(key=ED); p[13] = 0x04; add("unknown flag bit 2", resign(p, ED), C6, "E_PKG_FORMAT")
    p = c6(key=ED); p[150] = 1; add("reserved byte set", resign(p, ED), C6, "E_PKG_FORMAT")
    add("stranger key", c6(key=STRANGER), C6, "E_PKG_KEY")
    p = c6(key=ED); p[200] ^= 1; add("signature byte flipped", p, C6, "E_PKG_SIG")
    p = c6(key=ED); p[19] ^= 1; add("version edited after signing", p, C6, "E_PKG_SIG")
    # key_id names the P-256 key, but sig_alg says Ed25519 (and an Ed25519
    # signature is attached): a key must never verify under the other alg
    p = c6(key=P256); p[5] = 2; add("P-256 key id, sig_alg Ed25519", resign(p, ED), C6, "E_PKG_SIG")
    add("C6 package to a MG24", c6(key=ED), MG, "E_PKG_TARGET")
    add("MG24 package to a C6", mg(key=P256), C6, "E_PKG_TARGET")
    add("XIAO-C6 package to a NanoC6", build(c6img, 1, 0x0102, 1, ED), C6, "E_PKG_TARGET")
    add("C6 chip, MCUboot image_format", build(c6img, 1, 0x0101, 2, ED), C6, "E_PKG_TARGET")
    add("REQUIRES_REPARTITION", c6(key=ED, flags=1), C6, "E_PKG_REPARTITION")
    p = c6(key=ED); add("truncated transfer", p[:-100], C6, "E_PKG_SIZE")
    add("image larger than slot", c6(key=ED), dict(C6, slot=len(c6img) - 1), "E_PKG_SIZE")
    p = c6(key=ED); p[256 + 5000] ^= 1; add("payload byte flipped", p, C6, "E_PKG_SHA")
    add("min_running not met", c6(key=ED, min_running="0.7.1+0"), C6, "E_PKG_MIN", run="0.7.0+9")
    add("min_running met exactly", c6(key=ED, min_running="0.7.0+9"), C6, "OK", run="0.7.0+9")

    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        exe = compile_driver(tmp)
        fails = 0
        print(f"{'case':38} {'expected':18} {'magnet_pkg.c':18} {'mnpkg.py':18}")
        for i, (name, pkg, tgt, run, want, xfer) in enumerate(cases):
            f = tmp / f"case{i}.mnpkg"
            f.write_bytes(bytes(pkg))
            args = [str(exe), str(f), hex(tgt["chip"]), hex(tgt["board"]), str(tgt["fmt"]),
                    str(tgt["slot"]), run] + ([str(xfer)] if xfer else [])
            got = subprocess.run(args, capture_output=True, text=True).stdout.strip()
            # the Python verifier sees the same bytes (its check 4 is registry
            # consistency, not a target, so target/min/size-vs-slot rows are C-only)
            try:
                # the key the package NAMES (key_id), as a node's store lookup would
                named = next((k.public_key() for k in (ED, P256)
                              if mnpkg.key_id(k.public_key()) == bytes(pkg[124:132])), ED.public_key())
                mnpkg.verify_package(bytes(pkg), named)
                py = "OK"
            except mnpkg.PkgError as e:
                py = e.code
            ok = got == want
            fails += not ok
            print(f"{name:38} {want:18} {got:18} {py:18} {'' if ok else '  <-- MISMATCH'}")
        print(f"\n{len(cases) - fails}/{len(cases)} cases match the spec")
        return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
