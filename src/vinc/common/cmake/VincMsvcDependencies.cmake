# Repair localized MSVC dependency discovery without requiring an English language pack.
include_guard(GLOBAL)

function(_vinc_configure_msvc_dependencies)
    if(NOT CMAKE_HOST_WIN32 OR NOT CMAKE_GENERATOR MATCHES "Ninja"
            OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        return()
    endif()

    set(probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/VincShowIncludes")
    file(MAKE_DIRECTORY "${probe_dir}")
    # The non-ASCII filename verifies the output encoding instead of accepting
    # a plausible-looking, but corrupted, localized prefix.
    file(WRITE "${probe_dir}/vinc_標頭.hpp" "\n")
    file(WRITE "${probe_dir}/probe.cpp" "#include \"vinc_標頭.hpp\"\n")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" ${CMAKE_CXX_COMPILER_ARG1}
            /nologo /utf-8 /showIncludes /c probe.cpp /Foprobe.obj
        WORKING_DIRECTORY "${probe_dir}"
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        RESULT_VARIABLE result
        ENCODING UTF-8
        TIMEOUT 30)
    file(WRITE "${probe_dir}/output.log" "${output}\n${errors}")
    if(NOT result STREQUAL "0" OR NOT output MATCHES
            "(^|\n)([^\r\n]+:[ \t]+)[^\r\n]*vinc_標頭[.]hpp")
        message(FATAL_ERROR
            "Cannot verify UTF-8 MSVC /showIncludes output. Use a UTF-8 developer "
            "console and a UTF-8-capable MSVC toolchain. See ${probe_dir}/output.log")
    endif()

    # Ninja reads this variable from the TOP directory. A child SDK directory
    # alone cannot fix dependencies of its consuming executable. Repeat on each
    # configure because CMake's compiler cache may retain a misdecoded prefix.
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# Run after all project()/enable_language() calls, which can reload the old
# compiler detection result. This also covers add_subdirectory consumers.
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _vinc_configure_msvc_dependencies)
