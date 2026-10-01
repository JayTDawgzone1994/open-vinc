# Generate dedicated test messages and compile their public API before checking
# local storage, fixed loans and cross-process byte-image compatibility.

add_executable(message_storage_tests storage_tests.cpp)
target_link_libraries(message_storage_tests PRIVATE vinc::message_storage vinc::osal)
# Only framework integration tests can see the fixed-memory binding seam.
target_include_directories(message_storage_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
if(MSVC)
    target_compile_options(message_storage_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(message_storage_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME message.storage COMMAND message_storage_tests "$<TARGET_FILE:message_storage_tests>")
set_tests_properties(message.storage PROPERTIES TIMEOUT 90)

file(GLOB fixture_messages CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/messages/*.msg")
set(fixture_root "${CMAKE_CURRENT_BINARY_DIR}/fixture")
add_custom_command(OUTPUT "${fixture_root}/generation.stamp"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${fixture_root}"
    COMMAND $<TARGET_FILE:ribosome> --input "${CMAKE_CURRENT_SOURCE_DIR}/messages"
        --output "${fixture_root}/vinc_messages"
        --template "${CMAKE_CURRENT_SOURCE_DIR}/../../ribosome/data/message_sample.txt"
    COMMAND ${CMAKE_COMMAND} -E touch "${fixture_root}/generation.stamp"
    DEPENDS ribosome ${fixture_messages} "${CMAKE_CURRENT_SOURCE_DIR}/../../ribosome/data/message_sample.txt"
    VERBATIM)
add_custom_target(message_fixture_generation DEPENDS "${fixture_root}/generation.stamp")
add_executable(generated_message_tests generated_message_tests.cpp)
add_dependencies(generated_message_tests message_fixture_generation)
target_link_libraries(generated_message_tests PRIVATE vinc::message_storage vinc::osal)
target_include_directories(generated_message_tests PRIVATE "${fixture_root}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
if(MSVC)
    target_compile_options(generated_message_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(generated_message_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME message.generated COMMAND generated_message_tests "$<TARGET_FILE:generated_message_tests>")
set_tests_properties(message.generated PROPERTIES TIMEOUT 90)
