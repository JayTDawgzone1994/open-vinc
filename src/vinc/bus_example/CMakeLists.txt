# Build a process module demonstrating the public Bus API with generated messages.

vinc_add_process_module(vinc_bus_example
    MODULE_TYPE_NAME busExample MODULE_BASE_NAME busNode SOURCES bus_example.cpp)
target_link_libraries(vinc_bus_example PRIVATE vinc_messages)
