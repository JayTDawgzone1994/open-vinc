import vinc
import time

STOP_WAIT_SECONDS = 0.01


class Module(vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop):
    def on_start(self, args):
        self.mode = args[0] if args else "normal"
        if self.mode == "start-error":
            raise ValueError("python start failure fixture")
        print(f"PYTHON_READY {self.get_module_name()} {self.get_mid()}")

    def module_interface(self, args):
        if args == ["status"]:
            return 42
        if args == ["caught"]:
            try:
                self.context.pumpOnce()
            except vinc.VincError as error:
                assert error.error.domain == 3
                assert error.error.code == 18
                assert error.error.operation == 52
                assert "busy" in str(error).lower() or "outstanding" in str(error).lower()
                return 17
        if args == ["crash"]:
            raise RuntimeError("python command failure fixture")
        if args == ["escape-vinc"]:
            self.context.pumpOnce()
        if args == ["block"]:
            while True:
                pass
        return 1

    def module_autoComplete(self, args):
        return ["status", "caught", "crash", "block"]

    def on_stop(self):
        if self.mode == "stop-error":
            raise ValueError("python stop failure fixture")
        if self.mode == "stop-block":
            while True:
                time.sleep(STOP_WAIT_SECONDS)
        print("PYTHON_STOPPED")

    def __del__(self):
        # Native teardown revokes Context before releasing this Python object.
        try:
            self.context.get_mid()
        except vinc.VincError:
            print("PYTHON_CONTEXT_REVOKED")
