cmake_minimum_required(VERSION 3.20)
project(embedded_app LANGUAGES C CXX)

get_filename_component(open_vinc_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(VINC_BUILD_APPS OFF CACHE BOOL "Build the bundled demonstration App")
set(BUILD_TESTING OFF CACHE BOOL "Build framework tests")
add_subdirectory("${open_vinc_root}/src" open-vinc)
add_subdirectory("${open_vinc_root}/examples/module" example-module)
vinc_add_app(MyApp INSTANCE_NAME embedded-demo
    VINC_BUILD_CONFIG app.config STARTUP_SCRIPT startup.tcl)
