# This component owns its schemas, generated output and usage requirements.
cmake_minimum_required(VERSION 3.20)
include("${VINC_REPOSITORY_ROOT}/src/vinc/ribosome/cmake/VincMessages.cmake")
set_property(GLOBAL PROPERTY VINC_MESSAGE_OUTPUT "${VINC_MESSAGE_OUTPUT_DIR}")
set_property(GLOBAL PROPERTY VINC_MESSAGE_TEMPLATE
    "${VINC_REPOSITORY_ROOT}/src/vinc/ribosome/data/message_sample.txt")
add_message_dir("${CMAKE_CURRENT_SOURCE_DIR}/messages")
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL vinc_finalize_messages)
add_library(vinc_messages INTERFACE)
add_library(vinc::messages ALIAS vinc_messages)
target_include_directories(vinc_messages INTERFACE "${VINC_GENERATED_INCLUDE_DIR}")
target_compile_features(vinc_messages INTERFACE cxx_std_23)
target_link_libraries(vinc_messages INTERFACE vinc::message_storage)
