# First prove the toolchain works, then require each negative fixture to fail for
# the intended reason. A missing compiler or unrelated error must not count as success.
cmake_minimum_required(VERSION 3.20)
set(cases valid duplicate_code wrong_domain duplicate_domain duplicate_registration
    missing_finalize late_registration duplicate_finalize)
set(compiled_cases valid duplicate_code wrong_domain duplicate_domain)
foreach(case IN LISTS cases)
    set(build "${TEST_BINARY_DIR}/${case}")
    # Isolate each fixture run from old consumer objects and Error layouts.
    # The valid case below separately proves clean/build recovery without configure.
    if(case IN_LIST compiled_cases AND EXISTS "${build}/CMakeCache.txt")
        execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --config "${CONFIG}" --target clean
            RESULT_VARIABLE cleaned OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
        if(NOT cleaned STREQUAL "0")
            message(FATAL_ERROR "${case}: fixture cleanup failed: ${output}${errors}")
        endif()
    endif()
    set(generator_args -G "${GENERATOR}")
    if(PLATFORM)
        list(APPEND generator_args -A "${PLATFORM}")
    endif()
    if(TOOLSET)
        list(APPEND generator_args -T "${TOOLSET}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}" -B "${build}"
        ${generator_args} "-DCOMMON_DIR=${COMMON_DIR}" "-DCASE=${case}" -DFIXTURE_ALTERNATE=OFF
        "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
        RESULT_VARIABLE configured OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 45)
    file(WRITE "${build}/configure.log" "${output}\n${errors}")
    set(expected "")
    if(case STREQUAL "duplicate_registration")
        set(expected "registered twice")
    elseif(case STREQUAL "missing_finalize")
        set(expected "Call vinc_finalize_error_catalog")
    elseif(case STREQUAL "late_registration")
        set(expected "Register error catalogs before")
    elseif(case STREQUAL "duplicate_finalize")
        set(expected "must be called exactly once")
    endif()
    if(expected)
        if(configured STREQUAL "0" OR NOT "${output}${errors}" MATCHES "${expected}")
            message(FATAL_ERROR "${case}: wrong configure result: ${output}${errors}")
        endif()
        continue()
    endif()
    if(NOT configured STREQUAL "0")
        message(FATAL_ERROR "${case}: configuration failed: ${output}${errors}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --config "${CONFIG}"
        RESULT_VARIABLE built OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 60)
    file(WRITE "${build}/build.log" "${output}\n${errors}")
    if(case STREQUAL "valid")
        if(NOT built STREQUAL "0")
            message(FATAL_ERROR "valid fixture failed: ${output}${errors}")
        endif()
        execute_process(COMMAND "${CTEST_COMMAND}" --test-dir "${build}" -C "${CONFIG}" --output-on-failure
            RESULT_VARIABLE tested OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 15)
        if(NOT tested STREQUAL "0")
            message(FATAL_ERROR "fixture query failed: ${output}${errors}")
        endif()
        # Neither a normal clean nor an individually missing source may require
        # the caller to run configure again before querying the same catalog.
        foreach(rebuild IN ITEMS clean missing_source)
            set(build_options)
            if(rebuild STREQUAL "clean")
                list(APPEND build_options --clean-first)
            else()
                file(REMOVE "${build}/common/generated/error_catalog.cpp")
            endif()
            execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --config "${CONFIG}"
                ${build_options}
                RESULT_VARIABLE rebuilt OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 60)
            if(NOT rebuilt STREQUAL "0")
                message(FATAL_ERROR "${rebuild}: catalog rebuild failed: ${output}${errors}")
            endif()
            execute_process(COMMAND "${CTEST_COMMAND}" --test-dir "${build}" -C "${CONFIG}" --output-on-failure
                RESULT_VARIABLE retested OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 15)
            if(NOT retested STREQUAL "0")
                message(FATAL_ERROR "${rebuild}: catalog query failed: ${output}${errors}")
            endif()
        endforeach()
        # A different selected symbol must update the linked query without cleaning.
        execute_process(COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}" -B "${build}"
            -DFIXTURE_ALTERNATE=ON
            RESULT_VARIABLE updated OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 45)
        if(NOT updated STREQUAL "0")
            message(FATAL_ERROR "catalog update configuration failed: ${output}${errors}")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --config "${CONFIG}"
            RESULT_VARIABLE rebuilt OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 60)
        if(NOT rebuilt STREQUAL "0")
            message(FATAL_ERROR "incremental catalog rebuild failed: ${output}${errors}")
        endif()
        execute_process(COMMAND "${CTEST_COMMAND}" --test-dir "${build}" -C "${CONFIG}" --output-on-failure
            RESULT_VARIABLE retested OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 15)
        if(NOT retested STREQUAL "0")
            message(FATAL_ERROR "incremental catalog query failed: ${output}${errors}")
        endif()
    elseif(built STREQUAL "0" OR NOT "${output}${errors}" MATCHES "Invalid or duplicate error catalog")
        message(FATAL_ERROR "${case}: wrong compile result: ${output}${errors}")
    endif()
endforeach()
