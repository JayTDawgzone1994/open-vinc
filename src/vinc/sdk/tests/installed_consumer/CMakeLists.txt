cmake_minimum_required(VERSION 3.20)
project(installed_sdk_consumer LANGUAGES CXX)

# These lookup-only cases simulate compiler metadata, never compile with it.
# Optional rejection must not import any part of an unusable package.
if(TEST_POLICY_OLD_MSVC)
    set(CMAKE_CXX_COMPILER_ID MSVC)
    set(CMAKE_CXX_COMPILER_VERSION 19.43.99999)
    find_package(vinc CONFIG QUIET)
    if(vinc_FOUND OR TARGET vinc::sdk OR TARGET vinc::common OR
       NOT vinc_NOT_FOUND_MESSAGE MATCHES "requires MSVC 19.44")
        message(FATAL_ERROR "Old MSVC package lookup did not fail cleanly: ${vinc_NOT_FOUND_MESSAGE}")
    endif()
    return()
endif()
if(TEST_POLICY_DIFFERENT_COMPILER)
    set(CMAKE_CXX_COMPILER_ID MSVC)
    set(CMAKE_CXX_COMPILER_VERSION 99.0)
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(TEST_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(TEST_OLD_LIBSTDCXX_ABI)
    string(APPEND CMAKE_CXX_FLAGS " -D_GLIBCXX_USE_CXX11_ABI=0")
endif()
if(TEST_WRONG_VERSION)
    find_package(vinc 999.0.0 EXACT CONFIG REQUIRED)
else()
    find_package(vinc CONFIG REQUIRED)
endif()
find_package(vinc CONFIG REQUIRED)
if(TARGET vinc::module_host OR TARGET vinc::module_entry OR TARGET vinc::test_host)
    message(FATAL_ERROR "Installed SDK must not export a separate Host entry/tool")
endif()

# Ribosome must resolve from the installed prefix. Generation is explicit in
# this stage-one fixture; add_message_dir/app assembly is stage two's contract.
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(OUTPUT "${generated}/vinc_messages/InstalledSample.hpp"
    COMMAND vinc::ribosome --input "${CMAKE_CURRENT_SOURCE_DIR}/messages"
        --output "${generated}/vinc_messages"
        --template "${vinc_DATA_DIR}/ribosome/message_sample.txt"
    DEPENDS vinc::ribosome messages/InstalledSample.msg
    VERBATIM)
add_executable(consumer main.cpp "${generated}/vinc_messages/InstalledSample.hpp")
target_include_directories(consumer PRIVATE "${generated}")
target_link_libraries(consumer PRIVATE vinc::sdk vinc::module_runtime)
if(MSVC)
    target_compile_options(consumer PRIVATE /utf-8)
endif()
vinc_deploy_licenses(consumer)
# Explicit resource deployment exercises the installed Tcl archive closure.
# General users will obtain automatic application deployment in stage two.
add_custom_target(tcl_resources
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${vinc_DATA_DIR}/tcl"
        "-DDESTINATION=$<TARGET_FILE_DIR:consumer>"
        -P "${vinc_DIR}/DeployTcl.cmake"
    VERBATIM)
add_dependencies(consumer tcl_resources)
enable_testing()
add_test(NAME consumer COMMAND consumer)
set_tests_properties(consumer PROPERTIES TIMEOUT 20)

# Test the installed designated integration seam with independent static OSAL
# copies. Hidden symbols keep a library's default owner local until explicit bind.
find_package(Threads REQUIRED)
foreach(image IN ITEMS a b)
    add_library(coordinator_${image} MODULE coordinator/coordinator_plugin.cpp)
    target_link_libraries(coordinator_${image} PRIVATE vinc::osal)
    set_target_properties(coordinator_${image} PROPERTIES
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
endforeach()
add_executable(coordinator_tests coordinator/coordinator_tests.cpp)
target_link_libraries(coordinator_tests PRIVATE vinc::osal Threads::Threads)
add_dependencies(coordinator_tests coordinator_a coordinator_b)
add_test(NAME coordinator.standalone COMMAND coordinator_tests)
foreach(mode IN ITEMS shared late)
    add_test(NAME coordinator.${mode} COMMAND coordinator_tests
        "$<TARGET_FILE:coordinator_a>" "$<TARGET_FILE:coordinator_b>" ${mode})
endforeach()
set_tests_properties(coordinator.standalone coordinator.shared coordinator.late
    PROPERTIES TIMEOUT 20)
