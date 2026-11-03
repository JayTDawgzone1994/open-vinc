"""Explicit SDK installation; CMake still owns package contents and ABI checks."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile


MANIFEST = ".vinc-sdk-install.json"
MANIFEST_FORMAT = 1
REGISTRY_ENTRY = "open-vinc-build-installer"
REGISTRY_KEY = r"Software\Kitware\CMake\Packages\vinc"
PROFILE = Path("lib/cmake/vinc/profile.txt")
SDK_DEFINITIONS = {
    "VINC_INSTALL_SDK": "ON",
    "VINC_BUILD_APPS": "OFF",
    "BUILD_TESTING": "OFF",
}
PROBE_TIMEOUT_SECONDS = 30
INSTALL_PHASES = {
    "check": "Check destination and build tools",
    "configure": "Configure SDK",
    "build": "Build SDK",
    "stage": "Stage and verify SDK",
    "replace": "Replace and verify installation",
    "register": "Register SDK with CMake",
}


class InstallOutput:
    """Presentation-only state; never drives installation or recovery decisions."""

    def __init__(self, args, display):
        self.display = display
        self.config = args.config
        self.destination = args.sdk_root or "(default location)"
        self.current_phase = "check"
        self.log_path = None
        self.declined = False
        self.verified = False
        self.installation = "Destination not replaced by this run."
        self.registration = "Not attempted."

    def phase(self, name, log_path=None):
        self.current_phase = name
        if log_path is not None:
            self.log_path = log_path

        number = list(INSTALL_PHASES).index(name) + 1
        self.display("\n[" + str(number) + "/" + str(len(INSTALL_PHASES)) + "] " + INSTALL_PHASES[name])

    def replacement(self, state):
        messages = {
            "unchanged": "Destination not replaced by this run.",
            "started": "Replacement started; if recovery fails, inspect destination and backup.",
            "restored": "Original destination restored; new SDK not installed.",
            "verified": "New SDK installed and verified.",
        }
        self.installation = messages[state]
        self.verified = state == "verified"

    def summary(self, outcome, reason=None):
        self.display("\nSDK installation " + outcome)
        if reason is not None:
            self.display("Stage: " + INSTALL_PHASES[self.current_phase])
            self.display("Reason: " + str(reason))

        self.display("Location: " + str(self.destination))
        self.display("Configuration: " + self.config)
        self.display("SDK status: " + self.installation)
        self.display("CMake registration: " + self.registration)
        if self.log_path is None:
            self.display("Log: not created for this run (build did not start).")
        else:
            self.display("Log: " + str(self.log_path) + " (bounded command output)")

        if self.verified:
            self.display("Explicit selection: -Dvinc_DIR=" + str(Path(self.destination) / "lib/cmake/vinc"))


def run_install(args, api):
    """Report SDK outcomes once while retaining child exit codes and Ctrl+C=130."""
    output = InstallOutput(args, api.display)
    api.display("Open-VINC SDK installation")
    try:
        code = install(args, api, output)
    except KeyboardInterrupt:
        output.summary("cancelled", "Interrupted by user.")
        return 130
    except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        output.summary("failed", error)
        return 1

    if code:
        output.summary("failed", "Command exited with code " + str(code) + "; see command output.")
    elif not output.declined:
        output.summary("completed")

    return code


def default_root():
    if sys.platform == "win32":
        local = os.environ.get("LOCALAPPDATA")
        if not local:
            raise RuntimeError("LOCALAPPDATA is missing; provide --sdk-root")

        return Path(local) / "OpenVINC" / "sdk"

    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "OpenVINC" / "sdk"

    data = os.environ.get("XDG_DATA_HOME")
    base = Path(data) if data else Path.home() / ".local" / "share"
    if not base.is_absolute():
        raise RuntimeError("XDG_DATA_HOME must be absolute; provide --sdk-root")

    return base / "open-vinc" / "sdk"


def reject_link(path):
    info = path.lstat()
    # Windows junctions are reparse points, even when is_symlink() is false.
    if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
        raise RuntimeError("SDK paths must not contain links/reparse points: " + str(path))


def checked_root(path, repository, build_directory=None):
    path = Path(os.path.abspath(path.expanduser()))
    for ancestor in [path, *path.parents]:
        if ancestor.exists() or ancestor.is_symlink():
            reject_link(ancestor)

    path = path.resolve()
    if path == Path(path.anchor) or path == Path.home().resolve():
        raise RuntimeError("Use a dedicated SDK directory, not a filesystem/user root")

    protected = [repository.resolve()]
    if build_directory is not None:
        protected.append(build_directory.resolve())

    for directory in protected:
        if path == directory or path in directory.parents or directory in path.parents:
            raise RuntimeError("SDK installation must be outside source/build trees: " + str(path))

    return path


def inventory(root):
    """No traversal through links; the manifest covers empty directories as well."""
    reject_link(root)
    files = {}
    directories = []
    for directory, names, filenames in os.walk(root, followlinks=False):
        current = Path(directory)
        for name in names:
            child = current / name
            reject_link(child)
            directories.append(child.relative_to(root).as_posix())

        for name in filenames:
            child = current / name
            reject_link(child)
            if not stat.S_ISREG(child.stat().st_mode):
                raise RuntimeError("Non-regular file in SDK: " + str(child))

            relative = child.relative_to(root).as_posix()
            if relative == MANIFEST:
                continue

            digest = hashlib.sha256()
            with child.open("rb") as handle:
                for block in iter(lambda: handle.read(1024 * 1024), b""):
                    digest.update(block)

            files[relative] = digest.hexdigest()

    return {"files": files, "directories": sorted(directories)}


def inspect_install(root):
    if not root.exists():
        return None

    reject_link(root)
    if not root.is_dir():
        raise RuntimeError("SDK destination is not a directory: " + str(root))

    if not any(root.iterdir()):
        return None

    marker = root / MANIFEST
    if not marker.is_file():
        raise RuntimeError("Directory is not an installer-owned SDK; choose an empty dedicated directory: " + str(root))

    reject_link(marker)
    record = json.loads(marker.read_text(encoding="utf-8"))
    if not isinstance(record, dict) or record.get("format") != MANIFEST_FORMAT or record.get("owner") != REGISTRY_ENTRY:
        raise RuntimeError("Unrecognized SDK ownership manifest: " + str(marker))

    if record.get("inventory") != inventory(root):
        raise RuntimeError("SDK has added, missing or modified files; preserve your changes before updating: " + str(root))

    if not (root / PROFILE).is_file() or not (root / "lib/cmake/vinc/vincConfig.cmake").is_file():
        raise RuntimeError("SDK package metadata is missing: " + str(root))

    if record.get("profile") != (root / PROFILE).read_text(encoding="utf-8"):
        raise RuntimeError("SDK profile and ownership manifest disagree: " + str(root))

    return record


def seal(root):
    record = {"format": MANIFEST_FORMAT, "owner": REGISTRY_ENTRY,
              "profile": (root / PROFILE).read_text(encoding="utf-8"), "inventory": inventory(root)}
    (root / MANIFEST).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return record


def register(root, display):
    """Update our one entry only. CMake does not order competing registry entries."""
    location = str(root / "lib" / "cmake" / "vinc")
    if os.name == "nt":
        import winreg
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, REGISTRY_KEY, 0,
                               winreg.KEY_READ | winreg.KEY_WRITE) as key:
            count = winreg.QueryInfoKey(key)[1]
            others = [winreg.EnumValue(key, index)[0] for index in range(count)]
            if any(name != REGISTRY_ENTRY for name in others):
                display("Warning: other Vinc registry entries exist; use explicit vinc_DIR to select this SDK.")

            winreg.SetValueEx(key, REGISTRY_ENTRY, 0, winreg.REG_SZ, location)
        return

    directory = Path.home() / ".cmake" / "packages" / "vinc"
    directory.mkdir(parents=True, exist_ok=True)
    if any(entry.name != REGISTRY_ENTRY for entry in directory.iterdir()):
        display("Warning: other Vinc registry entries exist; use explicit vinc_DIR to select this SDK.")

    descriptor, name = tempfile.mkstemp(prefix=".vinc-register-", dir=directory)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            handle.write(location + "\n")
            handle.flush()
            os.fsync(handle.fileno())

        os.replace(temporary, directory / REGISTRY_ENTRY)
    finally:
        temporary.unlink(missing_ok=True)


def verify(root, args, api, cmake, ninja, identity, environment, log):
    # The probe is outside the SDK so validation cannot pollute its ownership list.
    with tempfile.TemporaryDirectory(prefix=".vinc-sdk-probe-", dir=root.parent) as name:
        source = Path(name)
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(vinc_sdk_probe LANGUAGES CXX)\n"
            "set(CMAKE_CXX_STANDARD 23)\n"
            "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
            "set(CMAKE_CXX_EXTENSIONS OFF)\n"
            "find_package(vinc CONFIG REQUIRED)\n"
            "add_executable(probe main.cpp)\n"
            "target_link_libraries(probe PRIVATE vinc::sdk)\n", encoding="utf-8")
        (source / "main.cpp").write_text(
            "#include <vinc/error.hpp>\n"
            "int main() {\n"
            "    const auto text = vinc::error_to_string(vinc::Error{});\n"
            "    return text.empty() ? 1 : 0;\n"
            "}\n", encoding="utf-8")
        binary = source / "build"
        command = [cmake, "-S", str(source), "-B", str(binary), "-G", "Ninja",
                   "-DCMAKE_MAKE_PROGRAM=" + ninja, "-DCMAKE_BUILD_TYPE=" + args.config,
                   "-DCMAKE_CXX_COMPILER=" + identity["CXX"]["path"],
                   "-Dvinc_DIR=" + str(root / "lib/cmake/vinc")]
        for command in (command, [cmake, "--build", str(binary)]):
            if api.run_step(command, source, environment, log):
                raise RuntimeError("Installed SDK validation failed")

        executable = binary / ("probe.exe" if os.name == "nt" else "probe")
        result = subprocess.run([str(executable)], cwd=source, env=environment,
                                timeout=PROBE_TIMEOUT_SECONDS, capture_output=True)
        if result.returncode:
            log.write(result.stdout.decode("utf-8", errors="replace"))
            log.write(result.stderr.decode("utf-8", errors="replace"))
            raise RuntimeError("Installed SDK probe failed: " + str(result.returncode))


def replace_install(root, staged, expected, verify_final, report=None):
    """Rollback ordinary failures; a hard kill leaves a visible recovery directory.

    Both renames stay on the same filesystem. No directory-tree replacement is
    atomic across both renames; concurrent consumers must not build during update.
    """
    backup = root.with_name("." + root.name + ".sdk-backup")
    if backup.exists() or backup.is_symlink():
        raise RuntimeError("Previous SDK update needs recovery; preserve/inspect " + str(backup))

    if inspect_install(root) != expected:
        raise RuntimeError("SDK changed after confirmation; refusing replacement")

    had_directory = root.exists()
    if report is not None:
        report("started")

    try:
        if had_directory:
            root.rename(backup)

        staged.rename(root)
        verify_final()
    except BaseException:
        # Never delete the old installation during rollback. Keep both copies if
        # the OS prevents renaming (e.g. another process opened a library on Windows).
        try:
            restored = False
            # Inspect filesystem state, not booleans assigned after rename: Ctrl+C
            # can arrive between the successful syscall and the next assignment.
            if root.exists() and not staged.exists():
                root.rename(staged)
                restored = True
            if backup.exists():
                backup.rename(root)
                restored = True
        except OSError as recovery_error:
            raise RuntimeError("SDK rollback blocked; old SDK: " + str(backup)
                               + "; new SDK: " + str(root) + "; " + str(recovery_error)) from recovery_error

        if report is not None:
            report("restored" if restored else "unchanged")
        raise

    if report is not None:
        report("verified")

    if had_directory:
        # Revalidate ownership immediately before deleting the narrowly scoped backup.
        if inspect_install(backup) != expected:
            raise RuntimeError("New SDK installed, but backup changed; retained at " + str(backup))

        try:
            shutil.rmtree(backup)
        except OSError as error:
            raise RuntimeError("New SDK installed, but old SDK cleanup failed; inspect "
                               + str(backup) + ": " + str(error)) from error


def install(args, api, output=None):
    output = output or InstallOutput(args, api.display)
    output.phase("check")
    root = checked_root(args.sdk_root or default_root(), api.ROOT, args.build_dir)
    output.destination = root
    api.display("Location: " + str(root) + "\nConfiguration: " + args.config)
    expected = inspect_install(root)
    backup = root.with_name("." + root.name + ".sdk-backup")
    if backup.exists() or backup.is_symlink():
        raise RuntimeError("Interrupted SDK update requires recovery first: " + str(backup))

    if expected is not None:
        api.display("Existing SDK: " + str(root) + "\nProfile: " + expected["profile"])
        api.display("Update from checkout: " + str(api.ROOT) + " (" + args.config + ")")
        try:
            answer = input("Replace this SDK after building and validating the new SDK? [y/N] ")
        except EOFError:
            answer = ""

        if answer.strip().lower() not in ("y", "yes"):
            output.declined = True
            api.display("SDK installation cancelled; no changes made.")
            return 0

    # Refusal above creates neither build files nor installation/registry state.
    root.parent.mkdir(parents=True, exist_ok=True)
    lock_directory = root.with_name("." + root.name + ".install-lock")
    lock_directory.mkdir(exist_ok=True)
    reject_link(lock_directory)
    with api.build_lock(lock_directory):
        if inspect_install(root) != expected:
            raise RuntimeError("SDK changed while waiting for installation; run again")

        for key, value in SDK_DEFINITIONS.items():
            args.definitions.append(key + "=" + value)
            args.cache_values[key] = value

        def finish(cmake, ninja, directory, identity, environment, log):
            output.phase("stage")
            checked_root(root, api.ROOT, directory)
            # Avoid sudo/DESTDIR unexpectedly redirecting package output.
            environment = dict(environment)
            environment.pop("DESTDIR", None)
            with tempfile.TemporaryDirectory(prefix="." + root.name + ".stage-", dir=root.parent) as name:
                staged = Path(name) / "sdk"
                command = [cmake, "--install", str(directory), "--component", "VincSDK",
                           "--config", args.config, "--prefix", str(staged)]
                if api.run_step(command, args.source, environment, log):
                    raise RuntimeError("SDK installation failed; previous SDK was not replaced")

                verify(staged, args, api, cmake, ninja, identity, environment, log)
                seal(staged)
                output.phase("replace")
                replace_install(root, staged, expected,
                                lambda: verify(root, args, api, cmake, ninja, identity, environment, log),
                                report=output.replacement)

            output.phase("register")
            if not args.no_register:
                output.registration = "Update attempted; completion not confirmed."
                try:
                    register(root, api.display)
                except OSError as error:
                    output.registration = "Failed; use explicit vinc_DIR."
                    raise RuntimeError("SDK installed, but CMake registration failed: " + str(error)) from error

                output.registration = "Updated for this user; existing project caches unchanged."
            else:
                output.registration = "Skipped (--no-register); existing registration unchanged."
                api.display(output.registration)

            return 0

        return api.build(args, after_build=finish, progress=output.phase)
