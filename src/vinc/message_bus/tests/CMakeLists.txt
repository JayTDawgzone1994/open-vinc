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
target_link_libraries(message_bus_module_tests PRIVATE vinc_test_manager)
add_dependencies(message_bus_module_tests message_bus_module_helper)
add_test(NAME message_bus.module COMMAND message_bus_module_tests "$<TARGET_FILE:message_bus_module_helper>")
set_tests_properties(message_bus.module PROPERTIES TIMEOUT 120)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../common/cmake/VincPythonRequirements.cmake")
vinc_find_python(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_executable(message_bus_recovery_app recovery_app.cpp)
    target_link_libraries(message_bus_recovery_app PRIVATE vinc_test_manager)
    add_dependencies(message_bus_recovery_app message_bus_module_helper)
    add_test(NAME message_bus.app_restart_isolation
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/app_restart_isolation.py"
            "$<TARGET_FILE:message_bus_recovery_app>" "$<TARGET_FILE:message_bus_module_helper>")
    set_tests_properties(message_bus.app_restart_isolation PROPERTIES TIMEOUT 100)
    add_test(NAME message_bus.topic_limit_param
        COMMAND "${Python3_EXECUTABLE}" -E -s -B "${CMAKE_CURRENT_SOURCE_DIR}/topic_limit_param.py"
            "$<TARGET_FILE:message_bus_recovery_app>" "$<TARGET_FILE:message_bus_module_helper>")
    set_tests_properties(message_bus.topic_limit_param PROPERTIES TIMEOUT 100)
    add_test(NAME message_bus.release_routes
        COMMAND "${Python3_EXECUTABLE}" -E -s -B "${CMAKE_CURRENT_SOURCE_DIR}/topic_limit_param.py" --routes
            "$<TARGET_FILE:message_bus_recovery_app>" "$<TARGET_FILE:message_bus_module_helper>")
    set_tests_properties(message_bus.release_routes PROPERTIES TIMEOUT 100)
endif()

if(UNIX)
    add_executable(message_bus_topic_registration_tests topic_registration_tests.cpp)
    target_link_libraries(message_bus_topic_registration_tests PRIVATE vinc_test_manager)
    add_dependencies(message_bus_topic_registration_tests message_bus_module_helper)
    add_test(NAME message_bus.topic_registration_emfile
        COMMAND message_bus_topic_registration_tests "$<TARGET_FILE:message_bus_module_helper>")
    set_tests_properties(message_bus.topic_registration_emfile PROPERTIES TIMEOUT 60)
endif()

add_executable(message_bus_defaults_process_tests defaults_process_tests.cpp)
target_link_libraries(message_bus_defaults_process_tests PRIVATE vinc_test_manager vinc::param_manager)
add_dependencies(message_bus_defaults_process_tests message_bus_module_helper)
add_test(NAME message_bus.defaults_process
    COMMAND message_bus_defaults_process_tests "$<TARGET_FILE:message_bus_module_helper>")
set_tests_properties(message_bus.defaults_process PROPERTIES TIMEOUT 90)

# Compile the same implementation with local fault seams; never link the
# production Bus library into this target or export its test-only definition.
add_executable(message_bus_cleanup_tests
    cleanup_tests.cpp notification_snapshot_tests.cpp
    ../src/message_bus.cpp ../src/runtime.cpp ../src/bus_manager.cpp ../src/ring_buffer.cpp)
target_include_directories(message_bus_cleanup_tests PRIVATE ../include "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(message_bus_cleanup_tests PRIVATE VINC_MESSAGE_BUS_TESTING)
target_link_libraries(message_bus_cleanup_tests PRIVATE
    vinc::common vinc::message_storage vinc::control vinc::osal vinc_message_fixture)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Only the test Bus worker that owns the fixture reservation arms these calls.
    target_link_options(message_bus_cleanup_tests PRIVATE
        "LINKER:--wrap=ftruncate" "LINKER:--wrap=shm_unlink" "LINKER:--wrap=mmap")
endif()
if(MSVC)
    target_compile_options(message_bus_cleanup_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(message_bus_cleanup_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME message_bus.cleanup COMMAND message_bus_cleanup_tests)
set_tests_properties(message_bus.cleanup PROPERTIES TIMEOUT 90)
add_test(NAME message_bus.counter_registration COMMAND message_bus_cleanup_tests counter-registration)
set_tests_properties(message_bus.counter_registration PROPERTIES TIMEOUT 30)
add_test(NAME message_bus.notification_snapshots COMMAND message_bus_cleanup_tests notification-snapshots)
set_tests_properties(message_bus.notification_snapshots PROPERTIES TIMEOUT 30)
