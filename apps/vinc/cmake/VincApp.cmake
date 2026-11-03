include_guard(GLOBAL)

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
    # An explicit dependency orders generated headers before first compilation.
    add_custom_target(${target}_vinc_startup DEPENDS "${header}")
    add_dependencies(${target} ${target}_vinc_startup)
    target_sources(${target} PRIVATE "${header}")
    target_include_directories(${target} PRIVATE "${generated_directory}")
    target_compile_definitions(${target} PRIVATE VINC_EMBEDDED_STARTUP)
endfunction()
