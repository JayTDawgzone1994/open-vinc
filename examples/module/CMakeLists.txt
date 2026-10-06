# Register the minimal Plugin for independent execution by the framework ModuleHost.

vinc_add_process_module(vinc_module_example
    MODULE_TYPE_NAME exampleModule MODULE_BASE_NAME exampleNode SOURCES example_module.cpp)
