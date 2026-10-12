# One component, separate child-runtime and central-management link dependencies.
cmake_minimum_required(VERSION 3.20)
project(vinc_module_system LANGUAGES CXX)

set(VINC_NAME "open-vinc" CACHE STRING "Central instance name (lowercase ASCII, digits, underscore, hyphen)")
string(LENGTH "${VINC_NAME}" vinc_name_length)
if(NOT VINC_NAME MATCHES "^[a-z0-9_-]+$" OR vinc_name_length GREATER 48)
    message(FATAL_ERROR "VINC_NAME must contain 1..48 lowercase ASCII letters, digits, _ or -")
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/vinc/module/detail/instance_name.hpp"
    CONTENT "#pragma once\n#include <string_view>\nnamespace vinc::detail { inline constexpr std::string_view instance_name = \"${VINC_NAME}\"; }\n")

# Shared private codec/ownership implementation keeps both process endpoints aligned.
add_library(vinc_module_contract STATIC src/contract_report.cpp src/runtime/runtime_services.cpp)
target_include_directories(vinc_module_contract PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_module_contract PUBLIC vinc::osal)

add_library(vinc_module STATIC src/runtime/base_module.cpp src/runtime/context.cpp)
add_library(vinc::module ALIAS vinc_module)
target_include_directories(vinc_module PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_module PUBLIC vinc::message_bus_client vinc::param vinc::log)

add_library(vinc_module_runtime STATIC src/runtime/module_runtime.cpp src/runtime/plugin_loader.cpp src/runtime/module_locks.cpp src/runtime/module_params.cpp)
add_library(vinc::module_runtime ALIAS vinc_module_runtime)
target_link_libraries(vinc_module_runtime PUBLIC vinc::module vinc::message_bus vinc::control vinc_module_contract vinc::concurrency vinc::param_host vinc::log_host)

add_library(vinc_module_manager STATIC src/host/module_manager.cpp src/host/instance.cpp src/host/lock_registry.cpp)
add_library(vinc::module_manager ALIAS vinc_module_manager)
target_include_directories(vinc_module_manager PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(vinc_module_manager PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(vinc_module_manager PUBLIC vinc::common vinc::log vinc::param vinc::message_bus vinc::control vinc::output_hub PRIVATE vinc_module_contract vinc::concurrency vinc::param_manager vinc::log_manager)

# Linking this object target supplies main/wmain without consumers reaching into src/.
add_library(vinc_module_entry OBJECT src/runtime/module_main.cpp)
add_library(vinc::module_entry ALIAS vinc_module_entry)
target_link_libraries(vinc_module_entry PUBLIC vinc::module_runtime)
add_executable(vinc_module_host)
set_target_properties(vinc_module_host PROPERTIES OUTPUT_NAME ModuleHost)
target_link_libraries(vinc_module_host PRIVATE vinc::module_entry)

# A source-build default. Deployment owners may override this in Manager::create.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/vinc/module/detail/host_path.hpp"
    CONTENT "#pragma once\n#define VINC_MODULE_HOST_PATH u8\"$<TARGET_FILE:vinc_module_host>\"\n")
target_include_directories(vinc_module_manager PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>>")
add_dependencies(vinc_module_manager vinc_module_host)

foreach(target IN ITEMS vinc_module vinc_module_manager vinc_module_entry vinc_module_contract vinc_module_runtime vinc_module_host)
    target_compile_features(${target} PUBLIC cxx_std_23)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

include(cmake/VincModule.cmake)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Metadata has no dependency on the component runtime or selected catalog.
add_library(vinc_module_system_errors INTERFACE)
target_include_directories(vinc_module_system_errors INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_module_system_errors INTERFACE vinc::common)
vinc_register_error_catalog(TARGET vinc_module_system_errors
    HEADER vinc/module/detail/error_catalog.hpp SYMBOL vinc::module_system::detail::error_catalog)
