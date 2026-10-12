"""Private central-launched entry. Uses the installed interpreter, never embeds one."""
import os
import platform
import runpy
import struct
import sys
import sysconfig
import traceback
from pathlib import Path


def main():
    package = Path(__file__).resolve().parent
    build = runpy.run_path(str(package / "_build.py"))
    if (platform.python_implementation() != "CPython"
            or sys.version_info[:2] != build["VERSION"]
            or struct.calcsize("P") != build["POINTER_BYTES"]
            or sysconfig.get_platform() != build["PLATFORM"]
            or sysconfig.get_config_var("Py_GIL_DISABLED")):
        raise RuntimeError(
            f"VINC requires CPython {build['VERSION']} with "
            f"{build['POINTER_BYTES'] * 8}-bit ABI; selected {sys.executable}: {sys.version}"
        )
    # SDK import precedes user path admission; application imports happen only
    # inside native supervision, after the central connection/watchdog exist.
    sys.path.insert(0, str(package.parent))
    from vinc import _native
    return _native._run(sys.argv[1:])


if __name__ == "__main__":
    try:
        status = main()
    except BaseException:
        traceback.print_exc()
        status = 70
    # Native runtime has completed its supervised teardown. Do not enter an
    # unbounded interpreter shutdown waiting for user threads/atexit handlers.
    try:
        sys.stdout.flush()
        sys.stderr.flush()
    finally:
        os._exit(status)
