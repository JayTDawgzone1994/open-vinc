# Final SDK composition. Runtime components own their target/header install rules.
# Applications compile their catalog, identity and typed Modules in the consumer.
include(CMakePackageConfigHelpers)
include("${CMAKE_CURRENT_LIST_DIR}/CheckSdkAbi.cmake")
vinc_check_sdk_abi(VINC_SDK_ABI)
file(SHA256 "${VINC_REPOSITORY_ROOT}/src/vinc/common/include/vinc/config.hpp" VINC_CONFIG_HASH)
set(package_directory "${CMAKE_CURRENT_BINARY_DIR}/sdk-package")
file(MAKE_DIRECTORY "${package_directory}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/CheckInstall.cmake.in"
    "${package_directory}/CheckInstall.cmake" @ONLY)
file(WRITE "${package_directory}/profile.txt"
    "${VINC_SDK_VERSION}|${CMAKE_SYSTEM_NAME}|${CMAKE_CXX_COMPILER_ID}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_BUILD_TYPE}|${VINC_SDK_ABI}|${VINC_CONFIG_HASH}")
configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/vincConfig.cmake.in"
    "${package_directory}/vincConfig.cmake" INSTALL_DESTINATION lib/cmake/vinc)
write_basic_package_version_file("${package_directory}/vincConfigVersion.cmake"
    VERSION "${VINC_SDK_VERSION}" COMPATIBILITY ExactVersion)
install(EXPORT VincTargets NAMESPACE vinc:: DESTINATION lib/cmake/vinc COMPONENT VincSDK)
install(FILES "${package_directory}/vincConfig.cmake" "${package_directory}/vincConfigVersion.cmake"
    "${package_directory}/profile.txt"
    "${CMAKE_CURRENT_LIST_DIR}/CheckSdkAbi.cmake" "${CMAKE_CURRENT_LIST_DIR}/sdk_abi.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/VincInstalledResources.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/VincApplication.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/VincConfiguration.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/DeployApplication.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/VincOwnedFiles.cmake"
    "${VINC_REPOSITORY_ROOT}/src/vinc/module_system/cmake/VincModule.cmake"
    "${VINC_REPOSITORY_ROOT}/src/vinc/ribosome/cmake/VincMessages.cmake"
    "${VINC_REPOSITORY_ROOT}/apps/vinc/cmake/VincApp.cmake"
    "${VINC_REPOSITORY_ROOT}/apps/vinc/cmake/EmbedScript.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/DeployLicenses.cmake"
    "${VINC_REPOSITORY_ROOT}/src/vinc/common/cmake/VincMsvcDependencies.cmake"
    DESTINATION lib/cmake/vinc COMPONENT VincSDK)
install(FILES "${VINC_REPOSITORY_ROOT}/LICENSE" "${VINC_REPOSITORY_ROOT}/NOTICE"
    "${VINC_REPOSITORY_ROOT}/THIRD_PARTY_NOTICES.txt"
    DESTINATION share/vinc/licenses COMPONENT VincSDK)
install(FILES "${VINC_REPOSITORY_ROOT}/tools/configure/configure.py"
    "${VINC_REPOSITORY_ROOT}/tools/configure/Kconfig"
    "${VINC_REPOSITORY_ROOT}/tools/configure/Kconfig.python"
    "${VINC_REPOSITORY_ROOT}/tools/configure/README.md"
    DESTINATION share/vinc/configuration COMPONENT VincSDK)
set(config_ui false)
if(TARGET vinc_ui)
    set(config_ui true)
endif()
file(WRITE "${package_directory}/capabilities.json"
    "{\"UI\": ${config_ui}, \"BAG\": true, \"PYTHON\": true}\n")
install(FILES "${package_directory}/capabilities.json"
    DESTINATION share/vinc/configuration COMPONENT VincSDK)
# Keep the parser/UI pinned and offline. No pip or producer checkout is needed
# by installed consumers; Python and optional Tk/curses remain system tools.
install(DIRECTORY "${VINC_REPOSITORY_ROOT}/library/kconfiglib/upstream/"
    DESTINATION share/vinc/configuration/kconfiglib COMPONENT VincSDK
    FILES_MATCHING PATTERN "*.py" PATTERN "LICENSE.txt"
    PATTERN ".git" EXCLUDE PATTERN "tests" EXCLUDE PATTERN "examples" EXCLUDE
    PATTERN "__pycache__" EXCLUDE)

