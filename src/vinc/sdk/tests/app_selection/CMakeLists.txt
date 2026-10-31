cmake_minimum_required(VERSION 3.20)
project(app_selection LANGUAGES CXX)

# Only build-graph selection is under test. No Runtime or deployment is involved.
foreach(component module sdk messages)
    add_library(fixture_${component} INTERFACE)
    add_library(vinc::${component} ALIAS fixture_${component})
endforeach()
function(vinc_deploy_licenses target)
endfunction()
include("${VINC_SOURCE}/src/vinc/module_system/cmake/VincModule.cmake")
include("${VINC_SOURCE}/src/vinc/sdk/cmake/VincConfiguration.cmake")

add_custom_target(First ALL)
add_custom_target(Second ALL)
foreach(app First Second)
    vinc_register_app_configuration(${app}
        CONFIG "${app}.config"
        TOOL_DIRECTORY "${VINC_SOURCE}/tools/configure"
        UI_AVAILABLE TRUE BAG_AVAILABLE TRUE PYTHON_AVAILABLE TRUE)
endforeach()

# Register after the App, including a child directory, to exercise deferred scope.
add_subdirectory(modules)

function(record_selection)
    foreach(app First Second)
        get_target_property(selected ${app} VINC_APP_MODULE_TARGETS)
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/${app}.selected" CONTENT "${selected}")
        get_target_property(arguments ${app} VINC_APP_CONFIG_ARGUMENTS)
        string(REPLACE ";" "\n" arguments "${arguments}")
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/${app}.editor-args" CONTENT "${arguments}")
    endforeach()
    vinc_render_module_catalog(catalog camera)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected_catalog.hpp" CONTENT "${catalog}")
endfunction()
cmake_language(DEFER CALL record_selection)
