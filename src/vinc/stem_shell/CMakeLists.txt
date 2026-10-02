# Build reusable Shell logic plus Vinc, the central executable.
# Deploy Tcl resources beside executables and generate the catalog after module registration.

cmake_minimum_required(VERSION 3.20)
project(stem_shell LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../library/tcl" "${CMAKE_BINARY_DIR}/library/tcl")
add_library(vinc_shell STATIC src/stem_shell.cpp src/tcl_runtime.cpp src/command_line.cpp src/command_router.cpp src/script_service.cpp src/socket_shell.cpp)
target_include_directories(vinc_shell PUBLIC include)
target_link_libraries(vinc_shell PUBLIC module PRIVATE vinc_tcl)
add_executable(stem_shell src/main.cpp)
set_target_properties(stem_shell PROPERTIES OUTPUT_NAME Vinc)
vinc_deploy_tcl(stem_shell "${CMAKE_BINARY_DIR}")
target_include_directories(stem_shell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(stem_shell PRIVATE vinc_shell)
set_target_properties(stem_shell PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
get_property(PROCESS_TARGETS GLOBAL PROPERTY VINC_PROCESS_TARGETS)
get_property(PROCESS_CATALOG GLOBAL PROPERTY VINC_PROCESS_CATALOG)
if(PROCESS_TARGETS)
    add_dependencies(stem_shell ${PROCESS_TARGETS})
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/module-smoke.tcl"
        "exampleModule\nexampleNode hello\nexampleModule --name secondaryNode\nsecondaryNode status\nexit\n")
    add_test(NAME module.shell COMMAND stem_shell --shell-port 0 -S "${CMAKE_CURRENT_BINARY_DIR}/module-smoke.tcl")
    set_tests_properties(module.shell PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "secondaryNode status")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bus-smoke.tcl"
        "busExample --name busSubscriber sub demo\nbusExample --name busPublisher pub demo\nbusPublisher push 10\nbusSubscriber last\nbusSubscriber last\nbusSubscriber next\nbusPublisher loan 20\nbusSubscriber last\nbusPublisher commit\nbusSubscriber next\nbusPublisher stats\nexit\n")
    add_test(NAME bus.shell COMMAND stem_shell --shell-port 0 -S "${CMAKE_CURRENT_BINARY_DIR}/bus-smoke.tcl")
    set_tests_properties(bus.shell PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "value=20")
endif()
# Generate after collecting all process modules. Generator expressions resolve
# each executable path for this build configuration.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/module_catalog.hpp" CONTENT
"#pragma once\n#include <module/module_manager.hpp>\n#include <stdexcept>\nnamespace vinc { inline void register_built_modules(ModuleManager& manager) {\n${PROCESS_CATALOG}} }\n")
if(MSVC)
    target_compile_options(vinc_shell PRIVATE /W4 /utf-8)
    target_compile_options(stem_shell PRIVATE /W4)
else()
    target_compile_options(vinc_shell PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(stem_shell PRIVATE -Wall -Wextra -Wpedantic)
endif()
