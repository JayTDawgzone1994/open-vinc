"""Exercise generated bindings without starting a framework session."""
import gc
import copy
import math
import operator
import sys

sys.path.insert(0, sys.argv[1])
import vinc
from vinc._native import _test_messages as fixtures
from vinc.param import Type
from vinc.messages import int3

assert Type.Double is vinc.param.Type.Double
assert int3 is vinc.messages.int3


def raises(kind, action):
    try:
        action()
    except kind:
        return
    raise AssertionError(f"expected {kind.__name__}")


def check_arrays():
    message = vinc.make_message(fixtures.Frame)
    data = message.data()
    data.sequence = 2**64 - 1
    data.flags.assign([True, False, True])
    assert list(data.flags) == [True, False, True]
    assert data.flags[-1] is True
    raises(IndexError, lambda: data.flags[3])
    raises(IndexError, lambda: data.flags[-4])
    raises(IndexError, lambda: data.flags[2**100])
    raises(TypeError, lambda: data.flags.set(0, 1))
    raises(TypeError, lambda: data.flags[1.0])
    raises(OverflowError, lambda: data.flags.resize(-1))
    data.payload.assign(bytes([0, 127, 255]))
    raises(OverflowError, lambda: data.payload.set(0, 256))
    raises(TypeError, lambda: data.payload.set(0, True))
    raises(OverflowError, lambda: data.payload.assign([1, 256]))
    assert list(data.payload) == [0, 127, 255]
    data.payload.reserve(64)
    data.payload.push_back(42)
    assert data.payload.capacity >= 64
    data.payload.pop_back()
    data.payload.shrink_to_fit()
    data.groups.resize(2)
    data.groups[0].resize(1)
    detection = data.groups[0][0]
    detection.id = 42
    detection.position.x = 1.25
    detection.position.copy = 2.5
    independent_point = copy.copy(detection.position)
    independent_point.copy = 3.5
    assert detection.position.copy == 2.5
    detection.label.assign("中文\x00測試")
    assert detection.label.decode() == "中文\x00測試"
    detection.label.assign(b"\xff\x00")
    assert detection.label.to_bytes() == b"\xff\x00"
    raises(UnicodeDecodeError, detection.label.decode)
    data.detections.assign([detection])
    detection.id = 43
    assert data.detections[0].id == 42
    independent = data.detections.at(0)
    independent.id = 100
    assert data.detections[0].id == 42
    labels = data.labels
    labels.assign([b"one", "two"])
    assert labels[1].decode() == "two"
    removed = data.groups[0]
    data.groups.clear()
    raises(ReferenceError, lambda: len(removed))
    raises(ReferenceError, lambda: detection.id)
    data.groups.resize(2)
    raises(ReferenceError, lambda: len(removed))
    duplicate = message.copy()
    data.detections[0].id = 20
    assert duplicate.data().detections[0].id == 42
    encoded = message.serialize()
    restored = vinc.make_message(fixtures.Frame)
    restored.deserialize(encoded)
    assert restored.data().sequence == 2**64 - 1
    assert restored.data().detections[0].label.to_bytes() == b"\xff\x00"
    raises(vinc.VincError, lambda: restored.deserialize(encoded[:-1]))
    assert restored.data().detections[0].id == 20
    assert message.usage().limit_bytes == 0
    assert message.usage().allocated_bytes >= message.usage().compact_bytes
    message.compact()
    # Retaining a field keeps a local message alive, with no borrowed raw pointer.
    del message, data
    gc.collect()
    assert labels[0].decode() == "one"


def check_scalars():
    message = vinc.make_message(fixtures.Types)
    data = message.data()
    for name, bits, signed in (("a", 8, True), ("b", 8, False), ("c", 16, True),
                               ("d", 16, False), ("e", 32, True), ("f", 32, False),
                               ("g", 64, True), ("h", 64, False)):
        maximum = 2 ** (bits - int(signed)) - 1
        minimum = -(2 ** (bits - 1)) if signed else 0
        for value in (minimum, maximum):
            setattr(data, name, value)
            assert getattr(data, name) == value
        raises(OverflowError, lambda: setattr(data, name, maximum + 1))
        raises(OverflowError, lambda: setattr(data, name, minimum - 1))
    raises(TypeError, lambda: setattr(data, "yes", 1))
    data.precise = 1.1234567890123
    assert data.precise == 1.1234567890123
    assert fixtures.Types.constants["minimum"] == -(2**63)
    assert math.isnan(fixtures.Types.constants["not_a_number"])
    assert fixtures.Types.constants["greeting"] == b"hello // world"
    assert fixtures.Types.constants["embedded_zero"] == b"a\0b"
    raises(TypeError, lambda: operator.setitem(fixtures.Types.constants, "minimum", 0))
    value = vinc.make_message(vinc.messages.test)
    value.data().y.assign([1, 2, 3])
    assert list(value.data().y) == [1, 2, 3]
    fixture = vinc.make_message(fixtures.Frame)
    fixture.data().signed_values.assign(value.data().y)
    assert list(fixture.data().signed_values) == [1, 2, 3]
    assert value.publisher_instance() == 0
    assert value.get_topic_count() == 0


check_arrays()
check_scalars()
print("Python message bindings passed")
