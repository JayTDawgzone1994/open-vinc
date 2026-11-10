cmake_minimum_required(VERSION 3.20)
project(installed_recorder LANGUAGES CXX)
find_package(vinc CONFIG REQUIRED)
add_message_dir(messages)
add_module(recorder_probe MODULE_TYPE_NAME RecorderProbe SOURCES module.cpp)
target_link_libraries(recorder_probe PRIVATE vinc::bag)
vinc_add_app(RecorderApp INSTANCE_NAME sdk-recorder-wait
    VINC_BUILD_CONFIG app.config STARTUP_SCRIPT startup.tcl)
