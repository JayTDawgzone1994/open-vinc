function(_vinc_build_rpc variant)
    vinc_runtime_name(vinc_rpc vinc_rpc ${variant})
    vinc_runtime_name(vinc_rpc_host vinc_rpc_host ${variant})

    add_library(${vinc_rpc} STATIC src/protocol.cpp src/storage.cpp src/client.cpp src/address.cpp)
    vinc_apply_runtime(${vinc_rpc} ${variant})

    target_include_directories(${vinc_rpc} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    vinc_runtime_link_libraries(${vinc_rpc} ${variant} PUBLIC vinc::message_storage vinc::common vinc::control_protocol PRIVATE vinc::osal)

    # Only Host and central targets link the transport service, never application Plugins.
    add_library(${vinc_rpc_host} STATIC src/host.cpp)
    vinc_apply_runtime(${vinc_rpc_host} ${variant})

    vinc_runtime_link_libraries(${vinc_rpc_host} ${variant} PUBLIC ${vinc_rpc} vinc::control)
    if(variant STREQUAL "NATIVE")
        add_library(vinc_rpc_manager STATIC src/manager.cpp)

        target_link_libraries(vinc_rpc_manager PUBLIC ${vinc_rpc_host})
    endif()

    set(runtime_targets ${vinc_rpc} ${vinc_rpc_host})
    if(variant STREQUAL "NATIVE")
        list(APPEND runtime_targets vinc_rpc_manager)
    endif()
    foreach(target IN LISTS runtime_targets)
        target_compile_features(${target} PUBLIC cxx_std_23)
        set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /utf-8)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endforeach()
endfunction()
vinc_runtime_variants(_vinc_build_rpc)
add_library(vinc::rpc ALIAS vinc_rpc)
add_library(vinc::rpc_host ALIAS vinc_rpc_host)
add_library(vinc::rpc_manager ALIAS vinc_rpc_manager)

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
