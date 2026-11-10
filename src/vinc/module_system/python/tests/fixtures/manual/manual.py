import threading
import time
import vinc

LIVING_POLL_SECONDS = 0.01
WAIT_TEST_TIMEOUT_SECONDS = 3.0
MODULE_ERROR_DOMAIN = 3
INVALID_ARGUMENT = 1
BUSY = 18
WAIT_FOR_WORK = 71


class Module(vinc.BaseModule, mode=vinc.RunMode.Manual):
    def on_start(self, args):
        self.mode = args[0] if args else "finish"
        self.worker = None

    def main(self):
        if self.mode == "finish":
            return
        if self.mode == "main-error":
            raise RuntimeError("python main failure fixture")
        if self.mode == "wait":
            self.verify_wait()
            while self.context.living():
                batch = self.context.pumpOnce()
                if batch.callbacks == 0 and batch.commands == 0:
                    self.context.waitForWork()
            return
        self.worker = threading.Thread(target=self.context.pumpLoop)
        self.worker.start()
        while self.context.living():
            time.sleep(LIVING_POLL_SECONDS)

    def module_interface(self, args):
        if self.mode == "wait":
            try:
                self.context.waitForWork()
            except vinc.VincError as error:
                assert error.error.domain == MODULE_ERROR_DOMAIN
                assert error.error.code == BUSY
                assert error.error.operation == WAIT_FOR_WORK
            else:
                raise AssertionError("a pump command must not enter a nested wait")
        return 24

    def verify_wait(self):
        self.context.waitForWork(timeout=0)
        self.context.waitForWork(timeout=0.001)
        self.context.waitForWork(timeout=None)
        for invalid in (-1, float("nan"), float("inf"), float("-inf"), 1e300):
            try:
                self.context.waitForWork(timeout=invalid)
            except vinc.VincError as error:
                assert error.error.domain == MODULE_ERROR_DOMAIN
                assert error.error.code == INVALID_ARGUMENT
                assert error.error.operation == WAIT_FOR_WORK
            else:
                raise AssertionError("invalid timeout accepted")

        observed_busy = threading.Event()
        stop = threading.Event()
        failures = []

        def contender():
            try:
                while not stop.is_set():
                    try:
                        self.context.waitForWork(timeout=0)
                    except vinc.VincError as error:
                        assert error.error.code == BUSY
                        assert error.error.operation == WAIT_FOR_WORK
                        # Python ran while the other caller held native wait admission.
                        observed_busy.set()
                    time.sleep(0)
            except BaseException as error:
                failures.append(error)

        worker = threading.Thread(target=contender)
        worker.start()
        try:
            deadline = time.monotonic() + WAIT_TEST_TIMEOUT_SECONDS
            while not observed_busy.is_set() and time.monotonic() < deadline:
                try:
                    self.context.waitForWork()
                except vinc.VincError as error:
                    assert error.error.code == BUSY
            assert observed_busy.is_set(), "wait must release GIL while holding admission"
        finally:
            stop.set()
            worker.join(WAIT_TEST_TIMEOUT_SECONDS)
        assert not worker.is_alive()
        assert not failures, failures

    def on_stop(self):
        if self.worker is not None:
            self.worker.join()
