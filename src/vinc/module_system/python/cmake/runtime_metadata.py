"""Record deployment floors from the selected Visual Studio's actual CRT files."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess


def toolchain(compiler):
    compiler = Path(compiler).resolve()
    toolset = compiler.parents[3]
    if compiler.name.lower() != "cl.exe" or toolset.parent.name != "MSVC":
        raise RuntimeError(f"Unsupported MSVC compiler layout: {compiler}")

    header = toolset / "include" / "crtversion.h"
    contents = header.read_text(encoding="utf-8")
    version = []
    for field in ("MAJOR", "MINOR", "BUILD", "RBUILD"):
        match = re.search(r"^#define\s+_VC_CRT_" + field + r"_VERSION\s+(\d+)\s*$",
                          contents, re.MULTILINE)
        if match is None:
            raise RuntimeError(f"Cannot determine MSVC CRT {field} from {header}")
        version.append(int(match.group(1)))

    return toolset, tuple(version)


def locate_redist(compiler):
    toolset, header_version = toolchain(compiler)
    root = toolset.parents[2] / "Redist/MSVC"
    candidates = []
    for directory in root.glob("*"):
        if not directory.is_dir() or not re.fullmatch(r"\d+\.\d+\.\d+", directory.name):
            continue

        version = tuple(int(part) for part in directory.name.split("."))
        if version[0] == header_version[0] and version[:2] >= header_version[:2]:
            candidates.append((version, directory))

    if not candidates:
        raise RuntimeError(
            f"No compatible Visual Studio CRT deployment files under {root}. "
            "Install the selected toolchain's C++ Redistributable/development components. "
            "System32 and crtversion.h are not DLL deployment baselines.")

    # Use the newest compatible deployment package within this VC installation.
    # Directory/header patch numbers are not DLL file versions. In particular,
    # serviced 14.44 headers and its Redist DLLs carry different patch numbers.
    return max(candidates)[1]


def pe_machine(path):
    with Path(path).open("rb") as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b"MZ":
            raise RuntimeError(f"Invalid PE image: {path}")

        stream.seek(struct.unpack_from("<I", header, 60)[0])
        signature = stream.read(6)
        if len(signature) != 6 or signature[:4] != b"PE\0\0":
            raise RuntimeError(f"Invalid PE signature: {path}")

        return struct.unpack_from("<H", signature, 4)[0]


def deployment_directory(root, architecture, debug):
    directory = root / "debug_nonredist" if debug else root
    suffix = "DebugCRT" if debug else "CRT"
    matches = list((directory / architecture).glob(f"Microsoft.VC*.{suffix}"))
    if len(matches) != 1 or not matches[0].is_dir():
        raise RuntimeError(f"Expected one {architecture} {suffix} directory under {root}")

    return matches[0]


def imported_runtimes(compiler, native, debug):
    result = subprocess.run([str(Path(compiler).with_name("dumpbin.exe")), "/DEPENDENTS", str(native)],
                            capture_output=True, check=True)
    # DLL names are ASCII even when dumpbin's surrounding messages are localized.
    imports = re.findall(rb"^\s+([A-Za-z0-9_.-]+\.dll)\s*$", result.stdout,
                         re.MULTILINE | re.IGNORECASE)
    names = sorted({name.decode("ascii").lower() for name in imports
                    if name.lower().startswith((b"msvcp140", b"vcruntime140", b"concrt140"))})
    expected = "msvcp140d.dll" if debug else "msvcp140.dll"
    if expected not in names:
        raise RuntimeError(f"Expected dynamic CRT dependency {expected}, found {names}")

    return names


def deployment_versions(directory, names, machine, family, read_version):
    versions = {}
    for name in names:
        path = directory / name
        if pe_machine(path) != machine:
            raise RuntimeError(f"CRT deployment architecture mismatch: {path}")

        version = read_version(path)
        if version[0] != family[0] or version[:2] < family:
            raise RuntimeError(f"CRT deployment file predates selected toolchain: {path}: {version}")

        versions[name] = list(version)

    return versions


def file_version_reader():
    # Reuse the loader's version-resource reader without importing vinc/_native
    # or loading any inspected CRTs. This path also exists in installed SDKs.
    source = Path(__file__).resolve().parents[1] / "vinc/_windows_runtime.py"
    spec = importlib.util.spec_from_file_location("vinc_runtime_metadata_reader", source)
    runtime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runtime)
    return runtime._Windows().file_version


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--locate", action="store_true")
    parser.add_argument("--redist-root", type=Path)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--config")
    args = parser.parse_args()

    root = locate_redist(args.compiler)
    if args.locate:
        print(root.as_posix())
        return

    if not args.native or not args.output or not args.config or not args.redist_root:
        parser.error("generation requires --native, --output, --config and --redist-root")
    if args.redist_root.resolve() != root.resolve():
        raise RuntimeError("Visual Studio Redist selection changed; reconfigure before building")

    toolset, header_version = toolchain(args.compiler)
    debug = args.config.lower() == "debug"
    machine = pe_machine(args.native)
    architectures = {0x8664: "x64", 0xAA64: "arm64", 0x014C: "x86"}
    if machine not in architectures:
        raise RuntimeError(f"Unsupported native PE machine: {machine:#x}")

    directory = deployment_directory(root, architectures[machine], debug)
    names = imported_runtimes(args.compiler, args.native, debug)
    versions = deployment_versions(directory, names, machine, header_version[:2], file_version_reader())
    metadata = {
        "format": 2,
        "compiler": toolset.name,
        "config": args.config,
        "header_version": header_version,
        "baseline": directory.relative_to(toolset.parents[2]).as_posix(),
        "required": versions,
        "debug": debug,
    }
    text = json.dumps(metadata, indent=2) + "\n"
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != text:
        args.output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
