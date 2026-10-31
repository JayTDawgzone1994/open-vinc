include_guard(GLOBAL)

# Register source directories now; resolve the complete set after the parent
# project has added its own Modules and schemas.
function(add_message_dir directory)
    if(COMMAND vinc_prepare_installed_messages)
        vinc_prepare_installed_messages()
    endif()
    if(NOT ARGC EQUAL 1)
        message(FATAL_ERROR "add_message_dir requires exactly one directory")
    endif()
    get_filename_component(absolute "${directory}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT IS_DIRECTORY "${absolute}")
        message(FATAL_ERROR "Message directory does not exist: ${absolute}")
    endif()
    file(REAL_PATH "${absolute}" canonical)
    get_property(directories GLOBAL PROPERTY VINC_MESSAGE_DIRECTORIES)
    if(NOT canonical IN_LIST directories)
        set_property(GLOBAL APPEND PROPERTY VINC_MESSAGE_DIRECTORIES "${canonical}")
    endif()
endfunction()

function(vinc_finalize_messages)
    get_property(directories GLOBAL PROPERTY VINC_MESSAGE_DIRECTORIES)
    get_property(output GLOBAL PROPERTY VINC_MESSAGE_OUTPUT)
    get_property(template GLOBAL PROPERTY VINC_MESSAGE_TEMPLATE)
    get_property(generator GLOBAL PROPERTY VINC_MESSAGE_GENERATOR)
    if(NOT generator)
        set(generator ribosome)
    endif()
    set(arguments)
    set(outputs "${output}/all_messages.hpp" "${output}/message_types.hpp")
    set(names all_messages message_types)
    foreach(directory IN LISTS directories)
        list(APPEND arguments --input "${directory}")
        # Track additions/removals as well as contents. Schemas are deliberately
        # non-recursive: each directory is an explicit registration boundary.
        file(GLOB inputs LIST_DIRECTORIES false CONFIGURE_DEPENDS "${directory}/*.msg")
        foreach(input IN LISTS inputs)
            # Windows globbing ignores case; Ribosome accepts the literal .msg suffix.
            if(NOT input MATCHES "[.]msg$")
                continue()
            endif()
            get_filename_component(name "${input}" NAME_WE)
            string(TOLOWER "${name}" key)
            if(key IN_LIST names)
                message(FATAL_ERROR "Duplicate or reserved message name '${name}': ${input}")
            endif()
            list(APPEND names "${key}")
            list(APPEND outputs "${output}/${name}.hpp")
        endforeach()
    endforeach()
    # One writer owns the entire output tree. Keep the existing fail-closed,
    # every-build generation policy; all consumers wait on this target.
    add_custom_target(generate_messages
        COMMAND $<TARGET_FILE:${generator}> ${arguments}
            --output "${output}" --template "${template}"
        BYPRODUCTS ${outputs}
        DEPENDS ${generator}
        COMMENT "Ribosome: generate all registered message directories"
        VERBATIM)
    add_dependencies(vinc_messages generate_messages)
endfunction()
