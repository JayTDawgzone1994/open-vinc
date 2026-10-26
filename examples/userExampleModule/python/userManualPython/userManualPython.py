"""Manual scheduling without a separate pump thread."""
import time
import vinc

APPLICATION_IDLE_SECONDS = 0.01


class Module(vinc.BaseModule, mode=vinc.RunMode.Manual):
    def on_start(self, args):
        self.use_loop = "loop" in args
        self.finished = False

    def main(self):
        if self.use_loop:
            self.context.pumpLoop()
            return
        while self.context.living() and not self.finished:
            batch = self.context.pumpOnce()
            if batch.stopped:
                break
            if batch.callbacks or batch.commands or batch.lost_messages:
                print(f"PUMP callbacks={batch.callbacks} commands={batch.commands} "
                      f"lost={batch.lost_messages}")
            # Put bounded application work here; avoid busy polling an empty queue.
            time.sleep(APPLICATION_IDLE_SECONDS)
        # Returning None means normal completion; no automatic restart is requested.

    def module_interface(self, args):
        if args == ["finish"] and not self.use_loop:
            self.finished = True
        else:
            print(f"MANUAL {self.get_module_name()} living={self.context.living()}")
        return 0
