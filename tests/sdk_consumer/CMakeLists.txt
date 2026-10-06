# Standalone consuming project: no manually supplied component include directories.
cmake_minimum_required(VERSION 3.20)
project(vinc_external_consumer LANGUAGES CXX)
if(NOT VINC_SOURCE_DIR)
    message(FATAL_ERROR "Set VINC_SOURCE_DIR to the Open-VINC repository root")
endif()
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(VINC_BUILD_APPS ON CACHE BOOL "" FORCE)
set(VINC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_subdirectory("${VINC_SOURCE_DIR}/src" open-vinc)

add_executable(external_consumer main.cpp)
target_link_libraries(external_consumer PRIVATE vinc::sdk vinc::messages)

# A public-header dependency of an external SDK target must survive nested
# configure/build invocations and localized compiler diagnostics.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/incremental/value.hpp"
    "#pragma once\ninline constexpr int sdk_incremental_value = 1;\n")
add_executable(sdk_incremental_probe incremental.cpp)
target_include_directories(sdk_incremental_probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/incremental")
target_link_libraries(sdk_incremental_probe PRIVATE vinc::sdk)
add_dependencies(external_consumer sdk_incremental_probe)
vinc_add_process_module(external_module
    MODULE_TYPE_NAME ExternalModule MODULE_BASE_NAME externalNode SOURCES module.cpp)

# Change the central executable's output AFTER the framework has registered its
# deployment target. Both default copy targets resolve to the same directory.
set(central_directory "${CMAKE_CURRENT_BINARY_DIR}/central bin")
set_target_properties(shell external_consumer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${central_directory}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${central_directory}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${central_directory}")
vinc_deploy_tcl(external_consumer)
# Explicit destinations remain available to framework packaging code.
vinc_deploy_tcl(shell "${CMAKE_CURRENT_BINARY_DIR}/explicit resources")
add_dependencies(external_consumer external_module)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/executables.cmake" CONTENT
    "set(central [==[$<TARGET_FILE:shell>]==])\nset(consumer [==[$<TARGET_FILE:external_consumer>]==])\nset(module [==[$<TARGET_FILE:external_module>]==])\nset(incremental_probe [==[$<TARGET_FILE:sdk_incremental_probe>]==])\n")

# A child runtime must not acquire the central manager or output service through its target.
get_target_property(runtime_links vinc_module LINK_LIBRARIES)
if(runtime_links MATCHES "module_manager|vinc::output_hub")
    message(FATAL_ERROR "Child runtime unexpectedly links central management: ${runtime_links}")
endif()
