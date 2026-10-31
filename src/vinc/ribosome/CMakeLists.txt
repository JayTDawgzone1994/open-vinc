# Build the .msg compiler before generated-header consumers.
# Configured paths select defaults; explicit generator arguments can override them.

cmake_minimum_required(VERSION 3.20)
project(ribosome LANGUAGES CXX)

# Use the same language level as the framework.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generator executable sources.
# The current entry point is in ribosome.cpp.
set(SOURCES
    src/ribosome.cpp
    # Add a separate entry-point source here only if main is moved out.
)

# Build the compiler invoked by generate_messages.
add_executable(${PROJECT_NAME} ${SOURCES})

# Escape CMake paths as C++ UTF-8 literals, including standalone builds.
function(ribosome_path_literal output value)
    string(REPLACE "\\" "\\\\" escaped "${value}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    set(${output} "${escaped}" PARENT_SCOPE)
endfunction()
ribosome_path_literal(ribosome_input_literal "${VINC_MESSAGE_INPUT_DIR}")
ribosome_path_literal(ribosome_output_literal "${VINC_MESSAGE_OUTPUT_DIR}")
ribosome_path_literal(ribosome_template_literal "${CMAKE_CURRENT_SOURCE_DIR}/data/message_sample.txt")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ribosome/ribosome_config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ribosome_config.hpp.configured"
    @ONLY
)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/ribosome_config.hpp"
    INPUT "${CMAKE_CURRENT_BINARY_DIR}/ribosome_config.hpp.configured")

target_include_directories(ribosome PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")

# Historical filesystem-library override, retained as a commented example.
# target_link_libraries(${PROJECT_NAME} stdc++fs)

# Enable compiler diagnostics for the generator.
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(MINGW)
    target_link_options(ribosome PRIVATE -municode)
endif()

if(VINC_INSTALL_SDK)
    # Same generator, but no developer-machine defaults in the installed tool.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sdk-config/ribosome_config.hpp"
        "#pragma once\n#include <string_view>\nnamespace vinc {\nconstexpr std::u8string_view kDefaultInputPath{};\nconstexpr std::u8string_view kDefaultOutputPath{};\nconstexpr std::u8string_view kDefaultTemplatePath{};\n}\n")
    add_executable(vinc_ribosome_sdk src/ribosome.cpp)
    target_include_directories(vinc_ribosome_sdk PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/sdk-config")
    target_compile_definitions(vinc_ribosome_sdk PRIVATE VINC_RIBOSOME_INSTALLED_TOOL)
    set_target_properties(vinc_ribosome_sdk PROPERTIES OUTPUT_NAME ribosome
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sdk-tools")
endif()

if(BUILD_TESTING)
    add_executable(ribosome_hash_tests tests/hash_tests.cpp)
    target_include_directories(ribosome_hash_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    add_test(NAME ribosome.hash COMMAND ribosome_hash_tests)
    set_tests_properties(ribosome.hash PROPERTIES TIMEOUT 30)
    add_test(NAME ribosome.schema COMMAND ${CMAKE_COMMAND}
        "-DGENERATOR=$<TARGET_FILE:ribosome>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/schema-tests"
        "-DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/data/message_sample.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/schema_tests.cmake")
    set_tests_properties(ribosome.schema PROPERTIES TIMEOUT 60)

    # Compile real output to verify literal semantics and generated name lookup.
    set(regression_root "${CMAKE_CURRENT_BINARY_DIR}/regression/$<CONFIG>")
    set(regression_inputs
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/messages/Constants.msg"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/messages/Names.msg"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/messages/Point.msg")
    add_custom_command(OUTPUT "${regression_root}/generation.stamp"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${regression_root}"
        COMMAND $<TARGET_FILE:ribosome> --input "${CMAKE_CURRENT_SOURCE_DIR}/tests/messages"
            --output "${regression_root}/vinc_messages"
            --template "${CMAKE_CURRENT_SOURCE_DIR}/data/message_sample.txt"
        COMMAND ${CMAKE_COMMAND} -E touch "${regression_root}/generation.stamp"
        DEPENDS ribosome ${regression_inputs} "${CMAKE_CURRENT_SOURCE_DIR}/data/message_sample.txt"
        BYPRODUCTS "${regression_root}/vinc_messages/all_messages.hpp"
            "${regression_root}/vinc_messages/message_types.hpp"
            "${regression_root}/vinc_messages/Constants.hpp"
            "${regression_root}/vinc_messages/Names.hpp"
            "${regression_root}/vinc_messages/Point.hpp"
        VERBATIM)
    add_executable(ribosome_generated_tests tests/generated_tests.cpp "${regression_root}/generation.stamp")
    target_include_directories(ribosome_generated_tests PRIVATE "${regression_root}")
    target_link_libraries(ribosome_generated_tests PRIVATE vinc::message_storage)
    if(MSVC)
        target_compile_options(ribosome_generated_tests PRIVATE /W4 /utf-8 /permissive-)
    else()
        target_compile_options(ribosome_generated_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ribosome.generated COMMAND ribosome_generated_tests)
    set_tests_properties(ribosome.generated PROPERTIES TIMEOUT 20)
endif()
