"""PlatformIO pre-script: inject the firmware version from git.

Sets -DCFG_FW_VERSION="<git describe>" so /api/status and /api/selftest
report a real, traceable version. Falls back to a static string when git
or the .git dir is unavailable (e.g. a source tarball).
"""

import subprocess

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

FALLBACK = "0.1.0-nogit"


def git_version():
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            stderr=subprocess.DEVNULL,
        )
        return out.decode().strip() or FALLBACK
    except Exception:
        return FALLBACK


version = git_version()
print(f"firmware version: {version}")
env.Append(CPPDEFINES=[("CFG_FW_VERSION", env.StringifyMacro(version))])  # noqa: F821
