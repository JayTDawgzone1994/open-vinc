include_guard(GLOBAL)

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
    if(NOT MOD_MODULE_TYPE_NAME MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "Build catalog ModuleType names must be ASCII identifiers")
    endif()
    if(MOD_STOP_TIMEOUT_CONFIG AND NOT MOD_STOP_TIMEOUT_CONFIG MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "STOP_TIMEOUT_CONFIG must name a vinc::config constant")
    endif()
    get_property(types GLOBAL PROPERTY VINC_PROCESS_TYPES)
    if(MOD_MODULE_TYPE_NAME IN_LIST types)
        message(FATAL_ERROR "Duplicate ModuleType: ${MOD_MODULE_TYPE_NAME}")
    endif()
    vinc_add_module_plugin(${TARGET} ${MOD_SOURCES})
    set_property(TARGET ${TARGET} PROPERTY EXCLUDE_FROM_ALL TRUE)
    target_link_libraries(${TARGET} PRIVATE vinc::sdk vinc::messages)
    set_target_properties(${TARGET} PROPERTIES VINC_MODULE_TYPE "${MOD_MODULE_TYPE_NAME}"
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
# The legacy source App still passes all registered targets until composition migrates.
function(vinc_render_module_catalog output)
    cmake_parse_arguments(CATALOG "DEPLOYED" "" "" ${ARGN})
    set(catalog "")
    foreach(TARGET IN LISTS CATALOG_UNPARSED_ARGUMENTS)
        get_target_property(MOD_MODULE_TYPE_NAME ${TARGET} VINC_MODULE_TYPE)
        get_target_property(MOD_STOP_TIMEOUT_CONFIG ${TARGET} VINC_STOP_TIMEOUT_CONFIG)
        set(plugin_path "std::filesystem::path(u8\"$<TARGET_FILE:${TARGET}>\")")
        if(CATALOG_DEPLOYED)
            set(plugin_path "directory / std::filesystem::path(u8\"modules/${MOD_MODULE_TYPE_NAME}/$<TARGET_FILE_NAME:${TARGET}>\")")
        endif()
        set(policy_adjustment "")
        if(MOD_STOP_TIMEOUT_CONFIG)
            set(policy_adjustment "policy.stop_timeout = vinc::config::${MOD_STOP_TIMEOUT_CONFIG};")
        endif()
        set(catalog_entry [=[
    {
        vinc::ModulePolicy policy;
        @policy_adjustment@
        auto registered = manager.register_module_type({
            "@MOD_MODULE_TYPE_NAME@",
            @plugin_path@,
            policy});
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
