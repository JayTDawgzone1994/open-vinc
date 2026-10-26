"""Exercise the installed layout, discovery, interpreter choice and real Shell."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

TIMEOUT_SECONDS = 30


def run(executable, script, cwd, python=None, environment=None):
    command = [str(executable), "--shell-port", "0"]
    if python is not None:
        command += ["--python", str(python)]
    command += ["-S", str(script)]
    result = subprocess.run(command, cwd=cwd, env=environment, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=TIMEOUT_SECONDS)
    text = result.stdout.decode("utf-8", errors="replace")
    if result.returncode != 0:
        raise AssertionError(text)
    return text


def main():
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
                                  for command in commands) + "exit\n", encoding="utf-8")
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
        # Version mismatch must reject startup while the central stays operable.
        (root / "python" / "vinc" / "_build.py").write_text(
            "VERSION = (3, 0)\nPOINTER_BYTES = 8\n", encoding="utf-8")
        output = run(executable, script, unrelated, sys.executable)
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
