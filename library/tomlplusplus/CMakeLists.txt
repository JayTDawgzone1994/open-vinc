# Header-only, pinned upstream; Param is the only runtime consumer.
set(toml_source "${CMAKE_CURRENT_SOURCE_DIR}/upstream")
if(NOT EXISTS "${toml_source}/include/toml++/toml.hpp")
    message(FATAL_ERROR "Initialize toml++: git submodule update --init --recursive")
endif()
if(EXISTS "${toml_source}/.git")
    find_package(Git REQUIRED)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${toml_source}" rev-parse HEAD
        OUTPUT_VARIABLE toml_actual_revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/revision.txt" toml_expected_revision)
    string(STRIP "${toml_expected_revision}" toml_expected_revision)
    if(NOT toml_actual_revision STREQUAL toml_expected_revision)
        message(FATAL_ERROR "toml++ differs from library/tomlplusplus/revision.txt")
    endif()
endif()
add_library(vinc_toml INTERFACE)
target_include_directories(vinc_toml SYSTEM INTERFACE "${toml_source}/include")
target_compile_definitions(vinc_toml INTERFACE TOML_EXCEPTIONS=0)

if(VINC_INSTALL_SDK)
    set_property(TARGET vinc_toml PROPERTY INTERFACE_INCLUDE_DIRECTORIES
        "$<BUILD_INTERFACE:${toml_source}/include>")
    set_property(TARGET vinc_toml PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
        "$<BUILD_INTERFACE:${toml_source}/include>")
    install(TARGETS vinc_toml EXPORT VincTargets ARCHIVE DESTINATION lib COMPONENT VincSDK)
    set_target_properties(vinc_toml PROPERTIES EXPORT_NAME detail_tomlplusplus)
endif()
