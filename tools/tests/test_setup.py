"""Offline setup policy tests; never install packages or build the project.

Run with Python 3.11+: python -m unittest discover -s tools/tests -v
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

    def test_recommended_check_never_saves_or_installs(self):
        result = self.shell(
            'parse_options --recommended --check; '
            'python() { echo UNEXPECTED_WRITE; }; save_toolchain; '
            'as_root() { echo UNEXPECTED_INSTALL; }; install_packages missing'
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("UNEXPECTED_", result.stdout)

    def test_recommended_ubuntu22_python_does_not_add_repository(self):
        result = self.shell(
            'recommended=1; system=ubuntu; ubuntu_version=22.04; '
            'python3() { return 1; }; '
            'install_packages() { echo UNEXPECTED_INSTALL; }; prepare_python'
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Prepare it manually", result.stdout)
        self.assertNotIn("UNEXPECTED_INSTALL", result.stdout)

    def test_recommended_ubuntu24_uses_matching_python_development_files(self):
        result = self.shell(
            'recommended=1; system=ubuntu; ubuntu_version=24.04; '
            'python3() { return 1; }; python3.12() { return 0; }; '
            'install_packages() { printf "PACKAGES %s\\n" "$*"; }; prepare_python'
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("PACKAGES python3.12 python3.12-dev", result.stdout)

    def test_frontend_failure_does_not_publish_profile(self):
        result = self.shell(
            'detect_system() { system=linux; }; '
            'prepare_tools() { :; }; prepare_compilers() { :; }; '
            'prepare_python() { :; }; prepare_node() { :; }; prepare_submodules() { :; }; '
            'prepare_frontend() { return 37; }; save_toolchain() { echo UNEXPECTED_WRITE; }; main'
        )
        self.assertEqual(result.returncode, 37, result.stdout)
        self.assertNotIn("UNEXPECTED_WRITE", result.stdout)

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
            'system=ubuntu; ubuntu_version=24.04; as_root() { return 37; }; '
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

    def test_no_python_still_checks_build_interpreter(self):
        result = self.shell(
            'parse_options --check --no-ui --no-python; '
            'python3() { test "$2" = --interpreter-only; echo BUILD_PYTHON_CHECKED; }; '
            'node() { echo UNEXPECTED_NODE; return 39; }; '
            'prepare_python; prepare_node; prepare_frontend; '
            'test "$check_only:$with_ui:$with_python" = 1:0:0'
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("UNEXPECTED_", result.stdout)
        self.assertIn("BUILD_PYTHON_CHECKED", result.stdout)

    def test_unknown_linux_can_check_existing_tools(self):
        result = self.shell(
            'system=linux; check_only=1; '
            'tool_version_ok() { return 0; }; '
            'git() { echo git; }; cmake() { echo cmake; }; ninja() { echo ninja; }; '
            'install_packages() { echo UNEXPECTED_INSTALL; exit 39; }; prepare_tools'
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("UNEXPECTED_INSTALL", result.stdout)

    def test_unknown_linux_never_guesses_an_installer(self):
        result = self.shell(
            'system=linux; as_root() { echo UNEXPECTED_INSTALL; }; '
            'brew() { echo UNEXPECTED_INSTALL; }; install_packages missing'
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No automatic installation recipe", result.stdout)
        self.assertNotIn("UNEXPECTED_INSTALL", result.stdout)

    def test_old_ubuntu_has_no_implicit_install_recipe(self):
        result = self.shell('system=ubuntu; ubuntu_version=18.04; require_install_recipe')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("existing compatible tools can still build", result.stdout)

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

    def test_recommended_check_never_saves_or_installs(self):
        result = self.shell(
            "[void](Read-Options @('--recommended','--check')); "
            "function Invoke-Checked { throw 'UNEXPECTED_WRITE_OR_INSTALL' }; "
            "Save-Toolchain; Install-Package 'Missing.Dependency'"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("UNEXPECTED_WRITE_OR_INSTALL", result.stdout)

    def test_recommended_python_package_and_existing_interpreter(self):
        for existing in (True, False):
            with self.subTest(existing=existing):
                result = self.shell(
                    "$script:Recommended=$true; $env:VINC_SETUP_PYTHON=''; "
                    "$script:attempts=0; "
                    "function Select-Python { $script:attempts++; "
                    + ("return 'existing'" if existing else "if ($script:attempts -gt 1) { return 'installed' }")
                    + " }; function Install-Package { param($Id); Write-Host ('INSTALL '+$Id) }; Initialize-Python"
                )
                self.assertEqual(result.returncode, 0, result.stdout)
                if existing:
                    self.assertNotIn("INSTALL ", result.stdout)
                else:
                    self.assertIn("INSTALL Python.Python.3.14", result.stdout)

    def test_frontend_failure_does_not_publish_profile(self):
        result = self.shell(
            "function Initialize-Tools {}; function Import-DeveloperEnvironment {}; "
            "function Initialize-Python {}; function Initialize-Node {}; function Initialize-Submodules {}; "
            "function Initialize-Frontend { throw 'EXPECTED_FRONTEND_FAILURE' }; "
            "function Save-Toolchain { Write-Host UNEXPECTED_WRITE }; exit (Invoke-Setup @('--recommended'))"
        )
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotIn("UNEXPECTED_WRITE", result.stdout)

    def test_msvc_minimum_uses_actual_compiler(self):
        for version, accepted in ((1943, False), (1944, True), (1951, True)):
            with self.subTest(version=version):
                result = self.shell(
                    "$script:Temporary=Split-Path -Parent $PSCommandPath; "
                    "function cl.exe { $global:LASTEXITCODE=0; Write-Output '" + str(version) + "' }; "
                    "Test-MsvcMinimum"
                )
                self.assertEqual(result.returncode == 0, accepted, result.stdout)
                if not accepted:
                    self.assertIn("requires MSVC 19.44", result.stdout)

    def test_msvc_query_failure_is_not_a_successful_version(self):
        result = self.shell(
            "$script:Temporary=Split-Path -Parent $PSCommandPath; "
            "function cl.exe { $global:LASTEXITCODE=2; Write-Output '1944' }; "
            "Test-MsvcMinimum"
        )
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("Unable to query", result.stdout)

    def test_check_never_installs_missing_package(self):
        result = self.shell("$script:CheckOnly=$true; Install-Package 'Definitely.Missing'")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("rerun without --check", result.stdout)

    def test_native_failure_propagates(self):
        result = self.shell("Invoke-Checked 'cmd.exe' @('/d', '/c', 'exit 37'); Write-Host UNEXPECTED_CONTINUE")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Command failed (37)", result.stdout)
        self.assertNotIn("UNEXPECTED_CONTINUE", result.stdout)

    def test_no_python_still_checks_build_interpreter(self):
        result = self.shell(
            "[void](Read-Options @('--check','--no-ui','--no-python')); "
            "function Select-Python { Write-Host BUILD_PYTHON_CHECKED; return 'checked-python' }; "
            "function Test-Node { throw 'UNEXPECTED_NODE' }; "
            "Initialize-Python; Initialize-Node; Initialize-Frontend; "
            "if (-not $script:CheckOnly) { exit 31 }"
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("BUILD_PYTHON_CHECKED", result.stdout)

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
            "function Import-DeveloperEnvironment { $script:stages.Add('compiler'); "
            "Set-Content -LiteralPath (Join-Path $script:Temporary 'msvc-version.c') -Value '_MSC_VER' }; "
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
