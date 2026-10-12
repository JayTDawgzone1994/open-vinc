# Opt-in bindings: neither the central nor ordinary C++ consumers link Python.
find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module)
set_property(GLOBAL PROPERTY VINC_PYTHON_TEST_EXECUTABLE "${Python_EXECUTABLE}")
execute_process(COMMAND "${Python_EXECUTABLE}" -c
    "import sysconfig; print(sysconfig.get_platform()); print(int(bool(sysconfig.get_config_var('Py_GIL_DISABLED'))))"
    OUTPUT_VARIABLE python_abi OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
string(REPLACE "\n" ";" python_abi "${python_abi}")
list(GET python_abi 0 python_platform)
list(GET python_abi 1 python_free_threaded)
if(python_free_threaded)
    message(FATAL_ERROR "VINC Python requires a GIL-enabled CPython build")
endif()
if(MSVC AND NOT python_platform MATCHES "^win-")
    message(FATAL_ERROR "MSVC bindings require Windows CPython, not the MinGW Python ABI; set Python_EXECUTABLE")
endif()
add_subdirectory("${VINC_REPOSITORY_ROOT}/library/pybind11" "${CMAKE_CURRENT_BINARY_DIR}/pybind11")
pybind11_add_module(vinc_python_native MODULE NO_EXTRAS src/bindings.cpp src/services.cpp src/messages.cpp)
target_include_directories(vinc_python_native PRIVATE include)
target_link_libraries(vinc_python_native PRIVATE vinc::module_runtime vinc::errors vinc_messages)
target_compile_features(vinc_python_native PRIVATE cxx_std_23)
set_target_properties(vinc_python_native PROPERTIES
    OUTPUT_NAME _native CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(vinc_python_native PRIVATE /W4 /utf-8 /bigobj)
endif()

# Kept outside data/module: the application directory must never shadow the SDK.
set(python_package_dir "${VINC_BUILD_ROOT}/python/$<CONFIG>/vinc")
set_target_properties(vinc_python_native PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${python_package_dir}"
    RUNTIME_OUTPUT_DIRECTORY "${python_package_dir}")
foreach(file IN ITEMS __init__.py bootstrap.py)
    file(GENERATE OUTPUT "${python_package_dir}/${file}"
        INPUT "${CMAKE_CURRENT_SOURCE_DIR}/vinc/${file}")
endforeach()
file(GENERATE OUTPUT "${python_package_dir}/_build.py" CONTENT
    "VERSION = (${Python_VERSION_MAJOR}, ${Python_VERSION_MINOR})\nPOINTER_BYTES = ${CMAKE_SIZEOF_VOID_P}\nPLATFORM = '${python_platform}'\n")

# Deployment is attached to the final application target, so relocated SDKs work.
function(vinc_deploy_python target)
    set(python_package_dir "${VINC_BUILD_ROOT}/python/$<CONFIG>/vinc")
    add_dependencies(${target} vinc_python_native)
    add_custom_target(${target}_python_deployment ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/python/vinc"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${python_package_dir}" "$<TARGET_FILE_DIR:${target}>/python/vinc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/licenses/pybind11"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${VINC_REPOSITORY_ROOT}/library/pybind11/upstream/LICENSE"
            "$<TARGET_FILE_DIR:${target}>/licenses/pybind11/LICENSE"
        DEPENDS vinc_python_native VERBATIM)
    add_dependencies(${target} ${target}_python_deployment)
endfunction()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
