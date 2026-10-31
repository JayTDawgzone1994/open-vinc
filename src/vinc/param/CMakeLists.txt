add_library(vinc_param STATIC src/types.cpp src/params.cpp src/protocol.cpp)
add_library(vinc::param ALIAS vinc_param)
target_include_directories(vinc_param PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_param PUBLIC vinc::common PRIVATE vinc::control_protocol)

add_library(vinc_param_host STATIC src/storage.cpp)
add_library(vinc::param_host ALIAS vinc_param_host)
target_link_libraries(vinc_param_host PUBLIC vinc::param vinc::osal vinc::concurrency vinc::control_protocol)

# Only the central persistence target sees TOML; upstream code is never patched.
add_subdirectory("${VINC_REPOSITORY_ROOT}/library/tomlplusplus" "${VINC_BUILD_ROOT}/library/tomlplusplus")
add_library(vinc_param_manager STATIC src/persistence.cpp src/manager.cpp)
add_library(vinc::param_manager ALIAS vinc_param_manager)
target_link_libraries(vinc_param_manager PUBLIC vinc::param_host PRIVATE vinc_toml vinc::control)

foreach(target IN ITEMS vinc_param vinc_param_host vinc_param_manager)
    target_compile_features(${target} PUBLIC cxx_std_23)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

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
