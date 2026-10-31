cmake_minimum_required(VERSION 3.20)
project(installed_python LANGUAGES CXX)
find_package(vinc CONFIG REQUIRED)
vinc_add_app(PythonApp INSTANCE_NAME sdk-python VINC_BUILD_CONFIG vinc.conf)
# Late registration must reach the Python extension too.
add_message_dir(messages)
add_module(python_peer MODULE_TYPE_NAME NativePeer SOURCES peer.cpp)
set_target_properties(PythonApp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/application output")
