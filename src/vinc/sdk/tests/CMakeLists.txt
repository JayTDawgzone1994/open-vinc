# The consumer manually supplies no component include directories.
vinc_add_module_plugin(sdk_module_helper module_helper.cpp)
target_link_libraries(sdk_module_helper PRIVATE vinc::sdk)
add_executable(sdk_consumer_tests consumer.cpp)
target_link_libraries(sdk_consumer_tests PRIVATE vinc::sdk)
add_dependencies(sdk_consumer_tests sdk_module_helper)
add_test(NAME sdk.consumer COMMAND sdk_consumer_tests "$<TARGET_FILE:sdk_module_helper>")
set_tests_properties(sdk.consumer PROPERTIES TIMEOUT 30)

# Each supported public header must also compile as a translation unit's first include.
set(public_headers
    vinc/error.hpp
    vinc/common_error.hpp
    vinc/contract.hpp
    vinc/error_catalog.hpp
    vinc/result.hpp
    vinc/time.hpp
    vinc/config.hpp
    vinc/module/base_module.hpp
    vinc/module/context.hpp
    vinc/module/error.hpp
    vinc/module/mid.hpp
    vinc/module/contract_report.hpp
    vinc/module/host/module_manager.hpp
    vinc/message/base_message.hpp
    vinc/message/error.hpp
    vinc/message/storage.hpp
    vinc/message/generated.hpp
    vinc/message_bus/message_bus.hpp
    vinc/message_bus/error.hpp
    vinc/control/channel.hpp
    vinc/control/error.hpp
    vinc/control/protocol.hpp
    vinc/output_hub/output_hub.hpp
    vinc/output_hub/error.hpp
    vinc/output_hub/text_renderer.hpp
    vinc/shell/error.hpp
    vinc/shell/socket_shell_options.hpp
    vinc/concurrency/interprocess_mutex.hpp
    vinc/concurrency/error.hpp
    vinc/osal/path.hpp
    vinc/osal/dynamic_library.hpp
    vinc/osal/file_lock.hpp
    vinc/osal/error.hpp
    vinc/osal/pipe.hpp
    vinc/osal/process.hpp
    vinc/osal/process_pipe.hpp
    vinc/osal/random.hpp
    vinc/osal/shared_atomic.hpp
    vinc/osal/shared_memory.hpp
    vinc/osal/socket.hpp
    vinc/osal/socket_address.hpp
    vinc/osal/socket_poller.hpp
    vinc/osal/terminal.hpp
    vinc/vinc.hpp
)
set(header_sources)
foreach(header IN LISTS public_headers)
    string(REPLACE "/" "_" name "${header}")
    set(source "${CMAKE_CURRENT_BINARY_DIR}/headers/${name}.cpp")
    file(WRITE "${source}" "#include <${header}>\n")
    list(APPEND header_sources "${source}")
endforeach()
add_library(sdk_header_smoke OBJECT ${header_sources} public_boundary.cpp)
target_link_libraries(sdk_header_smoke PRIVATE vinc::sdk)

# Exercise add_subdirectory from a real parent, rather than inheriting this
# directory's include paths, compiler options or resource deployment location.
add_test(NAME sdk.external_consumer COMMAND "${CMAKE_COMMAND}"
    "-DVINC_SOURCE_DIR=${VINC_REPOSITORY_ROOT}"
    "-DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/external_consumer"
    "-DGENERATOR=${CMAKE_GENERATOR}"
    "-DCOMPILER=${CMAKE_CXX_COMPILER}"
    "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    "-DCONFIG=$<CONFIG>"
    -P "${VINC_REPOSITORY_ROOT}/tests/sdk_consumer/verify.cmake")
set_tests_properties(sdk.external_consumer PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
