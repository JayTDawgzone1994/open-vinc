# Both fixtures compile the production entry. Only the catalog/identity and
# embedded script differ; no alternate test main can hide a dispatch regression.
vinc_add_module_plugin(app_entry_plugin entry_module.cpp)
target_link_libraries(app_entry_plugin PRIVATE vinc::osal)
vinc_add_module_plugin(app_startup_failure_plugin startup_failure_module.cpp)
if(TARGET vinc_ui_errors)
    target_link_libraries(app_startup_failure_plugin PRIVATE vinc_ui_errors)
    target_compile_definitions(app_startup_failure_plugin PRIVATE VINC_STARTUP_UI_ERRORS)
endif()

set(entry_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
file(GENERATE OUTPUT "${entry_generated}/module_catalog.hpp" CONTENT [=[
#pragma once
#include <vinc/module/detail/manager_access.hpp>
#include <cstdlib>
namespace vinc {
inline Result<void> register_built_modules(ModuleManager& manager, const std::filesystem::path&) {
    if (std::getenv("VINC_TEST_OFFICIAL_FAILURE")) {
        vinc::detail::SystemModuleOptions options;
        options.name = "ui";
        auto registered = vinc::detail::ManagerAccess::register_system_module(manager,
            {"webUi", std::filesystem::path(u8"$<TARGET_FILE:app_startup_failure_plugin>")}, options);
        if (!registered) {
            return registered;
        }
    }
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
    target_include_directories(${target} PRIVATE "${entry_generated}" "${CMAKE_BINARY_DIR}/vinc-config")
    target_link_libraries(${target} PRIVATE vinc::shell vinc::module_runtime)
    if(TARGET vinc_ui_errors)
        target_link_libraries(${target} PRIVATE vinc_ui_errors)
        target_compile_definitions(${target} PRIVATE VINC_STARTUP_UI_ERRORS)
    endif()
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/entry space 測試/$<CONFIG>")
    add_dependencies(${target} app_entry_plugin app_startup_failure_plugin)
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

# Compile the production entry/catalog renderer against the real official Plugins.
# Test-only executable wiring avoids adding an installable App to user packages.
# A distinct identity isolates persistence and admission from the user's App.
if(TARGET vinc_module_ui AND TARGET vinc_bag_recorder)
    include("${VINC_REPOSITORY_ROOT}/src/vinc/common/cmake/VincPythonRequirements.cmake")
    vinc_find_python(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        set(system_generated "${CMAKE_CURRENT_BINARY_DIR}/system-services/$<CONFIG>")
        vinc_render_module_catalog(system_catalog vinc_module_ui vinc_bag_recorder)
        file(GENERATE OUTPUT "${system_generated}/module_catalog.hpp" CONTENT
            "#pragma once\n#include <vinc/module/detail/manager_access.hpp>\nnamespace vinc {\ninline Result<void> register_built_modules(ModuleManager& manager, const std::filesystem::path&) {\n${system_catalog}return {};\n}\n}\n")
        file(GENERATE OUTPUT "${system_generated}/vinc_app_config.hpp" CONTENT
            "#pragma once\n#include <string_view>\nnamespace vinc::app { inline constexpr std::string_view instance_name = \"system-services-test\"; }\n")
        add_executable(system_services_app ../src/main.cpp)
        target_include_directories(system_services_app PRIVATE "${CMAKE_BINARY_DIR}/vinc-config")
        target_compile_definitions(system_services_app PRIVATE VINC_COMPOSED_APP VINC_STARTUP_UI_ERRORS)
        target_include_directories(system_services_app PRIVATE "${system_generated}")
        target_link_libraries(system_services_app PRIVATE vinc::shell vinc::module_runtime vinc_ui_errors)
        set_target_properties(system_services_app PROPERTIES
            CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/system_services_app/$<CONFIG>")
        add_dependencies(system_services_app vinc_module_ui vinc_bag_recorder)
        vinc_deploy_tcl(system_services_app)
        vinc_deploy_licenses(system_services_app)
        add_test(NAME app.system_services COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/system_services.py" "$<TARGET_FILE:system_services_app>")
        set_tests_properties(app.system_services PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    endif()
endif()
