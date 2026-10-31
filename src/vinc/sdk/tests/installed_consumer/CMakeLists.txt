cmake_minimum_required(VERSION 3.20)
project(installed_sdk_consumer LANGUAGES CXX)
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
