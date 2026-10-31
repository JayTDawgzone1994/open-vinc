# Both fixtures compile the production entry. Only the catalog/identity and
# embedded script differ; no alternate test main can hide a dispatch regression.
vinc_add_module_plugin(app_entry_plugin entry_module.cpp)
target_link_libraries(app_entry_plugin PRIVATE vinc::osal)

set(entry_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
file(GENERATE OUTPUT "${entry_generated}/module_catalog.hpp" CONTENT [=[
#pragma once
#include <vinc/module/host/module_manager.hpp>
namespace vinc {
inline Result<void> register_built_modules(ModuleManager& manager, const std::filesystem::path&) {
    return manager.register_module_type({"EntryModule", std::filesystem::path(u8"$<TARGET_FILE:app_entry_plugin>")});
}
}
]=])
file(GENERATE OUTPUT "${entry_generated}/vinc_app_config.hpp" CONTENT
    "#pragma once\n#include <string_view>\nnamespace vinc::app { inline constexpr std::string_view instance_name = \"entry-test\"; }\n")

foreach(mode plain embedded)
    set(target "app_entry_${mode}")
    add_executable(${target} ../src/main.cpp)
    target_compile_definitions(${target} PRIVATE VINC_COMPOSED_APP)
    target_include_directories(${target} PRIVATE "${entry_generated}")
    target_link_libraries(${target} PRIVATE vinc::shell vinc::module_runtime)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/entry space 測試/$<CONFIG>")
    add_dependencies(${target} app_entry_plugin)
    vinc_deploy_tcl(${target})
    vinc_deploy_licenses(${target})
    set(ENTRY_FILENAME "${target}${CMAKE_EXECUTABLE_SUFFIX}")
    configure_file(entry_startup.tcl.in "${CMAKE_CURRENT_BINARY_DIR}/${mode}.tcl" @ONLY)
endforeach()
vinc_embed_startup(app_entry_embedded "${CMAKE_CURRENT_BINARY_DIR}/embedded.tcl")

file(GENERATE OUTPUT "${entry_generated}/test.cmake" CONTENT
    "set(plain [==[$<TARGET_FILE:app_entry_plain>]==])\nset(embedded [==[$<TARGET_FILE:app_entry_embedded>]==])\nset(script [==[${CMAKE_CURRENT_BINARY_DIR}/plain.tcl]==])\nset(work [==[${CMAKE_CURRENT_BINARY_DIR}/entry-work/$<CONFIG>]==])\n")
add_test(NAME app.entry COMMAND "${CMAKE_COMMAND}" "-DMANIFEST=${entry_generated}/test.cmake"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/entry.cmake")
set_tests_properties(app.entry PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

# Reuse the behavioral suites against the actual production main's Host branch.
# Dedicated injection Hosts inside individual suites remain explicit overrides.
function(add_app_host_test name driver)
    add_dependencies(${driver} app_entry_plain)
    add_test(NAME "app.${name}" COMMAND "${CMAKE_COMMAND}" -E env
        "VINC_TEST_HOST_PATH=$<TARGET_FILE:app_entry_plain>"
        "$<TARGET_FILE:${driver}>" ${ARGN})
    set_tests_properties("app.${name}" PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endfunction()

add_app_host_test(process module_integration_tests "$<TARGET_FILE:module_process_helper>"
    "$<TARGET_FILE:silent_module_helper>" "$<TARGET_FILE:plugin_stall>")
add_app_host_test(context module_context_tests "$<TARGET_FILE:module_context_helper>")
add_app_host_test(contract module_contract_tests "$<TARGET_FILE:module_process_helper>")
add_app_host_test(debug module_debug_tests "$<TARGET_FILE:module_process_helper>")
add_app_host_test(plugin module_plugin_tests "$<TARGET_FILE:plugin_valid>"
    "$<TARGET_FILE:plugin_no_entry>" "$<TARGET_FILE:plugin_bad_version>"
    "$<TARGET_FILE:plugin_null_factory>" "$<TARGET_FILE:plugin_reject>"
    "$<TARGET_FILE:plugin_stall>" "$<TARGET_FILE:plugin_throw_constructor>")
add_app_host_test(message_bus message_bus_module_tests "$<TARGET_FILE:message_bus_module_helper>")
add_app_host_test(param param_process_tests "$<TARGET_FILE:param_module_helper>")
add_app_host_test(log log_process_tests "$<TARGET_FILE:log_module_helper>")
add_app_host_test(rpc rpc_process_tests "$<TARGET_FILE:rpc_module_fixture>")
