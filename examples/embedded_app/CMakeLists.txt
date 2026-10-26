cmake_minimum_required(VERSION 3.20)
project(embedded_app LANGUAGES C CXX)

get_filename_component(open_vinc_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(VINC_NAME "embedded-demo" CACHE STRING "Central instance identity")
set(VINC_BUILD_APPS ON CACHE BOOL "Build the application")
set(VINC_BUILD_EXAMPLES ON CACHE BOOL "Include exampleModule used by startup.tcl")
set(BUILD_TESTING OFF CACHE BOOL "Build framework tests")
add_subdirectory("${open_vinc_root}/src" open-vinc)

vinc_configure_app(NAME MyApp STARTUP_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/startup.tcl")
