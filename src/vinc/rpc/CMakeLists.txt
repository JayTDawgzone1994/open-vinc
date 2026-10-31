add_library(vinc_rpc STATIC src/protocol.cpp src/storage.cpp src/client.cpp)
add_library(vinc::rpc ALIAS vinc_rpc)
target_include_directories(vinc_rpc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_rpc PUBLIC vinc::message_storage vinc::common vinc::control_protocol PRIVATE vinc::osal)

# Only Host and central targets link the transport service, never application Plugins.
add_library(vinc_rpc_host STATIC src/host.cpp)
add_library(vinc::rpc_host ALIAS vinc_rpc_host)
target_link_libraries(vinc_rpc_host PUBLIC vinc::rpc vinc::control)
add_library(vinc_rpc_manager STATIC src/manager.cpp)
add_library(vinc::rpc_manager ALIAS vinc_rpc_manager)
target_link_libraries(vinc_rpc_manager PUBLIC vinc::rpc_host)

foreach(target IN ITEMS vinc_rpc vinc_rpc_host vinc_rpc_manager)
    target_compile_features(${target} PUBLIC cxx_std_23)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
add_library(vinc_rpc_errors INTERFACE)
target_include_directories(vinc_rpc_errors INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_rpc_errors INTERFACE vinc::common)
vinc_register_error_catalog(TARGET vinc_rpc_errors
    HEADER vinc/rpc/detail/error_catalog.hpp SYMBOL vinc::rpc::detail::error_catalog)

# Installation keeps ownership here; standalone component builds remain unchanged.
if(COMMAND vinc_install_component)
    vinc_install_component(TARGETS
        vinc_rpc vinc_rpc_host vinc_rpc_manager vinc_rpc_errors)
endif()
