# Build framed control sessions on OSAL Socket; no application module code belongs here.

# Runtime linkage is owned by the framework, independent of a parent's defaults.
add_library(vinc_control_protocol STATIC src/protocol.cpp)
add_library(vinc::control_protocol ALIAS vinc_control_protocol)
target_include_directories(vinc_control_protocol PUBLIC include)
target_link_libraries(vinc_control_protocol PUBLIC vinc::common)
target_compile_features(vinc_control_protocol PUBLIC cxx_std_23)
add_library(vinc_control STATIC src/channel.cpp)
add_library(vinc::control ALIAS vinc_control)
target_include_directories(vinc_control PUBLIC include)
target_link_libraries(vinc_control PUBLIC vinc::osal vinc::control_protocol)
target_compile_features(vinc_control PUBLIC cxx_std_23)
if(MSVC)
  target_compile_options(vinc_control PRIVATE /W4 /utf-8)
else()
  target_compile_options(vinc_control PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Metadata has no dependency on the component runtime or selected catalog.
add_library(vinc_control_channel_errors INTERFACE)
target_include_directories(vinc_control_channel_errors INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_control_channel_errors INTERFACE vinc::common)
vinc_register_error_catalog(TARGET vinc_control_channel_errors
    HEADER vinc/control/detail/error_catalog.hpp SYMBOL vinc::control::detail::error_catalog)
