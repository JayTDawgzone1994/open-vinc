# Isolated tests need a Host without central composition. Keep its path and
# build dependency on this test-only interface, never on the production Manager.
if(MSVC)
    # Explicitly built by runtime-policy validation; ordinary builds do not pay
    # for the additional static-CRT closure during the stage-one rollout.
    add_executable(module_python_runtime_link_tests EXCLUDE_FROM_ALL python_runtime_link.cpp)
    vinc_apply_runtime(module_python_runtime_link_tests PYTHON)
    include("${CMAKE_CURRENT_SOURCE_DIR}/../python/cmake/VincPythonRuntime.cmake")
    vinc_python_runtime_targets(python_runtime python_errors)
    target_link_libraries(module_python_runtime_link_tests PRIVATE
        ${python_runtime} ${python_errors})
    set_target_properties(module_python_runtime_link_tests PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
add_executable(vinc_test_host host_main.cpp)
target_link_libraries(vinc_test_host PRIVATE vinc::module_runtime)
if(MSVC)
    get_filename_component(compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
    # This opt-in validation target builds both link closures, inspects actual
    # imports and runs the static bootstrap probe without altering default builds.
    add_custom_target(vinc_check_python_runtime
        COMMAND "${CMAKE_COMMAND}"
            "-DDUMPBIN=${compiler_directory}/dumpbin.exe"
            "-DSTATIC_IMAGE=$<TARGET_FILE:module_python_runtime_link_tests>"
            "-DNATIVE_IMAGE=$<TARGET_FILE:vinc_test_host>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/python_runtime_link.cmake"
        DEPENDS module_python_runtime_link_tests vinc_test_host VERBATIM)
endif()
set_target_properties(vinc_test_host PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
set(test_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
file(GENERATE OUTPUT "${test_generated}/vinc_test/host_path.hpp"
    CONTENT "#pragma once\n#define VINC_TEST_HOST_PATH u8\"$<TARGET_FILE:vinc_test_host>\"\n")
add_library(vinc_test_manager INTERFACE)
target_link_libraries(vinc_test_manager INTERFACE vinc::module_manager)
target_include_directories(vinc_test_manager INTERFACE "${test_generated}")
add_dependencies(vinc_test_manager vinc_test_host)

# Real Manager/Socket code with bounded observation gates. The regression now
# requires queued work to wake supervision; historical pre-fix traces are retained.
add_executable(module_control_wakeup_host control_wakeup_host.cpp control_wakeup_channel.cpp)
target_link_libraries(module_control_wakeup_host PRIVATE vinc::module_runtime)
add_executable(module_control_wakeup_tests control_wakeup_tests.cpp control_wakeup_channel.cpp)
target_link_libraries(module_control_wakeup_tests PRIVATE
    vinc_test_manager vinc::concurrency vinc::rpc_manager)
add_dependencies(module_control_wakeup_tests module_control_wakeup_host message_bus_module_helper)
foreach(target IN ITEMS module_control_wakeup_host module_control_wakeup_tests)
    target_compile_definitions(${target} PRIVATE VINC_CONTROL_WAKEUP_TESTING)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
add_test(NAME module.control_wakeup COMMAND module_control_wakeup_tests
    "$<TARGET_FILE:module_control_wakeup_host>" "$<TARGET_FILE:message_bus_module_helper>")
set_tests_properties(module.control_wakeup PROPERTIES TIMEOUT 60)

# Exercise the same scheduler with two real control peers and the real bounded
# RPC admission queue. Only the test parks its downstream worker.
add_executable(module_control_scheduler_host control_scheduler_host.cpp)
target_link_libraries(module_control_scheduler_host PRIVATE vinc_test_manager vinc::control vinc::rpc)
add_executable(module_control_scheduler_tests control_scheduler_tests.cpp control_wakeup_channel.cpp)
target_link_libraries(module_control_scheduler_tests PRIVATE vinc_test_manager vinc::concurrency vinc::rpc_manager)
target_compile_definitions(module_control_scheduler_tests PRIVATE VINC_CONTROL_WAKEUP_TESTING)
target_include_directories(module_control_scheduler_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(module_control_scheduler_tests module_control_scheduler_host)
foreach(target IN ITEMS module_control_scheduler_tests module_control_scheduler_host)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
add_test(NAME module.control_scheduler COMMAND module_control_scheduler_tests "$<TARGET_FILE:module_control_scheduler_host>")
set_tests_properties(module.control_scheduler PROPERTIES TIMEOUT 60)

# Optional matched scheduling comparison; no benchmark is added to normal CTest.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../common/cmake/VincPythonRequirements.cmake")
vinc_find_python(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    vinc_add_module_plugin(module_control_cost_plugin control_cost/module.cpp)
    set_target_properties(module_control_cost_plugin PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_link_libraries(module_control_cost_plugin PRIVATE vinc_message_fixture)
    set(cost_baseline "${CMAKE_CURRENT_BINARY_DIR}/control_cost/polling_manager.cpp")
    add_custom_command(OUTPUT "${cost_baseline}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/control_cost/baseline.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/host/module_manager.cpp" "${cost_baseline}"
        DEPENDS control_cost/baseline.py control_cost/polling_monitor.inc ../src/host/module_manager.cpp
        VERBATIM)
    foreach(policy IN ITEMS notified polling)
        add_executable(module_control_cost_${policy} EXCLUDE_FROM_ALL
            control_cost/cost.cpp control_wakeup_channel.cpp)
        if(policy STREQUAL "polling")
            target_sources(module_control_cost_${policy} PRIVATE "${cost_baseline}")
            set_source_files_properties("${cost_baseline}" PROPERTIES HEADER_FILE_ONLY TRUE)
            set(cost_source "${cost_baseline}")
        else()
            set(cost_source "${CMAKE_CURRENT_SOURCE_DIR}/../src/host/module_manager.cpp")
        endif()
        target_compile_definitions(module_control_cost_${policy} PRIVATE
            VINC_CONTROL_WAKEUP_TESTING "VINC_CONTROL_COST_SOURCE=\"${cost_source}\"")
        target_include_directories(module_control_cost_${policy} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(module_control_cost_${policy} PRIVATE vinc_test_manager vinc::concurrency vinc::rpc_manager)
        if(WIN32)
            target_link_libraries(module_control_cost_${policy} PRIVATE Psapi)
        endif()
        add_dependencies(module_control_cost_${policy} module_control_cost_plugin module_control_wakeup_host)
    endforeach()
endif()

add_executable(module_query_tests query_tests.cpp)
target_link_libraries(module_query_tests PRIVATE vinc::module_runtime vinc::errors)
add_test(NAME module.queries COMMAND module_query_tests)
set_tests_properties(module.queries PROPERTIES TIMEOUT 25)
add_executable(module_lock_protocol_tests lock_protocol_tests.cpp)
target_link_libraries(module_lock_protocol_tests PRIVATE vinc::module_runtime vinc::errors)
add_test(NAME module.lock_protocol COMMAND module_lock_protocol_tests)
set_tests_properties(module.lock_protocol PROPERTIES TIMEOUT 30)
add_executable(module_instance_tests instance_tests.cpp)
target_link_libraries(module_instance_tests PRIVATE vinc_test_manager)
add_test(NAME module.instance COMMAND module_instance_tests)
set_tests_properties(module.instance PROPERTIES TIMEOUT 45)
add_executable(module_publisher_defaults_tests publisher_defaults_tests.cpp)
target_link_libraries(module_publisher_defaults_tests PRIVATE
    vinc::module_manager vinc::param_manager vinc::concurrency vinc::errors)
add_test(NAME module.publisher_defaults COMMAND module_publisher_defaults_tests)
set_tests_properties(module.publisher_defaults PROPERTIES TIMEOUT 45)
# Build real child fixtures, including a silent peer, for session/lifecycle failure tests.
vinc_add_module_plugin(module_lock_helper lock_helper.cpp)
add_executable(module_lock_tests lock_tests.cpp)
target_link_libraries(module_lock_tests PRIVATE vinc_test_manager vinc::concurrency)
add_dependencies(module_lock_tests module_lock_helper)
add_test(NAME module.locks COMMAND module_lock_tests "$<TARGET_FILE:module_lock_helper>")
set_tests_properties(module.locks PROPERTIES TIMEOUT 60)
add_executable(module_mid_tests mid_tests.cpp)
target_link_libraries(module_mid_tests PRIVATE vinc::module)
add_test(NAME module.mid COMMAND module_mid_tests)
set_tests_properties(module.mid PROPERTIES TIMEOUT 10)

vinc_add_module_plugin(module_context_helper context_helper.cpp)
target_link_libraries(module_context_helper PRIVATE vinc_message_fixture)
add_executable(module_context_tests context_tests.cpp)
target_link_libraries(module_context_tests PRIVATE vinc_test_manager)
add_dependencies(module_context_tests module_context_helper)
add_test(NAME module.context COMMAND module_context_tests "$<TARGET_FILE:module_context_helper>")
set_tests_properties(module.context PROPERTIES TIMEOUT 90)

# This disposable Host compiles the real Runtime/Bus with diagnostic gates.
# Controlled gates verify receiver lifetime without installing a test Host.
add_executable(module_shutdown_host shutdown_host.cpp shutdown_bus.cpp)
target_link_libraries(module_shutdown_host PRIVATE vinc::module_runtime)
target_compile_definitions(module_shutdown_host PRIVATE VINC_SHUTDOWN_TESTING)
target_include_directories(module_shutdown_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(module_shutdown_tests shutdown_tests.cpp)
target_link_libraries(module_shutdown_tests PRIVATE vinc_test_manager vinc::module)
add_dependencies(module_shutdown_tests module_shutdown_host module_context_helper)
foreach(target IN ITEMS module_shutdown_host module_shutdown_tests)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
add_test(NAME module.shutdown_evidence COMMAND module_shutdown_tests
    "$<TARGET_FILE:module_shutdown_host>" "$<TARGET_FILE:module_context_helper>")
set_tests_properties(module.shutdown_evidence PROPERTIES TIMEOUT 45)

vinc_add_module_plugin(module_process_helper module_helper.cpp)
target_link_libraries(module_process_helper PRIVATE vinc::module)
add_executable(module_integration_tests module_tests.cpp)
target_link_libraries(module_integration_tests PRIVATE vinc_test_manager)
add_dependencies(module_integration_tests module_process_helper)
add_executable(silent_module_helper silent_helper.cpp)
target_link_libraries(silent_module_helper PRIVATE vinc::control vinc::module)
add_dependencies(module_integration_tests silent_module_helper)
add_executable(module_error_protocol_tests error_protocol_tests.cpp)
target_link_libraries(module_error_protocol_tests PRIVATE vinc::module_manager vinc::errors)
foreach(target IN ITEMS module_process_helper silent_module_helper module_integration_tests module_error_protocol_tests)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
add_test(NAME module.process COMMAND module_integration_tests "$<TARGET_FILE:module_process_helper>"
    "$<TARGET_FILE:silent_module_helper>" "$<TARGET_FILE:plugin_stall>")
set_tests_properties(module.process PROPERTIES TIMEOUT 90)
add_test(NAME module.error_protocol COMMAND module_error_protocol_tests)
set_tests_properties(module.error_protocol PROPERTIES TIMEOUT 10)

add_executable(module_system_tests system_module_tests.cpp)
target_link_libraries(module_system_tests PRIVATE vinc_test_manager)
add_dependencies(module_system_tests module_process_helper module_context_helper)
add_test(NAME module.system COMMAND module_system_tests
    "$<TARGET_FILE:module_process_helper>" "$<TARGET_FILE:module_context_helper>")
set_tests_properties(module.system PROPERTIES TIMEOUT 60)

# Include the component implementation in isolated fixtures to inspect private
# lifetime invariants without exposing test APIs in the production libraries.
add_executable(module_manager_fault_tests manager_fault_tests.cpp)
target_link_libraries(module_manager_fault_tests PRIVATE vinc_test_manager vinc::concurrency vinc::rpc_manager)
add_dependencies(module_manager_fault_tests module_process_helper)
add_executable(module_runtime_fault_tests runtime_fault_tests.cpp)
target_link_libraries(module_runtime_fault_tests PRIVATE vinc::module_runtime)
add_executable(module_context_wait_tests context_wait_tests.cpp)
target_link_libraries(module_context_wait_tests PRIVATE vinc::module_runtime)
foreach(target IN ITEMS module_manager_fault_tests module_runtime_fault_tests module_context_wait_tests)
    target_compile_definitions(${target} PRIVATE VINC_MODULE_TESTING)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
add_test(NAME module.manager_faults COMMAND module_manager_fault_tests "$<TARGET_FILE:module_process_helper>")
set_tests_properties(module.manager_faults PROPERTIES TIMEOUT 45)
add_test(NAME module.runtime_faults COMMAND module_runtime_fault_tests)
set_tests_properties(module.runtime_faults PROPERTIES TIMEOUT 15)
add_test(NAME module.context_wait COMMAND module_context_wait_tests)
set_tests_properties(module.context_wait PROPERTIES TIMEOUT 20)

add_executable(module_topic_scheduler_tests topic_scheduler_tests.cpp)
target_compile_definitions(module_topic_scheduler_tests PRIVATE VINC_MODULE_TESTING)
target_link_libraries(module_topic_scheduler_tests PRIVATE vinc::module vinc::errors)
add_test(NAME module.topic_scheduler COMMAND module_topic_scheduler_tests)
set_tests_properties(module.topic_scheduler PROPERTIES TIMEOUT 25)
vinc_add_module_plugin(module_parallel_helper parallel_helper.cpp)
target_link_libraries(module_parallel_helper PRIVATE vinc_message_fixture)
add_executable(module_parallel_tests parallel_tests.cpp)
target_link_libraries(module_parallel_tests PRIVATE vinc_test_manager)
add_dependencies(module_parallel_tests module_parallel_helper)
add_test(NAME module.parallel COMMAND module_parallel_tests "$<TARGET_FILE:module_parallel_helper>")
set_tests_properties(module.parallel PROPERTIES TIMEOUT 35)
vinc_add_module_plugin(module_parallel_stop_helper parallel_stop_helper.cpp)
target_link_libraries(module_parallel_stop_helper PRIVATE vinc_message_fixture)
add_executable(module_parallel_stop_tests parallel_stop_tests.cpp)
target_link_libraries(module_parallel_stop_tests PRIVATE vinc_test_manager)
add_dependencies(module_parallel_stop_tests module_parallel_stop_helper)
add_test(NAME module.parallel_stop COMMAND module_parallel_stop_tests "$<TARGET_FILE:module_parallel_stop_helper>")
set_tests_properties(module.parallel_stop PROPERTIES TIMEOUT 65)

add_executable(module_contract_tests contract_tests.cpp)
target_link_libraries(module_contract_tests PRIVATE vinc_test_manager vinc_module_contract)
add_dependencies(module_contract_tests module_process_helper)
if(MSVC)
    target_compile_options(module_contract_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(module_contract_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME module.contract COMMAND module_contract_tests "$<TARGET_FILE:module_process_helper>")
set_tests_properties(module.contract PROPERTIES TIMEOUT 60)

# Isolated copies provide checkpoints; production has no test hook or adapter.
add_executable(module_contract_fault_helper contract_fault_helper.cpp)
target_link_libraries(module_contract_fault_helper PRIVATE vinc::module_runtime)
target_compile_definitions(module_contract_fault_helper PRIVATE VINC_FAULT_HOST)
vinc_add_module_plugin(module_contract_fault_plugin contract_fault_helper.cpp)
add_executable(module_contract_supervision_tests contract_supervision_tests.cpp)
target_link_libraries(module_contract_supervision_tests PRIVATE vinc_test_manager vinc_module_contract vinc::concurrency vinc::rpc_manager)
add_dependencies(module_contract_supervision_tests module_contract_fault_helper module_contract_fault_plugin)
foreach(target IN ITEMS module_contract_fault_helper module_contract_supervision_tests)
    target_compile_definitions(${target} PRIVATE VINC_MODULE_TESTING)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
add_test(NAME module.contract_supervision
    COMMAND module_contract_supervision_tests "$<TARGET_FILE:module_contract_fault_plugin>" "$<TARGET_FILE:module_contract_fault_helper>")
set_tests_properties(module.contract_supervision PROPERTIES TIMEOUT 75)

# Two real shared libraries each link a private static Common copy. Test exports
# are distinct; only service-table callbacks lead from either image into the Host.
foreach(plugin IN ITEMS a b)
    add_library(runtime_services_plugin_${plugin} SHARED runtime_services_plugin.cpp)
    target_link_libraries(runtime_services_plugin_${plugin} PRIVATE vinc::common)
    target_compile_definitions(runtime_services_plugin_${plugin} PRIVATE
        BRIDGE_ENTRY=bridge_plugin_${plugin} BRIDGE_REASON="plugin-${plugin}")
endforeach()
add_executable(runtime_services_host runtime_services_host.cpp)
target_link_libraries(runtime_services_host PRIVATE vinc_module_contract
    runtime_services_plugin_a runtime_services_plugin_b)
add_executable(module_runtime_services_tests runtime_services_tests.cpp)
target_link_libraries(module_runtime_services_tests PRIVATE vinc_module_contract)
add_dependencies(module_runtime_services_tests runtime_services_host)
foreach(target IN ITEMS runtime_services_plugin_a runtime_services_plugin_b
                        runtime_services_host module_runtime_services_tests)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
add_test(NAME module.runtime_services
    COMMAND module_runtime_services_tests "$<TARGET_FILE:runtime_services_host>")
set_tests_properties(module.runtime_services PROPERTIES TIMEOUT 120)

# Production loader fault cases run in disposable Host processes.
foreach(mode IN ITEMS valid no_entry bad_version null_factory reject stall throw_constructor late_coordinator)
    vinc_add_module_plugin(plugin_${mode} plugin_fixture.cpp)
    if(NOT mode STREQUAL "valid")
        string(TOUPPER "${mode}" mode_definition)
        target_compile_definitions(plugin_${mode} PRIVATE "VINC_PLUGIN_${mode_definition}")
    endif()
endforeach()
add_executable(module_plugin_tests plugin_tests.cpp)
add_dependencies(module_integration_tests plugin_stall)
target_link_libraries(module_plugin_tests PRIVATE vinc_test_manager vinc::module)
add_dependencies(module_plugin_tests plugin_valid plugin_no_entry plugin_bad_version plugin_null_factory plugin_reject plugin_stall plugin_throw_constructor plugin_late_coordinator)
add_test(NAME module.plugin COMMAND module_plugin_tests
    "$<TARGET_FILE:plugin_valid>" "$<TARGET_FILE:plugin_no_entry>" "$<TARGET_FILE:plugin_bad_version>"
    "$<TARGET_FILE:plugin_null_factory>" "$<TARGET_FILE:plugin_reject>" "$<TARGET_FILE:plugin_stall>" "$<TARGET_FILE:plugin_throw_constructor>" "$<TARGET_FILE:plugin_late_coordinator>")
# Leave room for the sequential cases and their bounded failure-path cleanup.
# Per-case startup/wait limits still decide failures; this is only the outer kill guard.
set_tests_properties(module.plugin PROPERTIES TIMEOUT 180)

# Scripted peers hold failure reports until the controller observes the central
# snapshot. Only this Manager copy uses the existing native-termination seam.
add_executable(module_failure_order_helper failure_order_helper.cpp)
target_link_libraries(module_failure_order_helper PRIVATE vinc_test_manager vinc::control vinc::module)
add_executable(module_failure_order_tests failure_order_tests.cpp)
target_link_libraries(module_failure_order_tests PRIVATE vinc_test_manager vinc::concurrency vinc::rpc_manager)
target_compile_definitions(module_failure_order_tests PRIVATE VINC_MODULE_TESTING)
target_include_directories(module_failure_order_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(module_failure_order_tests module_failure_order_helper)
foreach(target IN ITEMS module_failure_order_tests module_failure_order_helper)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
add_test(NAME module.failure_order COMMAND module_failure_order_tests "$<TARGET_FILE:module_failure_order_helper>")
set_tests_properties(module.failure_order PROPERTIES TIMEOUT 120)
add_executable(module_plugin_initialize_tests plugin_initialize_tests.cpp)
target_link_libraries(module_plugin_initialize_tests PRIVATE vinc::module)
add_test(NAME module.plugin_initialize COMMAND module_plugin_initialize_tests)
set_tests_properties(module.plugin_initialize PROPERTIES TIMEOUT 15)
add_executable(module_debug_tests debug_tests.cpp)
target_link_libraries(module_debug_tests PRIVATE vinc_test_manager vinc::shell)
add_dependencies(module_debug_tests module_process_helper)
if(MSVC)
    target_compile_options(module_debug_tests PRIVATE /W4 /utf-8)
endif()
add_test(NAME module.debug COMMAND module_debug_tests "$<TARGET_FILE:module_process_helper>")
set_tests_properties(module.debug PROPERTIES TIMEOUT 60)
