# Build the common OSAL API with exactly one supported native backend.
# Feature probes below enforce descriptor-isolation requirements before compiling POSIX spawn.

cmake_minimum_required(VERSION 3.20)
project(vinc_osal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
endif()

# An isolated OSAL checkout includes its standard-library-only sibling common.
# Parent projects may provide the target themselves before adding OSAL.
if(NOT TARGET vinc::common)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../common" "${CMAKE_CURRENT_BINARY_DIR}/common")
endif()

find_package(Threads REQUIRED)
add_library(vinc_osal_errors STATIC src/native_error.cpp)
target_include_directories(vinc_osal_errors PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_osal_errors PUBLIC vinc::common)
set_target_properties(vinc_osal_errors PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
vinc_register_error_catalog(TARGET vinc_osal_errors
    HEADER vinc/osal/detail/error_catalog.hpp SYMBOL vinc::osal::detail::error_catalog)
add_library(vinc_osal STATIC src/process.cpp src/pipe.cpp src/process_pipe.cpp src/process_pipe_native.cpp
    src/socket_address.cpp src/socket.cpp src/socket_poller.cpp src/shared_memory.cpp src/shared_atomic.cpp src/terminal.cpp src/random.cpp
    src/file_lock.cpp src/path.cpp src/dynamic_library.cpp)
add_library(vinc::osal ALIAS vinc_osal)
target_compile_features(vinc_osal PUBLIC cxx_std_23)
target_include_directories(vinc_osal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_osal PUBLIC vinc::common vinc::errors PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

if(WIN32)
    target_sources(vinc_osal PRIVATE src/windows/process.cpp src/windows/pipe.cpp src/windows/socket_poller.cpp src/windows/shared_memory.cpp src/windows/file_lock.cpp)
    target_link_libraries(vinc_osal PRIVATE bcrypt advapi32 ws2_32)
    target_compile_definitions(vinc_osal PRIVATE _WIN32_WINNT=0x0601)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    include(CheckCXXSymbolExists)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
        check_cxx_symbol_exists(posix_spawn_file_actions_addclosefrom_np "spawn.h" VINC_OSAL_HAS_CLOSEFROM)
        if(NOT VINC_OSAL_HAS_CLOSEFROM)
            message(FATAL_ERROR "OSAL requires posix_spawn_file_actions_addclosefrom_np (glibc >= 2.34) for descriptor isolation")
        endif()
        target_compile_definitions(vinc_osal PRIVATE _GNU_SOURCE)
        target_sources(vinc_osal PRIVATE src/linux/monitor.cpp src/linux/pipe_monitor.cpp src/linux/socket_poller.cpp)
    else()
        check_cxx_symbol_exists(POSIX_SPAWN_CLOEXEC_DEFAULT "spawn.h" VINC_OSAL_HAS_CLOEXEC_DEFAULT)
        if(NOT VINC_OSAL_HAS_CLOEXEC_DEFAULT)
            message(FATAL_ERROR "OSAL requires POSIX_SPAWN_CLOEXEC_DEFAULT for descriptor isolation")
        endif()
        target_compile_definitions(vinc_osal PRIVATE VINC_OSAL_MACOS)
        target_sources(vinc_osal PRIVATE src/macos/monitor.cpp src/macos/pipe_monitor.cpp src/macos/socket_poller.cpp)
    endif()
    check_cxx_symbol_exists(posix_spawn_file_actions_addchdir_np "spawn.h" VINC_OSAL_HAS_SPAWN_CHDIR)
    if(VINC_OSAL_HAS_SPAWN_CHDIR)
        target_compile_definitions(vinc_osal PRIVATE VINC_OSAL_HAS_SPAWN_CHDIR)
    endif()
    cmake_pop_check_state()
    target_sources(vinc_osal PRIVATE src/posix/process.cpp src/posix/pipe.cpp src/posix/shared_memory.cpp src/posix/file_lock.cpp)
else()
    message(FATAL_ERROR "OSAL currently has backends only for Windows, Linux, and macOS")
endif()

if(MSVC)
    target_compile_options(vinc_osal PRIVATE /W4 /utf-8)
else()
    target_compile_options(vinc_osal PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    vinc_finalize_error_catalog()
endif()
