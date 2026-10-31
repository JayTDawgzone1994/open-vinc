# Opt-in bindings: neither the central nor ordinary C++ consumers link Python.
include(cmake/VincPython.cmake)
vinc_build_python_bindings(vinc_python_native "${CMAKE_CURRENT_SOURCE_DIR}"
    "${VINC_REPOSITORY_ROOT}/library/pybind11/upstream"
    "${VINC_BUILD_ROOT}/python/$<CONFIG>/vinc" SOURCE_CHECKOUT)

# Deployment is attached to the final application target, so relocated SDKs work.
function(vinc_deploy_python target)
    set(python_package_dir "${VINC_BUILD_ROOT}/python/$<CONFIG>/vinc")
    get_target_property(resources vinc_python_native VINC_PYTHON_RESOURCES)
    set(manifest "${CMAKE_CURRENT_BINARY_DIR}/${target}_python_$<CONFIG>.cmake")
    file(GENERATE OUTPUT "${manifest}" CONTENT
        "set(source [==[${python_package_dir}]==])\nset(destination [==[$<TARGET_FILE_DIR:${target}>/python/vinc]==])\nset(resources [==[${resources}]==])\n")
    add_dependencies(${target} vinc_python_native)
    add_custom_target(${target}_python_deployment ALL
        COMMAND "${CMAKE_COMMAND}" "-DMANIFEST=${manifest}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cmake/CopyPython.cmake"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/licenses/pybind11"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${VINC_REPOSITORY_ROOT}/library/pybind11/upstream/LICENSE"
            "$<TARGET_FILE_DIR:${target}>/licenses/pybind11/LICENSE"
        DEPENDS vinc_python_native VERBATIM)
    add_dependencies(${target} ${target}_python_deployment)
endfunction()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
