include_guard(GLOBAL)

# Resolve from this installed file on every call, not a caller's variable scope.
function(vinc_deploy_licenses target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "vinc_deploy_licenses requires an existing binary target")
    endif()
    get_target_property(kind "${target}" TYPE)
    if(NOT kind MATCHES "^(EXECUTABLE|MODULE_LIBRARY|SHARED_LIBRARY|STATIC_LIBRARY)$")
        message(FATAL_ERROR "vinc_deploy_licenses requires a binary target")
    endif()
    get_target_property(registered "${target}" VINC_LICENSES_DEPLOYED)
    if(registered)
        return()
    endif()
    get_filename_component(prefix "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../.." ABSOLUTE)
    add_custom_target(${target}_vinc_licenses
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_ROOT=${prefix}/share/vinc/licenses"
            "-DDESTINATION=$<TARGET_FILE_DIR:${target}>/licenses/open-vinc"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/DeployLicenses.cmake"
        VERBATIM)
    add_dependencies(${target} ${target}_vinc_licenses)
    set_property(TARGET "${target}" PROPERTY VINC_LICENSES_DEPLOYED TRUE)
endfunction()
