"""Local profile persistence and real pre-Python launcher behavior."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("profile_under_test", ROOT / "tools/toolchain_profile.py")
PROFILE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROFILE)


def values():
    return {key: sys.executable for key in
            ("build_python", "config_python", "module_python", "cc", "cxx", "cmake", "ninja")}


def arguments(definitions=None):
    return SimpleNamespace(cache_values=dict(definitions or {}), definitions=[])


class ProfileTests(unittest.TestCase):
    def test_round_trip_and_patch_metadata_is_not_an_exact_requirement(self):
        with tempfile.TemporaryDirectory() as directory:
            data = dict(values(), python_version="3.11.0", cmake_version="4.4.0")
            path = PROFILE.save(data, directory)
            self.assertEqual(PROFILE.load(directory)["cmake"], sys.executable)
            self.assertEqual(path.parent, Path(directory) / ".vinc/toolchains")
            self.assertNotIn("PATH=", path.read_text())

    def test_failed_validation_preserves_previous_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            path = PROFILE.save(values(), directory)
            before = path.read_bytes()
            with self.assertRaises(ValueError):
                PROFILE.save(dict(values(), unknown="bad"), directory)
            self.assertEqual(path.read_bytes(), before)

    def test_failed_atomic_replace_preserves_previous_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            path = PROFILE.save(values(), directory)
            before = path.read_bytes()
            with mock.patch.object(PROFILE.os, "replace", side_effect=OSError("denied")):
                with self.assertRaises(OSError):
                    PROFILE.save(values(), directory)
            self.assertEqual(path.read_bytes(), before)
            self.assertEqual(list(path.parent.iterdir()), [path])

    def test_reject_malformed_data_without_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            path = PROFILE.save(values(), directory)
            original = path.read_text()
            for text in (original + "format=1\n", "not a record", original.replace("format=1", "format=2"),
                         original + "shell=$(touch unexpected)\n", "x" * (PROFILE.MAX_PROFILE_BYTES + 1)):
                with self.subTest(text=text[:35]):
                    path.write_text(text)
                    with self.assertRaises(RuntimeError):
                        PROFILE.load(directory)

    def test_paths_are_literal_and_allow_spaces_and_equals(self):
        with tempfile.TemporaryDirectory() as directory:
            data = dict(values(), node=str(Path(directory) / "a b=literal/node"))
            PROFILE.save(data, directory)
            self.assertEqual(PROFILE.load(directory)["node"], data["node"])

    def test_platform_and_wsl_distribution_are_separate(self):
        with mock.patch.object(PROFILE.sys, "platform", "linux"):
            with mock.patch.dict(os.environ, {"WSL_DISTRO_NAME": "Ubuntu-24.04"}):
                first = PROFILE.profile_path()
            with mock.patch.dict(os.environ, {"WSL_DISTRO_NAME": "Ubuntu-22.04"}):
                second = PROFILE.profile_path()
            with mock.patch.dict(os.environ, {"WSL_DISTRO_NAME": ""}):
                third = PROFILE.profile_path()
        self.assertEqual(len({first, second, third}), 3)

    def test_apply_preserves_explicit_compilers_and_python(self):
        args = arguments({"Python_EXECUTABLE": "explicit-python", "Python3_EXECUTABLE": "explicit-config"})
        environment = {"CC": "explicit-c", "CXX": "explicit-cxx", "PATH": "original"}
        PROFILE.apply(args, environment, values())
        self.assertEqual(args.definitions, [])
        self.assertEqual(environment["CC"], "explicit-c")
        self.assertEqual(environment["CXX"], "explicit-cxx")

    def test_defaults_and_no_module_python_when_skipped(self):
        data = values()
        del data["module_python"]
        args = arguments()
        PROFILE.apply(args, {"PATH": ""}, data)
        self.assertEqual(args.cache_values, {"Python3_EXECUTABLE": sys.executable})

    def test_invalid_remembered_tool_never_falls_back(self):
        with self.assertRaisesRegex(RuntimeError, "Remembered tool"):
            PROFILE.selected_tool("cmake", {"cmake": "/missing/cmake"}, {})
        self.assertEqual(PROFILE.selected_tool("cmake", {"cmake": "/missing/cmake"},
                                             {"VINC_BUILD_CMAKE": "explicit"}), "explicit")

    def test_same_python_patch_is_accepted_but_minor_change_is_not(self):
        version = sys.version_info
        args = arguments({"Python_EXECUTABLE": sys.executable})
        cache = {"Python_EXECUTABLE": sys.executable,
                 "_Python_INTERPRETER_PROPERTIES": f"Python;{version.major};{version.minor};0"}
        PROFILE.check_python_cache(cache, args)
        cache["_Python_INTERPRETER_PROPERTIES"] = "Python;3;0;0"
        with self.assertRaisesRegex(RuntimeError, "ABI changed"):
            PROFILE.check_python_cache(cache, args)

    def test_changed_module_python_requires_new_build_directory(self):
        with self.assertRaisesRegex(RuntimeError, "another --build-dir"):
            PROFILE.check_python_cache({"Python_EXECUTABLE": "/previous/python",
                                        "_Python_INTERPRETER_PROPERTIES": "Python;3;11;0"},
                                       arguments({"Python_EXECUTABLE": sys.executable}))

    def test_unused_python_cache_entry_does_not_pin_an_abi(self):
        PROFILE.check_python_cache({"Python_EXECUTABLE": "/previous/python"},
                                   arguments({"Python_EXECUTABLE": sys.executable}))

    @unittest.skipUnless(os.name == "nt", "Windows NMAKE integration contract")
    def test_saved_compiler_does_not_pollute_nmake_cc_environment(self):
        spec = importlib.util.spec_from_file_location("build_profile_test", ROOT / "build.py")
        build = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(build)
        args = arguments()
        args.source = build.ROOT / "src"
        child = {"PATH": os.environ.get("PATH", ""), "VSCMD_VER": "17.14"}
        with mock.patch.dict(os.environ, {}, clear=True), \
                mock.patch.object(build.profiles, "load", return_value=values()), \
                mock.patch.object(build.toolchain, "build_environment", return_value=child), \
                mock.patch.object(build, "build_with_environment") as execute:
            build.build(args)
        self.assertNotIn("CC", execute.call_args.args[1])
        self.assertNotIn("CXX", execute.call_args.args[1])
        self.assertEqual(args.cache_values["CMAKE_C_COMPILER"], sys.executable)


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vinc launcher space ")
        self.root = Path(self.temporary.name)
        (self.root / "src").mkdir()
        (self.root / "tools").mkdir()
        for name in ("build.bat", "build.sh", "tools/build_launcher.ps1"):
            shutil.copyfile(ROOT / name, self.root / name)
        (self.root / "build.py").write_text(
            "import json,sys,os\nprint(json.dumps({'python':sys.executable,'args':sys.argv[1:],"
            "'install':os.environ.get('PYTHON_MANAGER_AUTOMATIC_INSTALL'),"
            "'legacy':os.environ.get('PYLAUNCHER_ALLOW_INSTALL')}))\n",
            encoding="utf-8")
        self.environment = dict(os.environ)
        self.environment.pop("VINC_BUILD_INTERPRETER", None)
        PROFILE.save(values(), self.root)

    def tearDown(self):
        self.temporary.cleanup()

    def run_launcher(self, *args):
        command = [str(self.root / "build.bat")] if os.name == "nt" else ["sh", str(self.root / "build.sh")]
        return subprocess.run(command + list(args), env=self.environment, cwd=self.root,
                              capture_output=True, text=True, timeout=30)

    def test_remembered_interpreter_and_argument_boundaries(self):
        args = ("--config", "Release", "--build-dir", str(self.root / "output space"),
                "-DTEST=one two=three", '-DJSON={"key":"a b"}', "-DTRAIL=ends\\")
        result = self.run_launcher(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        output = json.loads(result.stdout)
        self.assertEqual(Path(output["python"]).resolve(), Path(sys.executable).resolve())
        self.assertEqual(output["args"], list(args))

    def test_missing_remembered_python_fails(self):
        PROFILE.save(dict(values(), build_python=str(self.root / "missing-python")), self.root)
        result = self.run_launcher()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Remembered Python", result.stderr)

    def test_explicit_interpreter_overrides_missing_remembered_python(self):
        PROFILE.save(dict(values(), build_python=str(self.root / "missing-python")), self.root)
        self.environment["VINC_BUILD_INTERPRETER"] = sys.executable
        result = self.run_launcher("--jobs", "2")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_explicit_interpreter_does_not_fall_back(self):
        self.environment["VINC_BUILD_INTERPRETER"] = str(self.root / "missing-python")
        result = self.run_launcher()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Python was not found", result.stderr)

    def test_external_source_does_not_use_local_profile(self):
        PROFILE.profile_path(self.root).write_text("invalid\n")
        external = self.root / "external"
        external.mkdir()
        result = self.run_launcher("--source", str(external))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_help_does_not_require_profile(self):
        PROFILE.profile_path(self.root).write_text("invalid\n")
        result = self.run_launcher("--help")
        self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(os.name == "nt", "Windows Python launcher policy")
    def test_build_disables_launcher_automatic_install(self):
        self.environment["PYTHON_MANAGER_AUTOMATIC_INSTALL"] = "true"
        self.environment["PYLAUNCHER_ALLOW_INSTALL"] = "1"
        result = self.run_launcher()
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)
        self.assertEqual(output["install"], "false")
        self.assertIsNone(output["legacy"])


if __name__ == "__main__":
    unittest.main()
