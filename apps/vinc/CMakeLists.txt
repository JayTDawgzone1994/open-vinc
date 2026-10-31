# Assemble the central executable and its catalog after examples register.
add_executable(shell src/main.cpp)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/VincApp.cmake")
if(BUILD_TESTING)
    add_test(NAME app.embedding COMMAND "${CMAKE_COMMAND}"
        "-DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/embedding-test"
        "-DEMBED_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedScript.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/embedding.cmake")
    set_tests_properties(app.embedding PROPERTIES TIMEOUT 30)
    add_subdirectory(tests)
endif()
set_target_properties(shell PROPERTIES OUTPUT_NAME Vinc)
vinc_deploy_licenses(shell)
vinc_deploy_tcl(shell)
add_custom_command(TARGET shell POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:shell>/licenses/tomlplusplus"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${VINC_REPOSITORY_ROOT}/library/tomlplusplus/upstream/LICENSE"
        "$<TARGET_FILE_DIR:shell>/licenses/tomlplusplus/LICENSE"
    VERBATIM)
target_include_directories(shell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
target_link_libraries(shell PRIVATE vinc::shell vinc::module_runtime)
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

if(BUILD_TESTING AND VINC_APP_BAG)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bag-smoke.tcl"
        "recorder status\nexit\n")
    add_test(NAME bag.shell COMMAND shell --shell-port 0 -S "${CMAKE_CURRENT_BINARY_DIR}/bag-smoke.tcl")
    set_tests_properties(bag.shell PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "bag: idle")
endif()

# Finalize in the outer project, after external add_module calls have returned.
set_property(GLOBAL PROPERTY VINC_CENTRAL_CATALOG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
foreach(name VINC_BUILD_ROOT VINC_REPOSITORY_ROOT VINC_NAME VINC_APP_UI VINC_APP_PYTHON)
    set_property(GLOBAL PROPERTY "VINC_SOURCE_APP_${name}" "${${name}}")
endforeach()
function(vinc_finalize_module_catalog)
    foreach(name VINC_BUILD_ROOT VINC_REPOSITORY_ROOT VINC_NAME VINC_APP_UI VINC_APP_PYTHON)
        get_property(${name} GLOBAL PROPERTY "VINC_SOURCE_APP_${name}")
    endforeach()
    get_property(catalog_directory GLOBAL PROPERTY VINC_CENTRAL_CATALOG_DIR)
    get_property(PROCESS_TARGETS GLOBAL PROPERTY VINC_PROCESS_TARGETS)
    vinc_render_module_catalog(PROCESS_CATALOG ${PROCESS_TARGETS})
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
    set(manifest "set(root [==[${CMAKE_BINARY_DIR}]==])\nset(identity [==[${VINC_NAME}]==])\nset(module_files)\n")
    foreach(target vinc_bag_recorder vinc_module_ui)
        if(TARGET ${target})
            string(APPEND manifest "list(APPEND module_files [==[$<TARGET_FILE:${target}>]==])\n")
        endif()
    endforeach()
    get_property(ui_dist GLOBAL PROPERTY VINC_UI_DIST)
    set(python_resources)
    if(TARGET vinc_python_native)
        get_target_property(python_resources vinc_python_native VINC_PYTHON_RESOURCES)
    endif()
    string(APPEND manifest "set(python_resources [==[${python_resources}]==])\n")
    string(APPEND manifest "set(with_ui ${VINC_APP_UI})\nset(ui_dist [==[${ui_dist}]==])\nset(ui_root [==[${VINC_BUILD_ROOT}/vinc_data/${VINC_NAME}/ui]==])\nset(with_python ${VINC_APP_PYTHON})\nset(python_source [==[${VINC_BUILD_ROOT}/python/$<CONFIG>/vinc]==])\nset(python_root [==[$<TARGET_FILE_DIR:shell>/python/vinc]==])\n")
    set(manifest_path "${catalog_directory}/$<CONFIG>/owned.cmake")
    file(GENERATE OUTPUT "${manifest_path}" CONTENT "${manifest}")
    add_custom_target(vinc_source_app_resources
        COMMAND "${CMAKE_COMMAND}" "-DMANIFEST=${manifest_path}"
            -P "${VINC_REPOSITORY_ROOT}/src/vinc/sdk/cmake/TrackSourceApplication.cmake"
        DEPENDS ${PROCESS_TARGETS} VERBATIM)
    if(TARGET shell_python_deployment)
        add_dependencies(vinc_source_app_resources shell_python_deployment)
    endif()
    add_dependencies(shell vinc_source_app_resources)
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
