# Build the byte-image/container runtime independently of OSAL and Message Bus.
# Only the public storage API is exported; framework_access.hpp stays an internal seam.

cmake_minimum_required(VERSION 3.20)
project(vinc_message_storage LANGUAGES CXX)

function(_vinc_build_message_storage variant)
    vinc_runtime_name(vinc_message_storage vinc_message_storage ${variant})

    add_library(${vinc_message_storage} STATIC src/storage.cpp)
    vinc_apply_runtime(${vinc_message_storage} ${variant})

    vinc_runtime_link_libraries(${vinc_message_storage} ${variant} PUBLIC vinc::common)

    target_compile_features(${vinc_message_storage} PUBLIC cxx_std_23)
    set_target_properties(${vinc_message_storage} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_include_directories(${vinc_message_storage} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    if(MSVC)
        target_compile_options(${vinc_message_storage} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${vinc_message_storage} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
vinc_runtime_variants(_vinc_build_message_storage)
add_library(vinc::message_storage ALIAS vinc_message_storage)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Metadata providers depend only on common, never the storage runtime or catalog integration.
add_library(vinc_message_storage_errors INTERFACE)
target_include_directories(vinc_message_storage_errors INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_message_storage_errors INTERFACE vinc::common)
vinc_register_error_catalog(TARGET vinc_message_storage_errors
    HEADER vinc/message/detail/error_catalog.hpp SYMBOL vinc::message::detail::error_catalog)

# Installation keeps ownership here; standalone component builds remain unchanged.
if(COMMAND vinc_install_component)
    vinc_install_component(TARGETS
        vinc_message_storage vinc_message_storage_errors)
endif()
