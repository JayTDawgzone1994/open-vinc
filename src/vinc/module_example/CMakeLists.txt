# Register the minimal example as its own process executable and central launch entry.

vinc_add_process_module(vinc_module_example
    MODULE_TYPE_NAME exampleModule MODULE_BASE_NAME exampleNode SOURCES example_module.cpp)
