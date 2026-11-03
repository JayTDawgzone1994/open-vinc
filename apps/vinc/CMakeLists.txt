# The bundled App uses the same composition and deployment as SDK consumers.
vinc_add_app(Vinc INSTANCE_NAME open-vinc VINC_BUILD_CONFIG vinc.conf)
get_target_property(config_output Vinc VINC_APP_CONFIG_OUTPUT)
include("${config_output}/preflight/features.cmake")

if(MSVC)
    target_compile_options(Vinc PRIVATE /W4)
else()
    target_compile_options(Vinc PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(BUILD_TESTING)
    add_test(NAME app.embedding COMMAND "${CMAKE_COMMAND}"
        "-DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/embedding-test"
        "-DEMBED_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedScript.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/embedding.cmake")
    set_tests_properties(app.embedding PROPERTIES TIMEOUT 30)
    add_subdirectory(tests)
    add_subdirectory("${VINC_REPOSITORY_ROOT}/src/vinc/ui/tests" ui-tests)
    add_subdirectory("${VINC_REPOSITORY_ROOT}/src/vinc/bag/tests" bag-tests)
endif()

if(BUILD_TESTING)
    if(VINC_MODULE_exampleModule)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/module-smoke.tcl"
            "exampleModule\nexampleNode hello\nexampleModule --name secondaryNode\nsecondaryNode status\nexit\n")
        add_test(NAME module.shell COMMAND Vinc --shell-port 0 -S "${CMAKE_CURRENT_BINARY_DIR}/module-smoke.tcl")
        set_tests_properties(module.shell PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "secondaryNode status")
    endif()
    if(VINC_MODULE_messageBusExample)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/message_bus-smoke.tcl"
            "messageBusExample --name messageBusSubscriber sub demo\nmessageBusExample --name messageBusPublisher pub demo\nmessageBusPublisher push 10\nmessageBusSubscriber last\nmessageBusSubscriber last\nmessageBusSubscriber last\nmessageBusPublisher loan 20\nmessageBusSubscriber last\nmessageBusPublisher commit\nmessageBusSubscriber last\nmessageBusPublisher stats\nexit\n")
        add_test(NAME message_bus.shell COMMAND Vinc --shell-port 0 -S "${CMAKE_CURRENT_BINARY_DIR}/message_bus-smoke.tcl")
        set_tests_properties(message_bus.shell PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "value=20")
    endif()
endif()

if(BUILD_TESTING)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bag-smoke.tcl"
        "recorder status\nexit\n")
    add_test(NAME bag.shell COMMAND Vinc --shell-port 0 -S "${CMAKE_CURRENT_BINARY_DIR}/bag-smoke.tcl")
    set_tests_properties(bag.shell PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "bag: idle")
endif()

if(VINC_APP_PYTHON)
    if(VINC_MODULE_userExampleModule AND VINC_MODULE_userManualModule)
        # Explicit deployment avoids overwriting user-edited scripts on every build.
        add_custom_target(user_examples_python
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${VINC_REPOSITORY_ROOT}/examples/userExampleModule/python"
                "$<TARGET_FILE_DIR:Vinc>/data/module"
            DEPENDS Vinc VERBATIM)
        if(BUILD_TESTING)
            get_property(python_example_executable GLOBAL PROPERTY VINC_PYTHON_TEST_EXECUTABLE)
            add_test(NAME examples.user_module COMMAND "${python_example_executable}"
                "${VINC_REPOSITORY_ROOT}/examples/userExampleModule/tests/smoke.py"
                "$<TARGET_FILE:Vinc>" "${VINC_REPOSITORY_ROOT}/examples/userExampleModule")
            # The fixture restarts the central to verify saved/live Param separation.
            set_tests_properties(examples.user_module PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
        endif()
    endif()
    if(BUILD_TESTING)
        get_property(python_test_executable GLOBAL PROPERTY VINC_PYTHON_TEST_EXECUTABLE)
        add_test(NAME module.python_shell COMMAND "${python_test_executable}"
            "${VINC_REPOSITORY_ROOT}/src/vinc/module_system/python/tests/shell_test.py"
            "$<TARGET_FILE:Vinc>"
            "${VINC_REPOSITORY_ROOT}/src/vinc/module_system/python/tests/fixtures")
        set_tests_properties(module.python_shell PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endif()
endif()
