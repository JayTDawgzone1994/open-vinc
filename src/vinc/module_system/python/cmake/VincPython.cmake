include_guard(GLOBAL)

# Compatible applications share one binding image. Source and installed SDK builds
# compile this adapter against their own message catalog and the same Runtime.
function(vinc_build_python_bindings target source_root pybind_root output_directory)
    cmake_parse_arguments(PARSE_ARGV 4 BINDING "SOURCE_CHECKOUT" "" "")
    find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module)
    execute_process(COMMAND "${Python_EXECUTABLE}" -c
        "import platform, struct, sysconfig; print(platform.python_implementation()); print(struct.calcsize('P')); print(sysconfig.get_platform()); print(int(bool(sysconfig.get_config_var('Py_GIL_DISABLED'))))"
        OUTPUT_VARIABLE abi OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    string(REPLACE "\n" ";" abi "${abi}")
    list(GET abi 0 implementation)
    list(GET abi 1 pointer_bytes)
    list(GET abi 2 python_platform)
    list(GET abi 3 free_threaded)
    if(NOT implementation STREQUAL "CPython" OR NOT pointer_bytes EQUAL 8 OR free_threaded)
        message(FATAL_ERROR "Vinc bindings require 64-bit, GIL-enabled CPython")
    endif()
    if(MSVC AND NOT python_platform MATCHES "^win-")
        message(FATAL_ERROR "MSVC bindings require Windows CPython; set Python_EXECUTABLE")
    endif()
    if(TARGET pybind11::module)
        message(FATAL_ERROR "Vinc Python bindings must initialize their pinned pybind11 before other pybind11 consumers")
    endif()
    # Keep upstream options scoped here; never rewrite the parent's cache or
    # trigger dependency downloads. Installed material has no Git dependency.
    set(PYBIND11_TEST OFF)
    set(PYBIND11_INSTALL OFF)
    set(PYBIND11_FINDPYTHON ON)
    set(PYBIND11_NOPYTHON OFF)
    if(BINDING_SOURCE_CHECKOUT)
        add_subdirectory("${pybind_root}/.." "${CMAKE_CURRENT_BINARY_DIR}/vinc-pybind11" EXCLUDE_FROM_ALL)
    else()
        add_subdirectory("${pybind_root}" "${CMAKE_CURRENT_BINARY_DIR}/vinc-pybind11" EXCLUDE_FROM_ALL)
    endif()
    pybind11_add_module(${target} MODULE NO_EXTRAS
        "${source_root}/src/bindings.cpp" "${source_root}/src/services.cpp"
        "${source_root}/src/messages.cpp" "${source_root}/src/rpc.cpp")
    target_include_directories(${target} PRIVATE "${source_root}/include")
    target_link_libraries(${target} PRIVATE vinc::module_runtime vinc::errors vinc::messages)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME _native CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        LIBRARY_OUTPUT_DIRECTORY "${output_directory}"
        RUNTIME_OUTPUT_DIRECTORY "${output_directory}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /bigobj)
    endif()
    vinc_deploy_licenses(${target})
    set(resources __init__.py bootstrap.py _windows_runtime.py _build.py)
    foreach(file __init__.py bootstrap.py _windows_runtime.py)
        file(GENERATE OUTPUT "${output_directory}/${file}" INPUT "${source_root}/vinc/${file}")
    endforeach()
    set(msvc_runtime False)
    if(MSVC)
        set(msvc_runtime True)
        set(metadata_script "${source_root}/cmake/runtime_metadata.py")
        get_filename_component(msvc_binary_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        get_filename_component(msvc_toolset_dir "${msvc_binary_dir}/../../.." ABSOLUTE)
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
            "${metadata_script}" "${msvc_toolset_dir}/include/crtversion.h")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${Python_EXECUTABLE}" "${metadata_script}"
                --compiler "${CMAKE_CXX_COMPILER}" --native "$<TARGET_FILE:${target}>"
                --config "$<CONFIG>" --output "${output_directory}/_msvc_runtime.json"
            BYPRODUCTS "${output_directory}/_msvc_runtime.json" VERBATIM)
        list(APPEND resources _msvc_runtime.json)
    endif()
    file(GENERATE OUTPUT "${output_directory}/_build.py" CONTENT
        "VERSION = (${Python_VERSION_MAJOR}, ${Python_VERSION_MINOR})\nPOINTER_BYTES = ${pointer_bytes}\nPLATFORM = '${python_platform}'\nMSVC_RUNTIME = ${msvc_runtime}\n")
    list(APPEND resources "$<TARGET_FILE_NAME:${target}>")
    foreach(name LICENSE NOTICE THIRD_PARTY_NOTICES.txt)
        list(APPEND resources "licenses/open-vinc/${name}")
    endforeach()
    file(GENERATE OUTPUT "${output_directory}/licenses/pybind11/LICENSE"
        INPUT "${pybind_root}/LICENSE")
    list(APPEND resources licenses/pybind11/LICENSE)
    set_property(TARGET ${target} PROPERTY VINC_PYTHON_RESOURCES "${resources}")
    set_property(TARGET ${target} PROPERTY VINC_PYTHON_SOURCE_ROOT "${source_root}")
    set_property(TARGET ${target} PROPERTY VINC_PYTHON_ABI_EXECUTABLE "${Python_EXECUTABLE}")
    set_property(TARGET ${target} PROPERTY VINC_PYTHON_DIRECTORY "${output_directory}")
    set_property(GLOBAL PROPERTY VINC_PYTHON_TEST_EXECUTABLE "${Python_EXECUTABLE}")
    set(Python_EXECUTABLE "${Python_EXECUTABLE}" PARENT_SCOPE)
endfunction()
