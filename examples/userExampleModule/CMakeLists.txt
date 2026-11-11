add_module(vinc_user_example
    MODULE_TYPE_NAME userExampleModule
    SOURCES cpp/user_example_module.cpp cpp/message_examples.cpp)
target_include_directories(vinc_user_example PRIVATE cpp/include)

add_module(vinc_user_manual_example
    MODULE_TYPE_NAME userManualModule
    SOURCES cpp/manual_module.cpp)

add_subdirectory(python/userExamplePython)
add_subdirectory(python/userManualPython)

# Opt-in tutorials: registration does not select them into an App by default.
add_module(vinc_parallel_example MODULE_TYPE_NAME parallelExample SOURCES cpp/parallel_module.cpp)
add_subdirectory(python/parallelPython)
