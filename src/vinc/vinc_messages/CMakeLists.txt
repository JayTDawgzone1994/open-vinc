cmake_minimum_required(VERSION 3.20)

# Generated headers and their BaseMessage dependency; no compiled library.
add_library(vinc_messages INTERFACE)
target_include_directories(vinc_messages INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)
target_compile_features(vinc_messages INTERFACE cxx_std_23)
add_dependencies(vinc_messages generate_messages)
