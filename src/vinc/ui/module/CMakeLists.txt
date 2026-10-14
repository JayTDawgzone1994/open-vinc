add_module(vinc_module_ui MODULE_TYPE_NAME webUi STOP_TIMEOUT_CONFIG ui_stop_timeout SOURCES ui_module.cpp)
target_link_libraries(vinc_module_ui PRIVATE vinc::ui)
get_property(ui_dist GLOBAL PROPERTY VINC_UI_DIST)
set(ui_deploy_root "${VINC_BUILD_ROOT}/vinc_data/${VINC_NAME}/ui")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/vinc_ui_paths.hpp"
    CONTENT "#pragma once\ninline constexpr auto vinc_ui_default_root = R\"(${ui_deploy_root})\";\n" @ONLY)
target_include_directories(vinc_module_ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
# Always-run copying also repairs deployment after a user removed generated assets.
# External plugin directories are preserved; only the official package is managed.
add_custom_target(vinc_ui_deploy
    COMMAND "${VINC_UI_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/../web/tools/deploy.mjs"
        "${ui_dist}" "${ui_deploy_root}" "${VINC_BUILD_ROOT}"
    DEPENDS vinc_ui_web VERBATIM)
add_dependencies(vinc_module_ui vinc_ui_deploy)

if(BUILD_TESTING)
    add_executable(vinc_ui_module_tests ../tests/module_tests.cpp)
    target_link_libraries(vinc_ui_module_tests PRIVATE vinc::module_manager vinc_ui_http)
    add_dependencies(vinc_ui_module_tests vinc_module_ui vinc_module_host)
    add_test(NAME ui.module COMMAND vinc_ui_module_tests "$<TARGET_FILE:vinc_module_ui>" "${ui_dist}")
    set_tests_properties(ui.module PROPERTIES TIMEOUT 60)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ui-smoke.tcl" "webUi --port 0\nui status\nexit\n")
    add_test(NAME ui.shell COMMAND "$<TARGET_FILE:shell>" --shell-port 0 -S "${CMAKE_CURRENT_BINARY_DIR}/ui-smoke.tcl")
    set_tests_properties(ui.shell PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE PASS_REGULAR_EXPRESSION "UI running=1 port=")
endif()
