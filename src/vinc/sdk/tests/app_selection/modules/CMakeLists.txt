foreach(name camera detector shared official)
    add_module(${name} MODULE_TYPE_NAME ${name} SOURCES module.cpp)
    target_compile_definitions(${name} PRIVATE VINC_FIXTURE_CUSTOMIZED)
    set_target_properties(${name} PROPERTIES
        PREFIX "" SUFFIX ".plugin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/$<CONFIG>")
endforeach()
set_property(TARGET official PROPERTY VINC_MODULE_OFFICIAL TRUE)

# This must never be compiled by either App or the default all target.
add_module(unselected MODULE_TYPE_NAME unselected SOURCES forbidden.cpp)
if(DUPLICATE_TYPE)
    add_module(duplicate MODULE_TYPE_NAME camera SOURCES module.cpp)
endif()
