# Aggregate usage requirements without moving ownership out of components.
add_library(vinc_sdk INTERFACE)
add_library(vinc::sdk ALIAS vinc_sdk)
target_include_directories(vinc_sdk INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(vinc_sdk INTERFACE cxx_std_23)
target_link_libraries(vinc_sdk INTERFACE
    vinc::common vinc::module vinc::message_storage vinc::concurrency vinc::osal
    "$<$<NOT:$<BOOL:$<TARGET_PROPERTY:VINC_MODULE_PLUGIN>>>:vinc::module_manager;vinc::message_bus;vinc::control;vinc::output_hub;vinc::shell>")
# Obtain public header locations from their owning targets, without linking their services.
target_include_directories(vinc_sdk INTERFACE
    "$<TARGET_PROPERTY:vinc::output_hub,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:vinc::shell,INTERFACE_INCLUDE_DIRECTORIES>")
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Installation keeps ownership here; standalone component builds remain unchanged.
if(COMMAND vinc_install_component)
    vinc_install_component(TARGETS
        vinc_sdk)
endif()
