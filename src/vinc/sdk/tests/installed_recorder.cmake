cmake_minimum_required(VERSION 3.20)
function(run_checked name)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output
        ERROR_VARIABLE errors TIMEOUT 240)
    file(WRITE "${work}/${name}.log" "${output}\n${errors}")
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "${name} failed (${result}): ${output}\n${errors}")
    endif()
endfunction()

# Each run owns a fresh directory; never delete an existing SDK or application.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(work "${SDK_BUILD}/recorder-sdk-${run_id}")
if(EXISTS "${work}")
    message(FATAL_ERROR "Recorder SDK test directory already exists")
endif()
file(MAKE_DIRECTORY "${work}/unrelated cwd")
run_checked(install_sdk "${CMAKE_COMMAND}" --install "${SDK_BUILD}"
    --component VincSDK --config "${CONFIG}" --prefix "${work}/sdk original")
file(RENAME "${work}/sdk original" "${work}/sdk moved")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/installed_recorder/" DESTINATION "${work}/source")
run_checked(configure "${CMAKE_COMMAND}" -S "${work}/source" -B "${work}/consumer"
    -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
    "-DCMAKE_BUILD_TYPE=${CONFIG}" "-Dvinc_DIR=${work}/sdk moved/lib/cmake/vinc")
run_checked(build "${CMAKE_COMMAND}" --build "${work}/consumer" --config "${CONFIG}"
    --target RecorderApp --parallel 2)
run_checked(install_app "${CMAKE_COMMAND}" --install "${work}/consumer"
    --config "${CONFIG}" --prefix "${work}/app original")
file(RENAME "${work}/app original" "${work}/app moved")
file(RENAME "${work}/consumer" "${work}/consumer hidden")
file(RENAME "${work}/sdk moved" "${work}/sdk hidden")
if(WIN32)
    set(suffix .exe)
endif()
execute_process(COMMAND "${work}/app moved/RecorderApp/RecorderApp${suffix}" --shell-port 0
    WORKING_DIRECTORY "${work}/unrelated cwd" RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 45)
file(WRITE "${work}/run.log" "${output}\n${errors}")
if(NOT result STREQUAL "0" OR output MATCHES "INSTALLED_RECORDER_FAILED"
        OR NOT output MATCHES "INSTALLED_RECORDER_WAIT_OK"
        OR NOT output MATCHES "All module processes exited")
    message(FATAL_ERROR "Relocated Recorder failed: ${output}\n${errors}")
endif()
message(STATUS "Installed Recorder passed: ${work}")
