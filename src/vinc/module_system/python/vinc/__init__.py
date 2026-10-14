"""VINC Python Module SDK. Importing this package creates no framework session."""
from enum import Enum
import sys
from . import _build

if sys.platform == "win32" and _build.MSVC_RUNTIME:
    from ._windows_runtime import prepare as _prepare_runtime
    _prepare_runtime()
    del _prepare_runtime

from ._native import Error, ErrorRecord, NativeError, PumpStats, error_to_string, format_utc
from ._native import messages, message_bus, param, log, rpc

# Public package aliases for the statically bound extension submodules.
for _name in ("messages", "message_bus", "param", "log", "rpc"):
    sys.modules[f"{__name__}.{_name}"] = globals()[_name]
del _name


def make_message(message_type):
    """Create an independently owned local message of a compiled Ribosome type."""
    if not isinstance(message_type, type) or not hasattr(message_type, "_advertise"):
        raise TypeError("expected a compiled VINC message type")
    return message_type()


class VincError(Exception):
    """A failed VINC operation, including the original structured cause chain."""

    def __init__(self, error):
        self.error = error
        super().__init__(error_to_string(error))


class RunMode(Enum):
    FrameworkLoop = 0
    Manual = 1


class BaseModule:
    """Subclass with an explicit mode; VINC alone attaches the native Context."""

    def __init_subclass__(cls, *, mode, **kwargs):
        super().__init_subclass__(**kwargs)
        if not isinstance(mode, RunMode):
            raise TypeError("mode must be vinc.RunMode")
        cls._vinc_mode = mode
        if mode is RunMode.FrameworkLoop and "main" in cls.__dict__:
            raise TypeError("FrameworkLoop owns main(); use Manual to implement it")

    @property
    def context(self):
        return self._vinc_context

    def get_mid(self):
        return self.context.get_mid()

    def get_module_name(self):
        return self.context.get_module_name()

    def on_name(self, args):
        # args is mutable; consume only our explicit naming option.
        if "--name" not in args:
            raise ValueError("Python Modules require --name <instance>; no automatic suffix")
        index = args.index("--name")
        if index + 1 >= len(args):
            raise ValueError("--name requires an instance name")
        name = args[index + 1]
        del args[index:index + 2]
        return name

    def on_start(self, args):
        pass

    def main(self):
        raise NotImplementedError("Manual Modules must implement main()")

    def on_stop(self):
        pass

    def module_interface(self, args):
        print("No commands implemented")
        return 1

    def module_autoComplete(self, args):
        return []
