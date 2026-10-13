# Assemble the central executable and its catalog after examples register.
add_executable(shell src/main.cpp)
set_target_properties(shell PROPERTIES OUTPUT_NAME Vinc)
vinc_deploy_tcl(shell)
add_custom_command(TARGET shell POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:shell>/licenses/tomlplusplus"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${VINC_REPOSITORY_ROOT}/library/tomlplusplus/upstream/LICENSE"
        "$<TARGET_FILE_DIR:shell>/licenses/tomlplusplus/LICENSE"
    VERBATIM)
target_include_directories(shell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
target_link_libraries(shell PRIVATE vinc::shell)
set_target_properties(shell PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${VINC_BUILD_ROOT}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${VINC_BUILD_ROOT}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${VINC_BUILD_ROOT}"
)

if(BUILD_TESTING AND VINC_BUILD_EXAMPLES)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/module-smoke.tcl"
        "exampleModule\nexampleNode hello\nexampleModule --name secondaryNode\nsecondaryNode status\nexit\n")
    add_test(NAME module.shell COMMAND shell --shell-port 0 -S "${CMAKE_CURRENT_BINARY_DIR}/module-smoke.tcl")
    set_tests_properties(module.shell PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "secondaryNode status")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/message_bus-smoke.tcl"
        "messageBusExample --name messageBusSubscriber sub demo\nmessageBusExample --name messageBusPublisher pub demo\nmessageBusPublisher push 10\nmessageBusSubscriber last\nmessageBusSubscriber last\nmessageBusSubscriber last\nmessageBusPublisher loan 20\nmessageBusSubscriber last\nmessageBusPublisher commit\nmessageBusSubscriber last\nmessageBusPublisher stats\nexit\n")
    add_test(NAME message_bus.shell COMMAND shell --shell-port 0 -S "${CMAKE_CURRENT_BINARY_DIR}/message_bus-smoke.tcl")
    set_tests_properties(message_bus.shell PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "value=20")
endif()

if(BUILD_TESTING)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bag-smoke.tcl"
        "bagRecorder\nrecorder status\nexit\n")
    add_test(NAME bag.shell COMMAND shell --shell-port 0 -S "${CMAKE_CURRENT_BINARY_DIR}/bag-smoke.tcl")
    set_tests_properties(bag.shell PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "bag: idle")
endif()

# Finalize in the outer project, after external add_module calls have returned.
set_property(GLOBAL PROPERTY VINC_CENTRAL_CATALOG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
function(vinc_finalize_module_catalog)
    get_property(catalog_directory GLOBAL PROPERTY VINC_CENTRAL_CATALOG_DIR)
    get_property(PROCESS_TARGETS GLOBAL PROPERTY VINC_PROCESS_TARGETS)
    get_property(PROCESS_CATALOG GLOBAL PROPERTY VINC_PROCESS_CATALOG)
    if(PROCESS_TARGETS)
        add_dependencies(shell ${PROCESS_TARGETS})
    endif()

    # Generate after collecting all process modules. Generator expressions resolve
    # each Plugin library path for this build configuration.
    set(catalog_template [=[
#pragma once

// Generated from the registered process-module targets. Edit the CMake sources instead.
#include <vinc/module/host/module_manager.hpp>
#include <vinc/config.hpp>


namespace vinc {
inline Result<void> register_built_modules(ModuleManager& manager) {
@PROCESS_CATALOG@    return {};
}
} // namespace vinc
    ]=])
    string(CONFIGURE "${catalog_template}" catalog_contents @ONLY)
    file(GENERATE
        OUTPUT "${catalog_directory}/$<CONFIG>/module_catalog.hpp"
        CONTENT "${catalog_contents}"
    )
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL vinc_finalize_module_catalog)

if(MSVC)
    target_compile_options(shell PRIVATE /W4 /utf-8)
else()
    target_compile_options(shell PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(VINC_BUILD_PYTHON)
    vinc_deploy_python(shell)
    if(VINC_BUILD_EXAMPLES)
        # Explicit deployment avoids overwriting user-edited scripts on every build.
        add_custom_target(user_examples_python
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${VINC_REPOSITORY_ROOT}/examples/userExampleModule/python"
                "$<TARGET_FILE_DIR:shell>/data/module"
            DEPENDS shell VERBATIM)
        if(BUILD_TESTING)
            get_property(python_example_executable GLOBAL PROPERTY VINC_PYTHON_TEST_EXECUTABLE)
            add_test(NAME examples.user_module COMMAND "${python_example_executable}"
                "${VINC_REPOSITORY_ROOT}/examples/userExampleModule/tests/smoke.py"
                "$<TARGET_FILE:shell>" "${VINC_REPOSITORY_ROOT}/examples/userExampleModule")
            set_tests_properties(examples.user_module PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
        endif()
    endif()
    if(BUILD_TESTING)
        get_property(python_test_executable GLOBAL PROPERTY VINC_PYTHON_TEST_EXECUTABLE)
        add_test(NAME module.python_shell COMMAND "${python_test_executable}"
            "${VINC_REPOSITORY_ROOT}/src/vinc/module_system/python/tests/shell_test.py"
            "$<TARGET_FILE:shell>"
            "${VINC_REPOSITORY_ROOT}/src/vinc/module_system/python/tests/fixtures")
        set_tests_properties(module.python_shell PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endif()
endif()
