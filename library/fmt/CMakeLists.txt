cmake_policy(SET CMP0077 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SHARED_LIBS OFF)
set(FMT_DOC OFF)
set(FMT_INSTALL OFF)
set(FMT_TEST OFF)
set(FMT_FUZZ OFF)
set(FMT_CUDA_TEST OFF)
set(FMT_MODULE OFF)
set(FMT_PEDANTIC OFF)
set(FMT_WERROR OFF)
set(FMT_OS OFF)
set(FMT_SYSTEM_HEADERS ON)
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}/upstream" rev-parse HEAD
    OUTPUT_VARIABLE actual_revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/revision.txt" expected_revision)
string(STRIP "${expected_revision}" expected_revision)
if(NOT actual_revision STREQUAL expected_revision)
    message(FATAL_ERROR "Dependency revision mismatch; initialize the pinned submodule")
endif()
add_subdirectory(upstream EXCLUDE_FROM_ALL)
set_target_properties(fmt PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

if(MSVC AND VINC_DECLARE_PYTHON_RUNTIME)
    vinc_apply_runtime(fmt NATIVE)
    # The pinned upstream's compiled implementation is format.cc with FMT_OS=0.
    # Do not link its /MD archive into the Python image or patch upstream files.
    add_library(fmt_python_runtime STATIC EXCLUDE_FROM_ALL upstream/src/format.cc)
    vinc_apply_runtime(fmt_python_runtime PYTHON)
    target_include_directories(fmt_python_runtime SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/upstream/include")
    target_compile_definitions(fmt_python_runtime PRIVATE FMT_OS=0)
    target_compile_options(fmt_python_runtime PUBLIC /utf-8)
    if(DEFINED FMT_UNICODE AND NOT FMT_UNICODE)
        target_compile_definitions(fmt_python_runtime PUBLIC FMT_UNICODE=0)
    endif()
    if(FMT_SAFE_DURATION_CAST)
        target_compile_definitions(fmt_python_runtime PUBLIC FMT_SAFE_DURATION_CAST)
    endif()
    set_target_properties(fmt_python_runtime PROPERTIES
        POSITION_INDEPENDENT_CODE ON CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

if(VINC_INSTALL_SDK)
    set_target_properties(fmt PROPERTIES EXPORT_NAME detail_fmt)
    # Upstream records PUBLIC_HEADER relative to its own directory. Installation
    # is owned by this wrapper, so preserve that base explicitly.
    get_target_property(fmt_headers fmt PUBLIC_HEADER)
    list(TRANSFORM fmt_headers PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/upstream/")
    set_target_properties(fmt PROPERTIES PUBLIC_HEADER "${fmt_headers}")
    install(TARGETS fmt EXPORT VincTargets ARCHIVE DESTINATION lib COMPONENT VincSDK
        PUBLIC_HEADER DESTINATION include/fmt COMPONENT VincSDK)
    if(TARGET fmt_python_runtime)
        set_target_properties(fmt_python_runtime PROPERTIES
            EXCLUDE_FROM_ALL FALSE EXPORT_NAME detail_fmt_python_runtime
            INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/upstream/include>;$<INSTALL_INTERFACE:include>"
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/upstream/include>;$<INSTALL_INTERFACE:include>")
        install(TARGETS fmt_python_runtime EXPORT VincTargets
            ARCHIVE DESTINATION lib COMPONENT VincSDK)
    endif()
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/upstream/include/fmt"
        DESTINATION include COMPONENT VincSDK FILES_MATCHING PATTERN "*.h")
endif()
