"""SDK ownership, confirmation, rollback and package-registry contracts."""

import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import uuid


ROOT = Path(__file__).resolve().parents[2]
SDK_SPEC = importlib.util.spec_from_file_location("vinc_sdk_installer_tests", ROOT / "tools/sdk_install.py")
SDK = importlib.util.module_from_spec(SDK_SPEC)
SDK_SPEC.loader.exec_module(SDK)

SPEC = importlib.util.spec_from_file_location("vinc_build_sdk_tests", ROOT / "build.py")
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vinc-sdk-test-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name).resolve()
        self.root = self.work / "sdk with spaces"
        self.staged = self.work / "staged"

    def make_sdk(self, directory, version="old"):
        config = directory / "lib/cmake/vinc"
        config.mkdir(parents=True)
        (config / "profile.txt").write_text(version, encoding="utf-8")
        (config / "vincConfig.cmake").write_text("# fixture " + version, encoding="utf-8")
        return SDK.seal(directory)

    def test_modes_and_rejected_combinations(self):
        self.assertEqual(BUILD.arguments([]).config, "Debug")
        args = BUILD.arguments(["--install-vinc-sdk"])
        self.assertEqual(args.config, "Release")
        directory = BUILD.default_build_directory(args, {"CXX": {"id": "GNU"}})
        self.assertIn("-sdk-release", directory.name)
        for options in (["--sdk-root", str(self.root)], ["--no-register"],
                        ["--install-vinc-sdk", "--target", "one"],
                        ["--install-vinc-sdk", "-DVINC_BUILD_APPS=ON"],
                        ["--install-vinc-sdk", "-DCMAKE_INSTALL_PREFIX=somewhere"]):
            with self.subTest(options=options), mock.patch("sys.stderr", io.StringIO()):
                with self.assertRaises(SystemExit):
                    BUILD.arguments(options)

    def test_no_or_eof_has_no_build_or_filesystem_effects(self):
        self.make_sdk(self.root)
        before = SDK.inventory(self.work)
        for answer in ("n", "", "anything", EOFError()):
            args = BUILD.arguments(["--install-vinc-sdk", "--sdk-root", str(self.root)])
            kwargs = {"side_effect": answer} if isinstance(answer, Exception) else {"return_value": answer}
            with mock.patch("builtins.input", **kwargs), mock.patch.object(BUILD, "build") as build:
                self.assertEqual(SDK.install(args, BUILD), 0)
                build.assert_not_called()
            self.assertEqual(SDK.inventory(self.work), before)

    def test_cli_helper_does_not_import_foreign_tools_package(self):
        self.make_sdk(self.root)
        foreign = self.work / "python-path"
        (foreign / "tools").mkdir(parents=True)
        (foreign / "tools/__init__.py").write_text("raise RuntimeError('foreign tools imported')\n", encoding="utf-8")
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(foreign)
        result = subprocess.run([sys.executable, "-B", str(ROOT / "build.py"), "--install-vinc-sdk",
                                 "--sdk-root", str(self.root)], input="n\n", env=environment,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("no changes made", result.stdout)

    def test_unknown_directory_and_modified_files_rejected(self):
        self.root.mkdir()
        (self.root / "personal.txt").write_text("keep", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "not an installer-owned"):
            SDK.inspect_install(self.root)
        (self.root / "personal.txt").unlink()
        self.make_sdk(self.root)
        for path in (self.root / "extra.txt", self.root / "lib/cmake/vinc/profile.txt"):
            path.write_text("modified", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "added, missing or modified"):
                SDK.inspect_install(self.root)
            if path.name == "extra.txt":
                path.unlink()

    def test_extra_empty_directory_rejected(self):
        self.make_sdk(self.root)
        (self.root / "personal").mkdir()
        with self.assertRaisesRegex(RuntimeError, "added, missing or modified"):
            SDK.inspect_install(self.root)

    def test_unsafe_destinations_rejected(self):
        for path in (ROOT, ROOT / "build/sdk", ROOT.parent, Path.home()):
            with self.subTest(path=path), self.assertRaises(RuntimeError):
                SDK.checked_root(path, ROOT)
        self.assertEqual(SDK.checked_root(self.root, ROOT), self.root)

    def test_symlink_rejected_without_touching_target(self):
        target = self.work / "target"
        target.mkdir()
        try:
            self.root.symlink_to(target, target_is_directory=True)
        except OSError:
            self.skipTest("OS does not grant symbolic-link creation")
        with self.assertRaises(RuntimeError):
            SDK.checked_root(self.root, ROOT)
        self.assertTrue(target.is_dir())

    @unittest.skipUnless(os.name == "nt", "Windows junction")
    def test_windows_junction_rejected(self):
        target = self.work / "target"
        target.mkdir()
        result = subprocess.run(["cmd", "/d", "/c", "mklink", "/J", str(self.root), str(target)],
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        try:
            with self.assertRaisesRegex(RuntimeError, "reparse"):
                SDK.checked_root(self.root, ROOT)
            self.assertTrue(target.is_dir())
        finally:
            self.root.rmdir()

    def test_successful_update_removes_old_files(self):
        old = self.make_sdk(self.root)
        new = self.make_sdk(self.staged, "new")
        SDK.replace_install(self.root, self.staged, old, lambda: None)
        self.assertEqual(SDK.inspect_install(self.root), new)
        self.assertFalse(self.staged.exists())
        self.assertFalse(self.root.with_name("." + self.root.name + ".sdk-backup").exists())

    def test_verification_error_or_interrupt_restores_old_sdk(self):
        for failure in (RuntimeError("validation failure"), KeyboardInterrupt()):
            with self.subTest(failure=failure):
                old = self.make_sdk(self.root) if not self.root.exists() else SDK.inspect_install(self.root)
                if not self.staged.exists():
                    self.make_sdk(self.staged, "new")
                with self.assertRaises(type(failure)):
                    SDK.replace_install(self.root, self.staged, old, mock.Mock(side_effect=failure))
                self.assertEqual(SDK.inspect_install(self.root), old)
                self.assertEqual((self.staged / SDK.PROFILE).read_text(), "new")

    def test_failed_second_rename_restores_old_sdk(self):
        old = self.make_sdk(self.root)
        self.make_sdk(self.staged, "new")
        rename = Path.rename
        def fail_new(path, destination):
            if path == self.staged:
                raise PermissionError("locked")
            return rename(path, destination)
        with mock.patch.object(Path, "rename", fail_new), self.assertRaises(PermissionError):
            SDK.replace_install(self.root, self.staged, old, lambda: None)
        self.assertEqual(SDK.inspect_install(self.root), old)

    def test_interrupt_after_first_rename_restores_old_sdk(self):
        old = self.make_sdk(self.root)
        self.make_sdk(self.staged, "new")
        rename = Path.rename
        def interrupt_after_move(path, destination):
            result = rename(path, destination)
            if path == self.root:
                raise KeyboardInterrupt()
            return result
        with mock.patch.object(Path, "rename", interrupt_after_move), self.assertRaises(KeyboardInterrupt):
            SDK.replace_install(self.root, self.staged, old, lambda: None)
        self.assertEqual(SDK.inspect_install(self.root), old)

    def test_install_lock_rejects_second_installer(self):
        self.make_sdk(self.root)
        first = BUILD.arguments(["--install-vinc-sdk", "--sdk-root", str(self.root)])
        second = BUILD.arguments(["--install-vinc-sdk", "--sdk-root", str(self.root)])
        def run_second(args, after_build, progress):
            return SDK.install(second, BUILD)
        with mock.patch("builtins.input", return_value="y"), mock.patch.object(BUILD, "build", side_effect=run_second):
            with self.assertRaisesRegex(RuntimeError, "Another build"):
                SDK.install(first, BUILD)

    def test_interrupted_backup_and_changed_sdk_rejected(self):
        old = self.make_sdk(self.root)
        self.make_sdk(self.staged, "new")
        backup = self.root.with_name("." + self.root.name + ".sdk-backup")
        backup.mkdir()
        with self.assertRaisesRegex(RuntimeError, "needs recovery"):
            SDK.replace_install(self.root, self.staged, old, lambda: None)
        backup.rmdir()
        (self.root / "additional").write_text("keep", encoding="utf-8")
        with self.assertRaises(RuntimeError):
            SDK.replace_install(self.root, self.staged, old, lambda: None)

    def test_build_failure_preserves_existing_install(self):
        old = self.make_sdk(self.root)
        args = BUILD.arguments(["--install-vinc-sdk", "--sdk-root", str(self.root)])
        with mock.patch("builtins.input", return_value="y"), mock.patch.object(BUILD, "build", return_value=7):
            self.assertEqual(SDK.install(args, BUILD), 7)
        self.assertEqual(SDK.inspect_install(self.root), old)

    def test_install_failure_and_registration_failure_boundaries(self):
        old = self.make_sdk(self.root)
        for fail_install in (True, False):
            args = BUILD.arguments(["--install-vinc-sdk", "--sdk-root", str(self.root)])
            def build(args, after_build, progress):
                return after_build("cmake", "ninja", self.work / "build", {}, {}, io.StringIO())
            def install_command(command, *unused):
                staged = Path(command[command.index("--prefix") + 1])
                self.make_sdk(staged, "new")
                return 1 if fail_install else 0
            with mock.patch("builtins.input", return_value="y"), \
                    mock.patch.object(BUILD, "build", side_effect=build), \
                    mock.patch.object(BUILD, "run_step", side_effect=install_command), \
                    mock.patch.object(SDK, "verify"), \
                    mock.patch.object(SDK, "register", side_effect=PermissionError("denied")) as register:
                with self.assertRaisesRegex(RuntimeError, "installation failed" if fail_install else "registration failed"):
                    SDK.install(args, BUILD)
                if fail_install:
                    register.assert_not_called()
                    self.assertEqual(SDK.inspect_install(self.root), old)
                else:
                    self.assertEqual((self.root / SDK.PROFILE).read_text(), "new")

    def test_output_tracks_real_install_outcomes(self):
        for scenario in ("success", "no-register", "build", "stage", "verify", "register"):
            with self.subTest(scenario=scenario):
                destination = self.work / scenario
                old = self.make_sdk(destination)
                arguments = ["--install-vinc-sdk", "--sdk-root", str(destination)]
                if scenario == "no-register":
                    arguments.append("--no-register")
                args = BUILD.arguments(arguments)
                lines = []
                log_path = self.work / (scenario + ".log")

                def build(args, after_build, progress):
                    progress("configure", log_path)
                    progress("build", log_path)
                    if scenario == "build":
                        return 7
                    return after_build("cmake", "ninja", self.work / "build", {}, {}, io.StringIO())

                def install_command(command, *unused):
                    staged = Path(command[command.index("--prefix") + 1])
                    self.make_sdk(staged, "new")
                    return 1 if scenario == "stage" else 0

                def verify(root, *unused):
                    if scenario == "verify" and root == destination:
                        raise RuntimeError("final probe failed")

                with mock.patch("builtins.input", return_value="y"), \
                        mock.patch.object(BUILD, "display", side_effect=lines.append), \
                        mock.patch.object(BUILD, "build", side_effect=build), \
                        mock.patch.object(BUILD, "run_step", side_effect=install_command), \
                        mock.patch.object(SDK, "verify", side_effect=verify), \
                        mock.patch.object(SDK, "register") as register:
                    if scenario == "register":
                        register.side_effect = PermissionError("registration denied")
                    result = SDK.run_install(args, BUILD)

                text = "\n".join(lines)
                self.assertIn("Configuration: Release", text)
                self.assertIn("Log: " + str(log_path), text)
                if scenario in ("success", "no-register"):
                    self.assertEqual(result, 0)
                    self.assertEqual(text.count("SDK installation completed"), 1)
                    headings = ["[" + str(index) + "/6]" for index in range(1, 7)]
                    self.assertEqual(sorted(headings, key=text.index), headings)
                    self.assertIn("SDK status: New SDK installed and verified.", text)
                    if scenario == "no-register":
                        register.assert_not_called()
                        self.assertIn("Skipped (--no-register)", text)
                    else:
                        self.assertIn("Updated for this user", text)
                else:
                    self.assertEqual(result, 7 if scenario == "build" else 1)
                    self.assertIn("SDK installation failed", text)
                    self.assertNotIn("SDK installation completed", text)
                    if scenario == "verify":
                        self.assertIn("Stage: Replace and verify installation", text)
                        self.assertIn("Original destination restored", text)
                    elif scenario == "register":
                        self.assertIn("Stage: Register SDK with CMake", text)
                        self.assertIn("SDK status: New SDK installed and verified.", text)
                        self.assertIn("CMake registration: Failed", text)
                    else:
                        self.assertIn("Destination not replaced by this run", text)
                        self.assertNotIn("[5/6]", text)

                if scenario in ("build", "stage", "verify"):
                    self.assertEqual(SDK.inspect_install(destination), old)

    def test_cancel_and_early_failure_do_not_claim_a_new_log_or_completion(self):
        self.make_sdk(self.root)
        args = BUILD.arguments(["--install-vinc-sdk", "--sdk-root", str(self.root)])
        lines = []
        with mock.patch.object(BUILD, "display", side_effect=lines.append), \
                mock.patch("builtins.input", side_effect=KeyboardInterrupt()):
            result = SDK.run_install(args, BUILD)
        self.assertEqual(result, 130)
        self.assertIn("SDK installation cancelled", "\n".join(lines))
        self.assertIn("Log: not created", "\n".join(lines))
        self.assertNotIn("[2/6]", "\n".join(lines))

        lines.clear()
        (self.root / "unowned.txt").write_text("keep", encoding="utf-8")
        with mock.patch.object(BUILD, "display", side_effect=lines.append):
            result = SDK.run_install(args, BUILD)
        self.assertEqual(result, 1)
        self.assertIn("Stage: Check destination and build tools", "\n".join(lines))
        self.assertIn("Log: not created", "\n".join(lines))

    def test_cleanup_failure_keeps_verified_state_in_summary(self):
        old = self.make_sdk(self.root)
        self.make_sdk(self.staged, "new")
        args = BUILD.arguments(["--install-vinc-sdk", "--sdk-root", str(self.root)])
        lines = []
        output = SDK.InstallOutput(args, lines.append)
        output.destination = self.root
        with mock.patch.object(SDK.shutil, "rmtree", side_effect=PermissionError("locked")):
            with self.assertRaises(RuntimeError) as failure:
                SDK.replace_install(self.root, self.staged, old, lambda: None, output.replacement)
        output.summary("failed", failure.exception)
        self.assertIn("New SDK installed and verified", "\n".join(lines))
        self.assertIn("old SDK cleanup failed", "\n".join(lines))
        self.assertIn("Explicit selection:", "\n".join(lines))

    def test_replacement_failure_does_not_falsely_report_restored(self):
        for scenario in ("rename-denied", "rollback-blocked"):
            with self.subTest(scenario=scenario):
                root = self.work / scenario
                staged = self.work / (scenario + "-staged")
                old = self.make_sdk(root)
                self.make_sdk(staged, "new")
                states = []
                rename = Path.rename

                def fail_rename(path, destination):
                    if path == root and (scenario == "rename-denied" or destination == staged):
                        raise PermissionError("locked")
                    return rename(path, destination)

                with mock.patch.object(Path, "rename", fail_rename):
                    with self.assertRaises((PermissionError, RuntimeError)):
                        SDK.replace_install(root, staged, old,
                                            mock.Mock(side_effect=RuntimeError("final probe failed")), states.append)

                if scenario == "rename-denied":
                    self.assertEqual(states, ["started", "unchanged"])
                    self.assertEqual(SDK.inspect_install(root), old)
                else:
                    self.assertEqual(states, ["started"])
                    backup = root.with_name("." + root.name + ".sdk-backup")
                    self.assertEqual(SDK.inspect_install(backup), old)

    def test_actual_cmake_user_registry_lookup(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake not installed")
        package = "vinc_registry_probe_" + uuid.uuid4().hex
        config = self.root / "lib/cmake/vinc"
        config.mkdir(parents=True)
        (config / (package + "Config.cmake")).write_text(
            "set(REGISTRY_PROBE_FOUND TRUE)\n", encoding="utf-8")
        script = self.work / "lookup.cmake"
        script.write_text("find_package(" + package + " CONFIG REQUIRED)\n"
                          "if(NOT REGISTRY_PROBE_FOUND)\n"
                          "  message(FATAL_ERROR \"wrong package\")\nendif()\n", encoding="utf-8")
        environment = dict(os.environ)
        if os.name == "nt":
            import winreg
            key_path = "Software\\Kitware\\CMake\\Packages\\" + package
            try:
                with mock.patch.object(SDK, "REGISTRY_KEY", key_path):
                    SDK.register(self.root, lambda text: None)
                    with mock.patch.object(winreg, "SetValueEx", side_effect=PermissionError("denied")):
                        with self.assertRaises(PermissionError):
                            SDK.register(self.staged, lambda text: None)
                result = subprocess.run([cmake, "-P", str(script)], env=environment,
                                        capture_output=True, text=True, timeout=30)
            finally:
                # This unique test key is not the user's actual Vinc registration.
                winreg.DeleteKey(winreg.HKEY_CURRENT_USER, key_path)
        else:
            with mock.patch.object(Path, "home", return_value=self.work):
                SDK.register(self.root, lambda text: None)
            (self.work / ".cmake/packages/vinc").rename(self.work / ".cmake/packages" / package)
            environment["HOME"] = str(self.work)
            result = subprocess.run([cmake, "-P", str(script)], env=environment,
                                    capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipIf(os.name == "nt", "POSIX registry; Windows uses HKCU")
    def test_registry_update_preserves_other_entries_and_failed_write(self):
        with mock.patch.object(Path, "home", return_value=self.work):
            SDK.register(self.root, lambda text: None)
            registry = self.work / ".cmake/packages/vinc"
            entry = registry / SDK.REGISTRY_ENTRY
            before = entry.read_bytes()
            other = registry / "another-installer"
            other.write_text("keep", encoding="utf-8")
            with mock.patch("os.replace", side_effect=PermissionError("denied")):
                with self.assertRaises(PermissionError):
                    SDK.register(self.staged, lambda text: None)
            self.assertEqual(entry.read_bytes(), before)
            self.assertEqual(other.read_text(), "keep")
            self.assertEqual(len(list(registry.iterdir())), 2)


if __name__ == "__main__":
    unittest.main()
