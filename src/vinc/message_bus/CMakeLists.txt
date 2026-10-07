# Build the shared-memory Message Bus and its private BusManager/ring buffer implementation.
# Applications receive the typed public API; module runtime wiring stays private.

add_library(vinc_message_bus_client STATIC src/ring_buffer.cpp src/message_bus.cpp)
add_library(vinc::message_bus_client ALIAS vinc_message_bus_client)
target_include_directories(vinc_message_bus_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_message_bus_client PUBLIC vinc::common vinc::message_storage vinc::control_protocol vinc::osal)
target_compile_features(vinc_message_bus_client PUBLIC cxx_std_23)
add_library(vinc_message_bus STATIC src/runtime.cpp src/bus_manager.cpp)
add_library(vinc::message_bus ALIAS vinc_message_bus)
target_include_directories(vinc_message_bus PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_message_bus PUBLIC vinc::message_bus_client vinc::control)
target_compile_features(vinc_message_bus PUBLIC cxx_std_23)
if(MSVC)
    target_compile_options(vinc_message_bus PRIVATE /W4 /utf-8)
else()
    target_compile_options(vinc_message_bus PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Metadata has no dependency on the component runtime or selected catalog.
add_library(vinc_message_bus_errors INTERFACE)
target_include_directories(vinc_message_bus_errors INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_message_bus_errors INTERFACE vinc::common)
vinc_register_error_catalog(TARGET vinc_message_bus_errors
    HEADER vinc/message_bus/detail/error_catalog.hpp SYMBOL vinc::message_bus::detail::error_catalog)
