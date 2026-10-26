include_guard(GLOBAL)

# Keep attribution with the artifact, including separately shipped Plugins and
# Python packages. Only these framework-owned filenames are copied; the parent
# application's LICENSE and unrelated files are never overwritten or removed.
function(vinc_deploy_licenses target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "vinc_deploy_licenses requires an existing target: ${target}")
    endif()

    get_target_property(kind "${target}" TYPE)
    if(NOT kind MATCHES "^(EXECUTABLE|MODULE_LIBRARY|SHARED_LIBRARY|STATIC_LIBRARY)$")
        message(FATAL_ERROR "vinc_deploy_licenses requires a binary target: ${target}")
    endif()

    get_target_property(registered "${target}" VINC_LICENSES_DEPLOYED)
    if(registered)
        return()
    endif()

    get_filename_component(source_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../.." ABSOLUTE)
    set(deploy_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/DeployLicenses.cmake")
    foreach(name IN ITEMS LICENSE NOTICE THIRD_PARTY_NOTICES.txt)
        if(NOT EXISTS "${source_root}/${name}")
            message(FATAL_ERROR "Missing Open-VINC license input: ${source_root}/${name}")
        endif()
    endforeach()

    # An always-run prerequisite repairs missing notices without relinking the
    # binary. TARGET_FILE_DIR also follows output-directory changes made later
    # by a parent project. It creates no dependency back to the binary itself.
    add_custom_target(${target}_vinc_licenses
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_ROOT=${source_root}"
            "-DDESTINATION=$<TARGET_FILE_DIR:${target}>/licenses/open-vinc"
            -P "${deploy_script}"
        DEPENDS "${source_root}/LICENSE" "${source_root}/NOTICE"
            "${source_root}/THIRD_PARTY_NOTICES.txt" "${deploy_script}"
        VERBATIM)
    add_dependencies(${target} ${target}_vinc_licenses)
    set_property(TARGET "${target}" PROPERTY VINC_LICENSES_DEPLOYED TRUE)
endfunction()
