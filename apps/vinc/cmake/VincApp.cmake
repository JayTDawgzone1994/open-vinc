include_guard(GLOBAL)

# Configure the existing central target after add_subdirectory(open-vinc/src).
# Instance identity is intentionally left to the earlier VINC_NAME setting.
function(vinc_configure_app)
    cmake_parse_arguments(PARSE_ARGV 0 APP "" "NAME;STARTUP_SCRIPT" "")
    if(APP_UNPARSED_ARGUMENTS OR APP_KEYWORDS_MISSING_VALUES OR
       NOT APP_NAME OR NOT APP_STARTUP_SCRIPT)
        message(FATAL_ERROR "vinc_configure_app requires NAME and STARTUP_SCRIPT")
    endif()
    if(NOT TARGET shell)
        message(FATAL_ERROR "vinc_configure_app requires VINC_BUILD_APPS=ON and add_subdirectory first")
    endif()
    get_property(configured TARGET shell PROPERTY VINC_APP_CONFIGURED)
    if(configured)
        message(FATAL_ERROR "vinc_configure_app may be called only once per build")
    endif()
    if(NOT APP_NAME MATCHES "^[A-Za-z0-9_-]+$")
        message(FATAL_ERROR "Application NAME must contain ASCII letters, digits, _ or - (no extension)")
    endif()
    get_filename_component(script "${APP_STARTUP_SCRIPT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${script}" OR IS_DIRECTORY "${script}")
        message(FATAL_ERROR "Startup script is not a file: ${script}")
    endif()

    vinc_embed_startup(shell "${script}")
    set_target_properties(shell PROPERTIES OUTPUT_NAME "${APP_NAME}" VINC_APP_CONFIGURED TRUE)
endfunction()

# Shared by source and installed application composition. The caller owns the
# central target; embedding never creates another executable or runtime service.
function(vinc_embed_startup target script)
    set(generated_directory "${CMAKE_CURRENT_BINARY_DIR}/${target}_startup")
    set(header "${generated_directory}/embedded_startup.hpp")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${script}" "-DOUTPUT=${header}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedScript.cmake"
        DEPENDS "${script}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedScript.cmake"
        COMMENT "Embedding application startup script"
        VERBATIM)
    # The command belongs to the consumer directory, while shell belongs to a
    # framework subdirectory. An explicit target orders their first compilation.
    add_custom_target(${target}_vinc_startup DEPENDS "${header}")
    add_dependencies(${target} ${target}_vinc_startup)
    target_sources(${target} PRIVATE "${header}")
    target_include_directories(${target} PRIVATE "${generated_directory}")
    target_compile_definitions(${target} PRIVATE VINC_EMBEDDED_STARTUP)
endfunction()
