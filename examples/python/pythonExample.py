"""Copy to <Vinc directory>/data/module/pythonExample/pythonExample.py."""
import vinc


class Module(vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop):
    def on_start(self, args):
        self.gain = 1
        print(f"Started {self.get_module_name()}, MID={self.get_mid()}")

    def module_interface(self, args):
        if args == ["status"]:
            print(f"gain = {self.gain}")
            return 0

        if len(args) != 2 or args[0] != "gain":
            print("Usage: status | gain <integer>")
            return 1

        try:
            self.gain = int(args[1])
        except ValueError:
            print("gain must be an integer")
            return 1

        return 0

    def module_autoComplete(self, args):
        return ["status", "gain"]

    def on_stop(self):
        print(f"Stopped {self.get_module_name()}")
