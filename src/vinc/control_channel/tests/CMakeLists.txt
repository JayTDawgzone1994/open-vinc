add_executable(control_channel_tests channel_tests.cpp)
target_link_libraries(control_channel_tests PRIVATE vinc::control)
if(MSVC)
    target_compile_options(control_channel_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(control_channel_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME control.channel COMMAND control_channel_tests)
set_tests_properties(control.channel PROPERTIES TIMEOUT 90)

# Compile this component's sources with test-only I/O gates. Do not link the
# production control library as well, or export the hook define/include path.
add_executable(control_channel_fault_tests
    channel_fault_tests.cpp ../src/channel.cpp ../src/protocol.cpp)
target_include_directories(control_channel_fault_tests PRIVATE ../include "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(control_channel_fault_tests PRIVATE VINC_CONTROL_CHANNEL_TESTING)
target_link_libraries(control_channel_fault_tests PRIVATE vinc::osal)
if(MSVC)
    target_compile_options(control_channel_fault_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(control_channel_fault_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME control.faults COMMAND control_channel_fault_tests)
set_tests_properties(control.faults PROPERTIES TIMEOUT 30)
