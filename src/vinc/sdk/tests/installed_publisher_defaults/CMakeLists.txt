cmake_minimum_required(VERSION 3.20)
project(installed_publisher_defaults LANGUAGES CXX)
find_package(vinc CONFIG REQUIRED)
add_message_dir(messages)
add_module(publisher_defaults_probe MODULE_TYPE_NAME PublisherDefaultsProbe SOURCES module.cpp)
vinc_add_app(PublisherDefaultsApp INSTANCE_NAME sdk-publisher-defaults
    VINC_BUILD_CONFIG app.config STARTUP_SCRIPT startup.tcl)
