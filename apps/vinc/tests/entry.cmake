cmake_minimum_required(VERSION 3.20)
include("${MANIFEST}")
file(MAKE_DIRECTORY "${work}")
set(launch_timeout_seconds 15)

function(reject_host app)
    execute_process(COMMAND "${app}" ${ARGN}
        WORKING_DIRECTORY "${work}" TIMEOUT ${launch_timeout_seconds}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT "${result}" STREQUAL "64" OR NOT error MATCHES "Invalid module bootstrap")
        message(FATAL_ERROR "Host rejection failed (${result}): ${output}${error}")
    endif()
    if("${output}${error}" MATCHES "Socket Shell|ENTRY_SCRIPT|Shell startup|Usage:")
        message(FATAL_ERROR "Invalid Host entered central: ${output}${error}")
    endif()
endfunction()

# Hard-coded old/future markers deliberately test protocol incompatibility.
# The active marker is read from the production protocol at configure/run time.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../../src/vinc/module_system/include/vinc/module/detail/module_protocol.hpp" protocol)
string(REGEX MATCH "version_marker\\[\\] = \"([^\"]+)\"" matched "${protocol}")
if(NOT matched)
    message(FATAL_ERROR "Missing bootstrap marker")
endif()
set(marker "${CMAKE_MATCH_1}")
foreach(app "${plain}" "${embedded}")
    reject_host("${app}" "${marker}")
    reject_host("${app}" --vinc-module-v0)
    reject_host("${app}" --vinc-module-v999999)
    reject_host("${app}" --vinc-module)
    reject_host("${app}" "${marker}" endpoint token invalid-mid 40 400 2000 200
        EntryModule absent-report missing-plugin 0 absent-notification)
endforeach()

function(check_flow app)
    execute_process(COMMAND "${app}" --shell-port 0 ${ARGN}
        WORKING_DIRECTORY "${work}" TIMEOUT ${launch_timeout_seconds}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    file(WRITE "${work}/last-flow.log" "${output}\n${error}")
    if(NOT "${result}" STREQUAL "0" OR NOT output MATCHES "ENTRY_FLOW_OK" OR
       output MATCHES "ENTRY_FLOW_FAILED")
        message(FATAL_ERROR "Self-host flow failed (${result}): ${output}${error}")
    endif()
    string(REGEX MATCHALL "ENTRY_SCRIPT_ONCE" scripts "${output}")
    list(LENGTH scripts script_count)
    string(REGEX MATCHALL "ENTRY_PID=[0-9]+" pids "${output}")
    list(REMOVE_DUPLICATES pids)
    list(LENGTH pids pid_count)
    string(REGEX MATCHALL "Socket Shell ready - listening on" listeners "${output}")
    list(LENGTH listeners listener_count)
    if(NOT script_count EQUAL 1 OR NOT pid_count EQUAL 2 OR NOT listener_count EQUAL 1)
        message(FATAL_ERROR "Role isolation failed: ${output}${error}")
    endif()
endfunction()
check_flow("${plain}" -S "${script}" --vinc-module-v0)
check_flow("${embedded}")

# Verify real redirected stdout both through a pipe and directly to a UTF-8 file.
# The console renderer must never bypass the redirected destination or add ANSI.
set(unicode_script "${work}/unicode-output.tcl")
file(WRITE "${unicode_script}" "puts {UTF8_OUTPUT: 中文 ✅}\nexit\n")
string(ASCII 27 escape)
foreach(destination pipe file)
    if(destination STREQUAL "pipe")
        execute_process(COMMAND "${plain}" --shell-port 0 -S "${unicode_script}"
            WORKING_DIRECTORY "${work}" TIMEOUT ${launch_timeout_seconds}
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error ENCODING UTF-8)
    else()
        execute_process(COMMAND "${plain}" --shell-port 0 -S "${unicode_script}"
            WORKING_DIRECTORY "${work}" TIMEOUT ${launch_timeout_seconds}
            RESULT_VARIABLE result OUTPUT_FILE "${work}/unicode-output.txt" ERROR_VARIABLE error)
        file(READ "${work}/unicode-output.txt" output)
    endif()
    string(FIND "${output}" "${escape}" ansi_position)
    if(NOT "${result}" STREQUAL "0" OR NOT output MATCHES "UTF8_OUTPUT: 中文 ✅" OR
       NOT output MATCHES "\\[OK\\] Tcl ready" OR NOT ansi_position EQUAL -1)
        message(FATAL_ERROR "Redirected UTF-8 ${destination} failed (${result}): ${output}${error}")
    endif()
endforeach()

# The production entry keeps running after an official Module fails, prints one
# concise row, and leaves the full cross-component cause chain in framework Log.
get_filename_component(app_directory "${plain}" DIRECTORY)
file(GLOB_RECURSE previous_logs "${app_directory}/vinc_data/entry-test/logs/*.log")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env VINC_TEST_OFFICIAL_FAILURE=1
    "${plain}" --shell-port 0 -S "${script}"
    WORKING_DIRECTORY "${work}" TIMEOUT ${launch_timeout_seconds}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT "${result}" STREQUAL "0" OR NOT output MATCHES "ENTRY_FLOW_OK" OR
   NOT output MATCHES "1 startup step failed" OR "${output}${error}" MATCHES "domain=")
    message(FATAL_ERROR "Startup presentation/isolation failed: ${result}: ${output}${error}")
