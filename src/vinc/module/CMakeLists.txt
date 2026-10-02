# Build central supervision and the shared child runtime. The helper function
# creates a separate executable per module and records it in the central launch catalog.

cmake_minimum_required(VERSION 3.20)
project(module LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SOURCES
    src/module_manager.cpp
    src/base_module.cpp
    src/module_runtime.cpp
)

add_library(${PROJECT_NAME} ${SOURCES})
target_link_libraries(${PROJECT_NAME} PUBLIC vinc::control vinc::bus vinc::output)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../bus/src")

# The framework provides main(); a module only registers its BaseModule subclass.
function(vinc_add_process_module TARGET)
    cmake_parse_arguments(MOD "" "MODULE_TYPE_NAME;MODULE_BASE_NAME" "SOURCES" ${ARGN})
    if(NOT MOD_MODULE_TYPE_NAME OR NOT MOD_MODULE_BASE_NAME OR NOT MOD_SOURCES)
        message(FATAL_ERROR "vinc_add_process_module requires MODULE_TYPE_NAME, MODULE_BASE_NAME and SOURCES")
    endif()
    if(NOT MOD_MODULE_TYPE_NAME MATCHES "^[A-Za-z_][A-Za-z0-9_]*$" OR NOT MOD_MODULE_BASE_NAME MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "Build catalog names must be ASCII identifiers; runtime registration also supports UTF-8 names")
    endif()
    add_executable(${TARGET} ${MOD_SOURCES} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/module_main.cpp")
    target_link_libraries(${TARGET} PRIVATE module)
    set_property(GLOBAL APPEND PROPERTY VINC_PROCESS_TARGETS ${TARGET})
    # Record the built executable's absolute path. This is a build-tree catalog,
    # not yet a relocatable installation manifest.
    set_property(GLOBAL APPEND_STRING PROPERTY VINC_PROCESS_CATALOG
        "    if (!manager.register_module_type({\"${MOD_MODULE_TYPE_NAME}\", std::filesystem::path(u8\"$<TARGET_FILE:${TARGET}>\"), \"${MOD_MODULE_BASE_NAME}\"})) throw std::runtime_error(\"module catalog registration failed\");\n")
endfunction()

target_include_directories(${PROJECT_NAME} PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${vinc_SOURCE_DIR}/include
)

message(STATUS "[Holon] CMAKE_SOURCE_DIR: ${CMAKE_SOURCE_DIR}")

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
