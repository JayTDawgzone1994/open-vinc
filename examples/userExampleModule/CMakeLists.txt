add_module(vinc_user_example
    MODULE_TYPE_NAME userExampleModule
    SOURCES cpp/user_example_module.cpp cpp/message_examples.cpp)
target_include_directories(vinc_user_example PRIVATE cpp/include)

add_module(vinc_user_manual_example
    MODULE_TYPE_NAME userManualModule
    SOURCES cpp/manual_module.cpp)
