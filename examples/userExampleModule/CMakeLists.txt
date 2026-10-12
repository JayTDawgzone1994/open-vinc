vinc_add_process_module(vinc_user_example
    MODULE_TYPE_NAME userExampleModule MODULE_BASE_NAME userCpp
    SOURCES cpp/user_example_module.cpp cpp/message_examples.cpp)
target_include_directories(vinc_user_example PRIVATE cpp/include)
target_link_libraries(vinc_user_example PRIVATE vinc::sdk vinc::messages)

vinc_add_process_module(vinc_user_manual_example
    MODULE_TYPE_NAME userManualModule MODULE_BASE_NAME manualCpp
    SOURCES cpp/manual_module.cpp)
target_link_libraries(vinc_user_manual_example PRIVATE vinc::sdk)
