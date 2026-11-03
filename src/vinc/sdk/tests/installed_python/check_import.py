import sys
import ctypes
import hashlib
import json
import os
from pathlib import Path

# Optional native fixture deliberately recreates the old-DLL-first deployment.
old_runtime = None
if sys.platform == "win32" and os.environ.get("VINC_TEST_OLD_MSVC_RUNTIME"):
    old_runtime = ctypes.WinDLL(os.environ["VINC_TEST_OLD_MSVC_RUNTIME"])

sys.path.insert(0, str(Path(sys.argv[1]) / "python"))
import vinc

if sys.platform == "win32":
    package = Path(vinc.__file__).parent
    metadata = json.loads((package / "_msvc_runtime.json").read_text())
    assert vinc._build.MSVC_RUNTIME_MODE == "static"
    assert metadata["mode"] == "static" and metadata["required"] == {}
    native = package / metadata["native"]["filename"]
    with native.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == metadata["native"]["sha256"]
    if old_runtime is not None:
        from vinc._windows_runtime import _Windows
        expected = Path(os.environ["VINC_TEST_OLD_MSVC_RUNTIME"]).resolve()
        assert any(path.resolve() == expected for path in _Windows().loaded())

# C++ schema constants are not exposed as Python class attributes. The native
# peer's compiled revision and schema compatibility are checked by the Bus flow.
assert hasattr(vinc.messages, "AddedPacket") == (sys.argv[3] == "yes")
assert not hasattr(vinc, "_test_messages")
message = vinc.make_message(vinc.messages.SdkPacket)
message.data().value = 123
restored = vinc.make_message(vinc.messages.SdkPacket)
restored.deserialize(message.serialize())
assert restored.data().value == 123
print("INSTALLED_IMPORT_OK", flush=True)
