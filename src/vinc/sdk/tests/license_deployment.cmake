cmake_minimum_required(VERSION 3.20)
if(NOT IS_ABSOLUTE "${TEST_BINARY_DIR}" OR NOT IS_ABSOLUTE "${VINC_SOURCE_DIR}")
    message(FATAL_ERROR "License fixture requires absolute paths")
endif()
# These paths are embedded in generated CMake source, where native Windows
# backslashes would otherwise become escape sequences.
file(TO_CMAKE_PATH "${TEST_BINARY_DIR}" TEST_BINARY_DIR)
file(TO_CMAKE_PATH "${VINC_SOURCE_DIR}" VINC_SOURCE_DIR)
set(source "${TEST_BINARY_DIR}/fixture source")
set(binary "${TEST_BINARY_DIR}/fixture build")
set(output "${binary}/shared output/${CONFIG}")
file(MAKE_DIRECTORY "${source}" "${output}/licenses/open-vinc")
file(WRITE "${source}/main.cpp" "int main() { return 0; }\n")
file(WRITE "${source}/value.cpp" "int value() { return 1; }\n")
file(WRITE "${output}/LICENSE" "Application license sentinel\n")
file(WRITE "${output}/licenses/open-vinc/user-note.txt" "Preserve unrelated files\n")
set(project [=[
cmake_minimum_required(VERSION 3.20)
project(license_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include("@VINC_SOURCE_DIR@/src/vinc/sdk/cmake/VincLicenses.cmake")
add_executable(notice_app main.cpp)
add_library(notice_archive STATIC value.cpp)
vinc_deploy_licenses(notice_app)
vinc_deploy_licenses(notice_app)
vinc_deploy_licenses(notice_archive)
# The helper must resolve final output locations, not this earlier call site.
set_target_properties(notice_app PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared output/$<CONFIG>")
set_target_properties(notice_archive PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/shared output/$<CONFIG>")
]=])
string(CONFIGURE "${project}" project @ONLY)
file(WRITE "${source}/CMakeLists.txt" "${project}")

function(run_checked label)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 90)
    file(WRITE "${TEST_BINARY_DIR}/${label}.log" "${output}\n${errors}")
    if(NOT status STREQUAL "0")
        message(FATAL_ERROR "License ${label} failed: ${output}\n${errors}")
    endif()
endfunction()

function(check_delivery root)
    foreach(name IN ITEMS LICENSE NOTICE THIRD_PARTY_NOTICES.txt)
        set(deployed "${root}/licenses/open-vinc/${name}")
        if(NOT EXISTS "${deployed}")
            message(FATAL_ERROR "Missing attribution: ${deployed}")
        endif()
        file(SHA256 "${VINC_SOURCE_DIR}/${name}" expected)
        file(SHA256 "${deployed}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Attribution changed in deployment: ${deployed}")
        endif()
    endforeach()
endfunction()

run_checked(configure "${CMAKE_COMMAND}" -S "${source}" -B "${binary}"
    -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${COMPILER}"
    "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}" "-DCMAKE_BUILD_TYPE=${CONFIG}")
run_checked(build "${CMAKE_COMMAND}" --build "${binary}" --config "${CONFIG}" --parallel 2)
check_delivery("${output}")

# Up-to-date targets must restore removed notices without a source edit/relink.
file(REMOVE "${output}/licenses/open-vinc/NOTICE" "${output}/licenses/open-vinc/THIRD_PARTY_NOTICES.txt")
run_checked(restore "${CMAKE_COMMAND}" --build "${binary}" --config "${CONFIG}" --target notice_app)
check_delivery("${output}")
file(READ "${output}/LICENSE" application_license)
file(READ "${output}/licenses/open-vinc/user-note.txt" unrelated)
if(NOT application_license STREQUAL "Application license sentinel\n" OR
        NOT unrelated STREQUAL "Preserve unrelated files\n")
    message(FATAL_ERROR "License deployment overwrote application-owned files")
endif()

set(relocated "${TEST_BINARY_DIR}/relocated package")
file(MAKE_DIRECTORY "${relocated}")
file(COPY "${output}/licenses" DESTINATION "${relocated}")
check_delivery("${relocated}")

# Missing source terms must fail before overwriting an existing destination.
file(MAKE_DIRECTORY "${TEST_BINARY_DIR}/incomplete source")
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_ROOT=${TEST_BINARY_DIR}/incomplete source"
    "-DDESTINATION=${output}/licenses/open-vinc"
    -P "${VINC_SOURCE_DIR}/src/vinc/sdk/cmake/DeployLicenses.cmake"
    RESULT_VARIABLE missing OUTPUT_VARIABLE missing_output ERROR_VARIABLE missing_error TIMEOUT 10)
if(missing STREQUAL "0" OR NOT "${missing_output}${missing_error}" MATCHES "Missing Open-VINC license input")
    message(FATAL_ERROR "Missing license input did not fail closed")
endif()
check_delivery("${output}")
