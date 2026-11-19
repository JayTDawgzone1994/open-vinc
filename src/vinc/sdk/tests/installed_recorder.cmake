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
if(DEFINED RECORDER_WORK)
    if(NOT IS_ABSOLUTE "${RECORDER_WORK}")
        message(FATAL_ERROR "RECORDER_WORK must be absolute")
    endif()
    set(work "${RECORDER_WORK}")
else()
    set(work "${SDK_BUILD}/recorder-sdk-${run_id}")
endif()
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
    --target RecorderApp RecorderOther recorder_decode_check --parallel 2)
run_checked(install_app "${CMAKE_COMMAND}" --install "${work}/consumer"
    --config "${CONFIG}" --prefix "${work}/app original")
file(RENAME "${work}/app original" "${work}/app moved")
if(WIN32)
    set(suffix .exe)
endif()
set(decoder "${work}/consumer/recorder_decode_check${suffix}")
if(EXISTS "${work}/consumer/${CONFIG}/recorder_decode_check${suffix}")
    set(decoder "${work}/consumer/${CONFIG}/recorder_decode_check${suffix}")
endif()
file(COPY "${decoder}" DESTINATION "${work}/decoder")
file(RENAME "${work}/consumer" "${work}/consumer hidden")
file(RENAME "${work}/sdk moved" "${work}/sdk hidden")
find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)
if(HIDE_PRODUCER)
    file(RELATIVE_PATH relative_work "${SDK_BUILD}" "${work}")
    if(NOT relative_work MATCHES "^[.][.]/" OR EXISTS "${SDK_BUILD}.hidden")
        message(FATAL_ERROR "Hiding producer requires external work and an unused .hidden path")
    endif()
    file(RENAME "${SDK_BUILD}" "${SDK_BUILD}.hidden")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/verify_recorder.py"
    "${work}" "${work}/decoder/recorder_decode_check${suffix}"
    "${work}/app moved/RecorderApp/RecorderApp${suffix}"
    "${work}/app moved/RecorderOther/RecorderOther${suffix}"
    WORKING_DIRECTORY "${work}/unrelated cwd" RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 180)
if(HIDE_PRODUCER)
    file(RENAME "${SDK_BUILD}.hidden" "${SDK_BUILD}")
endif()
file(WRITE "${work}/run.log" "${output}\n${errors}")
if(NOT result STREQUAL "0" OR NOT output MATCHES "INSTALLED_RECORDER_MULTI_APP_CONTENT_OK")
    message(FATAL_ERROR "Relocated Recorder failed: ${output}\n${errors}")
endif()
message(STATUS "Installed Recorder passed: ${work}")
