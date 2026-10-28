# UI dependencies remain private; the general SDK does not link HTTP or JSON.
foreach(dependency cpp-httplib json picosha2)
    add_subdirectory("${VINC_REPOSITORY_ROOT}/library/${dependency}" "${VINC_BUILD_ROOT}/library/${dependency}")
endforeach()
find_program(VINC_UI_NODE NAMES node REQUIRED)
set(web_root "${CMAKE_CURRENT_SOURCE_DIR}/web")
if(NOT EXISTS "${web_root}/node_modules/vite/bin/vite.js")
    message(FATAL_ERROR "UI frontend dependencies missing: run npm ci in src/vinc/ui/web (or set VINC_BUILD_UI=OFF)")
endif()
set(ui_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ui_dist "${CMAKE_CURRENT_BINARY_DIR}/dist")
file(GLOB_RECURSE web_sources CONFIGURE_DEPENDS
    "${web_root}/workspace/*" "${web_root}/sdk/*" "${web_root}/plugins/*" "${web_root}/tools/*")
set(ui_license_inputs
    "${VINC_REPOSITORY_ROOT}/LICENSE"
    "${VINC_REPOSITORY_ROOT}/NOTICE"
    "${VINC_REPOSITORY_ROOT}/THIRD_PARTY_NOTICES.txt"
    "${web_root}/node_modules/vite/LICENSE.md")
foreach(package IN ITEMS vue @vue/runtime-core @vue/runtime-dom @vue/shared @vue/reactivity
    @vue-flow/core
    @dagrejs/dagre
    @dagrejs/graphlib
    @vueuse/core
    @vueuse/shared
    @vueuse/metadata
    @vueuse/core/node_modules/vue-demi
    @vueuse/shared/node_modules/vue-demi
    d3-color
    d3-dispatch
    d3-drag
    d3-ease
    d3-interpolate
    d3-selection
    d3-timer
    d3-transition
    d3-zoom)
    list(APPEND ui_license_inputs "${web_root}/node_modules/${package}/LICENSE")
endforeach()
add_custom_command(OUTPUT "${ui_dist}/ready.stamp" "${ui_generated}/vinc_ui_official.hpp"
    COMMAND "${VINC_UI_NODE}" "${web_root}/tools/build.mjs" "${ui_dist}" "${ui_generated}"
    DEPENDS ${web_sources} ${ui_license_inputs}
        "${web_root}/package.json" "${web_root}/package-lock.json" "${web_root}/vite.config.ts"
    WORKING_DIRECTORY "${web_root}" VERBATIM)
add_custom_target(vinc_ui_web DEPENDS "${ui_dist}/ready.stamp" "${ui_generated}/vinc_ui_official.hpp")
add_library(vinc_ui STATIC src/assets.cpp src/server.cpp src/graph_requests.cpp)
add_library(vinc::ui ALIAS vinc_ui)
add_dependencies(vinc_ui vinc_ui_web)
target_include_directories(vinc_ui PUBLIC include PRIVATE "${ui_generated}")
target_link_libraries(vinc_ui PUBLIC vinc::common PRIVATE vinc::osal vinc_ui_http vinc_ui_json vinc_ui_hash)
target_compile_features(vinc_ui PUBLIC cxx_std_23)
add_library(vinc_ui_errors INTERFACE)
target_include_directories(vinc_ui_errors INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vinc_ui_errors INTERFACE vinc::common)
vinc_register_error_catalog(TARGET vinc_ui_errors HEADER vinc/ui/detail/error_catalog.hpp SYMBOL vinc::ui::detail::error_catalog)
if(MSVC)
    target_compile_options(vinc_ui PRIVATE /W4 /utf-8)
else()
    target_compile_options(vinc_ui PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_property(GLOBAL PROPERTY VINC_UI_DIST "${ui_dist}")
if(BUILD_TESTING)
    add_executable(vinc_ui_graph_tests tests/graph_tests.cpp)
    target_link_libraries(vinc_ui_graph_tests PRIVATE vinc::ui vinc::module vinc::message_bus vinc::errors)
    add_test(NAME ui.graph COMMAND vinc_ui_graph_tests)
    set_tests_properties(ui.graph PROPERTIES TIMEOUT 25)
    add_executable(vinc_ui_topic_tests tests/topic_tests.cpp)
    target_link_libraries(vinc_ui_topic_tests PRIVATE vinc::ui vinc::message_bus_client vinc_messages vinc::errors)
    add_test(NAME ui.topics COMMAND vinc_ui_topic_tests)
    set_tests_properties(ui.topics PROPERTIES TIMEOUT 15)
    add_executable(vinc_ui_tests tests/server_tests.cpp)
    target_link_libraries(vinc_ui_tests PRIVATE vinc::ui vinc::errors vinc_ui_http vinc_ui_json)
    add_test(NAME ui.server COMMAND vinc_ui_tests "${ui_dist}")
    set_tests_properties(ui.server PROPERTIES TIMEOUT 40)
    add_executable(vinc_ui_test_server tests/serve.cpp)
    target_link_libraries(vinc_ui_test_server PRIVATE vinc::ui vinc::errors vinc::osal)
endif()
