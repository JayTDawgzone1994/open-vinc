# Standalone SDK and framework deployment regression. Only this fixture's build
# directory is changed; neither the source tree nor the pinned Tcl is copied back.
cmake_minimum_required(VERSION 3.20)

if(NOT IS_ABSOLUTE "${TEST_BINARY_DIR}" OR NOT IS_ABSOLUTE "${VINC_SOURCE_DIR}")
    message(FATAL_ERROR "SDK fixture requires absolute source and build directories")
endif()
file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")
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
    "-DCMAKE_BUILD_TYPE=${CONFIG}")
run_checked(build "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}" --config "${CONFIG}"
    --target external_consumer shell --parallel 4)
include("${TEST_BINARY_DIR}/executables.cmake")
run_checked(consumer "${consumer}" "${module}")

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

# No application calls initialize_tcl or activates SocketShell. The real Vinc
# entry performs both before interpreting the user's startup script.
set(script "${TEST_BINARY_DIR}/startup.tcl")
file(WRITE "${script}" "puts SDK_SCRIPT_READY\nexit\n")
function(run_central label executable)
    execute_process(COMMAND "${executable}" --shell-port 0 -S "${script}"
        WORKING_DIRECTORY "${TEST_BINARY_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 20)
    file(WRITE "${TEST_BINARY_DIR}/${label}.log" "${output}\n${errors}")
    if(NOT result STREQUAL "0" OR NOT output MATCHES "SDK_SCRIPT_READY"
            OR NOT output MATCHES "Socket Shell listening on 127\\.0\\.0\\.1:[1-9][0-9]*")
        message(FATAL_ERROR "${label}: automatic services failed (${result}): ${output}${errors}")
    endif()
endfunction()
run_central(central "${central}")

# Copy only shipped artifacts, then run from a different working directory.
set(relocated "${TEST_BINARY_DIR}/relocated space")
file(MAKE_DIRECTORY "${relocated}")
file(COPY "${central}" "${central_directory}/lib" DESTINATION "${relocated}")
get_filename_component(central_name "${central}" NAME)
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
