import vinc


class Module(vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop,
             pump_options=vinc.PumpOptions(topic_workers=33)):
    pass
