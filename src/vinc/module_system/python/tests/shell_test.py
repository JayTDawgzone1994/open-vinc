"""Exercise the installed layout, discovery, interpreter choice and real Shell."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

TIMEOUT_SECONDS = 30


def use_default_windows_error_mode():
    if os.name != "nt":
        return

    import ctypes
    from ctypes import wintypes

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.SetErrorMode.argtypes = [wintypes.UINT]
    kernel.SetErrorMode.restype = wintypes.UINT
    # The App inherits mode 0 so the malformed interpreter fixture exercises
    # OSAL's production launch scope, not dialog suppression by the test runner.
    kernel.SetErrorMode(0)


def run(executable, script, cwd, python=None, environment=None):
    command = [str(executable), "--shell-port", "0"]
    if python is not None:
        command += ["--python", str(python)]
    command += ["-S", str(script)]
    try:
        result = subprocess.run(command, cwd=cwd, env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired as error:
        captured = (error.output or b"").decode("utf-8", errors="replace")
        raise AssertionError("Central did not finish within the test deadline:\n" + captured) from error
    text = result.stdout.decode("utf-8", errors="replace")
    if result.returncode != 0:
        raise AssertionError(text)
    assert "CENTRAL_STILL_ALIVE" in text, text
    return text


def main():
    use_default_windows_error_mode()
    original = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="vinc-python-部署-") as temporary:
        root = Path(temporary)
        executable = root / original.name
        shutil.copy2(original, executable)
        for library in original.parent.glob("*.dll"):
            shutil.copy2(library, root / library.name)
        shutil.copytree(original.parent / "lib", root / "lib")
        shutil.copytree(original.parent / "python" / "vinc", root / "python" / "vinc")
        modules = root / "data" / "module"
        target = modules / "framework"
        shutil.copytree(fixture / "framework", target)
        (modules / "missingEntry").mkdir()
        (modules / "invalid.name").mkdir()
        # A nonrecursive scanner must not discover this inner directory.
        nested = target / "nested"
        nested.mkdir()
        (nested / "nested.py").write_text("raise RuntimeError('must not run')", encoding="utf-8")
        script = root / "smoke.tcl"
        commands = ["framework --name front", "front status", "front caught", "front invalid",
                    "framework --name second", "second status"]
        # Tcl normally stops on a nonzero command. These scenarios deliberately
        # exercise failures, so catch them and always request central shutdown.
        script.write_text("".join("catch {" + command + "} result; puts $result\n"
                                  for command in commands) + "puts CENTRAL_STILL_ALIVE\nexit\n", encoding="utf-8")
        unrelated = root / "unrelated"
        unrelated.mkdir()
        output = run(executable, script, unrelated, sys.executable)
        assert "PYTHON_READY front" in output, output
        assert "PYTHON_READY second" in output, output
        assert "PYTHON_STOPPED" in output, output
        assert "PYTHON_CONTEXT_REVOKED" in output, output
        assert "missingEntry" in output and "invalid.name" in output, output
        # Default means PATH, not the Python selected when VINC was built.
        environment = dict(os.environ)
        environment["PATH"] = str(Path(sys.executable).parent) + os.pathsep + environment.get("PATH", "")
        output = run(executable, script, unrelated, environment=environment)
        assert "PYTHON_READY front" in output, output
        candidate_name = "python.exe" if os.name == "nt" else "python3"
        first = root / "first candidate"
        first.mkdir()
        candidate = first / candidate_name
        candidate.mkdir()
        search_environment = dict(environment)
        search_environment["PATH"] = str(first) + os.pathsep + environment["PATH"]
        output = run(executable, script, unrelated, environment=search_environment)
        assert "PYTHON_READY front" in output, output
        candidate.rmdir()
        if os.name != "nt":
            candidate.write_text("not executable", encoding="utf-8")
            candidate.chmod(0o600)
            output = run(executable, script, unrelated, candidate, search_environment)
            assert "PYTHON_READY" not in output, output
            output = run(executable, script, unrelated, environment=search_environment)
            assert "PYTHON_READY front" in output, output
            candidate.unlink()
            candidate.symlink_to(first / "missing-target")
            output = run(executable, script, unrelated, environment=search_environment)
            assert "PYTHON_READY front" in output, output
            candidate.unlink()
        # Once selected, an invalid executable must fail instead of falling back
        # to the compatible interpreter which is still later in PATH.
        candidate.write_bytes(b"not an executable image\n")
        candidate.chmod(0o700)
        output = run(executable, script, unrelated, environment=search_environment)
        assert "PYTHON_READY" not in output, output
        assert "All module processes exited" in output, output
        candidate.unlink()
        # Empty PATH segments do not mean cwd; an explicit '.' still does.
        cwd_candidate = unrelated / candidate_name
        cwd_candidate.write_bytes(b"not an executable image\n")
        cwd_candidate.chmod(0o700)
        empty_environment = dict(environment, PATH=os.pathsep + environment["PATH"] + os.pathsep)
        output = run(executable, script, unrelated, environment=empty_environment)
        assert "PYTHON_READY front" in output, output
        relative_environment = dict(environment, PATH="." + os.pathsep + environment["PATH"])
        output = run(executable, script, unrelated, environment=relative_environment)
        assert "PYTHON_READY" not in output, output
        cwd_candidate.unlink()
        # Preserve the venv spelling: resolving its symlink loses site-packages.
        venv = root / "中文 venv"
        subprocess.run([sys.executable, "-m", "venv", "--without-pip", str(venv)],
                       check=True, timeout=TIMEOUT_SECONDS)
        venv_bin = venv / ("Scripts" if os.name == "nt" else "bin")
        module_source = target / "framework.py"
        original_source = module_source.read_text(encoding="utf-8")
        module_source.write_text("import sys\nprint('SELECTED_PREFIX=' + sys.prefix)\n" + original_source,
                                 encoding="utf-8")
        venv_environment = dict(environment, PATH=str(venv_bin) + os.pathsep + environment["PATH"])
        output = run(executable, script, unrelated, environment=venv_environment)
        assert "SELECTED_PREFIX=" + str(venv) in output, output
        assert "PYTHON_READY front" in output, output
        module_source.write_text(original_source, encoding="utf-8")
        # Version mismatch must reject startup while the central stays operable.
        metadata = root / "python" / "vinc" / "_build.py"
        metadata.write_text(metadata.read_text(encoding="utf-8") + "\nVERSION = (3, 0)\n",
                            encoding="utf-8")
        output = run(executable, script, unrelated, sys.executable)
        assert "VINC requires CPython" in output, output
        assert "PYTHON_READY" not in output, output
        output = run(executable, script, unrelated, environment=environment)
        assert "VINC requires CPython" in output, output
        assert "PYTHON_READY" not in output, output
        # Runtime import failure has a traceback and remains a Module failure.
        shutil.copy2(original.parent / "python/vinc/_build.py", root / "python/vinc/_build.py")
        (target / "framework.py").write_text(
            "raise RuntimeError('EXPECTED_IMPORT_TRACEBACK')", encoding="utf-8")
        output = run(executable, script, unrelated, sys.executable)
        assert "EXPECTED_IMPORT_TRACEBACK" in output, output
        print("Python Shell discovery and deployment passed")


if __name__ == "__main__":
    main()
