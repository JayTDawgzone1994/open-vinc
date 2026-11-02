include_guard(GLOBAL)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../common/cmake/VincPythonRequirements.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/../../../common/cmake/VincPythonSelection.cmake")
else()
    include("${CMAKE_CURRENT_LIST_DIR}/../../../../lib/cmake/vinc/VincPythonSelection.cmake")
endif()

# Compatible applications share one binding image. Source and installed SDK builds
# compile this adapter against their own message catalog and the same Runtime.
function(vinc_build_python_bindings target source_root pybind_root output_directory)
    cmake_parse_arguments(PARSE_ARGV 4 BINDING "SOURCE_CHECKOUT" "" "")
    if(NOT VINC_SELECTED_PYTHON)
        message(FATAL_ERROR "Internal error: resolve App Kconfig before creating Python bindings")
    endif()
    # FindPython receives an explicit internal selection, never a user cache input.
    set(Python_EXECUTABLE "${VINC_SELECTED_PYTHON}")
    vinc_find_python(Python REQUIRED COMPONENTS Interpreter Development.Module)
    vinc_python_minimum(python_minimum)
    string(REPLACE "." ", " python_minimum_tuple "${python_minimum}")
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
        message(FATAL_ERROR "MSVC bindings require Windows CPython; select it in App Kconfig")
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
    set(runtime_mode dynamic)
    if(MSVC)
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/VincPythonRuntime.cmake")
        vinc_python_runtime_targets(runtime_target errors_target)
        vinc_apply_runtime(${target} PYTHON)
        # This is a selected binding, not an unused archive declaration.
        set_property(TARGET ${target} PROPERTY EXCLUDE_FROM_ALL FALSE)
        target_link_libraries(${target} PRIVATE ${runtime_target} ${errors_target} vinc_message_headers)
        set(runtime_mode static)
    else()
        # Linux/macOS retain their validated system-runtime policy.
        target_link_libraries(${target} PRIVATE vinc::module_runtime vinc::errors vinc::messages)
    endif()
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME _native CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
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
        set(metadata_arguments --mode "${runtime_mode}")
        set(msvc_redist_files)
        if(runtime_mode STREQUAL "dynamic")
            execute_process(COMMAND "${Python_EXECUTABLE}" "${metadata_script}"
                --compiler "${CMAKE_CXX_COMPILER}" --locate
                OUTPUT_VARIABLE msvc_redist_dir OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
            file(GLOB_RECURSE msvc_redist_files CONFIGURE_DEPENDS "${msvc_redist_dir}/*.dll")
            list(APPEND metadata_arguments --redist-root "${msvc_redist_dir}")
        endif()
        # Track the actual deployment files, including Debug CRTs. A serviced DLL
        # must regenerate metadata even when no C++ source needs recompilation.
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
            "${metadata_script}" "${msvc_toolset_dir}/include/crtversion.h"
            "${source_root}/vinc/_windows_runtime.py" ${msvc_redist_files})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${Python_EXECUTABLE}" "${metadata_script}"
                --compiler "${CMAKE_CXX_COMPILER}" --native "$<TARGET_FILE:${target}>"
                ${metadata_arguments}
                --config "$<CONFIG>" --output "${output_directory}/_msvc_runtime.json"
            BYPRODUCTS "${output_directory}/_msvc_runtime.json" VERBATIM)
        list(APPEND resources _msvc_runtime.json)
    endif()
    file(GENERATE OUTPUT "${output_directory}/_build.py" CONTENT
        "MINIMUM_VERSION = (${python_minimum_tuple})\nVERSION = (${Python_VERSION_MAJOR}, ${Python_VERSION_MINOR})\nPOINTER_BYTES = ${pointer_bytes}\nPLATFORM = '${python_platform}'\nMSVC_RUNTIME = ${msvc_runtime}\nMSVC_RUNTIME_MODE = '${runtime_mode}'\n")
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
endfunction()
