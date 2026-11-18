cmake_minimum_required(VERSION 3.20)
project(installed_recorder LANGUAGES CXX)
find_package(vinc CONFIG REQUIRED)
add_message_dir(messages)
add_module(recorder_probe MODULE_TYPE_NAME RecorderProbe SOURCES module.cpp)
target_link_libraries(recorder_probe PRIVATE vinc::bag)
vinc_add_app(RecorderApp INSTANCE_NAME sdk-recorder-wait
    VINC_BUILD_CONFIG app.config STARTUP_SCRIPT startup.tcl)
vinc_add_app(RecorderOther INSTANCE_NAME sdk-recorder-other
    VINC_BUILD_CONFIG app.config STARTUP_SCRIPT startup.tcl)
# This standalone decoder is a test tool, not a supported custom application main.
add_executable(recorder_decode_check decode.cpp)
target_link_libraries(recorder_decode_check PRIVATE vinc::sdk vinc::messages)
