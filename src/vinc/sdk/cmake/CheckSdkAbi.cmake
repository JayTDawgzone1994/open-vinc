# Native-only, single-configuration SDKs deliberately reject implicit fallback.
# This probe checks the selected global toolchain/stdlib flags; arbitrary later
# per-target ABI overrides remain outside the matching-build contract.
function(vinc_check_sdk_abi output)
    if(CMAKE_CROSSCOMPILING OR CMAKE_CONFIGURATION_TYPES)
        message(FATAL_ERROR "Vinc SDK currently requires a native single-config build")
    endif()
    if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release)$")
        message(FATAL_ERROR "Vinc SDK requires explicit CMAKE_BUILD_TYPE=Debug or Release")
    endif()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Vinc SDK requires a 64-bit toolchain")
    endif()
    set(CMAKE_TRY_COMPILE_CONFIGURATION "${CMAKE_BUILD_TYPE}")
    # Use ordinary CMake compiler selection, including the consumer's stdlib
    # flags. The temporary executable has no framework services or dependencies.
    unset(vinc_abi_exit CACHE)
    unset(vinc_abi_compiled CACHE)
    try_run(vinc_abi_exit vinc_abi_compiled
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/VincSdkAbi"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sdk_abi.cpp"
        CMAKE_FLAGS "-DCMAKE_CXX_STANDARD=23" "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
            "-DCMAKE_CXX_EXTENSIONS=OFF"
        COMPILE_OUTPUT_VARIABLE compiler_output
        RUN_OUTPUT_VARIABLE fingerprint)
    if(NOT vinc_abi_compiled OR NOT vinc_abi_exit STREQUAL "0")
        message(FATAL_ERROR "Cannot validate Vinc SDK toolchain: ${compiler_output}")
    endif()
    string(STRIP "${fingerprint}" fingerprint)
    set(${output} "${fingerprint}" PARENT_SCOPE)
endfunction()
