add_module(vinc_module_ui MODULE_TYPE_NAME webUi STOP_TIMEOUT_CONFIG ui_stop_timeout SOURCES ui_module.cpp topic_monitor.cpp graph.cpp)
set_property(TARGET vinc_module_ui PROPERTY VINC_MODULE_OFFICIAL TRUE)
target_link_libraries(vinc_module_ui PRIVATE vinc::ui vinc_ui_json vinc_messages)
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
    vinc_add_module_plugin(vinc_ui_topic_source ../tests/topic_source.cpp)
    target_link_libraries(vinc_ui_topic_source PRIVATE vinc::module vinc_messages)
    vinc_add_module_plugin(vinc_ui_graph_source ../tests/graph_source.cpp)
    target_link_libraries(vinc_ui_graph_source PRIVATE vinc::module vinc_messages)
    add_executable(vinc_ui_module_tests ../tests/module_tests.cpp)
    target_link_libraries(vinc_ui_module_tests PRIVATE vinc_test_manager vinc_ui_http vinc_ui_json)
    add_dependencies(vinc_ui_module_tests vinc_module_ui vinc_ui_topic_source message_bus_module_helper vinc_ui_graph_source)
    add_test(NAME ui.module COMMAND vinc_ui_module_tests "$<TARGET_FILE:vinc_module_ui>" "${ui_dist}"
        "$<TARGET_FILE:vinc_ui_topic_source>" "$<TARGET_FILE:message_bus_module_helper>" "$<TARGET_FILE:vinc_ui_graph_source>")
    set_tests_properties(ui.module PROPERTIES TIMEOUT 90)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ui-smoke.tcl" "ui status\nexit\n")
    add_test(NAME ui.shell COMMAND "$<TARGET_FILE:shell>" --shell-port 0 -S "${CMAKE_CURRENT_BINARY_DIR}/ui-smoke.tcl")
    set_tests_properties(ui.shell PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE PASS_REGULAR_EXPRESSION "UI running=1 port=")
endif()
