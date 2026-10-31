import sys
from pathlib import Path

sys.path.insert(0, str(Path(sys.argv[1]) / "python"))
import vinc

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
