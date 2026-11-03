add_subdirectory("${VINC_REPOSITORY_ROOT}/library/tomlplusplus" "${VINC_BUILD_ROOT}/library/tomlplusplus")
function(_vinc_build_param variant)
    vinc_runtime_name(vinc_param vinc_param ${variant})
    vinc_runtime_name(vinc_param_host vinc_param_host ${variant})

    add_library(${vinc_param} STATIC src/types.cpp src/params.cpp src/protocol.cpp)
    vinc_apply_runtime(${vinc_param} ${variant})

    target_include_directories(${vinc_param} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    vinc_runtime_link_libraries(${vinc_param} ${variant} PUBLIC vinc::common PRIVATE vinc::control_protocol)

    add_library(${vinc_param_host} STATIC src/storage.cpp)
    vinc_apply_runtime(${vinc_param_host} ${variant})

    vinc_runtime_link_libraries(${vinc_param_host} ${variant} PUBLIC ${vinc_param} vinc::osal vinc::concurrency vinc::control_protocol)

    if(variant STREQUAL "NATIVE")
        # Only the central persistence target sees TOML; upstream code is never patched.
        add_library(vinc_param_manager STATIC src/persistence.cpp src/manager.cpp)

        target_link_libraries(vinc_param_manager PUBLIC ${vinc_param_host} PRIVATE vinc_toml vinc::control)
    endif()

    set(runtime_targets ${vinc_param} ${vinc_param_host})
    if(variant STREQUAL "NATIVE")
        list(APPEND runtime_targets vinc_param_manager)
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
vinc_runtime_variants(_vinc_build_param)
add_library(vinc::param ALIAS vinc_param)
add_library(vinc::param_host ALIAS vinc_param_host)
add_library(vinc::param_manager ALIAS vinc_param_manager)

add_library(vinc_param_errors INTERFACE)
target_include_directories(vinc_param_errors INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_param_errors INTERFACE vinc::common)
vinc_register_error_catalog(TARGET vinc_param_errors
    HEADER vinc/param/detail/error_catalog.hpp SYMBOL vinc::param::detail::error_catalog)


# Installation keeps ownership here; standalone component builds remain unchanged.
if(COMMAND vinc_install_component)
    vinc_install_component(TARGETS
        vinc_param vinc_param_host vinc_param_manager vinc_param_errors)
endif()
