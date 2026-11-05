include_guard(GLOBAL)

# Native and Python Modules share one portable ModuleType namespace.
function(vinc_check_module_type name)
    if(NOT name MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "Build catalog ModuleType names must be ASCII identifiers")
    endif()
    string(TOLOWER "${name}" key)
    if(key MATCHES "^(webui|bagrecorder)$")
        message(FATAL_ERROR "ModuleType ${name} is reserved for the official App Module")
    endif()
    get_property(types GLOBAL PROPERTY VINC_PROCESS_TYPES)
    foreach(type IN LISTS types)
        string(TOLOWER "${type}" existing)
        if(key STREQUAL existing)
            message(FATAL_ERROR "Duplicate ModuleType or case-insensitive deployment collision: ${name} / ${type}")
        endif()
    endforeach()
endfunction()

# Explicit files preserve package layout without copying an entire source tree.
function(add_python_module target)
    cmake_parse_arguments(PARSE_ARGV 1 MOD "" "MODULE_TYPE_NAME" "SOURCES")
    if(MOD_UNPARSED_ARGUMENTS OR MOD_KEYWORDS_MISSING_VALUES OR
            NOT MOD_MODULE_TYPE_NAME OR NOT MOD_SOURCES)
        message(FATAL_ERROR "add_python_module requires MODULE_TYPE_NAME and SOURCES")
    endif()
    vinc_check_module_type("${MOD_MODULE_TYPE_NAME}")
    set(sources)
    set(paths)
    set(keys)
    file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" root)
    if(root MATCHES "[;|\r\n]|\\$<|\\]==\\]")
        message(FATAL_ERROR "Python Module source root contains unsupported CMake path syntax: ${root}")
    endif()
    foreach(path IN LISTS MOD_SOURCES)
        if(IS_ABSOLUTE "${path}" OR path MATCHES "(^|/)\\.\\.(/|$)|[;|\\\\\r\n]" OR
                path MATCHES "\\$<|\\]==\\]")
            message(FATAL_ERROR "Python SOURCES must be relative files inside the calling directory: ${path}")
        endif()
        cmake_path(NORMAL_PATH path)
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/${path}")
        if(NOT EXISTS "${source}" OR IS_DIRECTORY "${source}")
            message(FATAL_ERROR "Missing Python Module source file: ${source}")
        endif()
        file(REAL_PATH "${source}" resolved)
        cmake_path(IS_PREFIX root "${resolved}" NORMALIZE inside)
        if(NOT inside)
            message(FATAL_ERROR "Python Module source escapes its directory: ${path}")
        endif()
        string(TOLOWER "${path}" key)
        if(key IN_LIST keys)
            message(FATAL_ERROR "Python Module deployment paths collide: ${path}")
        endif()
        list(APPEND keys "${key}")
        list(APPEND sources "${source}")
        list(APPEND paths "${path}")
    endforeach()
    if(NOT "${MOD_MODULE_TYPE_NAME}.py" IN_LIST paths)
        message(FATAL_ERROR "Python Module requires entry ${MOD_MODULE_TYPE_NAME}.py in SOURCES")
    endif()

    # This target describes deployable files; it never executes Python or pip.
    add_custom_target(${target} SOURCES ${sources})
    set_target_properties(${target} PROPERTIES
        VINC_MODULE_TYPE "${MOD_MODULE_TYPE_NAME}"
        VINC_MODULE_KIND python
        VINC_PYTHON_MODULE_SOURCES "${sources}"
        VINC_PYTHON_MODULE_PATHS "${paths}"
        VINC_MODULE_CONFIG_SYMBOL "VINC_MODULE_${MOD_MODULE_TYPE_NAME}")
    set_property(GLOBAL APPEND PROPERTY VINC_PROCESS_TARGETS "${target}")
    set_property(GLOBAL APPEND PROPERTY VINC_PROCESS_TYPES "${MOD_MODULE_TYPE_NAME}")
endfunction()

