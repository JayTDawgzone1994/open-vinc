import os
import vinc
from helpers import greeting

# Published Param error identities; reject permission failures specifically.
PARAM_ERROR_DOMAIN = 10
PARAM_NOT_FOUND = 4
PARAM_PERMISSION_DENIED = 6


class Module(vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop):
    def on_start(self, args):
        print(greeting(), flush=True)
        print(f"PYTHON_PID={os.getpid()}", flush=True)
        self.revision = int(args[0])
        self.received = False
        self.rpc_received = False
        self.changed = False
        self.publisher = self.context.advertise(vinc.messages.SdkPacket, "sdk/request")
        assert self.publisher.stats().requested_slots == 4
        assert self.publisher.stats().max_message_bytes == 8192
        options = vinc.message_bus.PublisherOptions()
        assert options.slots is None and options.max_message_bytes is None
        options.max_message_bytes = 4096
        partial = self.context.advertise(vinc.messages.SdkPacket, "sdk/partial", options)
        assert partial.stats().requested_slots == 4
        assert partial.stats().max_message_bytes == 4096
        partial.close()
        print("INSTALLED_PYTHON_PUBLISHER_DEFAULTS_OK", flush=True)
        self.subscriber = self.context.subscribe(vinc.messages.SdkPacket, "sdk/reply", self.receive)
        self.context.param.register_param("gain", vinc.param.Type.Int32, 7,
                                         restart_hint=vinc.param.RestartHint.App)
        self.context.param.register_param("plain", vinc.param.Type.Int32, 1)
        plain = self.context.param.read_current("plain")
        assert plain.permission == vinc.param.Permission.Limited
        assert plain.restart_hint == vinc.param.RestartHint.None_
        # No pump has run: current reads neither subscribe nor refresh the cache.
        self.context.param.set("gain", vinc.param.Type.Int32, 7)
        initial = self.context.param.read_current("gain")
        assert initial.value == 7
        assert initial.default_value == 7
        assert initial.permission == vinc.param.Permission.Limited
        assert initial.restart_hint == vinc.param.RestartHint.App
        watches_before = len(self.context.param.watches())
        native = self.context.param.read_current("native.fixed")
        assert native.value == 7
        assert native.restart_hint == vinc.param.RestartHint.Module
        assert len(self.context.param.watches()) == watches_before
        try:
            self.context.param.update("gain", vinc.param.Type.Int32)
        except vinc.VincError as error:
            assert error.error.domain == PARAM_ERROR_DOMAIN
            assert error.error.code == PARAM_NOT_FOUND
        else:
            raise AssertionError("read_current must not populate the pump cache")
        try:
            self.context.param.set("native.fixed", vinc.param.Type.Int32, 9)
        except vinc.VincError as error:
            assert error.error.domain == PARAM_ERROR_DOMAIN
            assert error.error.code == PARAM_PERMISSION_DENIED
        else:
            raise AssertionError("foreign Limited writes must be rejected")
        self.context.param.set("native.shared", vinc.param.Type.Int32, 9)
        shared = self.context.param.read_current("native.shared")
        assert shared.value == 9
        assert shared.permission == vinc.param.Permission.Public
        assert shared.restart_hint == vinc.param.RestartHint.None_
        self.context.param.set("gain", vinc.param.Type.Int32, 8)
        assert self.context.param.read_current("gain").value == 8
        assert initial.value == 7
        print("PYTHON_PARAM_CURRENT_OK", flush=True)
        self.context.param.on_change(self.changed_param)
        self.context.log.info("INSTALLED_PYTHON_LOG")

    def changed_param(self, snapshot):
        assert snapshot["py.gain"].restart_hint == vinc.param.RestartHint.App
        self.changed = snapshot["py.gain"].value == 8

    def receive(self, message):
        assert message.publisher_name() == "native"
        assert message.data().value == self.revision + 1
        self.received = True

    def completed(self, reply, error):
        assert error is None, error
        status, response = reply
        assert status.code == 0
        assert response.data().value == self.revision + 2
        self.rpc_received = True

    def module_interface(self, args):
        if args[-1] == "send":
            message = vinc.make_message(vinc.messages.SdkPacket)
            message.data().value = self.revision
            self.publisher.push(message)
            self.context.call_rpc(vinc.messages.SdkPacket, vinc.messages.SdkPacket,
                                  "native", "echo", message, self.completed)
            return 0
        if self.received and self.rpc_received and self.changed:
            assert self.context.param.update("gain", vinc.param.Type.Int32) == 8
            print("PYTHON_PARAM_RESTART_OK", flush=True)
            print(f"PYTHON_FLOW_REVISION={self.revision}", flush=True)
            return 0
        return 1
