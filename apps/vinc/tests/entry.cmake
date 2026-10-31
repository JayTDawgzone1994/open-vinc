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
    string(REGEX MATCHALL "Socket Shell listening on" listeners "${output}")
    list(LENGTH listeners listener_count)
    if(NOT script_count EQUAL 1 OR NOT pid_count EQUAL 2 OR NOT listener_count EQUAL 1)
        message(FATAL_ERROR "Role isolation failed: ${output}${error}")
    endif()
endfunction()
check_flow("${plain}" -S "${script}" --vinc-module-v0)
check_flow("${embedded}")

execute_process(COMMAND "${embedded}" -S "${script}"
    WORKING_DIRECTORY "${work}" TIMEOUT ${launch_timeout_seconds}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT "${result}" STREQUAL "2" OR NOT error MATCHES "fixed embedded startup script")
    message(FATAL_ERROR "Embedded CLI rejection changed: ${output}${error}")
endif()
message(STATUS "App role dispatch, self-host paths and embedded isolation passed")