# Ship binding recipes independently of the producer's Python build option.
# Consumer applications compile the adapter with their own Python/message ABI.
set(python_source "${VINC_REPOSITORY_ROOT}/src/vinc/module_system/python")
install(DIRECTORY "${python_source}/src" "${python_source}/include" "${python_source}/vinc"
    DESTINATION share/vinc/python COMPONENT VincSDK
    PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
install(FILES "${python_source}/cmake/VincPython.cmake" "${python_source}/cmake/runtime_metadata.py"
    DESTINATION share/vinc/python/cmake COMPONENT VincSDK)
set(pybind_source "${VINC_REPOSITORY_ROOT}/library/pybind11/upstream")
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${pybind_source}" rev-parse HEAD
    OUTPUT_VARIABLE pybind_revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
file(READ "${VINC_REPOSITORY_ROOT}/library/pybind11/revision.txt" pybind_expected)
string(STRIP "${pybind_expected}" pybind_expected)
if(NOT pybind_revision STREQUAL pybind_expected)
    message(FATAL_ERROR "pybind11 revision mismatch; restore the pinned SDK build material")
endif()
install(DIRECTORY "${pybind_source}/include" "${pybind_source}/tools"
    DESTINATION share/vinc/python/pybind11 COMPONENT VincSDK
    PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
install(FILES "${pybind_source}/CMakeLists.txt" "${pybind_source}/LICENSE"
    "${VINC_REPOSITORY_ROOT}/library/pybind11/revision.txt"
    DESTINATION share/vinc/python/pybind11 COMPONENT VincSDK)

# Imported upstream archives cannot be exported by install(TARGETS). Tcl owns
# their native platform metadata and the separate resource installation below.
get_property(tcl_package_targets GLOBAL PROPERTY VINC_TCL_PACKAGE_TARGETS)
install(FILES "${tcl_package_targets}" DESTINATION lib/cmake/vinc COMPONENT VincSDK)
install(TARGETS vinc_ribosome_sdk EXPORT VincTools
    RUNTIME DESTINATION bin COMPONENT VincSDK)
set_target_properties(vinc_ribosome_sdk PROPERTIES EXPORT_NAME ribosome)
install(EXPORT VincTools NAMESPACE vinc:: DESTINATION lib/cmake/vinc COMPONENT VincSDK)
install(FILES "${VINC_REPOSITORY_ROOT}/src/vinc/ribosome/data/message_sample.txt"
    DESTINATION share/vinc/ribosome COMPONENT VincSDK)
install(DIRECTORY "${VINC_REPOSITORY_ROOT}/vinc_messages/messages/"
    DESTINATION share/vinc/messages COMPONENT VincSDK FILES_MATCHING PATTERN "*.msg")
install(FILES "${VINC_REPOSITORY_ROOT}/apps/vinc/src/main.cpp"
    DESTINATION share/vinc/application COMPONENT VincSDK)
install(FILES "${VINC_REPOSITORY_ROOT}/src/vinc/bag/module/recorder_module.cpp"
    DESTINATION share/vinc/modules/bag COMPONENT VincSDK)
if(TARGET vinc_ui)
    install(FILES "${VINC_REPOSITORY_ROOT}/src/vinc/ui/module/ui_module.cpp"
        "${VINC_REPOSITORY_ROOT}/src/vinc/ui/module/topic_monitor.cpp"
        "${VINC_REPOSITORY_ROOT}/src/vinc/ui/module/graph.cpp"
        DESTINATION share/vinc/modules/ui COMPONENT VincSDK)
endif()

if(BUILD_TESTING)
    set(VINC_TEST_SDK_PYTHON_EXECUTABLE "" CACHE FILEPATH "Interpreter/development environment for installed Python SDK integration tests")
    if(VINC_TEST_SDK_PYTHON_EXECUTABLE)
        if(TARGET vinc_ui AND CMAKE_GENERATOR STREQUAL "Ninja")
            find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
            add_test(NAME sdk.multi_app COMMAND "${Python3_EXECUTABLE}" -B
                "${VINC_REPOSITORY_ROOT}/src/vinc/sdk/tests/multi_app.py"
                --cmake "${CMAKE_COMMAND}" --compiler "${CMAKE_CXX_COMPILER}"
                --generator "${CMAKE_GENERATOR}" --make-program "${CMAKE_MAKE_PROGRAM}"
                --config "${CMAKE_BUILD_TYPE}" --python "${VINC_TEST_SDK_PYTHON_EXECUTABLE}"
                --work "${VINC_BUILD_ROOT}/multi-app-test" --sdk-build "${VINC_BUILD_ROOT}")
            set_tests_properties(sdk.multi_app PROPERTIES TIMEOUT 1200 RUN_SERIAL TRUE)
        endif()
        add_test(NAME sdk.installed_python COMMAND "${CMAKE_COMMAND}"
            "-DSDK_BUILD=${VINC_BUILD_ROOT}"
            "-DFIXTURE=${VINC_REPOSITORY_ROOT}/src/vinc/sdk/tests/installed_python"
            "-DCOMPILER=${CMAKE_CXX_COMPILER}" "-DCONFIG=${CMAKE_BUILD_TYPE}"
            "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}" "-DGENERATOR=${CMAKE_GENERATOR}"
            "-DPYTHON=${VINC_TEST_SDK_PYTHON_EXECUTABLE}"
            -P "${VINC_REPOSITORY_ROOT}/src/vinc/sdk/tests/installed_python.cmake")
        set_tests_properties(sdk.installed_python PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)
    endif()
    add_test(NAME sdk.installed_consumer COMMAND "${CMAKE_COMMAND}"
        "-DSDK_BUILD=${VINC_BUILD_ROOT}"
        "-DWORK=${VINC_BUILD_ROOT}/sdk-install-test"
        "-DFIXTURE=${VINC_REPOSITORY_ROOT}/src/vinc/sdk/tests/installed_consumer"
        "-DCOMPILER=${CMAKE_CXX_COMPILER}" "-DCONFIG=${CMAKE_BUILD_TYPE}"
        "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}" "-DGENERATOR=${CMAKE_GENERATOR}"
        -P "${VINC_REPOSITORY_ROOT}/src/vinc/sdk/tests/installed_consumer.cmake")
    set_tests_properties(sdk.installed_consumer PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    add_test(NAME sdk.installed_application COMMAND "${CMAKE_COMMAND}"
        "-DSDK_BUILD=${VINC_BUILD_ROOT}"
        "-DFIXTURE=${VINC_REPOSITORY_ROOT}/src/vinc/sdk/tests/installed_application"
        "-DCOMPILER=${CMAKE_CXX_COMPILER}" "-DCONFIG=${CMAKE_BUILD_TYPE}"
        "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}" "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DWITH_UI=${VINC_BUILD_UI}"
        -P "${VINC_REPOSITORY_ROOT}/src/vinc/sdk/tests/installed_application.cmake")
    set_tests_properties(sdk.installed_application PROPERTIES TIMEOUT 360 RUN_SERIAL TRUE)
endif()
