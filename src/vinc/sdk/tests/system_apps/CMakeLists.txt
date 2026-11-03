cmake_minimum_required(VERSION 3.20)
project(system_apps LANGUAGES CXX)
find_package(vinc CONFIG REQUIRED)

# Two independent official-service compositions; no Python Module or binding.
foreach(name SystemA SystemB)
    string(TOLOWER "${name}" identity)
    vinc_add_app(${name} INSTANCE_NAME "acceptance-${identity}" VINC_BUILD_CONFIG app.config)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/${name}-$<CONFIG>.path" CONTENT "$<TARGET_FILE:${name}>")
endforeach()
