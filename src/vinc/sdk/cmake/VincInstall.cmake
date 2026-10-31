include_guard(GLOBAL)

# Components opt in explicitly. Preserve their build-tree usage requirements;
# installed headers share one prefix without exporting source/build paths.
function(vinc_install_component)
    if(NOT VINC_INSTALL_SDK)
        return()
    endif()
    cmake_parse_arguments(PARSE_ARGV 0 COMPONENT "" "" "TARGETS")
    if(COMPONENT_UNPARSED_ARGUMENTS OR NOT COMPONENT_TARGETS)
        message(FATAL_ERROR "vinc_install_component requires TARGETS")
    endif()
    foreach(target IN LISTS COMPONENT_TARGETS)
        get_target_property(includes ${target} INTERFACE_INCLUDE_DIRECTORIES)
        set(build_includes)
        if(includes)
            foreach(directory IN LISTS includes)
                list(APPEND build_includes "$<BUILD_INTERFACE:${directory}>")
            endforeach()
        endif()
        set_property(TARGET ${target} PROPERTY INTERFACE_INCLUDE_DIRECTORIES
            "${build_includes};$<INSTALL_INTERFACE:include>")
        string(REGEX REPLACE "^vinc_" "" export_name "${target}")
        set_target_properties(${target} PROPERTIES EXPORT_NAME "${export_name}")
    endforeach()
    install(TARGETS ${COMPONENT_TARGETS} EXPORT VincTargets
        ARCHIVE DESTINATION lib COMPONENT VincSDK
        LIBRARY DESTINATION lib COMPONENT VincSDK
        RUNTIME DESTINATION bin COMPONENT VincSDK)
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include")
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/" DESTINATION include
            COMPONENT VincSDK FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
    endif()
endfunction()
