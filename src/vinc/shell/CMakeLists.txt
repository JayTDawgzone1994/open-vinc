# Build reusable Shell logic; apps/vinc owns the central executable.
# Tcl resources are deployed by application/test composition, not by linking this library.

cmake_minimum_required(VERSION 3.20)
project(shell LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../library/tcl" "${CMAKE_BINARY_DIR}/library/tcl")
add_library(vinc_shell STATIC
    src/shell.cpp
    src/tcl_runtime.cpp
    src/command_line.cpp
    src/command_router.cpp
    src/script_service.cpp
    src/socket_shell.cpp
)
add_library(vinc::shell ALIAS vinc_shell)
# Only immutable metadata enters the common catalog; no Tcl/runtime dependency.
add_library(vinc_shell_errors INTERFACE)
target_include_directories(vinc_shell_errors INTERFACE include)
target_link_libraries(vinc_shell_errors INTERFACE vinc::common)
vinc_register_error_catalog(TARGET vinc_shell_errors
    HEADER vinc/shell/detail/error_catalog.hpp SYMBOL vinc::shell::detail::error_catalog)
target_compile_features(vinc_shell PUBLIC cxx_std_23)
target_include_directories(vinc_shell PUBLIC include)
target_link_libraries(vinc_shell PUBLIC vinc::module_manager vinc::common PRIVATE "$<BUILD_INTERFACE:vinc_tcl>" "$<INSTALL_INTERFACE:vinc::detail_tcl>" vinc::log_manager)
if(MSVC)
    target_compile_options(vinc_shell PRIVATE /W4 /utf-8)
else()
    target_compile_options(vinc_shell PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Installation keeps ownership here; standalone component builds remain unchanged.
if(COMMAND vinc_install_component)
    vinc_install_component(TARGETS
        vinc_shell vinc_shell_errors)
endif()
