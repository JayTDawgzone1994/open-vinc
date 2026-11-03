include_guard(GLOBAL)

# Inspect actual files rather than trusting the install rules or an old manifest.
# The SDK has one build-time tool; an application has one process executable and
# native Plugins. Test helpers and the retired standalone Host belong to neither.
function(verify_package_inventory root kind executable report)
    if(NOT IS_DIRECTORY "${root}" OR NOT EXISTS "${root}/${executable}")
        message(FATAL_ERROR "Missing ${kind} package/executable: ${root}/${executable}")
    endif()
    if(NOT kind STREQUAL "sdk" AND NOT kind STREQUAL "app")
        message(FATAL_ERROR "Unknown inventory kind: ${kind}")
    endif()
    file(GLOB_RECURSE files LIST_DIRECTORIES FALSE RELATIVE "${root}" "${root}/*")
    list(SORT files)
    set(inventory "# ${kind} inventory: SHA-256  relative path\n")
    set(entry_found FALSE)
    foreach(relative IN LISTS files)
        get_filename_component(name "${relative}" NAME)
        string(TOLOWER "${name}" lower_name)
        if(lower_name MATCHES "^(modulehost|vinc_module_host|vinc_test_host)(\\..*)?$"
                OR lower_name STREQUAL "module_main.cpp"
                OR lower_name STREQUAL "host_path.hpp"
                OR lower_name MATCHES "_tests(\\.exe)?$")
            message(FATAL_ERROR "Unexpected Host/test artifact: ${relative}")
        endif()
        file(READ "${root}/${relative}" magic LIMIT 4 HEX)
        # PE, ELF and thin/fat Mach-O. Archives and source/resources are not images.
        if(magic MATCHES "^(4d5a|7f454c46|feedface|cefaedfe|feedfacf|cffaedfe|cafebabe|bebafeca|cafebabf|bfbafeca)")
            if(relative STREQUAL executable)
                set(entry_found TRUE)
            elseif(kind STREQUAL "app" AND relative MATCHES "^modules/[^/]+/[^/]+\\.(dll|so|dylib)$")
                # Native Plugins are libraries in their catalog-owned directories.
            else()
                message(FATAL_ERROR "Unexpected native image in ${kind}: ${relative}")
            endif()
        elseif(lower_name MATCHES "\\.exe$")
            message(FATAL_ERROR "Unexpected or invalid executable: ${relative}")
        endif()
        file(SHA256 "${root}/${relative}" digest)
        string(APPEND inventory "${digest}  ${relative}\n")
    endforeach()
    if(NOT entry_found)
        message(FATAL_ERROR "Expected native executable was not found: ${executable}")
    endif()
    file(WRITE "${report}" "${inventory}")
    list(LENGTH files file_count)
    message(STATUS "Verified ${kind} inventory: ${file_count} files, one process executable")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    verify_package_inventory("${INVENTORY_ROOT}" "${INVENTORY_KIND}"
        "${INVENTORY_EXECUTABLE}" "${INVENTORY_REPORT}")
endif()
