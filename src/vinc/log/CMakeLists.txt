add_subdirectory("${VINC_REPOSITORY_ROOT}/library/fmt" "${VINC_BUILD_ROOT}/library/fmt")
add_subdirectory("${VINC_REPOSITORY_ROOT}/library/spdlog" "${VINC_BUILD_ROOT}/library/spdlog")
function(_vinc_build_log variant)
    vinc_runtime_name(vinc_log vinc_log ${variant})
    vinc_runtime_name(vinc_log_host vinc_log_host ${variant})

    add_library(${vinc_log} STATIC src/log.cpp)
    vinc_apply_runtime(${vinc_log} ${variant})

    target_include_directories(${vinc_log} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    vinc_runtime_link_libraries(${vinc_log} ${variant} PUBLIC vinc::common fmt::fmt)
    add_library(${vinc_log_host} STATIC src/transport.cpp)
    vinc_apply_runtime(${vinc_log_host} ${variant})

    vinc_runtime_link_libraries(${vinc_log_host} ${variant} PUBLIC ${vinc_log} PRIVATE vinc::control)
    if(variant STREQUAL "NATIVE")
        add_library(vinc_log_manager STATIC src/files.cpp src/manager.cpp)

        target_link_libraries(vinc_log_manager PUBLIC ${vinc_log_host} PRIVATE vinc::control vinc::output_hub spdlog::spdlog)
    endif()

    set(runtime_targets ${vinc_log} ${vinc_log_host})
    if(variant STREQUAL "NATIVE")
        list(APPEND runtime_targets vinc_log_manager)
    endif()
    foreach(target IN LISTS runtime_targets)
        target_compile_features(${target} PUBLIC cxx_std_23)
        set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /utf-8)
        endif()
    endforeach()
endfunction()
vinc_runtime_variants(_vinc_build_log)
add_library(vinc::log ALIAS vinc_log)
add_library(vinc::log_host ALIAS vinc_log_host)
add_library(vinc::log_manager ALIAS vinc_log_manager)

add_library(vinc_log_errors INTERFACE)
target_include_directories(vinc_log_errors INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_log_errors INTERFACE vinc::common)
vinc_register_error_catalog(TARGET vinc_log_errors HEADER vinc/log/detail/error_catalog.hpp SYMBOL vinc::log::detail::error_catalog)

# Installation keeps ownership here; standalone component builds remain unchanged.
if(COMMAND vinc_install_component)
    vinc_install_component(TARGETS
        vinc_log vinc_log_host vinc_log_manager vinc_log_errors)
endif()
