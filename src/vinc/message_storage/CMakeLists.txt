# Build the byte-image/container runtime independently of OSAL and Bus.
# Only the public storage API is exported; framework_access.hpp stays an internal seam.

cmake_minimum_required(VERSION 3.20)
project(vinc_message_storage LANGUAGES CXX)
add_library(vinc_message_storage STATIC src/storage.cpp)
add_library(vinc::message_storage ALIAS vinc_message_storage)
target_compile_features(vinc_message_storage PUBLIC cxx_std_23)
set_target_properties(vinc_message_storage PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(vinc_message_storage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(vinc_message_storage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
if(MSVC)
    target_compile_options(vinc_message_storage PRIVATE /W4 /utf-8)
else()
    target_compile_options(vinc_message_storage PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
