cmake_minimum_required(VERSION 3.20)
function(run_checked label)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 240)
    file(WRITE "${work}/${label}.log" "${output}\n${errors}")
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "${label} failed (${result}): ${output}\n${errors}")
    endif()
endfunction()

# Fixed, fixture-owned child; never accept an arbitrary deletion target.
get_filename_component(work "${SDK_BUILD}/sdk-publisher-defaults-test" ABSOLUTE)
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}/unrelated cwd")
run_checked(install "${CMAKE_COMMAND}" --install "${SDK_BUILD}" --component VincSDK --prefix "${work}/original SDK")
file(RENAME "${work}/original SDK" "${work}/moved SDK")
file(COPY "${FIXTURE}/" DESTINATION "${work}/source")
run_checked(configure "${CMAKE_COMMAND}" -S "${work}/source" -B "${work}/consumer"
    -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}" "-Dvinc_DIR=${work}/moved SDK/lib/cmake/vinc")
run_checked(build "${CMAKE_COMMAND}" --build "${work}/consumer" --target PublisherDefaultsApp --parallel 2)
run_checked(install_app "${CMAKE_COMMAND}" --install "${work}/consumer" --prefix "${work}/original App")
file(RENAME "${work}/original App" "${work}/moved App")
file(RENAME "${work}/moved SDK" "${work}/hidden SDK")
file(RENAME "${work}/consumer" "${work}/hidden consumer")
if(WIN32)
    set(suffix .exe)
endif()
execute_process(COMMAND "${work}/moved App/PublisherDefaultsApp/PublisherDefaultsApp${suffix}" --shell-port 0
    WORKING_DIRECTORY "${work}/unrelated cwd" RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 35)
file(WRITE "${work}/run.log" "${output}\n${errors}")
if(NOT result STREQUAL "0" OR output MATCHES "PUBLISHER_DEFAULTS_SDK_FAILED"
    OR NOT output MATCHES "PUBLISHER_DEFAULTS_SDK_OK" OR NOT output MATCHES "All module processes exited")
    message(FATAL_ERROR "Installed Publisher defaults failed: ${result}\n${output}\n${errors}")
endif()
message(STATUS "Installed Publisher defaults: custom message, original/current capacities, correction, relocation and shutdown passed")
