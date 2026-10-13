#!/usr/bin/env python3
"""mcuboot_sign.py — sign a MG24 application image OFFLINE (tools/keys/README.md).

A release build (MN_SIGN_OFFLINE=1, public MCUboot key) leaves the app
unsigned. This tool carries it to the machine that holds the private key and
back, with the exact imgtool parameters the build would have used:

  build machine:    mcuboot_sign.py job  <zephyr-build-dir> -o job/
  signing machine:  mcuboot_sign.py sign job/ --key mcuboot_release.pem -o signed/
                    (then mnpkg.py build signed/zephyr.signed.bin ... for OTA)
  back on the bench: mcuboot_sign.py install signed/ <zephyr-build-dir>
                    west flash -d <zephyr-build-dir>    (MCUboot + signed app)

`job` records the build's imgtool arguments (version, header size, slot size,
alignment) and the SHA-256 of what it copies; `sign` refuses a job whose
files changed, signs .bin and .hex, and verifies the result against the key.
The signing machine needs only this file and imgtool (`pip install imgtool`,
or MCUboot's scripts/imgtool.py).
"""
import argparse
import hashlib
import json
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

APP = "firmware-zephyr"      # the app image's sysbuild name


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def imgtool_cmd():
    """imgtool on PATH, else MCUboot's script next to a west workspace."""
    if shutil.which("imgtool"):
        return ["imgtool"]
    for base in (Path.home() / "zephyrproject", Path.cwd()):
        s = base / "bootloader/mcuboot/scripts/imgtool.py"
        if s.exists():
            return [sys.executable, str(s)]
    sys.exit("imgtool not found: pip install imgtool")


def cmd_job(a):
    img = Path(a.build) / APP
    ninja = (img / "build.ninja").read_text()
    m = re.search(r"imgtool\.py sign (.*?) \S+/zephyr\.bin \S+/zephyr\.signed\.bin", ninja)
    if not m:
        sys.exit("no imgtool sign step in the build — not an MCUboot build?")
    args = shlex.split(m.group(1))
    if "--key" in args or "-k" in args:
        sys.exit("this build signs the app itself — rebuild with -DMN_SIGN_OFFLINE=1 "
                 "and the PUBLIC key as SB_CONFIG_BOOT_SIGNATURE_KEY_FILE")
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=False)
    for f in ("zephyr.bin", "zephyr.hex"):
        shutil.copy2(img / "zephyr" / f, out / f)
    job = {"imgtool_args": args,
           "sha256": {f: sha(out / f) for f in ("zephyr.bin", "zephyr.hex")}}
    (out / "sign.json").write_text(json.dumps(job, indent=2) + "\n")
    print(f"{out}/: zephyr.bin zephyr.hex sign.json  ({' '.join(args)})")


def cmd_sign(a):
    job_dir, out = Path(a.job), Path(a.out)
    job = json.loads((job_dir / "sign.json").read_text())
    for f, h in job["sha256"].items():
        if sha(job_dir / f) != h:
            sys.exit(f"refusing: {f} changed since the job was made")
    out.mkdir(parents=True, exist_ok=True)
    tool = imgtool_cmd()
    for src, dst in (("zephyr.bin", "zephyr.signed.bin"), ("zephyr.hex", "zephyr.signed.hex")):
        subprocess.run(tool + ["sign"] + job["imgtool_args"] + ["--key", a.key,
                       str(job_dir / src), str(out / dst)], check=True)
    r = subprocess.run(tool + ["verify", "-k", a.key, str(out / "zephyr.signed.bin")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("verify FAILED:\n" + r.stdout + r.stderr)
    print(r.stdout.strip())
    print(f"{out}/zephyr.signed.bin  sha256 {sha(out / 'zephyr.signed.bin')}")


def cmd_install(a):
    dst = Path(a.build) / APP / "zephyr"
    for f in ("zephyr.signed.bin", "zephyr.signed.hex"):
        shutil.copy2(Path(a.signed) / f, dst / f)
    print(f"installed into {dst}/ — now: west flash -d {a.build}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    j = sub.add_parser("job"); j.add_argument("build"); j.add_argument("-o", "--out", required=True)
    s = sub.add_parser("sign"); s.add_argument("job"); s.add_argument("--key", required=True)
    s.add_argument("-o", "--out", required=True)
    i = sub.add_parser("install"); i.add_argument("signed"); i.add_argument("build")
    a = ap.parse_args()
    {"job": cmd_job, "sign": cmd_sign, "install": cmd_install}[a.cmd](a)


if __name__ == "__main__":
    main()
