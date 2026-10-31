cmake_minimum_required(VERSION 3.20)

function(run_checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result
        OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 90)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Command failed: ${ARGV}\n${output}\n${errors}")
    endif()
endfunction()

function(expect_rejected name expected)
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/source" -B "${WORK}/${name}"
        -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${COMPILER}"
        "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
        "-Dvinc_DIR=${moved}/lib/cmake/vinc" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 60)
    if(result STREQUAL "0" OR NOT "${output}\n${errors}" MATCHES "${expected}")
        message(FATAL_ERROR "${name} was not rejected for the expected reason:\n${output}\n${errors}")
    endif()
endfunction()

# Only remove this fixture's prior output, never a caller-supplied arbitrary tree.
get_filename_component(expected_work "${SDK_BUILD}/sdk-install-test" ABSOLUTE)
get_filename_component(actual_work "${WORK}" ABSOLUTE)
if(NOT actual_work STREQUAL expected_work)
    message(FATAL_ERROR "Unexpected SDK test working directory")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(original "${WORK}/original prefix")
set(moved "${WORK}/moved SDK")
run_checked("${CMAKE_COMMAND}" --install "${SDK_BUILD}" --component VincSDK --prefix "${original}")
file(RENAME "${original}" "${moved}")
file(COPY "${FIXTURE}/" DESTINATION "${WORK}/source")
if(WIN32)
    set(executable_suffix .exe)
else()
    set(executable_suffix "")
endif()
execute_process(COMMAND "${moved}/bin/ribosome${executable_suffix}"
    RESULT_VARIABLE no_arguments OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
if(no_arguments STREQUAL "0" OR NOT errors MATCHES "requires explicit")
    message(FATAL_ERROR "Installed Ribosome used producer defaults: ${output}\n${errors}")
endif()

# An exported target may contain neither source nor producer-build paths.
file(GLOB configs "${moved}/lib/cmake/vinc/*.cmake")
foreach(config IN LISTS configs)
    file(READ "${config}" contents)
    string(FIND "${contents}" "${SDK_BUILD}" build_reference)
    string(FIND "${contents}" "${original}" prefix_reference)
    if(NOT build_reference EQUAL -1 OR NOT prefix_reference EQUAL -1)
        message(FATAL_ERROR "Non-relocatable package configuration: ${config}")
    endif()
endforeach()

run_checked("${CMAKE_COMMAND}" -S "${WORK}/source" -B "${WORK}/consumer"
    -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${COMPILER}"
    "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    "-DCMAKE_PREFIX_PATH=${moved}")
run_checked("${CMAKE_COMMAND}" --build "${WORK}/consumer" --parallel 2)
run_checked("${CMAKE_CTEST_COMMAND}" --test-dir "${WORK}/consumer" --output-on-failure)
# Preserve localized MSVC /showIncludes dependency discovery for user headers,
# not only the first successful compilation.
run_checked("${CMAKE_COMMAND}" -E sleep 1)
file(WRITE "${WORK}/source/local_value.hpp" "#pragma once\nconstexpr int sdk_test_value = 43;\n")
run_checked("${CMAKE_COMMAND}" --build "${WORK}/consumer" --parallel 2)
execute_process(COMMAND "${WORK}/consumer/consumer${executable_suffix}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 20)
if(NOT result STREQUAL "0" OR NOT output MATCHES "local_value=43")
    message(FATAL_ERROR "Consumer header edit did not rebuild correctly: ${output}\n${errors}")
endif()
foreach(name LICENSE NOTICE THIRD_PARTY_NOTICES.txt)
    file(SHA256 "${moved}/share/vinc/licenses/${name}" expected)
    file(SHA256 "${WORK}/consumer/licenses/open-vinc/${name}" actual)
    if(NOT expected STREQUAL actual)
        message(FATAL_ERROR "License delivery mismatch: ${name}")
    endif()
endforeach()

if(CONFIG STREQUAL "Debug")
    set(other_config Release)
else()
    set(other_config Debug)
endif()
expect_rejected(configuration "SDK requires native" "-DCMAKE_BUILD_TYPE=${other_config}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${SDK_BUILD}"
    --component VincSDK --config "${other_config}" --prefix "${WORK}/wrong-config"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
if(result STREQUAL "0" OR NOT errors MATCHES "Install this SDK with --config")
    message(FATAL_ERROR "Wrong install configuration was not rejected: ${output}\n${errors}")
endif()
expect_rejected(version "version" -DTEST_WRONG_VERSION=ON)
if(WIN32)
    expect_rejected(crt "ABI mismatch" -DTEST_STATIC_CRT=ON)
else()
    file(READ "${moved}/lib/cmake/vinc/profile.txt" sdk_profile)
    if(sdk_profile MATCHES "libstdcxx=.*cxx11=1;")
        expect_rejected(stdlib "ABI mismatch" -DTEST_OLD_LIBSTDCXX_ABI=ON)
    endif()
endif()
file(APPEND "${moved}/include/vinc/config.hpp" "\n// Deliberately mismatched SDK header.\n")
expect_rejected(config_header "differs from the compiled SDK")
# A rejected installation must leave the existing package untouched.
file(APPEND "${moved}/lib/cmake/vinc/profile.txt" "incompatible")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${SDK_BUILD}"
    --component VincSDK --prefix "${moved}" RESULT_VARIABLE install_result
    OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
if(install_result STREQUAL "0" OR NOT errors MATCHES "different build profile")
    message(FATAL_ERROR "Incompatible installation did not fail before copying: ${output}\n${errors}")
endif()
file(READ "${moved}/include/vinc/config.hpp" unchanged_header)
if(NOT unchanged_header MATCHES "Deliberately mismatched")
    message(FATAL_ERROR "Rejected install overwrote the previous SDK")
endif()
message(STATUS "Installed SDK: relocation, generation, execution and rejection checks passed")
