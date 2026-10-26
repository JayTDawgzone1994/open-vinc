add_executable(output_hub_tests output_hub_tests.cpp)
target_link_libraries(output_hub_tests PRIVATE vinc::output_hub)
add_dependencies(output_hub_tests osal_process_helper)
add_test(NAME output_hub.integration COMMAND output_hub_tests "$<TARGET_FILE:osal_process_helper>")
set_tests_properties(output_hub.integration PROPERTIES TIMEOUT 40)

# All fixture translation units use the same test-only class definition.
# Do not link the production OutputHub object into this executable.
add_executable(output_hub_fault_tests fault_tests.cpp ../src/text_renderer.cpp)
target_include_directories(output_hub_fault_tests PRIVATE ../include "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(output_hub_fault_tests PRIVATE VINC_OUTPUT_HUB_TESTING)
target_link_libraries(output_hub_fault_tests PRIVATE vinc::osal vinc::common)
if(MSVC)
    target_compile_options(output_hub_fault_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(output_hub_fault_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME output_hub.faults COMMAND output_hub_fault_tests)
set_tests_properties(output_hub.faults PROPERTIES TIMEOUT 20)
