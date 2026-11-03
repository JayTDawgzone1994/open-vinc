"""Native compiler discovery and non-destructive build-cache compatibility checks."""

import json
import ntpath
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TOOL_QUERY_TIMEOUT_SECONDS = 30
TOOLCHAIN_RECORD_FORMAT = 1


def fail(message):
    raise RuntimeError(message)


def executable(name, environment):
    path = shutil.which(str(name), path=environment.get("PATH"))
    if not path:
        setup = "tools\\setup.bat" if os.name == "nt" else "bash tools/setup.sh"
        fail("Missing tool: " + str(name) + ". Prepare dependencies with " + setup + " or adjust PATH.")

    # Compiler driver names (clang++, target-prefixed aliases) affect behavior.
    # Keep the invocation path; resolve links separately for identity comparison.
    return os.path.abspath(path)


def tool_output(command, environment, cwd, *, allowed_codes=(0,)):
    result = subprocess.run(command, cwd=cwd, env=environment, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            timeout=TOOL_QUERY_TIMEOUT_SECONDS)
    output = result.stdout.decode("utf-8", errors="replace").strip()
    if result.returncode not in allowed_codes:
        fail("Tool query failed (exit " + str(result.returncode) + "): "
             + str(command[0]) + "\n" + output)

    return output


def build_environment(profile=None):
    environment = dict(os.environ)
    if environment.get("CMAKE_GENERATOR") not in (None, "", "Ninja"):
        fail("CMAKE_GENERATOR conflicts with Ninja. Unset it or use CMake directly.")

    if any(environment.get(name) for name in ("CMAKE_TOOLCHAIN_FILE", "CMAKE_GENERATOR_PLATFORM",
                                             "CMAKE_GENERATOR_TOOLSET", "CMAKE_GENERATOR_INSTANCE")):
        fail("Toolchain files / generator platforms are outside this native wrapper; use CMake directly.")

    if os.name != "nt":
        return environment

    if environment.get("VSCMD_VER"):
        if environment.get("VSCMD_ARG_TGT_ARCH", "").lower() != "x64":
            fail("The current MSVC developer environment is not x64. Select an x64 environment first.")

        return environment

    if environment.get("VCINSTALLDIR") or environment.get("VCTOOLSINSTALLDIR"):
        fail("Partial/custom MSVC environment found. Open a complete x64 Developer terminal first.")

    selection = profile or {}
    if selection.get("vsdevcmd"):
        developer_script = Path(selection["vsdevcmd"])
    else:
        vswhere = Path(environment.get("PROGRAMFILES(X86)", "")) / "Microsoft Visual Studio/Installer/vswhere.exe"
        if not vswhere.is_file():
            fail("vswhere is missing. Run tools/setup.bat or use an x64 MSVC Developer terminal.")

        installation = tool_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                    "-property", "installationPath", "-utf8"], environment, ROOT)
        developer_script = Path(installation) / "Common7/Tools/VsDevCmd.bat"
    if not developer_script.is_file():
        fail("No MSVC C++ Build Tools installation was found. Run tools/setup.bat.")

    # Only cmd interprets this fixed command. The path comes from vswhere or the
    # local record; reject expansion characters instead of guessing escaping.
    if any(character in str(developer_script) for character in '%!"\r\n'):
        fail("Unsupported characters in VsDevCmd path; prepare the developer environment manually.")

    selectors = ""
    for key, option in (("toolset", "-vcvars_ver="), ("windows_sdk", "-winsdk=")):
        value = selection.get(key)
        if value:
            if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", value):
                fail("Invalid remembered native version selector: " + key)
            selectors += " " + option + value
    command = 'call "' + str(developer_script) + '" -no_logo -arch=x64 -host_arch=x64' + selectors + ' >nul && set'
    # cmd uses its own quoting rules, not the CRT rules of list2cmdline().
    shell_command = '"' + environment.get("COMSPEC", "cmd.exe") + '" /d /u /s /c "' + command + '"'
    result = subprocess.run(shell_command,
                            env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=TOOL_QUERY_TIMEOUT_SECONDS)
    if result.returncode:
        diagnostic = result.stderr.decode("utf-16-le", errors="replace").strip()
        fail("VsDevCmd failed; prepare the x64 Developer environment manually. " + diagnostic)

    for line in result.stdout.decode("utf-16-le", errors="strict").splitlines():
        name, separator, value = line.partition("=")
        if name and separator:
            # Windows environment names are case insensitive; avoid Path/PATH duplicates.
            environment[name.upper()] = value

    if environment.get("VSCMD_ARG_TGT_ARCH") != "x64":
        fail("VsDevCmd did not produce an x64 environment.")

    return environment


def selected_compilers(args, environment):
    compilers = {}
    for language, variable, default in (("C", "CC", "cc"), ("CXX", "CXX", "c++")):
        name = args.cache_values.get("CMAKE_" + language + "_COMPILER")
        name = name or environment.get(variable) or ("cl" if os.name == "nt" else default)
        path = executable(name, environment)
        if os.name == "nt" and Path(path).name.lower() != "cl.exe":
            fail("Windows wrapper builds require MSVC cl.exe. Use CMake directly for other toolchains.")

        compilers[language] = path

    return compilers


