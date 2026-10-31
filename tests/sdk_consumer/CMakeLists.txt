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
set(parent_cxx_flags "${CMAKE_CXX_FLAGS}")
set(parent_build_type "${CMAKE_BUILD_TYPE}")
set(parent_crt "${CMAKE_MSVC_RUNTIME_LIBRARY}")
set(BUILD_SHARED_LIBS ON)
add_subdirectory("${VINC_SOURCE_DIR}/src" open-vinc)
foreach(forbidden vinc_module_host vinc_module_entry vinc_test_host vinc_test_manager)
    if(TARGET ${forbidden})
        message(FATAL_ERROR "Production source composition leaked Host/test target: ${forbidden}")
    endif()
endforeach()
if(VINC_TEST_EMBEDDED_STARTUP)
    vinc_configure_app(NAME EmbeddedApp STARTUP_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/startup.tcl")
endif()

# Keep mutable schemas in the test build directory. Reconfiguration must not
# overwrite edits made by the incremental regression driver.
set(schema_root "${CMAKE_CURRENT_BINARY_DIR}/message-inputs")
if(NOT EXISTS "${schema_root}/initialized")
    file(MAKE_DIRECTORY "${schema_root}/common" "${schema_root}/camera")
    file(WRITE "${schema_root}/common/ExternalLeaf.msg" "uint32_t revision = 1;\nuint32_t value;\n")
    file(WRITE "${schema_root}/camera/ExternalFrame.msg" "ExternalLeaf leaf;\n")
    file(WRITE "${schema_root}/initialized" "owned by sdk.external_consumer\n")
endif()
add_message_dir("${schema_root}/common")
file(RELATIVE_PATH camera_messages "${CMAKE_CURRENT_SOURCE_DIR}" "${schema_root}/camera")
add_message_dir("${camera_messages}")
add_message_dir("${schema_root}/common/.")

add_executable(external_message_probe messages.cpp)
target_link_libraries(external_message_probe PRIVATE vinc::messages)
if(NOT BUILD_SHARED_LIBS OR NOT CMAKE_CXX_STANDARD EQUAL 23 OR CMAKE_CXX_EXTENSIONS OR
   NOT CMAKE_CXX_STANDARD_REQUIRED OR NOT "${CMAKE_CXX_FLAGS}" STREQUAL "${parent_cxx_flags}" OR
   NOT "${CMAKE_BUILD_TYPE}" STREQUAL "${parent_build_type}" OR NOT "${CMAKE_MSVC_RUNTIME_LIBRARY}" STREQUAL "${parent_crt}")
    message(FATAL_ERROR "Framework/dependency CMake changed parent build settings")
endif()
get_target_property(spdlog_type spdlog TYPE)
get_target_property(fmt_type fmt TYPE)
if(NOT spdlog_type STREQUAL "STATIC_LIBRARY" OR NOT fmt_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "Log dependencies must remain private static implementation choices")
endif()

add_executable(external_consumer main.cpp)
target_link_libraries(external_consumer PRIVATE vinc::sdk vinc::messages)
vinc_deploy_licenses(external_consumer)

# A public-header dependency of an external SDK target must survive nested
# configure/build invocations and localized compiler diagnostics.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/incremental/value.hpp"
    "#pragma once\ninline constexpr int sdk_incremental_value = 1;\n")
add_executable(sdk_incremental_probe incremental.cpp)
target_include_directories(sdk_incremental_probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/incremental")
target_link_libraries(sdk_incremental_probe PRIVATE vinc::sdk)
add_dependencies(external_consumer sdk_incremental_probe)
add_module(external_module
    MODULE_TYPE_NAME ExternalModule SOURCES module.cpp)
if(VINC_TEST_DUPLICATE_MODULE)
    add_module(duplicate_module MODULE_TYPE_NAME ExternalModule SOURCES module.cpp)
endif()
add_dependencies(external_consumer external_message_probe)

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
    "set(central [==[$<TARGET_FILE:shell>]==])\nset(consumer [==[$<TARGET_FILE:external_consumer>]==])\nset(module [==[$<TARGET_FILE:external_module>]==])\nset(incremental_probe [==[$<TARGET_FILE:sdk_incremental_probe>]==])\nset(message_probe [==[$<TARGET_FILE:external_message_probe>]==])\n")

# A child runtime must not acquire the central manager or output service through its target.
get_target_property(runtime_links vinc_module LINK_LIBRARIES)
if(runtime_links MATCHES "module_manager|vinc::output_hub")
    message(FATAL_ERROR "Child runtime unexpectedly links central management: ${runtime_links}")
endif()
