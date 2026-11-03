cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/package_inventory.cmake")
get_filename_component(work "${TEST_BUILD}/inventory-test" ABSOLUTE)
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}/sdk/bin" "${work}/app")
get_filename_component(tool_name "${TOOL}" NAME)
file(COPY "${TOOL}" DESTINATION "${work}/sdk/bin")
configure_file("${TOOL}" "${work}/app/PackageApp${SUFFIX}" COPYONLY)
verify_package_inventory("${work}/sdk" sdk "bin/${tool_name}" "${work}/sdk.txt")
verify_package_inventory("${work}/app" app "PackageApp${SUFFIX}" "${work}/app.txt")

function(expect_rejected kind entry diagnostic)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DINVENTORY_ROOT=${work}/${kind}" "-DINVENTORY_KIND=${kind}"
        "-DINVENTORY_EXECUTABLE=${entry}" "-DINVENTORY_REPORT=${work}/rejected.txt"
        -P "${CMAKE_CURRENT_LIST_DIR}/package_inventory.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
    if(result STREQUAL "0" OR NOT errors MATCHES "${diagnostic}")
        message(FATAL_ERROR "Inventory missed ${diagnostic}: ${output}\n${errors}")
    endif()
endfunction()

# Detect even a non-executable leftover Host; then detect a differently named image.
file(WRITE "${work}/sdk/bin/ModuleHost${SUFFIX}" "leftover")
expect_rejected(sdk "bin/${tool_name}" "Unexpected Host/test artifact")
file(REMOVE "${work}/sdk/bin/ModuleHost${SUFFIX}")
configure_file("${TOOL}" "${work}/app/OtherProgram${SUFFIX}" COPYONLY)
expect_rejected(app "PackageApp${SUFFIX}" "Unexpected native image")
file(REMOVE "${work}/app/OtherProgram${SUFFIX}")
verify_package_inventory("${work}/sdk" sdk "bin/${tool_name}" "${work}/sdk.txt")
verify_package_inventory("${work}/app" app "PackageApp${SUFFIX}" "${work}/app.txt")
