# Standalone SDK and framework deployment regression. Only this fixture's build
# directory is changed; neither the source tree nor the pinned Tcl is copied back.
cmake_minimum_required(VERSION 3.20)

if(NOT IS_ABSOLUTE "${TEST_BINARY_DIR}" OR NOT IS_ABSOLUTE "${VINC_SOURCE_DIR}")
    message(FATAL_ERROR "SDK fixture requires absolute source and build directories")
endif()
file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")
# Reset only the fixture-owned inputs, so a previous interrupted run is repeatable.
file(MAKE_DIRECTORY "${TEST_BINARY_DIR}/message-inputs/common" "${TEST_BINARY_DIR}/message-inputs/camera")
file(WRITE "${TEST_BINARY_DIR}/message-inputs/common/ExternalLeaf.msg" "uint32_t revision = 1;\nuint32_t value;\n")
file(WRITE "${TEST_BINARY_DIR}/message-inputs/camera/ExternalFrame.msg" "ExternalLeaf leaf;\n")
file(REMOVE "${TEST_BINARY_DIR}/message-inputs/camera/ExternalExtra.msg")
function(run_checked label)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 480)
    file(WRITE "${TEST_BINARY_DIR}/${label}.log" "${output}\n${errors}")
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "${label} failed (${result}): ${output}\n${errors}")
    endif()
endfunction()

run_checked(configure "${CMAKE_COMMAND}"
    -S "${VINC_SOURCE_DIR}/tests/sdk_consumer" -B "${TEST_BINARY_DIR}"
    -G "${GENERATOR}" "-DVINC_SOURCE_DIR=${VINC_SOURCE_DIR}"
    "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
    "-DCMAKE_BUILD_TYPE=${CONFIG}" -DVINC_TEST_DUPLICATE_MODULE=OFF -DVINC_TEST_EMBEDDED_STARTUP=OFF)
run_checked(build "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}" --config "${CONFIG}"
    --target external_consumer shell --parallel 4)
include("${TEST_BINARY_DIR}/executables.cmake")
run_checked(consumer "${consumer}" "${module}" "${central}")
run_checked(messages_initial "${message_probe}" 1 0)

# Cross-directory dependencies and the complete catalog must follow edits,
# additions, deletion and recovery without manually rerunning CMake.
file(WRITE "${TEST_BINARY_DIR}/message-inputs/common/ExternalLeaf.msg"
    "uint32_t revision = 2;\nuint32_t value;\n")
file(WRITE "${TEST_BINARY_DIR}/message-inputs/camera/ExternalExtra.msg" "ExternalFrame frame;\n")
run_checked(messages_added_build "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}"
    --config "${CONFIG}" --target external_consumer shell --parallel 4)
run_checked(messages_added "${message_probe}" 2 1)
file(REMOVE "${TEST_BINARY_DIR}/message-inputs/camera/ExternalExtra.msg")
run_checked(messages_removed_build "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}"
    --config "${CONFIG}" --target external_message_probe --parallel 4)
run_checked(messages_removed "${message_probe}" 2 0)
if(EXISTS "${TEST_BINARY_DIR}/open-vinc/generated/include/vinc_messages/ExternalExtra.hpp")
    message(FATAL_ERROR "Deleted message left a stale generated header")
endif()
file(WRITE "${TEST_BINARY_DIR}/message-inputs/common/ExternalLeaf.msg" "MissingType value;\n")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}"
    --config "${CONFIG}" --target external_message_probe --parallel 4
    RESULT_VARIABLE invalid_schema OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 120)
if(invalid_schema STREQUAL "0" OR NOT "${output}${errors}" MATCHES "Unknown type")
    message(FATAL_ERROR "Invalid external schema did not stop the build: ${output}${errors}")
endif()
file(WRITE "${TEST_BINARY_DIR}/message-inputs/common/ExternalLeaf.msg"
    "uint32_t revision = 2;\nuint32_t value;\n")
run_checked(messages_recovery_build "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}"
    --config "${CONFIG}" --target external_consumer shell --parallel 4)
run_checked(messages_recovered "${message_probe}" 2 0)

# Change only a header owned by this fixture, never a repository source. The
# executable's value must change too; a successful no-op build is not sufficient.
run_checked(incremental_before "${incremental_probe}" 1)
file(WRITE "${TEST_BINARY_DIR}/incremental/value.hpp"
    "#pragma once\ninline constexpr int sdk_incremental_value = 2;\n")
run_checked(incremental_build "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}"
    --config "${CONFIG}" --target sdk_incremental_probe --parallel 4)
run_checked(incremental_after "${incremental_probe}" 2)

function(check_resources root)
    foreach(resource IN ITEMS init.tcl license.terms licenses/utf8proc.txt
            licenses/libtommath.txt licenses/zlib.txt)
        if(NOT EXISTS "${root}/lib/tcl9.1/${resource}")
            message(FATAL_ERROR "Missing deployed Tcl resource: ${root}/${resource}")
        endif()
    endforeach()
