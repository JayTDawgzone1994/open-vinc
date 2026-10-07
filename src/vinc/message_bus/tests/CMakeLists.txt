# Use private ring buffer tests for synchronization edges and process tests for BusManager cleanup.

add_executable(message_bus_error_protocol_tests error_protocol_tests.cpp)
target_link_libraries(message_bus_error_protocol_tests PRIVATE vinc::message_bus)
add_test(NAME message_bus.error_protocol COMMAND message_bus_error_protocol_tests)
set_tests_properties(message_bus.error_protocol PROPERTIES TIMEOUT 15)

add_executable(message_bus_error_process_tests error_process_tests.cpp)
target_link_libraries(message_bus_error_process_tests PRIVATE vinc::message_bus vinc_message_fixture)
add_test(NAME message_bus.error_process
    COMMAND message_bus_error_process_tests "$<TARGET_FILE:message_bus_error_process_tests>")
set_tests_properties(message_bus.error_process PROPERTIES TIMEOUT 45)

add_executable(message_bus_ring_buffer_tests ring_buffer_tests.cpp)
target_link_libraries(message_bus_ring_buffer_tests PRIVATE vinc::message_bus)
add_test(NAME message_bus.ring_buffer COMMAND message_bus_ring_buffer_tests "$<TARGET_FILE:message_bus_ring_buffer_tests>")
set_tests_properties(message_bus.ring_buffer PROPERTIES TIMEOUT 30)
vinc_add_module_plugin(message_bus_module_helper module_helper.cpp)
target_link_libraries(message_bus_module_helper PRIVATE vinc::module vinc_message_fixture)
add_executable(message_bus_module_tests module_tests.cpp)
target_link_libraries(message_bus_module_tests PRIVATE vinc::module_manager)
add_dependencies(message_bus_module_tests message_bus_module_helper)
add_test(NAME message_bus.module COMMAND message_bus_module_tests "$<TARGET_FILE:message_bus_module_helper>")
set_tests_properties(message_bus.module PROPERTIES TIMEOUT 120)

# Compile the same implementation with local fault seams; never link the
# production Bus library into this target or export its test-only definition.
add_executable(message_bus_cleanup_tests
    cleanup_tests.cpp ../src/message_bus.cpp ../src/runtime.cpp ../src/bus_manager.cpp ../src/ring_buffer.cpp)
target_include_directories(message_bus_cleanup_tests PRIVATE ../include "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(message_bus_cleanup_tests PRIVATE VINC_MESSAGE_BUS_TESTING)
target_link_libraries(message_bus_cleanup_tests PRIVATE
    vinc::common vinc::message_storage vinc::control vinc::osal vinc_message_fixture)
if(MSVC)
    target_compile_options(message_bus_cleanup_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(message_bus_cleanup_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME message_bus.cleanup COMMAND message_bus_cleanup_tests)
set_tests_properties(message_bus.cleanup PROPERTIES TIMEOUT 90)
