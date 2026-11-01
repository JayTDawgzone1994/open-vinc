"""Build wrapper policy and real subprocess tests; no framework build required."""

import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("vinc_build", ROOT / "build.py")
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)
TOOLCHAIN = BUILD.toolchain
PROCESS = BUILD.process


class CompilerMinimumTests(unittest.TestCase):
    def test_native_msvc_boundary(self):
        for version in ("19.42.34444", "19.43.99999"):
            with self.subTest(version=version):
                with self.assertRaisesRegex(RuntimeError, "requires MSVC 19.44"):
                    TOOLCHAIN.check_compiler_minimum({"id": "MSVC", "version": version})

        for version in ("19.44", "19.44.1", "19.51.36257"):
            TOOLCHAIN.check_compiler_minimum({"id": "MSVC", "version": version})

    def test_other_compilers_keep_their_own_requirements(self):
        for compiler in ("Clang", "AppleClang", "GNU"):
            TOOLCHAIN.check_compiler_minimum({"id": compiler, "version": "18.1"})


def run_in_windows_console(command, code_page, cwd, *, timeout=120):
    """Use an isolated test console; never change the test runner's console."""
    controller = (
        "import ctypes,json,subprocess,sys\n"
        "api=ctypes.WinDLL('kernel32',use_last_error=True)\n"
        "cp=int(sys.argv[1])\n"
        "assert api.SetConsoleCP(cp) and api.SetConsoleOutputCP(cp)\n"
        "result=subprocess.run(json.loads(sys.argv[2]),capture_output=True)\n"
        "print(json.dumps({'code':result.returncode,'stdout':result.stdout.decode('utf-8',errors='replace'),"
        "'stderr':result.stderr.decode('utf-8',errors='replace'),"
        "'input_cp':api.GetConsoleCP(),'output_cp':api.GetConsoleOutputCP()}))\n")
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    result = subprocess.run([sys.executable, "-B", "-X", "utf8", "-c", controller,
                             str(code_page), json.dumps(command)], cwd=cwd,
                            capture_output=True, timeout=timeout,
                            creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=startup)
    if result.returncode:
        raise AssertionError(result.stderr.decode(errors="replace"))
    return json.loads(result.stdout)


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vinc-build-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "source with spaces"
        self.source.mkdir()
        (self.source / "CMakeLists.txt").write_text("project(test NONE)\n", encoding="utf-8")
        self.directory = self.root / "build with spaces"
        self.directory.mkdir()
        self.args = BUILD.arguments(["--source", str(self.source), "--build-dir", str(self.directory)])
        self.identity = {"platform": "linux", "machine": "x86_64"}
        for language, driver in (("C", "cc"), ("CXX", "c++")):
            path = str(self.root / driver)
            self.identity[language] = {"path": path, "resolved_path": path,
                                       "id": "GNU", "version": "13.3.0",
                                       "target": "x86_64-linux-gnu",
                                       "driver": "c++" if language == "CXX" else "c"}

    def test_argument_defaults_and_paths(self):
        self.assertEqual(BUILD.ROOT, ROOT)
        self.assertEqual(BUILD.arguments([]).source, ROOT / "src")
        self.assertEqual(self.args.config, "Debug")
        self.assertIsNone(self.args.jobs)
        self.assertIsNone(self.args.target)
        args = BUILD.arguments(["--source", str(self.source), "--config", "Release",
                                "--target", "CameraApp", "MonitorApp", "--jobs", "3",
                                "-DLABEL=one two;three"])
        self.assertEqual(args.target, ["CameraApp", "MonitorApp"])
        self.assertEqual(args.cache_values["LABEL"], "one two;three")

    def test_reject_conflicting_controls(self):
        for option in ("-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=x",
                       "-DCMAKE_GENERATOR=Ninja", "--preset=custom", "--clean", "--jobs=0"):
            with self.subTest(option=option), mock.patch("sys.stderr", io.StringIO()):
                with self.assertRaises(SystemExit):
                    BUILD.arguments(["--source", str(self.source), option])

    def test_reject_duplicate_definition(self):
        with mock.patch("sys.stderr", io.StringIO()), self.assertRaises(SystemExit):
            BUILD.arguments(["-DVALUE=one", "-DVALUE=two"])

    def test_cache_rejections_preserve_files(self):
        sentinel = self.directory / "keep.txt"
        sentinel.write_text("keep", encoding="utf-8")
        for entry in ("CMAKE_BUILD_TYPE:STRING=Release", "CMAKE_GENERATOR:INTERNAL=Unix Makefiles",
                      "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(self.root),
                      "CMAKE_CXX_COMPILER:FILEPATH=" + str(self.root / "other"),
                      "CMAKE_TOOLCHAIN_FILE:FILEPATH=toolchain.cmake"):
            with self.subTest(entry=entry):
                (self.directory / "CMakeCache.txt").write_text(entry, encoding="utf-8")
                with self.assertRaises(RuntimeError):
                    TOOLCHAIN.validate_cache(self.directory, self.args, self.identity)

                self.assertEqual(sentinel.read_text(), "keep")
                self.assertEqual((self.directory / "CMakeCache.txt").read_text(), entry)

    def test_compiler_version_change_rejected(self):
        state = self.directory / ".vinc-build-toolchain.json"
        TOOLCHAIN.save_toolchain_record(self.directory, self.args, self.identity)
        TOOLCHAIN.validate_cache(self.directory, self.args, self.identity)
        changed = json.loads(json.dumps(self.identity))
        changed["CXX"]["version"] = "2"
        with self.assertRaisesRegex(RuntimeError, r"CXX.version"):
            TOOLCHAIN.validate_cache(self.directory, self.args, changed)

    def test_legacy_banner_migration_preserves_cache(self):
        legacy = json.loads(json.dumps(self.identity))
        for language in ("C", "CXX"):
            legacy[language] = {"path": legacy[language]["path"],
                                "version": "g++ (Ubuntu 13.3.0) 13.3.0\nCopyright Free Software Foundation"}
        state = self.directory / ".vinc-build-toolchain.json"
        state.write_text(json.dumps(legacy), encoding="utf-8")
        cache = self.directory / "CMakeCache.txt"
        cache.write_text("VINC_BUILD_APPS:BOOL=OFF\nCUSTOM:STRING=keep\n", encoding="utf-8")
        with mock.patch("sys.stdout", io.StringIO()) as output:
            TOOLCHAIN.validate_cache(self.directory, self.args, self.identity)
        self.assertIn("legacy", output.getvalue())
        self.assertEqual(json.loads(state.read_text()), legacy)
        self.assertIn("CUSTOM:STRING=keep", cache.read_text())

    def test_msvc_banner_ignores_localized_usage(self):
        expected = {"id": "MSVC", "version": "19.51.36257", "target": "x86_64"}
        for suffix in ("版權所有 使用方法", "\ufffd\ufffd\ufffd", "Copyright and usage"):
            banner = "Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36257 for x64\n" + suffix
            self.assertEqual(TOOLCHAIN.compiler_banner(banner), expected)

    @unittest.skipUnless(os.name == "nt", "Windows legacy MSVC path records")
    def test_legacy_msvc_record_survives_decoding_and_path_variants(self):
        current = {"platform": "win32", "machine": "x86_64", "msvc_environment": {
            "VCTOOLSVERSION": "14.51.36231", "WINDOWSSDKVERSION": "10.0.26100.0",
            "INCLUDE": "C:/sdk/include;D:/VC/include", "LIB": "C:/sdk/lib"}}
        compiler = str(self.root / "cl.exe")
        for language in ("C", "CXX"):
            current[language] = {"id": "MSVC", "version": "19.51.36257", "target": "x86_64",
                                 "path": compiler, "resolved_path": compiler, "driver": "cl"}
        legacy = json.loads(json.dumps(current))
        legacy["machine"] = "AMD64"
        legacy["msvc_environment"]["INCLUDE"] = r"c:\sdk\\include;d:\vc\include"
        legacy["msvc_environment"]["WINDOWSSDKVERSION"] += "\\"
        for language in ("C", "CXX"):
            legacy[language] = {"path": compiler, "version":
                "Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36257 for x64\n\ufffd版權\ufffd"}
        state = self.directory / ".vinc-build-toolchain.json"
        state.write_text(json.dumps(legacy), encoding="utf-8")
        TOOLCHAIN.validate_cache(self.directory, self.args, current, report_unmanaged=False)
        TOOLCHAIN.save_toolchain_record(self.directory, self.args, current)
        TOOLCHAIN.validate_cache(self.directory, self.args, current)
        current["msvc_environment"]["WINDOWSSDKVERSION"] = "10.0.99999.0"
        with self.assertRaisesRegex(RuntimeError, "WINDOWSSDKVERSION"):
            TOOLCHAIN.validate_cache(self.directory, self.args, current)

    def test_cache_equivalent_driver_spelling_is_preserved(self):
        # Legacy wrappers stored resolved compiler paths. Passing the selected
        # alias back to CMake would unnecessarily reset its language cache.
        cached = str(self.root / "c++")
        self.identity["CXX"]["path"] = str(self.root / "g++")
        (self.directory / "CMakeCache.txt").write_text(
            "CMAKE_CXX_COMPILER:FILEPATH=" + cached + "\nCUSTOM:STRING=keep\n", encoding="utf-8")
        with mock.patch.object(TOOLCHAIN, "build_environment", return_value=dict(os.environ)), \
                mock.patch.object(TOOLCHAIN, "executable", side_effect=lambda name, env: name), \
                mock.patch.object(TOOLCHAIN, "tool_output", return_value="cmake version 3.20.0"), \
                mock.patch.object(TOOLCHAIN, "selected_compilers", return_value={"CXX": self.identity["CXX"]["path"]}), \
                mock.patch.object(TOOLCHAIN, "toolchain_identity", return_value=self.identity), \
                mock.patch.object(BUILD, "run_step", return_value=17) as run, \
                mock.patch("sys.stdout", io.StringIO()):
            self.assertEqual(BUILD.build(self.args), 17)
        self.assertIn("-DCMAKE_CXX_COMPILER=" + cached, run.call_args.args[0])

    def test_windows_environment_spelling_and_order(self):
        first = {"INCLUDE": r"C:\SDK\\include;D:/VC/include", "LIB": r"C:\SDK\lib",
                 "WINDOWSSDKVERSION": "10.0.26100.0\\"}
        second = {"INCLUDE": r"c:/sdk/include;d:\vc\include", "LIB": r"c:/sdk/lib",
                  "WINDOWSSDKVERSION": "10.0.26100.0"}
        self.assertEqual(TOOLCHAIN.normalized_windows_environment(first), TOOLCHAIN.normalized_windows_environment(second))
        second["INCLUDE"] = r"d:\vc\include;c:/sdk/include"
        self.assertNotEqual(TOOLCHAIN.normalized_windows_environment(first), TOOLCHAIN.normalized_windows_environment(second))

    def test_record_purpose_conflict_preserves_files(self):
        TOOLCHAIN.save_toolchain_record(self.directory, self.args, self.identity)
        state = self.directory / ".vinc-build-toolchain.json"
        before = state.read_bytes()
        self.args.install_vinc_sdk = True
        with self.assertRaisesRegex(RuntimeError, "build purpose"):
            TOOLCHAIN.validate_cache(self.directory, self.args, self.identity)
        self.assertEqual(state.read_bytes(), before)

    def test_bad_record_is_not_overwritten(self):
        state = self.directory / ".vinc-build-toolchain.json"
        for payload in ('{"broken":', '[]', '{"format":99,"purpose":"build"}'):
            with self.subTest(payload=payload):
                state.write_text(payload, encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "Cannot validate toolchain record"):
                    TOOLCHAIN.validate_cache(self.directory, self.args, self.identity)
                self.assertEqual(state.read_text(), payload)

    def test_failed_atomic_save_preserves_previous_record(self):
        TOOLCHAIN.save_toolchain_record(self.directory, self.args, self.identity)
        state = self.directory / ".vinc-build-toolchain.json"
        before = state.read_bytes()
        with mock.patch.object(BUILD.os, "replace", side_effect=OSError("injected replace failure")):
            with self.assertRaisesRegex(OSError, "injected"):
                TOOLCHAIN.save_toolchain_record(self.directory, self.args, self.identity)
        self.assertEqual(state.read_bytes(), before)
        self.assertFalse(list(self.directory.glob(".vinc-toolchain-*.tmp")))

    def test_failed_query_rejects_plausible_version(self):
        result = subprocess.CompletedProcess([], 7, b"clang version 18.1.3")
        with mock.patch.object(BUILD.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(RuntimeError, "Tool query failed.*7"):
                TOOLCHAIN.tool_output(["clang", "--version"], {}, self.root)
        with self.assertRaisesRegex(RuntimeError, "Cannot identify"):
            TOOLCHAIN.compiler_banner("permission denied")

    def test_utf8_log_cap_keeps_valid_prefix(self):
        for capacity in (1, 2, 3, 4, 5):
            output = io.BytesIO()
            with self.subTest(capacity=capacity), mock.patch.object(PROCESS, "LOG_LIMIT_BYTES", capacity), \
                    mock.patch("sys.stdout", io.StringIO()):
                PROCESS.BuildLog(output).write("中🙂end")
                text = output.getvalue().decode("utf-8", errors="strict")
                self.assertTrue("中🙂end".startswith(text.split("\n")[0]))
                self.assertIn("Log limit", text)

    @unittest.skipIf(os.name == "nt", "POSIX compiler symlink behavior")
    def test_compiler_symlink_keeps_driver_name(self):
        real = self.root / "clang"
        real.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        real.chmod(0o755)
        alias = self.root / "clang++"
        alias.symlink_to(real)
        selected = TOOLCHAIN.executable(str(alias), dict(os.environ))
        self.assertEqual(selected, str(alias))
        self.assertEqual(TOOLCHAIN.driver_role(selected, "Clang"), "c++")
        self.assertEqual(Path(selected).resolve(), real)

    def test_cross_process_lock_releases_after_exit(self):
        script = ("import sys\n"
                  "sys.path.insert(0, sys.argv[1])\n"
                  "import build\n"
                  "from pathlib import Path\n"
                  "with build.build_lock(Path(sys.argv[2])):\n"
                  "    print('locked', flush=True)\n"
                  "    sys.stdin.read()\n")
        process = subprocess.Popen([sys.executable, "-B", "-c", script, str(ROOT),
                                    str(self.directory)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            self.assertEqual(process.stdout.readline().strip(), b"locked")
            with self.assertRaisesRegex(RuntimeError, "Another build"):
                with BUILD.build_lock(self.directory):
                    self.fail("must not acquire")
        finally:
            process.kill()
            process.communicate(timeout=10)

        with BUILD.build_lock(self.directory):
            self.assertTrue((self.directory / ".vinc-build.lock").exists())

    def test_real_child_arguments_and_exit_code(self):
        payload = 'spaces & % ; $ ` Unicode-測試 "quotes"'
        output = self.root / "received.json"
        script = ("import json\n"
                  "import sys\n"
                  "from pathlib import Path\n"
                  "Path(sys.argv[1]).write_text(json.dumps(sys.argv[2:]))\n"
                  "sys.exit(7)\n")
        with mock.patch("sys.stdout", io.StringIO()):
            code = BUILD.run_step([sys.executable, "-B", "-c", script, str(output), payload],
                                  self.source, dict(os.environ), PROCESS.BuildLog(io.BytesIO()))

        self.assertEqual(code, 7)
        self.assertEqual(json.loads(output.read_text()), [payload])

    def test_log_cap_does_not_cap_console(self):
        output = io.BytesIO()
        with mock.patch.object(PROCESS, "LOG_LIMIT_BYTES", 5), mock.patch("sys.stdout", io.StringIO()) as console:
            log = PROCESS.BuildLog(output)
            log.write("12345678")
            log.write("later")
            self.assertIn("later", console.getvalue())

        self.assertTrue(output.getvalue().startswith(b"12345\n[Log limit"))
        self.assertNotIn(b"later", output.getvalue().split(b"]\n")[-1])

    def test_unicode_log_with_legacy_console(self):
        raw_console = io.BytesIO()
        console = io.TextIOWrapper(raw_console, encoding="cp950")
        output = io.BytesIO()
        with mock.patch("sys.stdout", console):
            PROCESS.BuildLog(output).write("UI ready \u2713 \U0001f600\n")

        self.assertIn(b"UI ready", raw_console.getvalue())
        self.assertIn("\U0001f600", output.getvalue().decode("utf-8"))

    @unittest.skipUnless(os.name == "nt", "Windows console code pages")
    def test_windows_worker_isolates_console_for_descendants(self):
        probe = "import ctypes,json; k=ctypes.windll.kernel32; print(json.dumps([k.GetConsoleCP(),k.GetConsoleOutputCP()]))"
        child = "import subprocess,sys; sys.exit(subprocess.call([sys.executable,'-c',sys.argv[1]]))"
        driver = (
            "import io,os,sys\nfrom pathlib import Path\n"
            "sys.path.insert(0,sys.argv[1])\nimport build\n"
            "sys.exit(build.run_step([sys.executable,'-c',sys.argv[2],sys.argv[3]],"
            "Path.cwd(),dict(os.environ),build.process.BuildLog(io.BytesIO())))\n")
        for code_page in (950, 65001):
            with self.subTest(code_page=code_page):
                result = run_in_windows_console(
                    [sys.executable, "-B", "-c", driver, str(ROOT), child, probe], code_page, self.root)
                self.assertEqual(result["code"], 0, result)
                self.assertIn("[65001, 65001]", result["stdout"])
                self.assertEqual(result["input_cp"], code_page)
                self.assertEqual(result["output_cp"], code_page)

    @unittest.skipUnless(os.name == "nt", "Windows worker control-pipe cancellation")
    def test_windows_worker_delivers_graceful_break(self):
        ready = self.root / "ready"
        stopped = self.root / "stopped"
        child = (
            "import signal,sys,time\nfrom pathlib import Path\n"
            "def stop(signum,frame):\n"
            "    Path(sys.argv[2]).write_text('graceful')\n"
            "    sys.exit(0)\n"
            "signal.signal(signal.SIGBREAK,stop)\n"
            "Path(sys.argv[1]).write_text('ready')\n"
            "while True:\n    time.sleep(0.02)\n")
        driver = (
            "import _thread,io,os,sys,threading,time\nfrom pathlib import Path\n"
            "sys.path.insert(0,sys.argv[1])\nimport build\n"
            "done=threading.Event()\n"
            "def interrupt():\n"
            "    deadline=time.monotonic()+10\n"
            "    while not Path(sys.argv[3]).exists() and time.monotonic()<deadline:\n"
            "        if done.wait(0.02):\n            return\n"
            "    _thread.interrupt_main()\n"
            "thread=threading.Thread(target=interrupt,daemon=True)\nthread.start()\n"
            "try:\n"
            "    build.run_step([sys.executable,'-c',sys.argv[2],sys.argv[3],sys.argv[4]],"
            "Path.cwd(),dict(os.environ),build.process.BuildLog(io.BytesIO()))\n"
            "except KeyboardInterrupt:\n    sys.exit(130)\n"
            "finally:\n    done.set()\n    thread.join()\n")
        result = run_in_windows_console([sys.executable, "-B", "-c", driver,
                                        str(ROOT), child, str(ready), str(stopped)], 950, self.root, timeout=20)
        self.assertEqual(result["code"], 130, result)
        self.assertEqual(stopped.read_text(), "graceful")
        self.assertEqual(result["output_cp"], 950)

    def test_missing_tool_does_not_choose_another(self):
        with self.assertRaisesRegex(RuntimeError, "Missing tool"):
            TOOLCHAIN.executable("vinc-deliberately-missing-compiler", {"PATH": str(self.root)})

    def test_environment_generator_conflict_preserves_parent(self):
        with mock.patch.dict(os.environ, {"CMAKE_GENERATOR": "Unix Makefiles"}):
            with self.assertRaisesRegex(RuntimeError, "conflicts with Ninja"):
                TOOLCHAIN.build_environment()

            self.assertEqual(os.environ["CMAKE_GENERATOR"], "Unix Makefiles")

    def test_configure_failure_never_starts_build(self):
        with mock.patch.object(TOOLCHAIN, "build_environment", return_value=dict(os.environ)), \
                mock.patch.object(TOOLCHAIN, "executable", side_effect=lambda name, env: name), \
                mock.patch.object(TOOLCHAIN, "tool_output", return_value="cmake version 3.20.0"), \
                mock.patch.object(TOOLCHAIN, "selected_compilers", return_value={"C": "cc", "CXX": "cxx"}), \
                mock.patch.object(TOOLCHAIN, "toolchain_identity", return_value=self.identity), \
                mock.patch.object(BUILD, "run_step", return_value=17) as run, \
                mock.patch("sys.stdout", io.StringIO()):
            self.assertEqual(BUILD.build(self.args), 17)

        self.assertEqual(run.call_count, 1)
        self.assertFalse((self.directory / ".vinc-build-toolchain.json").exists())

    def test_help_from_other_directory(self):
        environment = dict(os.environ)
        environment["VINC_BUILD_INTERPRETER"] = sys.executable
        if os.name == "nt":
            command = [str(ROOT / "build.bat"), "--help"]
        else:
            command = ["sh", str(ROOT / "build.sh"), "--help"]

        direct = [sys.executable, "-B", str(ROOT / "build.py"), "--help"]
        if os.name != "nt":
            direct = [str(ROOT / "build.py"), "--help"]

        for invocation in (command, direct):
            with self.subTest(invocation=invocation):
                result = subprocess.run(invocation, cwd=self.root, env=environment,
                                        capture_output=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(b"--source", result.stdout)
                self.assertFalse((self.root / "build").exists())

    def test_interrupt_cleans_grandchild(self):
        # The test timer simulates Ctrl+C inside the wrapper, even in CI without a
        # console. The grandchild ignores graceful signals; ownership must kill it.
        worker = self.root / "worker.py"
        heartbeat = self.root / "heartbeat"
        worker.write_text(
            "import signal\n"
            "import subprocess\n"
            "import sys\n"
            "import time\n"
            "from pathlib import Path\n"
            "if hasattr(signal, 'SIGBREAK'):\n"
            "    signal.signal(signal.SIGBREAK, signal.SIG_IGN)\n"
            "if len(sys.argv) == 2:\n"
            "    subprocess.Popen([sys.executable, __file__, sys.argv[1], 'grandchild'])\n"
            "else:\n"
            "    signal.signal(signal.SIGINT, signal.SIG_IGN)\n"
            "while True:\n"
            "    if len(sys.argv) > 2:\n"
            "        Path(sys.argv[1]).write_text(str(time.time()))\n"
            "    time.sleep(0.05)\n", encoding="utf-8")
        driver = (
            "import _thread\n"
            "import io\n"
            "import os\n"
            "import sys\n"
            "import threading\n"
            "import time\n"
            "from pathlib import Path\n"
            "sys.path.insert(0, sys.argv[1])\n"
            "import build\n"
            "stop = threading.Event()\n"
            "def interrupt_when_ready():\n"
            "    deadline = time.monotonic() + 10\n"
            "    while not Path(sys.argv[3]).exists() and time.monotonic() < deadline:\n"
            "        if stop.wait(0.02):\n"
            "            return\n"
            "    _thread.interrupt_main()\n"
            "timer = threading.Thread(target=interrupt_when_ready, daemon=True)\n"
            "timer.start()\n"
            "try:\n"
            "    build.run_step([sys.executable, sys.argv[2], sys.argv[3]], Path.cwd(), "
            "dict(os.environ), build.process.BuildLog(io.BytesIO()))\n"
            "except KeyboardInterrupt:\n"
            "    sys.exit(130)\n"
            "finally:\n"
            "    stop.set()\n"
            "    timer.join()\n")
        result = subprocess.run([sys.executable, "-B", "-c", driver, str(ROOT),
                                 str(worker), str(heartbeat)], cwd=self.root, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 130, result.stdout + result.stderr)
        self.assertTrue(heartbeat.exists())
        before = heartbeat.read_bytes()
        time.sleep(0.3)
        self.assertEqual(heartbeat.read_bytes(), before, "grandchild survived cancellation")


@unittest.skipUnless(os.environ.get("VINC_BUILD_TEST_CMAKE") == "1", "opt-in real CMake integration")
class CMakeIntegrationTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "MSVC showIncludes encoding")
    def test_msvc_unicode_header_incremental_from_both_code_pages(self):
        with tempfile.TemporaryDirectory(prefix="vinc-msvc-encoding-") as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            helper = (ROOT / "src/vinc/common/cmake/VincMsvcDependencies.cmake").as_posix()
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(encoding LANGUAGES CXX)\n'
                'add_executable(probe main.cpp)\ntarget_compile_options(probe PRIVATE /utf-8)\n'
                'include("' + helper + '")\n', encoding="utf-8")
            header = source / "value_標頭.hpp"
            (source / "main.cpp").write_text(
                '#include "value_標頭.hpp"\nint main() { return VALUE; }\n', encoding="utf-8")
            for code_page in (950, 65001):
                binary = root / str(code_page)
                command = [str(ROOT / "build.bat"), "--source", str(source), "--build-dir", str(binary)]
                for value in (1, 2):
                    with self.subTest(code_page=code_page, value=value):
                        header.write_text("#define VALUE " + str(value) + "\n", encoding="utf-8")
                        result = run_in_windows_console(command, code_page, root)
                        self.assertEqual(result["code"], 0, result)
                        self.assertEqual(result["input_cp"], code_page)
                        self.assertEqual(result["output_cp"], code_page)
                        execution = subprocess.run([str(binary / "probe.exe")], timeout=10)
                        self.assertEqual(execution.returncode, value)
                        output = (binary / "CMakeFiles/VincShowIncludes/output.log").read_text(encoding="utf-8")
                        self.assertIn("vinc_標頭.hpp", output)

    def test_external_clean_incremental_and_mismatch(self):
        with tempfile.TemporaryDirectory(prefix="vinc-build-cmake-") as temporary:
            root = Path(temporary)
            source = root / "source space"
            source.mkdir()
            binary = root / "binary space"
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(wrapper_fixture LANGUAGES CXX)\n'
                'add_executable(Camera camera.cpp)\nadd_library(Monitor OBJECT monitor.cpp)\n'
                'add_custom_target(Never ALL COMMAND "${CMAKE_COMMAND}" -E false)\n'
                'file(WRITE "${CMAKE_BINARY_DIR}/label.txt" "${LABEL}")\n', encoding="utf-8")
            (source / "camera.cpp").write_text(
                '#include <iostream>\n#include <string>\nint main()\n{\n'
                '    std::cout << std::string("driver-linked");\n    return 0;\n}\n')
            (source / "monitor.cpp").write_text("int monitor()\n{\n    return 2;\n}\n")
            command = [sys.executable, "-B", str(ROOT / "build.py"), "--source", str(source),
                       "--build-dir", str(binary), "--target", "Camera", "Monitor", "--jobs", "2",
                       "-DLABEL=space value;literal"]
            for run in range(2):
                environment = dict(os.environ)
                environment["VINC_BUILD_INTERPRETER"] = sys.executable
                invocation = command
                if os.name != "nt":
                    invocation = [str(ROOT / "build.py")] + command[3:]

                if run:
                    launcher = [str(ROOT / "build.bat")] if os.name == "nt" else ["sh", str(ROOT / "build.sh")]
                    invocation = launcher + command[3:]

                result = subprocess.run(invocation, cwd=root, env=environment,
                                        capture_output=True, timeout=120)
                self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode(errors="replace"))
                if run:
                    self.assertIn(b"no work to do", result.stdout)

            self.assertEqual((binary / "label.txt").read_text(), "space value;literal")
            program = binary / ("Camera.exe" if os.name == "nt" else "Camera")
            execution = subprocess.run([str(program)], capture_output=True, timeout=10)
            self.assertEqual(execution.returncode, 0, execution.stderr)
            self.assertEqual(execution.stdout, b"driver-linked")
            cache = (binary / "CMakeCache.txt").read_bytes()
            result = subprocess.run(command + ["--config", "Release"], cwd=root, capture_output=True, timeout=60)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"Incompatible CMAKE_BUILD_TYPE", result.stderr)
            self.assertEqual((binary / "CMakeCache.txt").read_bytes(), cache)


if __name__ == "__main__":
    unittest.main()
