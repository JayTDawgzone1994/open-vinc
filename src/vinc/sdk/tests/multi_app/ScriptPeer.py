import vinc


class Module(vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop):
    def on_start(self, args):
        self.message = vinc.make_message(vinc.messages.SelectionFrame)
        self.message.data().value = 42

    def module_interface(self, args):
        if self.message.data().value != 42:
            return 1
        print("PYTHON_MODULE_OK", flush=True)
        return 0
