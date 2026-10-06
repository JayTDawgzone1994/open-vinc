# Build a process module demonstrating the public Message Bus API with generated messages.

vinc_add_process_module(vinc_message_bus_example
    MODULE_TYPE_NAME messageBusExample MODULE_BASE_NAME messageBusNode SOURCES message_bus_example.cpp)
target_link_libraries(vinc_message_bus_example PRIVATE vinc_messages)