endfunction()
get_filename_component(central_directory "${central}" DIRECTORY)
check_resources("${central_directory}")
check_resources("${TEST_BINARY_DIR}/explicit resources")
function(check_licenses binary)
    get_filename_component(directory "${binary}" DIRECTORY)
    foreach(name IN ITEMS LICENSE NOTICE THIRD_PARTY_NOTICES.txt)
        set(deployed "${directory}/licenses/open-vinc/${name}")
        if(NOT EXISTS "${deployed}")
            message(FATAL_ERROR "Missing SDK attribution: ${deployed}")
        endif()
        file(SHA256 "${VINC_SOURCE_DIR}/${name}" expected)
        file(SHA256 "${deployed}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "SDK attribution differs from its source: ${deployed}")
        endif()
    endforeach()
endfunction()
check_licenses("${central}")
check_licenses("${consumer}")
check_licenses("${module}")

# No application calls initialize_tcl or activates SocketShell. The real Vinc
# entry performs both before interpreting the user's startup script.
set(script "${TEST_BINARY_DIR}/startup.tcl")
file(WRITE "${script}" "puts SDK_SCRIPT_READY\nExternalModule\nexternalNode schema\nexit\n")
function(run_central label executable)
    execute_process(COMMAND "${executable}" --shell-port 0 -S "${script}"
        WORKING_DIRECTORY "${TEST_BINARY_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 20 ENCODING UTF-8)
    file(WRITE "${TEST_BINARY_DIR}/${label}.log" "${output}\n${errors}")
    if(NOT result STREQUAL "0" OR NOT output MATCHES "SDK_SCRIPT_READY"
            OR NOT output MATCHES "Socket Shell listening on 127\\.0\\.0\\.1:[1-9][0-9]*"
            OR NOT output MATCHES "EXTERNAL_MODULE_SCHEMA=2")
        message(FATAL_ERROR "${label}: automatic services failed (${result}): ${output}${errors}")
    endif()
endfunction()
run_central(central "${central}")

# Copy only shipped artifacts, then run from a different working directory.
set(relocated "${TEST_BINARY_DIR}/relocated space")
file(MAKE_DIRECTORY "${relocated}")
file(COPY "${central}" "${central_directory}/lib" "${central_directory}/licenses" DESTINATION "${relocated}")
get_filename_component(central_name "${central}" NAME)
check_licenses("${relocated}/${central_name}")
run_central(relocated "${relocated}/${central_name}")

# A missing resource must fail with diagnostics, and a normal build must restore
# it even when no C++ source or executable has changed. Remove one known fixture file.
file(REMOVE "${central_directory}/lib/tcl9.1/init.tcl")
execute_process(COMMAND "${central}" --shell-port 0 -S "${script}"
    RESULT_VARIABLE missing OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 20)
if(missing STREQUAL "0" OR NOT "${output}${errors}" MATCHES "Missing Tcl runtime resources")
    message(FATAL_ERROR "Missing Tcl resources produced the wrong result: ${missing}: ${output}${errors}")
endif()
run_checked(restore "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}" --config "${CONFIG}"
    --target shell external_consumer --parallel 4)
check_resources("${central_directory}")
run_central(restored "${central}")

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${VINC_SOURCE_DIR}/tests/sdk_consumer"
    -B "${TEST_BINARY_DIR}" -DVINC_TEST_DUPLICATE_MODULE=ON
    RESULT_VARIABLE duplicate OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 120)
if(duplicate STREQUAL "0" OR NOT "${output}${errors}" MATCHES "Duplicate ModuleType")
    message(FATAL_ERROR "Duplicate ModuleType was not rejected: ${output}${errors}")
endif()
run_checked(restore_configuration "${CMAKE_COMMAND}" -S "${VINC_SOURCE_DIR}/tests/sdk_consumer"
    -B "${TEST_BINARY_DIR}" -DVINC_TEST_DUPLICATE_MODULE=OFF)

file(WRITE "${TEST_BINARY_DIR}/message-inputs/camera/externalleaf.msg" "uint8_t value;\n")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}"
    --config "${CONFIG}" --target external_message_probe --parallel 4
    RESULT_VARIABLE duplicate_message OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 120)
file(REMOVE "${TEST_BINARY_DIR}/message-inputs/camera/externalleaf.msg")
if(duplicate_message STREQUAL "0" OR NOT "${output}${errors}" MATCHES "Duplicate or reserved message")
    message(FATAL_ERROR "Cross-directory message collision was not rejected: ${output}${errors}")
endif()
run_checked(final_recovery "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}"
    --config "${CONFIG}" --target external_message_probe --parallel 4)
run_checked(final_catalog "${message_probe}" 2 0)

# Exercise the public application API from a parent directory after registration.
set(embedded_input "${TEST_BINARY_DIR}/startup.tcl")
file(WRITE "${embedded_input}" [=[
if {[info script] ne {embedded:startup.tcl}} {error identity}
source [file join $::vinc::app_dir companion.tcl]
if {[info script] ne {embedded:startup.tcl}} {error restored_identity}
puts {中文 😀}
puts {embedded-revision-1 $literal [unchanged]; "quotes"}
exit
]=])
run_checked(embedded_configure "${CMAKE_COMMAND}" -S "${VINC_SOURCE_DIR}/tests/sdk_consumer"
    -B "${TEST_BINARY_DIR}" -DVINC_TEST_EMBEDDED_STARTUP=ON)
