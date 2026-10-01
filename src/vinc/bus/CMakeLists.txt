# Build the shared-memory Bus and its private broker/ring implementation.
# Applications receive the typed public API; module runtime wiring stays private.

add_library(vinc_bus STATIC src/ring.cpp src/bus.cpp src/broker.cpp)
add_library(vinc::bus ALIAS vinc_bus)
target_include_directories(vinc_bus PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(vinc_bus PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../message_storage/src")
target_link_libraries(vinc_bus PUBLIC vinc::message_storage vinc::control vinc::osal)
target_compile_features(vinc_bus PUBLIC cxx_std_23)
if(MSVC)
    target_compile_options(vinc_bus PRIVATE /W4 /utf-8)
else()
    target_compile_options(vinc_bus PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