def normalized_architecture(value):
    aliases = {"amd64": "x86_64", "x64": "x86_64", "arm64": "aarch64"}
    return aliases.get(value.lower(), value.lower())


def compiler_banner(banner):
    """Extract stable fields, including from legacy localized/garbled records."""
    msvc = re.search(r"Microsoft[^\r\n]*?\b(\d+\.\d+\.\d+(?:\.\d+)?)\b[^\r\n]*?\b(x64|x86|ARM64|ARM)\b",
                     banner, re.IGNORECASE)
    if msvc:
        return {"id": "MSVC", "version": msvc[1], "target": normalized_architecture(msvc[2])}

    clang = re.search(r"(Apple )?clang version (\d+\.\d+(?:\.\d+)?)", banner)
    if clang:
        result = {"id": "AppleClang" if clang[1] else "Clang", "version": clang[2]}
        target = re.search(r"^Target:\s*(\S+)", banner, re.MULTILINE)
        if target:
            result["target"] = target[1]
        return result

    if "Free Software Foundation" in banner:
        first_line = banner.splitlines()[0]
        version = re.search(r"\b(\d+\.\d+(?:\.\d+)?)\s*$", first_line)
        if version:
            return {"id": "GNU", "version": version[1]}

    fail("Cannot identify a supported compiler/version. No toolchain record was accepted. "
         "Use native MSVC, GCC or Clang; compiler wrappers should use CMake's compiler-launcher option.")


def check_compiler_minimum(compiler, path=None):
    if compiler["id"] != "MSVC":
        return

    policy = ROOT / "src/vinc/common/cmake/msvc_minimum.txt"
    minimum = policy.read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+", minimum):
        fail("Invalid Open-VINC MSVC minimum version policy: " + str(policy))

    version = compiler["version"]
    if tuple(map(int, version.split(".")[:2])) < tuple(map(int, minimum.split("."))):
        fail("Open-VINC requires MSVC " + minimum + " or newer; selected compiler is "
             + version + ".\nPath: " + str(path or "not supplied")
             + "\nRun tools\\setup.bat --recommended, or select a supported toolset and a compatible build directory.")


def driver_role(path, compiler_id):
    if compiler_id == "MSVC":
        return "cl"

    name = Path(path).name
    if "++" in name:
        return "c++"
    if "clang-cl" in name:
        return "cl"
    return "c"


def toolchain_identity(compilers, environment):
    identity = {"platform": sys.platform, "machine": normalized_architecture(platform.machine())}
    query_environment = dict(environment)
    query_environment.update({"LC_ALL": "C", "LANG": "C", "VSLANG": "1033"})
    for language, path in compilers.items():
        version_args = [] if os.name == "nt" else ["--version"]
        # Some cl releases report missing input with exit 2. A recognizable MSVC
        # banner is still mandatory; other queries require successful exit.
        allowed_codes = (0, 2) if os.name == "nt" else (0,)
        output = tool_output([path] + version_args, query_environment, ROOT, allowed_codes=allowed_codes)
        compiler = compiler_banner(output)
        check_compiler_minimum(compiler, path)
        if os.name != "nt":
            compiler["target"] = tool_output([path, "-dumpmachine"], query_environment, ROOT)
            if not re.fullmatch(r"[A-Za-z0-9_.+-]+", compiler["target"]):
                fail("Invalid compiler target from " + path)

        compiler.update({"path": path, "resolved_path": str(Path(path).resolve()),
                         "driver": driver_role(path, compiler["id"])})
        identity[language] = compiler

    if os.name == "nt":
        identity["msvc_environment"] = {
            key: environment.get(key, "")
            for key in ("VCTOOLSVERSION", "WINDOWSSDKVERSION", "INCLUDE", "LIB")
        }

    return identity


def read_cache(directory):
    path = directory / "CMakeCache.txt"
    if not path.exists():
        return {}

    cache = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.match(r"([^/#][^:]*):[^=]*=(.*)$", line)
        if match:
            cache[match.group(1)] = match.group(2)

    return cache


def normalized_windows_environment(environment):
    result = {}
    for key in ("VCTOOLSVERSION", "WINDOWSSDKVERSION"):
        result[key] = environment.get(key, "").strip().rstrip("\\/")
    for key in ("INCLUDE", "LIB"):
        # Preserve order and empty entries; never change the actual child environment.
        result[key] = [ntpath.normcase(ntpath.normpath(path)) if path else ""
                       for path in environment.get(key, "").split(";")]
    return result


