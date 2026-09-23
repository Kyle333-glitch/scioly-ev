"""
Host-toolchain setup for the `native` PlatformIO environment.

PlatformIO's `native` platform needs a C/C++ compiler (gcc/g++) on PATH. That is
normal on Linux and macOS but not on a stock Windows development machine, where
students would otherwise have to install MinGW by hand.

If no system compiler is found, this script falls back to the PlatformIO-managed
MinGW toolchain declared via `platform_packages` in `[env:native]`. If a system
compiler is already present (MSYS2, WSL, a distro toolchain) it is left alone.

The script also links host test binaries statically: PlatformIO's test runner
executes the binary with a sanitized environment, so the MinGW runtime DLLs
would not be found on PATH at run time otherwise.
"""

import os
import shutil

from pathlib import Path

Import("env")  # noqa: F821 - provided by PlatformIO's SCons environment

MANAGED_TOOLCHAIN = "toolchain-gccmingw32"


def _system_compiler_present():
    return bool(shutil.which("gcc") and shutil.which("g++"))


def _use_managed_toolchain_if_needed():
    if _system_compiler_present():
        return
    managed_bin = (
        Path(env.subst("$PROJECT_PACKAGES_DIR")) / MANAGED_TOOLCHAIN / "bin"
    )
    if managed_bin.is_dir():
        os.environ["PATH"] = str(managed_bin) + os.pathsep + os.environ.get("PATH", "")
        env.PrependENVPath("PATH", str(managed_bin))
        print("native: using PlatformIO-managed MinGW toolchain from %s" % managed_bin)
    else:
        print(
            "native: no system gcc/g++ found and the PlatformIO-managed toolchain "
            "is missing. Install MinGW-w64, or run `pio pkg install -e native`."
        )


_use_managed_toolchain_if_needed()

# Keep the C++ runtime inside the test binary so it can be executed directly.
env.Append(LINKFLAGS=["-static", "-static-libgcc", "-static-libstdc++"])
