import vinc
from parallel_common import Behavior


class Module(Behavior, vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop,
             pump_options=vinc.PumpOptions(topic_workers=2)):
    # FrameworkLoop does not dispatch Behavior.main(); native main owns the loop.
    pass
