#
# patch_openthread_datetime.py — PlatformIO pre-build fix.
#
# The ESP-IDF `openthread` component defines, in its CMakeLists.txt:
#
#     string(TIMESTAMP OT_BUILD_TIMESTAMP " %Y-%m-%d %H:%M:%S UTC" UTC)
#     ... OPENTHREAD_BUILD_DATETIME="${OT_BUILD_TIMESTAMP}" ...
#
# That value contains SPACES. PlatformIO re-emits component compile-definitions
# WITHOUT requoting, so the space-containing value is split at the first space:
#
#     -DOPENTHREAD_BUILD_DATETIME="\"      2026-06-15 01:09:31 UTC\""
#                                  ^kept   ^^^^ stray tokens on the command line
#
# which makes every openthread/*.cpp fail with
#     error: invalid digit "9" in octal constant
#
# Fix: rewrite the TIMESTAMP format to be space-free (ISO-ish). Idempotent —
# safe to run every build; skips if already patched. Touches only the pinned
# framework-espidf package this project builds against.
#
Import("env")  # noqa: F821

import os
import re

FRAMEWORK_DIR = env.PioPlatform().get_package_dir("framework-espidf")
OT_CMAKE = os.path.join(FRAMEWORK_DIR, "components", "openthread", "CMakeLists.txt")

SPACEY = r'string(TIMESTAMP OT_BUILD_TIMESTAMP " %Y-%m-%d %H:%M:%S UTC" UTC)'
SAFE   = r'string(TIMESTAMP OT_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)'

if not os.path.isfile(OT_CMAKE):
    print("[patch_openthread_datetime] openthread CMakeLists not found; skipping")
else:
    with open(OT_CMAKE, "r") as f:
        text = f.read()
    if SAFE in text:
        print("[patch_openthread_datetime] already patched (space-free timestamp)")
    elif SPACEY in text:
        text = text.replace(SPACEY, SAFE)
        with open(OT_CMAKE, "w") as f:
            f.write(text)
        print("[patch_openthread_datetime] patched OPENTHREAD_BUILD_DATETIME to be space-free")
    else:
        # Format string drifted across IDF versions — fall back to a regex that
        # strips spaces from whatever TIMESTAMP format is there.
        m = re.search(r'string\(TIMESTAMP OT_BUILD_TIMESTAMP "([^"]*)" UTC\)', text)
        if m and " " in m.group(1):
            fixed = m.group(1).replace(" ", "")
            text = text.replace(m.group(0),
                                 'string(TIMESTAMP OT_BUILD_TIMESTAMP "%s" UTC)' % fixed)
            with open(OT_CMAKE, "w") as f:
                f.write(text)
            print("[patch_openthread_datetime] patched (regex fallback): '%s' -> '%s'"
                  % (m.group(1), fixed))
        else:
            print("[patch_openthread_datetime] no space-containing timestamp found; "
                  "nothing to do")
