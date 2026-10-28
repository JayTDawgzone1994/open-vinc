"""Offline setup policy tests; never install packages or build the project.

Run with Python 3.9+: python -m unittest discover -s tools/tests -v
Bash cases run on Linux/macOS; PowerShell cases run on Windows.
"""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


TOOLS = pathlib.Path(__file__).resolve().parents[1]
WINDOWS = os.name == "nt"


def run(command, cwd=None):
    return subprocess.run(
        command, cwd=cwd, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=60,
    )


@unittest.skipIf(WINDOWS, "native Bash tests run on Linux/macOS")
class BashPolicyTests(unittest.TestCase):
    def shell(self, body):
        return run(["bash", "-c", 'source "$1"; ' + body, "test", str(TOOLS / "setup.sh")])

    def test_help_from_other_working_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            result = run(["bash", str(TOOLS / "setup.sh"), "--help"], cwd=directory)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(list(pathlib.Path(directory).iterdir()), [])

    def test_unknown_option_fails_before_install(self):
        result = run(["bash", str(TOOLS / "setup.sh"), "--build"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unknown option", result.stdout)

    def test_numeric_version_comparison(self):
        result = self.shell(
            'version_at_least 3.20.0 3.20.0; '
            'version_at_least 4.0.0 3.20.0; '
            'if version_at_least 3.9.9 3.20.0; then exit 21; fi; '
            'if version_at_least broken 3.20.0; then exit 22; fi'
        )
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_check_never_installs_missing_package(self):
        result = self.shell(
            'check_only=1; as_root() { echo UNEXPECTED_INSTALL; }; '
            'install_packages missing-package'
        )
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotIn("UNEXPECTED_INSTALL", result.stdout)

    def test_install_failure_stops_execution(self):
        result = self.shell(
            'system=ubuntu; as_root() { return 37; }; '
            'install_packages missing-package; echo UNEXPECTED_CONTINUE'
        )
        self.assertEqual(result.returncode, 37, result.stdout)
        self.assertNotIn("UNEXPECTED_CONTINUE", result.stdout)

    def test_npm_failure_is_not_success(self):
        result = self.shell(
            'npm() { return 38; }; prepare_frontend; echo UNEXPECTED_CONTINUE'
        )
        self.assertEqual(result.returncode, 38, result.stdout)
        self.assertNotIn("UNEXPECTED_CONTINUE", result.stdout)

    def test_invalid_explicit_compiler_is_not_replaced(self):
        result = self.shell(
            'temporary=$(mktemp -d); trap \'rm -rf -- "$temporary"\' EXIT; '
            'CXX=/definitely/missing/compiler; '
            'install_packages() { echo UNEXPECTED_INSTALL; }; prepare_compilers'
        )
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotIn("UNEXPECTED_INSTALL", result.stdout)
        self.assertIn("Selected CXX", result.stdout)

    def test_optional_dependencies_can_be_skipped(self):
        result = self.shell(
            'parse_options --check --no-ui --no-python; '
            'python3() { echo UNEXPECTED_PYTHON; return 39; }; '
            'node() { echo UNEXPECTED_NODE; return 39; }; '
            'prepare_python; prepare_node; prepare_frontend; '
            'test "$check_only:$with_ui:$with_python" = 1:0:0'
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("UNEXPECTED_", result.stdout)

    def test_invalid_explicit_python_is_not_replaced(self):
        result = self.shell(
            'VINC_SETUP_PYTHON=/definitely/missing/python; '
            'install_packages() { echo UNEXPECTED_INSTALL; }; prepare_python'
        )
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotIn("UNEXPECTED_INSTALL", result.stdout)
        self.assertIn("Explicit VINC_SETUP_PYTHON", result.stdout)


@unittest.skipUnless(WINDOWS, "native PowerShell tests run on Windows")
class PowerShellPolicyTests(unittest.TestCase):
    def shell(self, body):
        # A file avoids PowerShell 5.1's native -Command quote rewriting.
        with tempfile.TemporaryDirectory() as directory:
            script = pathlib.Path(directory) / "test.ps1"
            entry = str(TOOLS / "setup.ps1").replace("'", "''")
            script.write_text(". '{}'\n{}\n".format(entry, body), encoding="utf-8-sig")
            return run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)])

    def test_help_from_other_working_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            result = run(["cmd.exe", "/d", "/c", str(TOOLS / "setup.bat"), "--help"], cwd=directory)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertEqual(list(pathlib.Path(directory).iterdir()), [])

    def test_unknown_option(self):
        result = self.shell("exit (Invoke-Setup @('--build'))")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("Unknown option", result.stdout)

    def test_check_never_installs_missing_package(self):
        result = self.shell("$script:CheckOnly=$true; Install-Package 'Definitely.Missing'")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("rerun without --check", result.stdout)

    def test_native_failure_propagates(self):
        result = self.shell("Invoke-Checked 'cmd.exe' @('/d', '/c', 'exit 37'); Write-Host UNEXPECTED_CONTINUE")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Command failed (37)", result.stdout)
        self.assertNotIn("UNEXPECTED_CONTINUE", result.stdout)

    def test_optional_dependencies_can_be_skipped(self):
        result = self.shell(
            "[void](Read-Options @('--check','--no-ui','--no-python')); "
            "function Select-Python { throw 'UNEXPECTED_PYTHON' }; "
            "function Test-Node { throw 'UNEXPECTED_NODE' }; "
            "Initialize-Python; Initialize-Node; Initialize-Frontend; "
            "if (-not $script:CheckOnly) { exit 31 }"
        )
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_setup_failure_stops_later_stages_and_cleans_probe_directory(self):
        result = self.shell(
            "function Initialize-Tools { throw 'EXPECTED_FAILURE' }; "
            "function Import-DeveloperEnvironment { Write-Host UNEXPECTED_CONTINUE }; "
            "$status=Invoke-Setup @('--check'); "
            "if ($status -ne 1) { exit 31 }; "
            "if (Test-Path -LiteralPath $script:Temporary) { exit 32 }"
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("UNEXPECTED_CONTINUE", result.stdout)

    def test_setup_does_not_add_configuration_or_build_stage(self):
        result = self.shell(
            "$script:stages=[Collections.Generic.List[string]]::new(); "
            "function Initialize-Tools { $script:stages.Add('tools') }; "
            "function Import-DeveloperEnvironment { $script:stages.Add('compiler') }; "
            "function Initialize-Python { $script:stages.Add('python') }; "
            "function Initialize-Node { $script:stages.Add('node') }; "
            "function Initialize-Submodules { $script:stages.Add('git') }; "
            "function Initialize-Frontend { $script:stages.Add('npm') }; "
            "$status=Invoke-Setup @('--check'); "
            "if ($status -ne 0) { exit 31 }; "
            "if (($script:stages -join ',') -ne 'tools,compiler,python,node,git,npm') { exit 32 }"
        )
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_missing_explicit_python_cannot_inherit_previous_success(self):
        result = self.shell(
            "$env:VINC_SETUP_PYTHON='Z:\\definitely-missing-setup-test\\python.exe'; "
            "$global:LASTEXITCODE=0; $script:CheckOnly=$true; "
            "Initialize-Python; Write-Host UNEXPECTED_CONTINUE"
        )
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("Explicit VINC_SETUP_PYTHON", result.stdout)
        self.assertNotIn("UNEXPECTED_CONTINUE", result.stdout)


@unittest.skipUnless(shutil.which("git"), "requires Git")
class SubmoduleProtectionTests(unittest.TestCase):
    def git(self, directory, *arguments):
        result = run(["git", "-C", str(directory), *arguments])
        self.assertEqual(result.returncode, 0, result.stdout)
        return result

    def initialize(self, directory):
        directory.mkdir()
        self.git(directory, "init", "-q")
        self.git(directory, "config", "user.name", "Setup Test")
        self.git(directory, "config", "user.email", "setup-test@example.invalid")

    def check(self, repository):
        if WINDOWS:
            prefix = str(TOOLS / "setup.ps1").replace("'", "''")
            target = str(repository).replace("'", "''")
            with tempfile.TemporaryDirectory() as directory:
                script = pathlib.Path(directory) / "test.ps1"
                script.write_text(
                    ". '{}'; $script:Repository='{}'; $script:CheckOnly=$true; Initialize-Submodules\n".format(prefix, target),
                    encoding="utf-8-sig",
                )
                return run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)])
        return run(["bash", "-c", 'source "$1"; repository="$2"; check_only=1; prepare_submodules',
                    "test", str(TOOLS / "setup.sh"), str(repository)])

    def test_pins_and_tracked_untracked_edits(self):
        with tempfile.TemporaryDirectory(prefix="setup tests ") as directory:
            parent = pathlib.Path(directory)
            source = parent / "source"
            repository = parent / "consumer"
            self.initialize(source)
            self.initialize(repository)
            (source / "data.txt").write_text("original\n")
            self.git(source, "add", "data.txt")
            self.git(source, "commit", "-qm", "fixture")
            self.git(repository, "-c", "protocol.file.allow=always", "submodule", "add", "-q", str(source), "vendor")
            self.git(repository, "commit", "-qam", "pin")
            vendor = repository / "vendor"
            self.assertEqual(self.check(repository).returncode, 0)
            (vendor / "data.txt").write_text("user edit\n")
            self.assertNotEqual(self.check(repository).returncode, 0)
            self.assertEqual((vendor / "data.txt").read_text(), "user edit\n")
            self.git(vendor, "restore", "data.txt")
            (vendor / "private.txt").write_text("untracked\n")
            self.assertNotEqual(self.check(repository).returncode, 0)
            self.assertTrue((vendor / "private.txt").is_file())
            (vendor / "private.txt").unlink()
            self.git(vendor, "-c", "user.name=Setup Test", "-c", "user.email=setup-test@example.invalid", "commit", "--allow-empty", "-qm", "different revision")
            result = self.check(repository)
            self.assertNotEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
