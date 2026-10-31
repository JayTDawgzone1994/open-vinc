# Deploy Tcl resources for tests that construct Shells. Native Windows console
# tests are separate from portable redirected-input and module integration tests.

add_executable(shell_unit_tests unit_tests.cpp)
target_link_libraries(shell_unit_tests PRIVATE vinc_shell vinc_test_manager)

add_executable(shell_terminal_tests terminal_tests.cpp)
target_link_libraries(shell_terminal_tests PRIVATE vinc::osal)

vinc_add_module_plugin(shell_process_helper helper.cpp)
target_link_libraries(shell_process_helper PRIVATE vinc::module)

add_executable(shell_integration_tests integration_tests.cpp)
target_link_libraries(shell_integration_tests PRIVATE vinc_shell vinc_test_manager)
vinc_deploy_tcl(shell_unit_tests)
vinc_deploy_tcl(shell_integration_tests)
add_dependencies(shell_integration_tests shell_process_helper)

add_executable(shell_socket_tests socket_shell_tests.cpp)
target_link_libraries(shell_socket_tests PRIVATE vinc_shell vinc_test_manager)
add_dependencies(shell_socket_tests shell_process_helper)
vinc_deploy_tcl(shell_socket_tests)
add_test(NAME shell.socket COMMAND shell_socket_tests "$<TARGET_FILE:shell_process_helper>")
set_tests_properties(shell.socket PROPERTIES TIMEOUT 40)

add_executable(shell_tcl_tests tcl_tests.cpp)
target_link_libraries(shell_tcl_tests PRIVATE vinc_shell vinc_test_manager)
add_dependencies(shell_tcl_tests shell_process_helper)
vinc_deploy_tcl(shell_tcl_tests)
if(MSVC)
    target_compile_options(shell_tcl_tests PRIVATE /W4 /utf-8)
endif()
add_test(NAME shell.tcl COMMAND shell_tcl_tests "$<TARGET_FILE:shell_process_helper>")
set_tests_properties(shell.tcl PROPERTIES TIMEOUT 40)

foreach(target IN ITEMS
    shell_unit_tests
    shell_terminal_tests
    shell_process_helper
    shell_integration_tests
    shell_socket_tests
)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

add_test(NAME shell.unit COMMAND shell_unit_tests)
add_test(NAME shell.terminal COMMAND shell_terminal_tests)
add_test(NAME shell.integration COMMAND shell_integration_tests "$<TARGET_FILE:shell_process_helper>")
set_tests_properties(shell.unit shell.terminal PROPERTIES TIMEOUT 15)
set_tests_properties(shell.integration PROPERTIES TIMEOUT 45)

if(WIN32 AND VINC_BUILD_APPS)
    add_executable(shell_console_tests console_tests.cpp)
    target_link_libraries(shell_console_tests PRIVATE vinc_shell vinc_test_manager)
    vinc_deploy_tcl(shell_console_tests)
    add_dependencies(shell_console_tests shell_process_helper)
    if(MSVC)
        target_compile_options(shell_console_tests PRIVATE /W4 /utf-8)
    else()
        target_compile_options(shell_console_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_dependencies(shell_console_tests shell)
    add_test(NAME shell.console COMMAND shell_console_tests "$<TARGET_FILE:shell>")
    set_tests_properties(shell.console PROPERTIES TIMEOUT 20)
endif()

# Compile private Shell implementations with deterministic I/O fault hooks.
# No production Shell object is linked into this fixture.
add_executable(shell_fault_tests fault_tests.cpp
    ../src/command_line.cpp ../src/command_router.cpp
    ../src/script_service.cpp ../src/tcl_runtime.cpp)
target_include_directories(shell_fault_tests PRIVATE ../include "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(shell_fault_tests PRIVATE VINC_SHELL_TESTING)
target_link_libraries(shell_fault_tests PRIVATE vinc_test_manager vinc::common vinc_tcl)
vinc_deploy_tcl(shell_fault_tests)
add_dependencies(shell_fault_tests shell_process_helper)
if(MSVC)
    target_compile_options(shell_fault_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(shell_fault_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME shell.faults COMMAND shell_fault_tests "$<TARGET_FILE:shell_process_helper>")
set_tests_properties(shell.faults PROPERTIES TIMEOUT 30)
