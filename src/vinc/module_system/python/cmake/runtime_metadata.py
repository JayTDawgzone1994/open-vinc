"""Record the selected MSVC headers and the linked extension's CRT imports."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--native", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--config", required=True)
    args = parser.parse_args()

    compiler = Path(args.compiler).resolve()
    # MSVC's bin/Host<arch>/<arch>/cl.exe and include belong to the same toolset.
    toolset = compiler.parents[3]
    header = toolset / "include" / "crtversion.h"
    contents = header.read_text(encoding="utf-8")
    version = []
    for field in ("MAJOR", "MINOR", "BUILD", "RBUILD"):
        match = re.search(r"^#define\s+_VC_CRT_" + field + r"_VERSION\s+(\d+)\s*$",
                          contents, re.MULTILINE)
        if match is None:
            raise RuntimeError(f"Cannot determine MSVC CRT {field} from {header}")
        version.append(int(match.group(1)))

    dumpbin = compiler.with_name("dumpbin.exe")
    result = subprocess.run([str(dumpbin), "/DEPENDENTS", args.native],
                            capture_output=True, check=True)
    # DLL names are ASCII even when dumpbin's surrounding messages are localized.
    imports = re.findall(rb"^\s+([A-Za-z0-9_.-]+\.dll)\s*$", result.stdout,
                         re.MULTILINE | re.IGNORECASE)
    names = sorted({name.decode("ascii").lower() for name in imports
                    if name.lower().startswith((b"msvcp140", b"vcruntime140", b"concrt140"))})
    debug = args.config.lower() == "debug"
    expected = "msvcp140d.dll" if debug else "msvcp140.dll"
    if expected not in names:
        raise RuntimeError(f"Expected dynamic CRT dependency {expected}, found {names}")

    metadata = {
        "format": 1,
        "compiler": toolset.name,
        "config": args.config,
        "minimum": version,
        "required": names,
        "debug": debug,
    }
    output = Path(args.output)
    text = json.dumps(metadata, indent=2) + "\n"
    if not output.exists() or output.read_text(encoding="utf-8") != text:
        output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
