import threading
import time
import vinc

LIVING_POLL_SECONDS = 0.01


class Module(vinc.BaseModule, mode=vinc.RunMode.Manual):
    def on_start(self, args):
        self.mode = args[0] if args else "finish"
        self.worker = None

    def main(self):
        if self.mode == "finish":
            return
        if self.mode == "main-error":
            raise RuntimeError("python main failure fixture")
        self.worker = threading.Thread(target=self.context.pumpLoop)
        self.worker.start()
        while self.context.living():
            time.sleep(LIVING_POLL_SECONDS)

    def module_interface(self, args):
        return 24

    def on_stop(self):
        if self.worker is not None:
            self.worker.join()