run_checked(embedded_build "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}" --config "${CONFIG}"
    --target shell --parallel 4)
include("${TEST_BINARY_DIR}/executables.cmake")
get_filename_component(embedded_directory "${central}" DIRECTORY)
file(WRITE "${embedded_directory}/companion.tcl" "puts companion-loaded\nreturn\nerror unreachable\n")

function(run_embedded label executable expected)
    execute_process(COMMAND "${executable}" --shell-port 0
        WORKING_DIRECTORY "${TEST_BINARY_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 20 ENCODING UTF-8)
    file(WRITE "${TEST_BINARY_DIR}/${label}.log" "${output}\n${errors}")
    if(NOT result STREQUAL "0" OR NOT output MATCHES "${expected}")
        message(FATAL_ERROR "${label}: embedded startup failed (${result}): ${output}${errors}")
    endif()
    if(expected STREQUAL "embedded-revision-1" AND NOT output MATCHES "中文 😀")
        message(FATAL_ERROR "Embedded UTF-8 did not survive generation and execution: ${output}")
    endif()
endfunction()
run_embedded(embedded_initial "${central}" "embedded-revision-1")
execute_process(COMMAND "${central}" -S missing.tcl
    RESULT_VARIABLE rejected OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
if(NOT rejected STREQUAL "2" OR NOT "${output}${errors}" MATCHES "fixed embedded startup script")
    message(FATAL_ERROR "Embedded application did not reject -S: ${output}${errors}")
endif()

set(embedded_relocated "${TEST_BINARY_DIR}/embedded relocated")
file(MAKE_DIRECTORY "${embedded_relocated}")
file(COPY "${central}" "${embedded_directory}/lib" "${embedded_directory}/licenses" "${embedded_directory}/companion.tcl"
    DESTINATION "${embedded_relocated}")
get_filename_component(embedded_name "${central}" NAME)
run_embedded(embedded_relocated "${embedded_relocated}/${embedded_name}" "companion-loaded")

# An input-only edit must update the executable without an explicit configure.
file(WRITE "${embedded_input}" "puts embedded-revision-2\nexit\n")
run_checked(embedded_incremental_build "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}" --config "${CONFIG}"
    --target shell --parallel 4)
run_embedded(embedded_updated "${central}" "embedded-revision-2")
string(ASCII 255 invalid_utf8)
file(WRITE "${embedded_input}" "${invalid_utf8}")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}" --config "${CONFIG}" --target shell
    RESULT_VARIABLE invalid_script OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 60)
if(invalid_script STREQUAL "0" OR NOT "${output}${errors}" MATCHES "valid UTF-8")
    message(FATAL_ERROR "Invalid startup encoding was not rejected: ${output}${errors}")
endif()
file(WRITE "${embedded_input}" "exit\n")
run_checked(embedded_recovery_build "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}" --config "${CONFIG}"
    --target shell --parallel 4)
run_checked(embedded_restore_configuration "${CMAKE_COMMAND}" -S "${VINC_SOURCE_DIR}/tests/sdk_consumer"
    -B "${TEST_BINARY_DIR}" -DVINC_TEST_EMBEDDED_STARTUP=OFF)

# Explicit configuration lives in this fixture's build tree, never the shared
# repository source. Module removal and restoration must survive normal builds.
set(features "${TEST_BINARY_DIR}/application.conf")
file(WRITE "${features}" "# CONFIG_VINC_APP_UI is not set\n# CONFIG_VINC_APP_BAG is not set\n")
run_checked(features_configure "${CMAKE_COMMAND}" -S "${VINC_SOURCE_DIR}/tests/sdk_consumer"
    -B "${TEST_BINARY_DIR}" "-DVINC_CONFIG_FILE=${features}")
run_checked(features_off "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}" --target shell --parallel 4)
file(READ "${TEST_BINARY_DIR}/.vinc-owned-files" owned)
if(owned MATCHES "bag_recorder|module_ui|/ui/app/")
    message(FATAL_ERROR "Disabled source application feature still owned/deployed")
endif()
file(WRITE "${features}" "CONFIG_VINC_APP_BAG=y\nCONFIG_VINC_APP_UI=y\n")
run_checked(features_on "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}" --target shell --parallel 4)
file(READ "${TEST_BINARY_DIR}/.vinc-owned-files" owned)
if(NOT owned MATCHES "bag_recorder" OR NOT owned MATCHES "module_ui")
    message(FATAL_ERROR "Source application feature restoration failed")
endif()
run_checked(features_restore_legacy "${CMAKE_COMMAND}" -S "${VINC_SOURCE_DIR}/tests/sdk_consumer"
    -B "${TEST_BINARY_DIR}" -U VINC_CONFIG_FILE)