def comparable_identity(identity, *, legacy=False):
    if not isinstance(identity, dict):
        raise ValueError("toolchain must be an object")
    result = {"platform": identity["platform"], "machine": normalized_architecture(identity["machine"])}
    for language in ("C", "CXX"):
        original = identity[language]
        compiler = compiler_banner(original["version"]) if legacy else dict(original)
        if not legacy:
            for field in ("id", "version", "target", "driver", "resolved_path", "path"):
                if not isinstance(compiler[field], str) or not compiler[field]:
                    raise ValueError(language + "." + field + " must be a nonempty string")

        compiler["driver"] = driver_role(original["path"], compiler["id"])
        resolved = original.get("resolved_path") or str(Path(original["path"]).resolve())
        compiler["path"] = ntpath.normcase(ntpath.normpath(resolved)) if result["platform"] == "win32" else resolved
        compiler.pop("resolved_path", None)
        result[language] = compiler

    if result["platform"] == "win32":
        result["msvc_environment"] = normalized_windows_environment(identity["msvc_environment"])
    return result


def identity_differences(previous, current):
    differences = []
    for key, old in previous.items():
        new = current.get(key)
        if isinstance(old, dict) and isinstance(new, dict):
            for difference in identity_differences(old, new):
                differences.append((key + "." + difference[0], difference[1], difference[2]))
        elif old != new:
            differences.append((key, old, new))
    return differences


def reject_cache(directory, differences):
    lines = ["Cannot reuse this build directory: " + str(directory)]
    for name, previous, current in differences:
        lines.extend(["  Changed: " + name, "  Previous: " + str(previous), "  Current: " + str(current)])
    lines.append("Use the original settings/toolchain or select another --build-dir. No files were cleaned.")
    fail("\n".join(lines))


def validate_record(state, args, identity, report_legacy, reporter):
    try:
        record = json.loads(state.read_text(encoding="utf-8"))
        if not isinstance(record, dict):
            raise ValueError("record must be an object")
        legacy = "format" not in record
        if legacy:
            old_identity = record
        else:
            if record["format"] != TOOLCHAIN_RECORD_FORMAT or record["purpose"] not in ("build", "sdk-install"):
                raise ValueError("unsupported record format or purpose")
            old_identity = record["toolchain"]
        previous = comparable_identity(old_identity, legacy=legacy)
        current = comparable_identity(identity)
    except (ValueError, KeyError, TypeError, AttributeError, RuntimeError) as error:
        fail("Cannot validate toolchain record " + str(state) + ": " + str(error)
             + "\nRestore a known-good record or select another --build-dir. No cache was removed.")

    differences = identity_differences(previous, current)
    if not legacy:
        purpose = "sdk-install" if args.install_vinc_sdk else "build"
        if record["purpose"] != purpose:
            differences.append(("build purpose", record["purpose"], purpose))
    if differences:
        reject_cache(state.parent, differences)
    if legacy and report_legacy:
        reporter("Note: legacy toolchain record matched its available fields; prior target/purpose "
                "may not be recorded. CMake cache options are retained; record upgrades after configure.")


def save_toolchain_record(directory, args, identity):
    record = {"format": TOOLCHAIN_RECORD_FORMAT,
              "purpose": "sdk-install" if args.install_vinc_sdk else "build", "toolchain": identity}
    state = directory / ".vinc-build-toolchain.json"
    descriptor, name = tempfile.mkstemp(prefix=".vinc-toolchain-", suffix=".tmp", dir=directory)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(record, handle, indent=2)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, state)
    finally:
        temporary.unlink(missing_ok=True)


def validate_cache(directory, args, identity, *, report_unmanaged=True, reporter=print):
    cache = read_cache(directory)
    expected = {"CMAKE_HOME_DIRECTORY": str(args.source), "CMAKE_GENERATOR": "Ninja",
                "CMAKE_BUILD_TYPE": args.config}
    for name, value in expected.items():
        if name not in cache:
            continue

        matches = cache[name] == value
        if name == "CMAKE_HOME_DIRECTORY":
            matches = Path(cache[name]).resolve() == args.source

        if not matches:
            fail("Incompatible " + name + " in " + str(directory) + ": " + cache[name]
                 + ". Choose a different --build-dir; nothing was cleaned.")

    if cache.get("CMAKE_TOOLCHAIN_FILE") or cache.get("CMAKE_CONFIGURATION_TYPES"):
        fail("This cache uses a toolchain file or multiple configurations. Use CMake directly.")

    for language in ("C", "CXX"):
        cached = cache.get("CMAKE_" + language + "_COMPILER")
        if cached:
            selected = identity[language]
            same_file = Path(cached).resolve() == Path(selected["resolved_path"])
            same_driver = driver_role(cached, selected["id"]) == selected["driver"]
            if not same_file or not same_driver:
                reject_cache(directory, [(language + " compiler invocation", cached, selected["path"])])

    state = directory / ".vinc-build-toolchain.json"
    if state.exists():
        validate_record(state, args, identity, report_unmanaged, reporter)

    if cache and not state.exists() and report_unmanaged:
        reporter("Note: existing CMake cache has no wrapper toolchain record; "
                "paths/config are checked, but the previous compiler version cannot be verified.")
