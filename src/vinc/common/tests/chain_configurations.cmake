# Rebuild the entire common consumer for each capacity. A per-TU macro override
# would hide the layout mismatch this fixture is intended to prevent.
cmake_minimum_required(VERSION 3.20)
foreach(depth IN ITEMS 2 5 1)
    set(source "${TEST_BINARY_DIR}/depth-${depth}/source")
    set(build "${TEST_BINARY_DIR}/depth-${depth}/build")
    file(MAKE_DIRECTORY "${source}")
    file(COPY "${COMMON_DIR}/" DESTINATION "${source}")
    set(config_header "${source}/include/vinc/config.hpp")
    file(READ "${config_header}" settings)
    if(NOT settings MATCHES "error_chain_max_depth = [0-9]+;")
        message(FATAL_ERROR "Missing numeric error-chain capacity in config.hpp")
    endif()
    string(REGEX REPLACE "error_chain_max_depth = [0-9]+;"
        "error_chain_max_depth = ${depth};" settings "${settings}")
    file(WRITE "${config_header}" "${settings}")
    # Rebuild every object for this layout. The copied source must not reuse
    # objects compiled against a different Error capacity.
    if(EXISTS "${build}/CMakeCache.txt")
        execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --config "${CONFIG}" --target clean
            RESULT_VARIABLE cleaned OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
        if(NOT cleaned STREQUAL "0")
            message(FATAL_ERROR "depth ${depth}: cleanup failed: ${output}${errors}")
        endif()
    endif()
    set(generator_args -G "${GENERATOR}")
    if(PLATFORM)
        list(APPEND generator_args -A "${PLATFORM}")
    endif()
    if(TOOLSET)
        list(APPEND generator_args -T "${TOOLSET}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${build}"
        ${generator_args} "-DCMAKE_CXX_COMPILER=${COMPILER}"
        "-DCMAKE_BUILD_TYPE=${CONFIG}" -DBUILD_TESTING=ON
        RESULT_VARIABLE configured OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 45)
    file(WRITE "${TEST_BINARY_DIR}/depth-${depth}/configure.log" "${output}\n${errors}")
    if(NOT configured STREQUAL "0")
        message(FATAL_ERROR "depth ${depth}: configure failed: ${output}${errors}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --config "${CONFIG}"
        --target common_error_chain_tests --parallel 2
        RESULT_VARIABLE built OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 60)
    file(WRITE "${TEST_BINARY_DIR}/depth-${depth}/build.log" "${output}\n${errors}")
    if(depth EQUAL 1)
        if(built STREQUAL "0" OR NOT "${output}${errors}" MATCHES "Error chains require at least two records")
            message(FATAL_ERROR "Invalid capacity did not fail for the expected reason: ${output}${errors}")
        endif()
    else()
        if(NOT built STREQUAL "0")
            message(FATAL_ERROR "depth ${depth}: build failed: ${output}${errors}")
        endif()
        execute_process(COMMAND "${CTEST_COMMAND}" --test-dir "${build}" -C "${CONFIG}"
            -R "^common[.]error_chain$" --output-on-failure -V
            RESULT_VARIABLE tested OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 15)
        file(WRITE "${TEST_BINARY_DIR}/depth-${depth}/test.log" "${output}\n${errors}")
        if(NOT tested STREQUAL "0")
            message(FATAL_ERROR "depth ${depth}: chain tests failed: ${output}${errors}")
        endif()
    endif()
endforeach()
