# This component owns its schemas, generated output and usage requirements.
cmake_minimum_required(VERSION 3.20)
file(GLOB message_inputs "${CMAKE_CURRENT_SOURCE_DIR}/messages/*.msg")
set(message_outputs "${VINC_MESSAGE_OUTPUT_DIR}/all_messages.hpp" "${VINC_MESSAGE_OUTPUT_DIR}/message_types.hpp")
foreach(input IN LISTS message_inputs)
    get_filename_component(name "${input}" NAME_WE)
    list(APPEND message_outputs "${VINC_MESSAGE_OUTPUT_DIR}/${name}.hpp")
endforeach()
add_custom_target(generate_messages
    COMMAND $<TARGET_FILE:ribosome>
        --input "${CMAKE_CURRENT_SOURCE_DIR}/messages"
        --output "${VINC_MESSAGE_OUTPUT_DIR}"
        --template "${VINC_REPOSITORY_ROOT}/src/vinc/ribosome/data/message_sample.txt"
    BYPRODUCTS ${message_outputs}
    DEPENDS ribosome
    COMMENT "Ribosome: generate messages in the build directory"
    VERBATIM
)
add_library(vinc_messages INTERFACE)
add_library(vinc::messages ALIAS vinc_messages)
target_include_directories(vinc_messages INTERFACE "${VINC_GENERATED_INCLUDE_DIR}")
target_compile_features(vinc_messages INTERFACE cxx_std_23)
target_link_libraries(vinc_messages INTERFACE vinc::message_storage)
add_dependencies(vinc_messages generate_messages)
