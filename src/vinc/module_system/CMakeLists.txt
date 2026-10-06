# One component, separate child-runtime and central-management link dependencies.
cmake_minimum_required(VERSION 3.20)
project(vinc_module_system LANGUAGES CXX)

# Shared private codec/ownership implementation keeps both process endpoints aligned.
add_library(vinc_module_contract STATIC src/contract_report.cpp src/runtime/runtime_services.cpp)
target_include_directories(vinc_module_contract PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_module_contract PUBLIC vinc::osal)

add_library(vinc_module STATIC src/runtime/base_module.cpp src/runtime/context.cpp)
add_library(vinc::module ALIAS vinc_module)
target_include_directories(vinc_module PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_module PUBLIC vinc::message_bus_client)

add_library(vinc_module_runtime STATIC src/runtime/module_runtime.cpp src/runtime/plugin_loader.cpp)
add_library(vinc::module_runtime ALIAS vinc_module_runtime)
target_link_libraries(vinc_module_runtime PUBLIC vinc::module vinc::message_bus vinc::control vinc_module_contract)

add_library(vinc_module_manager STATIC src/host/module_manager.cpp)
add_library(vinc::module_manager ALIAS vinc_module_manager)
target_include_directories(vinc_module_manager PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_module_manager PUBLIC vinc::common vinc::message_bus vinc::control vinc::output_hub PRIVATE vinc_module_contract)

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
