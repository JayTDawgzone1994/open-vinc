# Included by the source binding test setup after Python has been selected.
vinc_add_module_plugin(rpc_python_peer "${VINC_REPOSITORY_ROOT}/src/vinc/rpc/tests/python_peer.cpp")
target_link_libraries(rpc_python_peer PRIVATE vinc_messages)
add_executable(rpc_python_tests "${VINC_REPOSITORY_ROOT}/src/vinc/rpc/tests/python_process.cpp")
target_link_libraries(rpc_python_tests PRIVATE vinc_test_manager vinc::errors)
add_dependencies(rpc_python_tests rpc_python_peer vinc_python_native)
get_property(rpc_python GLOBAL PROPERTY VINC_PYTHON_TEST_EXECUTABLE)
add_test(NAME rpc.python COMMAND rpc_python_tests "${rpc_python}"
    "${python_package}/bootstrap.py"
    "${VINC_REPOSITORY_ROOT}/src/vinc/rpc/tests/python_module.py" "$<TARGET_FILE:rpc_python_peer>")
set_tests_properties(rpc.python PROPERTIES TIMEOUT 40)
