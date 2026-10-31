# Framework output routing with OSAL resources and shared configuration defaults.
add_library(vinc_output_hub_errors INTERFACE)
target_include_directories(vinc_output_hub_errors INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_output_hub_errors INTERFACE vinc::common)
vinc_register_error_catalog(TARGET vinc_output_hub_errors
    HEADER vinc/output_hub/detail/error_catalog.hpp SYMBOL vinc::output_hub::detail::error_catalog)
add_library(vinc_output_hub STATIC src/output_hub.cpp src/text_renderer.cpp)
add_library(vinc::output_hub ALIAS vinc_output_hub)
target_include_directories(vinc_output_hub PUBLIC include)
target_link_libraries(vinc_output_hub PUBLIC vinc::osal vinc::common)
target_compile_features(vinc_output_hub PUBLIC cxx_std_23)
if(MSVC)
    target_compile_options(vinc_output_hub PRIVATE /W4 /utf-8)
else()
    target_compile_options(vinc_output_hub PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Installation keeps ownership here; standalone component builds remain unchanged.
if(COMMAND vinc_install_component)
    vinc_install_component(TARGETS
        vinc_output_hub vinc_output_hub_errors)
endif()