endif()
string(REGEX MATCHALL "\\[FAIL\\] Web UI failed" failures "${output}")
list(LENGTH failures failure_count)
if(NOT failure_count EQUAL 1)
    message(FATAL_ERROR "Repeated/missing official failure: ${output}")
endif()
file(GLOB_RECURSE log_files "${app_directory}/vinc_data/entry-test/logs/*.log")
if(previous_logs)
    list(REMOVE_ITEM log_files ${previous_logs})
endif()
set(found_diagnostic FALSE)
foreach(log_file IN LISTS log_files)
    file(READ "${log_file}" contents)
    if(contents MATCHES "Web UI startup:.*ModuleSystem.*OSAL")
        set(found_diagnostic TRUE)
    endif()
endforeach()
if(NOT found_diagnostic)
    message(FATAL_ERROR "Startup error cause chain missing from framework Log")
endif()

execute_process(COMMAND "${plain}" --shell-ip invalid-address --shell-port 0
    WORKING_DIRECTORY "${work}" TIMEOUT ${launch_timeout_seconds}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT "${result}" STREQUAL "1" OR NOT error MATCHES "\\[FAIL\\] Socket Shell failed" OR
   NOT error MATCHES "VINC startup aborted" OR NOT error MATCHES "domain=" OR
   "${output}${error}" MATCHES "VINC ready")
    message(FATAL_ERROR "Aborted startup lost details or claimed readiness: ${result}: ${output}${error}")
endif()

execute_process(COMMAND "${embedded}" -S "${script}"
    WORKING_DIRECTORY "${work}" TIMEOUT ${launch_timeout_seconds}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT "${result}" STREQUAL "2" OR NOT error MATCHES "fixed embedded startup script")
    message(FATAL_ERROR "Embedded CLI rejection changed: ${output}${error}")
endif()
message(STATUS "App role dispatch, self-host paths and embedded isolation passed")

# Exercise production startup ordering, CLI -> live Param, and the shared Tcl
# correction path. This fixture's identity is isolated and runs serially.
file(WRITE "${work}/network.tcl" "param get vinc.shell_port\nparam get vinc.shell_ip\nparam set vinc.shell_ip invalid\nparam get vinc.shell_ip\nexit\n")
execute_process(COMMAND "${plain}" --shell-port 0 --shell-ip 127.0.0.1 -S "${work}/network.tcl"
    WORKING_DIRECTORY "${work}" TIMEOUT ${launch_timeout_seconds}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
file(WRITE "${work}/network.log" "${output}\n${error}")
if(NOT "${result}" STREQUAL "0" OR NOT output MATCHES "vinc.shell_port \\[uint16\\] = 0"
        OR NOT output MATCHES "restored 127.0.0.1" OR NOT output MATCHES "\\[restart: App\\]")
    message(FATAL_ERROR "Network Param startup/Tcl flow failed: ${result}: ${output}${error}")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../../../src/vinc/log/tests/app_settings.cmake")
verify_log_settings("${plain}" "${app_directory}/vinc_data/entry-test"
    "${work}/log-settings" "${launch_timeout_seconds}")
