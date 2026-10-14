# Upstream headers are used unchanged; no upstream options leak into parent scope.
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}/upstream" rev-parse HEAD
    OUTPUT_VARIABLE actual_revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/revision.txt" expected_revision)
string(STRIP "${expected_revision}" expected_revision)
if(NOT actual_revision STREQUAL expected_revision)
    message(FATAL_ERROR "Initialize the pinned json submodule")
endif()
add_library(vinc_ui_json INTERFACE)
target_include_directories(vinc_ui_json SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/upstream/single_include")
