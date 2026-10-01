# Interface target for generated classes. Consumers inherit both storage linkage
# and generation ordering, so a build cannot race compilation against header generation.

cmake_minimum_required(VERSION 3.20)

# Generated headers and their runtime/storage dependency.
add_library(vinc_messages INTERFACE)
target_include_directories(vinc_messages INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)
target_compile_features(vinc_messages INTERFACE cxx_std_23)
target_link_libraries(vinc_messages INTERFACE vinc::message_storage)
add_dependencies(vinc_messages generate_messages)