# Plugin targets never receive main or the Host's process services.
function(vinc_add_module_plugin TARGET)
    add_library(${TARGET} MODULE ${ARGN})
    target_link_libraries(${TARGET} PRIVATE vinc::module)
    set_target_properties(${TARGET} PROPERTIES
        VINC_MODULE_PLUGIN TRUE
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    vinc_deploy_licenses(${TARGET})
endfunction()

# Register availability, not App membership. Composition selects dependencies later.
function(add_module TARGET)
    if(COMMAND vinc_prepare_installed_messages)
        vinc_prepare_installed_messages()
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 MOD "" "MODULE_TYPE_NAME;STOP_TIMEOUT_CONFIG" "SOURCES")
    if(MOD_UNPARSED_ARGUMENTS OR MOD_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "Invalid add_module arguments: ${MOD_UNPARSED_ARGUMENTS};${MOD_KEYWORDS_MISSING_VALUES}")
    endif()
    if(NOT MOD_MODULE_TYPE_NAME OR NOT MOD_SOURCES)
        message(FATAL_ERROR "add_module requires MODULE_TYPE_NAME and SOURCES")
    endif()
    vinc_check_module_type("${MOD_MODULE_TYPE_NAME}")
    if(MOD_STOP_TIMEOUT_CONFIG AND NOT MOD_STOP_TIMEOUT_CONFIG MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "STOP_TIMEOUT_CONFIG must name a vinc::config constant")
    endif()
    vinc_add_module_plugin(${TARGET} ${MOD_SOURCES})
    set_property(TARGET ${TARGET} PROPERTY EXCLUDE_FROM_ALL TRUE)
    target_link_libraries(${TARGET} PRIVATE vinc::sdk vinc::messages)
    set_target_properties(${TARGET} PROPERTIES VINC_MODULE_TYPE "${MOD_MODULE_TYPE_NAME}" VINC_MODULE_KIND native
        VINC_STOP_TIMEOUT_CONFIG "${MOD_STOP_TIMEOUT_CONFIG}")
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /utf-8)
    endif()
    set_property(GLOBAL APPEND PROPERTY VINC_PROCESS_TARGETS ${TARGET})
    set_property(GLOBAL APPEND PROPERTY VINC_PROCESS_TYPES "${MOD_MODULE_TYPE_NAME}")
    # Preserve the exact ModuleType spelling: folding case would alias distinct types.
    set_property(TARGET ${TARGET} PROPERTY VINC_MODULE_CONFIG_SYMBOL "VINC_MODULE_${MOD_MODULE_TYPE_NAME}")
endfunction()

# Catalog rendering is separate from registration so each App can supply its set.
# Production catalogs use deployed paths; isolated tests can use build-tree paths.
function(vinc_render_module_catalog output)
    cmake_parse_arguments(CATALOG "DEPLOYED" "" "" ${ARGN})
    set(catalog "")
    foreach(TARGET IN LISTS CATALOG_UNPARSED_ARGUMENTS)
        get_target_property(kind ${TARGET} VINC_MODULE_KIND)
        if(kind STREQUAL "python")
            continue()
        endif()
        get_target_property(MOD_MODULE_TYPE_NAME ${TARGET} VINC_MODULE_TYPE)
        get_target_property(MOD_STOP_TIMEOUT_CONFIG ${TARGET} VINC_STOP_TIMEOUT_CONFIG)
        set(plugin_path "std::filesystem::path(u8\"$<TARGET_FILE:${TARGET}>\")")
        if(CATALOG_DEPLOYED)
            set(plugin_path "directory / std::filesystem::path(u8\"modules/native/${MOD_MODULE_TYPE_NAME}/$<TARGET_FILE_NAME:${TARGET}>\")")
        endif()
        set(policy_adjustment "")
        if(MOD_STOP_TIMEOUT_CONFIG)
            set(policy_adjustment "policy.stop_timeout = vinc::config::${MOD_STOP_TIMEOUT_CONFIG};")
        endif()
        get_target_property(system_name ${TARGET} VINC_SYSTEM_MODULE_NAME)
        get_target_property(system_toggle ${TARGET} VINC_SYSTEM_MODULE_TOGGLE)
        set(registration "manager.register_module_type")
        set(registration_prefix "")
        set(registration_suffix "")
        set(system_options "")
        if(system_name)
            set(system_options "vinc::detail::SystemModuleOptions system;\n        system.name = \"${system_name}\";")
            if(system_toggle)
                string(APPEND system_options "\n        system.enabled = true;")
            endif()
            set(registration "vinc::detail::ManagerAccess::register_system_module")
            set(registration_prefix "manager, ")
            set(registration_suffix ", std::move(system)")
        endif()
        set(catalog_entry [=[
    {
        vinc::ModulePolicy policy;
        @policy_adjustment@
        @system_options@
        auto registered = @registration@(@registration_prefix@{
            "@MOD_MODULE_TYPE_NAME@",
            @plugin_path@,
            policy}@registration_suffix@);
        if (!registered) {
            return std::unexpected(registered.error());
        }
    }

]=])
        # file(GENERATE) resolves the selected target's path per build configuration.
        string(CONFIGURE "${catalog_entry}" catalog_entry @ONLY)
        string(APPEND catalog "${catalog_entry}")
    endforeach()
    set(${output} "${catalog}" PARENT_SCOPE)
endfunction()
