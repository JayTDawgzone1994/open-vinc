"""Exercise the real CMake selection and cache contracts without compiling VINC."""

import os
import json
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import struct
import sysconfig
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
HELPER = ROOT / "src/vinc/common/cmake/VincPythonSelection.cmake"


def literal(value):
    delimiter = "="
    while "]" + delimiter + "]" in value:
        delimiter += "="
    return "[" + delimiter + "[" + str(value) + "]" + delimiter + "]"


@unittest.skipUnless(shutil.which("cmake"), "CMake required")
class PythonSelectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vinc-python-selection-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cmake = shutil.which("cmake")
        self.ninja = shutil.which("ninja")
        if not self.ninja:
            self.skipTest("Ninja required for compiler-independent fixtures")

    def configure(self, body, *, success=True, flags=(), env=None, cwd=None):
        (self.root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(Selection NONE)\n'
            f'include({literal(HELPER.as_posix())})\n' + body,
            encoding="utf-8")
        result = subprocess.run([self.cmake, "-S", str(self.root), "-B", str(self.root / "build"),
                                 "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + self.ninja, *flags],
                                env=env, cwd=cwd, capture_output=True, text=True, encoding="utf-8", timeout=30)
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode == 0, success, output)
        return output

    def custom(self, path=None, app="First"):
        return ('set(VINC_PYTHON_CUSTOM ON)\n'
                f'set(VINC_PYTHON_EXECUTABLE {literal(path or Path(sys.executable).as_posix())})\n'
                f'vinc_select_module_python({app})\n')

    def test_custom_selection_and_repeat_configuration(self):
        self.assertIn("App First: Module Python", self.configure(self.custom()))
        self.configure(self.custom())
        cache = (self.root / "build/CMakeCache.txt").read_text(encoding="utf-8")
        self.assertNotIn("\nPython_EXECUTABLE:", cache)

    def probe_environment(self, *, reply=None, delay=False, failure=False):
        # Instrument real interpreter startup, without replacing production CMake.
        # Only our ABI probe is faulted; CMake/FindPython retain their own behavior.
        hook = self.root / "probe hook"
        hook.mkdir(exist_ok=True)
        (hook / "sitecustomize.py").write_text(
            "import os, sys, time\n"
            "args = sys.orig_argv\n"
            "if '-c' in args:\n"
            "    with open(os.environ['VINC_PROBE_TEST_LOG'], 'a') as log:\n"
            "        log.write('call\\n')\n"
            "    code = args[args.index('-c') + 1]\n"
            "    if code.startswith('import json,platform,struct'):\n"
            "        if os.environ.get('VINC_PROBE_TEST_DELAY'):\n"
            "            time.sleep(60)\n"
            "        if os.environ.get('VINC_PROBE_TEST_FAILURE'):\n"
            "            print('probe startup failed', file=sys.stderr, flush=True)\n"
            "            os._exit(7)\n"
            "        if 'VINC_PROBE_TEST_REPLY' in os.environ:\n"
            "            print(os.environ['VINC_PROBE_TEST_REPLY'], flush=True)\n"
            "            os._exit(0)\n",
            encoding="utf-8")
        env = dict(os.environ, PYTHONPATH=str(hook), PYTHONDONTWRITEBYTECODE="1",
                   VINC_PROBE_TEST_LOG=str(self.root / "probe-calls.txt"))
        if reply is not None:
            env["VINC_PROBE_TEST_REPLY"] = reply
        if delay:
            env["VINC_PROBE_TEST_DELAY"] = "1"
        if failure:
            env["VINC_PROBE_TEST_FAILURE"] = "1"
        return env

    def probe_reply(self):
        return dict(implementation="CPython", version=platform.python_version(),
                    pointer_bytes=struct.calcsize("P"), platform=sysconfig.get_platform(),
                    free_threaded=False)

    def test_probe_snapshot_is_fresh_per_app_and_configuration(self):
        body = self.custom() + self.custom(app="Second")
        body += 'message(STATUS "SNAPSHOT=${VINC_SELECTED_PYTHON_VERSION}/${VINC_SELECTED_PYTHON_POINTER_BYTES}/${VINC_SELECTED_PYTHON_PLATFORM}")\n'
        env = self.probe_environment()
        expected = f"SNAPSHOT={platform.python_version()}/{struct.calcsize('P')}/{sysconfig.get_platform()}"
        for _ in range(2):
            self.assertIn(expected, self.configure(body, env=env))
        self.assertEqual((self.root / "probe-calls.txt").read_text().splitlines(), ["call"] * 4)

    def test_probe_rejects_malformed_fields_with_context(self):
        cases = ["not json", "[]"]
        for field in self.probe_reply():
            missing = self.probe_reply()
            del missing[field]
            cases.append(json.dumps(missing))
            wrong_type = self.probe_reply()
            wrong_type[field] = None
            cases.append(json.dumps(wrong_type))
        for field, value in [("version", "3.bad"), ("platform", ""), ("pointer_bytes", 8.5)]:
            invalid = self.probe_reply()
            invalid[field] = value
            cases.append(json.dumps(invalid))
        for reply in cases:
            with self.subTest(reply=reply):
                output = self.configure(self.custom(), env=self.probe_environment(reply=reply), success=False)
                self.assertIn("App First: invalid Python probe output", output)
                self.assertIn("".join(Path(sys.executable).as_posix().split()), "".join(output.split()))

    def test_probe_rejects_unsupported_interpreters(self):
        for field, value in [("implementation", "PyPy"), ("pointer_bytes", 4),
                             ("free_threaded", True), ("version", "3.10.0"), ("version", "4.0.0")]:
            with self.subTest(field=field, value=value):
                reply = self.probe_reply()
                reply[field] = value
                output = self.configure(self.custom(), env=self.probe_environment(reply=json.dumps(reply)), success=False)
                self.assertIn("requires 64-bit GIL-enabled CPython", output)

    def test_probe_timeout_and_failure_report_app_and_reason(self):
        output = self.configure(self.custom(), env=self.probe_environment(delay=True), success=False)
        self.assertIn("App First: cannot inspect Module Python", output)
        # CMake may wrap the diagnostic after a long interpreter path.
        self.assertRegex(output, r"\blimit\s+10s\b")
        self.assertIn("timeout", output.lower())
        output = self.configure(self.custom(), env=self.probe_environment(failure=True), success=False)
        self.assertIn("probe startup failed", output)

    def binding_preflight(self, discovery):
        binding = ROOT / "src/vinc/module_system/python/cmake/VincPython.cmake"
        body = self.custom() + f'include({literal(binding.as_posix())})\n'
        # Stop at the existing pybind ownership guard, after the real binding's
        # selection/discovery checks but before compiling any adapter targets.
        body += 'macro(vinc_find_python package)\n' + discovery + '\nendmacro()\n'
        body += 'add_library(pybind11::module INTERFACE IMPORTED)\n'
        body += 'vinc_build_python_bindings(probe unused unused unused)\n'
        return body

    def test_binding_reuses_probe_and_rejects_inconsistent_discovery(self):
        env = self.probe_environment()
        output = self.configure(self.binding_preflight(
            'set(Python_VERSION "${VINC_SELECTED_PYTHON_VERSION}")'), env=env, success=False)
        self.assertIn("must initialize their pinned pybind11", output)
        self.assertEqual((self.root / "probe-calls.txt").read_text().splitlines(), ["call"])
        for discovery in ['set(Python_VERSION "3.0.0")',
                          'set(Python_VERSION "${VINC_SELECTED_PYTHON_VERSION}")\nset(Python_EXECUTABLE "/other/python")']:
            output = self.configure(self.binding_preflight(discovery), success=False)
            self.assertIn("App First: Python discovery disagrees with selection", output)

    def test_patch_update_preserves_cache_and_binding_abi(self):
        self.configure(self.custom())
        reply = self.probe_reply()
        reply["version"] = f"{sys.version_info.major}.{sys.version_info.minor}.999"
        env = self.probe_environment(reply=json.dumps(reply))
        output = self.configure(self.binding_preflight(
            f'set(Python_VERSION "{platform.python_version()}")'), env=env, success=False)
        self.assertIn("must initialize their pinned pybind11", output)
        self.assertIn(reply["version"], output)

    def test_system_and_custom_share_first_path_interpreter(self):
        env = dict(os.environ, PATH=str(Path(sys.executable).parent) + os.pathsep + os.environ.get("PATH", ""))
        body = 'set(VINC_PYTHON_CUSTOM OFF)\nvinc_select_module_python(SystemApp)\n'
        body += 'set(first "${VINC_SELECTED_PYTHON}")\n'
        body += 'set(VINC_PYTHON_CUSTOM ON)\nset(VINC_PYTHON_EXECUTABLE "${first}")\n'
        body += 'vinc_select_module_python(CustomApp)\n'
        output = self.configure(body, env=env)
        self.assertIn("App CustomApp: Module Python", output)

    def test_missing_system_python_does_not_search_registry_or_other_paths(self):
        native_tools = str(Path(os.environ["SystemRoot"]) / "System32") if os.name == "nt" else ""
        output = self.configure('set(VINC_PYTHON_CUSTOM OFF)\nvinc_select_module_python(Missing)\n',
                                env=dict(os.environ, PATH=native_tools), success=False)
        self.assertIn("Module Python not found", output)

    def test_missing_custom_python_is_not_replaced(self):
        output = self.configure(self.custom((self.root / "missing-python").as_posix()), success=False)
        self.assertIn("Module Python not found", output)

    def system_environment(self, first):
        return dict(os.environ, PATH=str(first) + os.pathsep + str(Path(sys.executable).parent))

    def test_system_skips_directory_candidate(self):
        first = self.root / "first candidate"
        (first / ("python.exe" if os.name == "nt" else "python3")).mkdir(parents=True)
        output = self.configure('vinc_select_module_python(SystemApp)\n',
                                env=self.system_environment(first))
        self.assertIn(Path(sys.executable).as_posix(), output)

    @unittest.skipIf(os.name == "nt", "POSIX executable permissions")
    def test_system_skips_nonexecutable_and_broken_symlink(self):
        first = self.root / "first candidate"
        first.mkdir()
        candidate = first / "python3"
        candidate.write_text("not executable", encoding="utf-8")
        candidate.chmod(0o600)
        body = 'vinc_select_module_python(SystemApp)\n'
        self.configure(body, env=self.system_environment(first))
        candidate.unlink()
        candidate.symlink_to(first / "missing")
        self.configure(body, env=self.system_environment(first))

    def test_selected_broken_image_does_not_fall_back(self):
        first = self.root / "first candidate"
        first.mkdir()
        candidate = first / ("python.exe" if os.name == "nt" else "python3")
        candidate.write_bytes(b"not an executable image\n")
        candidate.chmod(0o700)
        output = self.configure('vinc_select_module_python(SystemApp)\n',
                                env=self.system_environment(first), success=False)
        self.assertIn("cannot inspect Module Python", output)
        self.assertIn("".join(candidate.as_posix().split()), "".join(output.split()))

    def test_empty_path_entries_do_not_implicitly_search_cwd(self):
        candidate = self.root / ("python.exe" if os.name == "nt" else "python3")
        candidate.write_bytes(b"not an executable image\n")
        candidate.chmod(0o700)
        output = self.configure('vinc_select_module_python(SystemApp)\n',
                                env=dict(os.environ, PATH=os.pathsep), cwd=self.root, success=False)
        self.assertIn("Module Python not found", output)
        # A deliberately supplied relative directory is distinct from an empty entry.
        output = self.configure('vinc_select_module_python(SystemApp)\n',
                                env=dict(os.environ, PATH="."), cwd=self.root, success=False)
        self.assertIn("cannot inspect Module Python", output)

    def test_parent_search_settings_do_not_replace_path_interpreter(self):
        body = (f'set(CMAKE_FIND_ROOT_PATH {literal(self.root.as_posix())})\n'
                'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM ONLY)\n'
                'set(CMAKE_FIND_APPBUNDLE ONLY)\n'
                'vinc_select_module_python(SystemApp)\n'
                'if(NOT CMAKE_FIND_ROOT_PATH_MODE_PROGRAM STREQUAL "ONLY" OR\n'
                '   NOT CMAKE_FIND_APPBUNDLE STREQUAL "ONLY")\n'
                '  message(FATAL_ERROR "Parent search policy changed")\nendif()\n')
        output = self.configure(body, env=self.system_environment(Path(sys.executable).parent))
        self.assertIn(Path(sys.executable).as_posix(), output)

    def test_external_python_cache_does_not_select_vinc_interpreter(self):
        other = (self.root / "nonexistent/python").as_posix()
        body = self.custom() + 'message(STATUS "SELECTED=${VINC_SELECTED_PYTHON}")\n'
        output = self.configure(body, flags=["-DPython_EXECUTABLE=" + other])
        self.assertIn("SELECTED=" + Path(sys.executable).as_posix(), output)
        cache = (self.root / "build/CMakeCache.txt").read_text(encoding="utf-8")
        self.assertIn("Python_EXECUTABLE:UNINITIALIZED=" + other, cache)

    def test_foreign_find_python_before_configuration_is_preserved(self):
        other = self.root / "other environment"
        subprocess.run([sys.executable, "-m", "venv", "--without-pip", str(other)],
                       check=True, timeout=30)
        executable = other / ("Scripts/python.exe" if os.name == "nt" else "bin/python3")
        configuration = ROOT / "src/vinc/sdk/cmake/VincConfiguration.cmake"
        body = f'set(Python_EXECUTABLE {literal(executable.as_posix())})\n'
        body += 'find_package(Python REQUIRED COMPONENTS Interpreter)\n'
        body += 'set(other_python "${Python_EXECUTABLE}")\n'
        body += f'include({literal(configuration.as_posix())})\n'
        body += self.custom()
        body += 'if(NOT Python_EXECUTABLE STREQUAL other_python)\n'
        body += '  message(FATAL_ERROR "VINC changed another consumer interpreter")\nendif()\n'
        self.configure(body)
        self.configure(body)

    def test_foreign_find_python_cache_is_not_legacy_vinc_evidence(self):
        body = 'set(_Python_EXECUTABLE "/foreign/python" CACHE INTERNAL "foreign")\n'
        body += 'set(_Python_INTERPRETER_PROPERTIES "unrelated" CACHE INTERNAL "foreign")\n'
        self.configure(body + self.custom())

    def test_legacy_vinc_package_without_identity_requires_new_directory(self):
        package = self.root / "build/vinc-python/Release/vinc"
        package.mkdir(parents=True)
        metadata = package / "_build.py"
        metadata.write_text("VERSION = (3, 11)\n")
        output = self.configure(self.custom(), success=False)
        self.assertIn("VINC Python package has no recorded interpreter identity", output)
        self.assertTrue(metadata.exists())

    def test_legacy_source_package_without_identity_requires_new_directory(self):
        source_build = self.root / "build/embedded-vinc"
        package = source_build / "python/Debug/vinc"
        package.mkdir(parents=True)
        metadata = package / "_build.py"
        metadata.write_text("VERSION = (3, 11)\n")
        body = f'set(VINC_BUILD_ROOT {literal(source_build.as_posix())})\n' + self.custom()
        output = self.configure(body, success=False)
        self.assertIn("VINC Python package has no recorded interpreter identity", output)
        self.assertTrue(metadata.exists())

    def test_changed_cached_abi_is_rejected(self):
        self.configure(self.custom())
        output = self.configure(self.custom(), flags=["-D_VINC_MODULE_PYTHON_ABI:INTERNAL=3.0/8/old"], success=False)
        self.assertIn("environment differs from this build cache", output)

    def test_minimum_version_failure_explains_selection(self):
        # Use the actual interpreter with an intentionally newer test policy;
        # this tests diagnostics, not a claim of running an obsolete CPython.
        for name in ("VincPythonSelection.cmake", "VincPythonRequirements.cmake"):
            shutil.copy2(HELPER.parent / name, self.root / name)
        (self.root / "python_minimum.txt").write_text("3.99\n")
        body = f'include({literal((self.root / "VincPythonSelection.cmake").as_posix())})\n'
        output = self.configure(body + self.custom(), success=False)
        self.assertIn("requires 64-bit GIL-enabled CPython >= 3.99", output)

    def test_missing_development_files_uses_kconfig_diagnostic(self):
        # FindPython failure injection exercises the shared REQUIRED diagnostic.
        (self.root / "FindPython.cmake").write_text(
            'set(Python_FOUND FALSE)\nset(Python_NOT_FOUND_MESSAGE "missing Development.Module")\n')
        body = self.custom() + 'set(Python_EXECUTABLE "${VINC_SELECTED_PYTHON}")\n'
        body += 'list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")\n'
        body += 'vinc_find_python(Python REQUIRED COMPONENTS Interpreter Development.Module)\n'
        output = self.configure(body, success=False)
        self.assertIn("missing Development.Module", output)
        self.assertIn("Select System/Custom Module Python in App Kconfig", output)

    def test_two_real_environments_conflict_and_preserve_venv_identity(self):
        venv = self.root / "中文 environment"
        subprocess.run([sys.executable, "-m", "venv", "--without-pip", str(venv)], check=True, timeout=30)
        interpreter = venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python3")
        output = self.configure(self.custom() + self.custom(interpreter.as_posix(), "Second"), success=False)
        self.assertIn("Apps in one build must use the same Python interpreter", output)
        self.assertIn("Second", output)
        output = self.configure(self.custom(interpreter.as_posix()), success=False)
        self.assertIn("environment differs from this build cache", output)


if __name__ == "__main__":
    unittest.main()
