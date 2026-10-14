# Build graph for the framework. Message generation is a build dependency,
# while process-module registration collects launch metadata for the central Shell.

cmake_minimum_required(VERSION 3.20)
project(vinc LANGUAGES CXX)

# =================================================================
# Require C++23 consistently across framework targets.
# =================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Source files contain UTF-8 text; do not depend on the Windows code page.
if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
endif()

# =================================================================
# Declare runtime libraries before application targets.
# =================================================================
# Explicit target dependencies below govern message generation.
add_subdirectory(common)         # Shared framework Result and error interface
add_subdirectory(ribosome)       # Build-time message compiler
add_subdirectory(osal)           # Independent OS process primitives
add_subdirectory(components)     # Reusable concurrency and message storage tools
add_subdirectory(output_hub)         # Bounded process-output fanout and plain-text rendering
add_subdirectory(control_channel)
add_subdirectory(log) # Shared framed Socket sessions
add_subdirectory(param)           # Typed parameters, shared snapshots and central persistence
add_subdirectory(rpc)             # Typed shared-memory request/reply services
include(module_system/cmake/VincModule.cmake)
add_subdirectory(message_bus)             # Shared-memory publish/subscribe
add_subdirectory(bag)
add_subdirectory(module_system)         # Process-module runtime and supervisor

# Build reusable Shell logic.
add_subdirectory(shell)

# The SDK adds only usage requirements; components retain their own targets.
add_subdirectory(sdk)
if(BUILD_TESTING)
    add_subdirectory(param/tests)
    add_subdirectory(log/tests)
    add_subdirectory(rpc/tests)
endif()

if(VINC_BUILD_APPS AND VINC_BUILD_UI)
    add_subdirectory(ui)
endif()

# Every provider has registered C++ metadata; freeze one catalog for this build.
vinc_finalize_error_catalog()
