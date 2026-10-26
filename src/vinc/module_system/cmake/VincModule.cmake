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

# Register an independently hosted Plugin in the build-tree catalog.
function(add_module TARGET)
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
    get_property(types GLOBAL PROPERTY VINC_PROCESS_TYPES)
    if(MOD_MODULE_TYPE_NAME IN_LIST types)
        message(FATAL_ERROR "Duplicate ModuleType: ${MOD_MODULE_TYPE_NAME}")
    endif()
    vinc_add_module_plugin(${TARGET} ${MOD_SOURCES})
    target_link_libraries(${TARGET} PRIVATE vinc::sdk vinc::messages)
    set_property(GLOBAL APPEND PROPERTY VINC_PROCESS_TARGETS ${TARGET})
    set_property(GLOBAL APPEND PROPERTY VINC_PROCESS_TYPES "${MOD_MODULE_TYPE_NAME}")
    # Record the built Plugin's absolute path. This is a build-tree catalog,
    # not yet a relocatable installation manifest.
    set(policy_adjustment "")
    if(MOD_STOP_TIMEOUT_CONFIG)
        if(NOT MOD_STOP_TIMEOUT_CONFIG MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
            message(FATAL_ERROR "STOP_TIMEOUT_CONFIG must name a vinc::config constant")
        endif()
        set(policy_adjustment "policy.stop_timeout = vinc::config::${MOD_STOP_TIMEOUT_CONFIG};")
    endif()
    set(catalog_entry [=[
    {
        vinc::ModulePolicy policy;
        @policy_adjustment@
        auto registered = manager.register_module_type({
            "@MOD_MODULE_TYPE_NAME@",
            std::filesystem::path(u8"$<TARGET_FILE:@TARGET@>"),
            policy});
        if (!registered) {
            return std::unexpected(registered.error());
        }
    }

]=])
    # Resolve module metadata now; file(GENERATE) resolves the target path per configuration later.
    string(CONFIGURE "${catalog_entry}" catalog_entry @ONLY)
    set_property(GLOBAL APPEND_STRING PROPERTY VINC_PROCESS_CATALOG "${catalog_entry}")
endfunction()
