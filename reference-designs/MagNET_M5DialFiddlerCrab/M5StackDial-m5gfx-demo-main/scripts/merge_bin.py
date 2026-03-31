"""PlatformIO post-build script to merge ESP32-S3 images into one binary.

Runs after `firmware.bin` is produced and generates `merged-firmware.bin`
in the same build directory using the bundled esptool.py.

The small compatibility layer below keeps static linters happy while still
working under PlatformIO's SCons runtime.
"""

from typing import Any
import os
import sys
import glob
import shlex

# Acquire the PlatformIO SCons `env`, but provide a dummy for linters/editors.
try:
    from SCons.Script import Import as SConsImport  # type: ignore
    SConsImport("env")  # type: ignore  # populates `env` in globals()
except Exception:  # pragma: no cover - editor/linters only
    env = None  # type: ignore

if 'env' not in globals() or env is None:  # type: ignore[name-defined]
    class _DummyPlatform:
        def get_package_dir(self, _name: str):
            return None

    class _DummyEnv(dict):
        def PioPlatform(self):
            return _DummyPlatform()

        def subst(self, s: str) -> str:
            return s

        def BoardConfig(self):
            return self

        def get(self, _k: str, default: Any = None) -> Any:  # type: ignore[override]
            return default

        def Execute(self, _cmd: Any) -> None:
            print("[merge_bin] (dry-run) would execute command")

        def AddPostAction(self, _target: str, _action: Any) -> None:
            pass

    env = _DummyEnv()  # type: ignore[assignment]


platform = env.PioPlatform()
esptool_dir = platform.get_package_dir("tool-esptoolpy")
esptool_py = os.path.join(esptool_dir, "esptool.py") if esptool_dir else None


def _merge_action(target: Any, source: Any, env: Any) -> None:  # SCons passes (target, source, env)
    del target, source  # unused; we work from the build dir
    build_dir = env.subst("$BUILD_DIR")

    def _pick(pattern):
        files = sorted(glob.glob(os.path.join(build_dir, pattern)))
        return files[0] if files else None

    bootloader = _pick("bootloader*.bin")
    partitions = _pick("partitions*.bin")
    firmware = _pick("firmware*.bin")
    merged = os.path.join(build_dir, "merged-firmware.bin")

    if not (bootloader and partitions and firmware):
        print(
            "[merge_bin] Skipped: missing inputs (bootloader=%s, partitions=%s, firmware=%s)"
            % (bootloader, partitions, firmware)
        )
        return
    if not esptool_py or not os.path.isfile(esptool_py):
        print("[merge_bin] Skipped: esptool.py not found in PlatformIO packages")
        return

    flash_size = env.BoardConfig().get("upload.flash_size", "8MB")
    python_exe = env.get("PYTHONEXE") or sys.executable or "python"
    cmd = [
        python_exe,
        esptool_py,
        "--chip",
        "esp32s3",
        "merge_bin",
        "-o",
        merged,
        "--flash_mode",
        "dio",
        "--flash_freq",
        "80m",
        "--flash_size",
        str(flash_size),
        "0x0",
        bootloader,
        "0x8000",
        partitions,
        "0x10000",
        firmware,
    ]
    print(
        f"[merge_bin] Creating: {merged}\n"
        f"           esptool={esptool_py}\n"
        f"           bootloader={bootloader}\n"
        f"           partitions={partitions}\n"
        f"           firmware={firmware}"
    )
    # SCons Execute works best with a single shell command string
    cmd_str = " ".join(shlex.quote(str(part)) for part in cmd)
    env.Execute(cmd_str)


# Run merge only after the final program binary has been built
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _merge_action)
