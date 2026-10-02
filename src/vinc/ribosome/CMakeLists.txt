# Build the .msg compiler before generated-header consumers.
# Configured paths select defaults; explicit generator arguments can override them.

cmake_minimum_required(VERSION 3.20)
project(ribosome LANGUAGES CXX)

# Use the same language level as the framework.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
# Public generator declarations.
# Supports includes such as <ribosome/ribosome.hpp>.
include_directories(include)

# Generator executable sources.
# The current entry point is in ribosome.cpp.
set(SOURCES
    src/ribosome.cpp
    # Add a separate entry-point source here only if main is moved out.
)

# Build the compiler invoked by generate_messages.
add_executable(${PROJECT_NAME} ${SOURCES})

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ribosome/ribosome_config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ribosome_config.hpp"
    @ONLY
)

target_include_directories(ribosome PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

# Historical filesystem-library override, retained as a commented example.
# target_link_libraries(${PROJECT_NAME} stdc++fs)

# Enable compiler diagnostics for the generator.
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
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
endif()
