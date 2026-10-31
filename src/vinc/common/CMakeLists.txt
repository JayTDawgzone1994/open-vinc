# Shared framework types; keep this layer independent of runtime components.
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vinc_common LANGUAGES CXX)
    include(CTest)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/VincErrors.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/VincMsvcDependencies.cmake")
add_library(vinc_common STATIC src/error.cpp src/contract.cpp src/runtime_services.cpp src/time.cpp)
add_library(vinc::common ALIAS vinc_common)

# The catalog is a separate composition target, never a dependency of common.
set(error_catalog_input "${CMAKE_CURRENT_BINARY_DIR}/generated/error_catalog.cpp.in")
set(error_catalog_source "${CMAKE_CURRENT_BINARY_DIR}/generated/error_catalog.cpp")
# Common owns both the output rule and its consuming target. The finalizer writes
# a configure-time input; a normal clean/build can recreate the compiled source.
add_custom_command(OUTPUT "${error_catalog_source}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${error_catalog_input}" "${error_catalog_source}"
    DEPENDS "${error_catalog_input}"
    VERBATIM)
add_library(vinc_errors STATIC "${error_catalog_source}")
add_library(vinc::errors ALIAS vinc_errors)
target_link_libraries(vinc_errors PUBLIC vinc::common)
set_target_properties(vinc_errors PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    VINC_ERROR_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _vinc_require_finalized_errors)

target_include_directories(vinc_common PUBLIC include)
target_compile_features(vinc_common PUBLIC cxx_std_23)
set_target_properties(vinc_common PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# Metadata remains separate from runtime code, including in standalone Common builds.
add_library(vinc_common_errors INTERFACE)
target_link_libraries(vinc_common_errors INTERFACE vinc::common)
vinc_register_error_catalog(TARGET vinc_common_errors
    HEADER vinc/detail/common_error_catalog.hpp SYMBOL vinc::common::detail::error_catalog)

if(MSVC)
    target_compile_options(vinc_common PRIVATE /W4 /utf-8)
else()
    target_compile_options(vinc_common PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_executable(common_time_tests tests/time_tests.cpp)
    target_link_libraries(common_time_tests PRIVATE vinc::common vinc::errors)
    add_test(NAME common.time COMMAND common_time_tests)
    set_tests_properties(common.time PROPERTIES TIMEOUT 10)

    add_executable(common_runtime_services_tests tests/runtime_services_tests.cpp)
    target_link_libraries(common_runtime_services_tests PRIVATE vinc::common vinc::errors)
    set_target_properties(common_runtime_services_tests PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(common_runtime_services_tests PRIVATE /W4 /utf-8)
    else()
        target_compile_options(common_runtime_services_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME common.runtime_services COMMAND "${CMAKE_COMMAND}"
        "-DPROBE=$<TARGET_FILE:common_runtime_services_tests>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_services.cmake")
    set_tests_properties(common.runtime_services PROPERTIES TIMEOUT 45)

    add_executable(common_error_tests tests/error_tests.cpp)
    target_link_libraries(common_error_tests PRIVATE vinc::common vinc::errors)
    add_test(NAME common.error COMMAND common_error_tests)
    set_tests_properties(common.error PROPERTIES TIMEOUT 10)
    add_executable(common_error_chain_tests tests/error_chain_tests.cpp)
    target_link_libraries(common_error_chain_tests PRIVATE vinc::common vinc::errors)
    add_test(NAME common.error_chain COMMAND common_error_chain_tests)
    set_tests_properties(common.error_chain PROPERTIES TIMEOUT 10)
    add_test(NAME common.error_chain_configurations COMMAND "${CMAKE_COMMAND}"
        "-DCOMMON_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/chain-configurations"
        "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DTOOLSET=${CMAKE_GENERATOR_TOOLSET}" "-DCOMPILER=${CMAKE_CXX_COMPILER}"
        "-DCONFIG=$<CONFIG>" "-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/chain_configurations.cmake")
    set_tests_properties(common.error_chain_configurations PROPERTIES TIMEOUT 240)
    add_test(NAME common.error_catalog COMMAND "${CMAKE_COMMAND}"
        "-DCOMMON_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/catalog_fixture"
        "-DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/catalog-tests"
        "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DTOOLSET=${CMAKE_GENERATOR_TOOLSET}" "-DCOMPILER=${CMAKE_CXX_COMPILER}"
        "-DCONFIG=$<CONFIG>" "-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/catalog_validation.cmake")
    set_tests_properties(common.error_catalog PROPERTIES TIMEOUT 180)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    vinc_finalize_error_catalog()
endif()

# Installation keeps ownership here; standalone component builds remain unchanged.
if(COMMAND vinc_install_component)
    vinc_install_component(TARGETS
        vinc_common vinc_errors vinc_common_errors)
endif()
