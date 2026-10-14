# Python lifecycle integration tests are private to Module System.
add_executable(module_python_tests python_tests.cpp)
target_link_libraries(module_python_tests PRIVATE vinc::module_manager)
add_dependencies(module_python_tests vinc_python_native)
add_test(NAME module.python COMMAND module_python_tests "${Python_EXECUTABLE}"
    "${VINC_BUILD_ROOT}/python/$<CONFIG>/vinc/bootstrap.py" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set_tests_properties(module.python PROPERTIES TIMEOUT 90)

# Compile the storage fixtures only in test builds. Source-local include paths
# prevent their catalog from shadowing the application's real message catalog.
file(GLOB fixture_messages CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/messages/*.msg")
set(fixture_includes "${CMAKE_CURRENT_BINARY_DIR}/messages/$<CONFIG>")
add_custom_command(OUTPUT "${fixture_includes}/generation.stamp"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${fixture_includes}"
    COMMAND $<TARGET_FILE:ribosome> --input "${CMAKE_CURRENT_SOURCE_DIR}/messages"
        --output "${fixture_includes}/vinc_messages"
        --template "${VINC_REPOSITORY_ROOT}/src/vinc/ribosome/data/message_sample.txt"
    COMMAND "${CMAKE_COMMAND}" -E touch "${fixture_includes}/generation.stamp"
    DEPENDS ribosome ${fixture_messages} "${VINC_REPOSITORY_ROOT}/src/vinc/ribosome/data/message_sample.txt"
    VERBATIM)
add_custom_target(python_message_fixture_generation DEPENDS "${fixture_includes}/generation.stamp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/message_bindings.cpp"
    TARGET_DIRECTORY vinc_python_native PROPERTIES INCLUDE_DIRECTORIES "${fixture_includes}")
target_sources(vinc_python_native PRIVATE message_bindings.cpp)
add_dependencies(vinc_python_native python_message_fixture_generation)
target_compile_definitions(vinc_python_native PRIVATE VINC_PYTHON_TEST_MESSAGES=1)
add_test(NAME module.python_messages COMMAND "${Python_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/message_tests.py" "${VINC_BUILD_ROOT}/python/$<CONFIG>")
set_tests_properties(module.python_messages PROPERTIES TIMEOUT 30)

if(MSVC)
    add_test(NAME module.python_runtime COMMAND "${Python_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime_tests.py" "${VINC_BUILD_ROOT}/python/$<CONFIG>")
    set_tests_properties(module.python_runtime PROPERTIES TIMEOUT 120)
endif()

vinc_add_module_plugin(module_python_peer native_peer.cpp)
target_link_libraries(module_python_peer PRIVATE vinc_messages)
add_executable(module_python_services_tests services_tests.cpp)
target_link_libraries(module_python_services_tests PRIVATE vinc::module_manager)
add_dependencies(module_python_services_tests module_python_peer vinc_python_native)
add_test(NAME module.python_services COMMAND module_python_services_tests "${Python_EXECUTABLE}"
    "${VINC_BUILD_ROOT}/python/$<CONFIG>/vinc/bootstrap.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/services_module.py" "$<TARGET_FILE:module_python_peer>")
set_tests_properties(module.python_services PROPERTIES TIMEOUT 90)
