cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/package_inventory.cmake")
function(run_checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors
        TIMEOUT 180)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Command failed: ${ARGV}\n${output}\n${errors}")
    endif()
endfunction()

# The default test owns one fixed child. A clean SDK-only producer may instead
# supply a fresh external work directory so the producer build can be hidden later.
if(DEFINED APPLICATION_WORK)
    if(NOT IS_ABSOLUTE "${APPLICATION_WORK}" OR EXISTS "${APPLICATION_WORK}")
        message(FATAL_ERROR "APPLICATION_WORK must be a new absolute directory")
    endif()
    set(work "${APPLICATION_WORK}")
else()
    get_filename_component(work "${SDK_BUILD}/sdk-application-test" ABSOLUTE)
    file(REMOVE_RECURSE "${work}")
endif()
file(MAKE_DIRECTORY "${work}/unrelated cwd")
run_checked("${CMAKE_COMMAND}" --install "${SDK_BUILD}" --component VincSDK --prefix "${work}/sdk original")
file(RENAME "${work}/sdk original" "${work}/sdk moved")
file(COPY "${FIXTURE}/" DESTINATION "${work}/source")
set(configuration -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${COMPILER}"
    "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    "-Dvinc_DIR=${work}/sdk moved/lib/cmake/vinc" "-DWITH_UI=${WITH_UI}")
run_checked("${CMAKE_COMMAND}" -S "${work}/source" -B "${work}/consumer" ${configuration} -DEMBED_STARTUP=ON)
run_checked("${CMAKE_COMMAND}" --build "${work}/consumer" --target PackageApp --parallel 2)
if(WIN32)
    set(suffix .exe)
endif()
verify_package_inventory("${work}/sdk moved" sdk "bin/ribosome${suffix}" "${work}/sdk-inventory.txt")
verify_package_inventory("${work}/consumer/application output" app "PackageApp${suffix}"
    "${work}/build-app-inventory.txt")

function(verify_run root revision embedded)
    verify_package_inventory("${root}" app "PackageApp${suffix}" "${root}-inventory.txt")
    set(arguments --shell-port 0)
    if(NOT embedded)
        list(APPEND arguments -S "${work}/source/startup.tcl")
    endif()
    execute_process(COMMAND "${root}/PackageApp${suffix}" ${arguments}
        WORKING_DIRECTORY "${work}/unrelated cwd" RESULT_VARIABLE result
        OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 35)
    file(WRITE "${root}/test-output.txt" "${output}\n${errors}")
    if(NOT result STREQUAL "0" OR output MATCHES "APPLICATION_FLOW_FAILED"
            OR NOT output MATCHES "APPLICATION_FLOW_OK"
            OR NOT output MATCHES "RECEIVED=${revision} publisher=sender"
            OR NOT output MATCHES "CALLBACK=${revision}"
            OR NOT output MATCHES "written=[1-9][0-9]*"
            OR NOT output MATCHES "All module processes exited")
        message(FATAL_ERROR "Installed application failed: ${result}\n${output}\n${errors}")
    endif()
    if(WITH_UI AND NOT output MATCHES "CUSTOM_UI_PREVIEW_OK")
        message(FATAL_ERROR "Missing custom UI decoding evidence: ${output}")
    endif()
    string(REGEX MATCHALL "PACKAGE_HOST_PID=[0-9]+" hosts "${output}")
    list(REMOVE_DUPLICATES hosts)
    list(LENGTH hosts host_count)
    if(NOT host_count EQUAL 2)
        message(FATAL_ERROR "Expected two distinct self-host Module processes: ${output}")
    endif()
    if(NOT IS_DIRECTORY "${root}/vinc_data/sdk-package-app")
        message(FATAL_ERROR "Consumer identity was not used for application data")
    endif()
endfunction()

# Packaging copies framework resources, not live params/logs or user data.
run_checked("${CMAKE_COMMAND}" --install "${work}/consumer" --prefix "${work}/original app")
file(RENAME "${work}/original app" "${work}/moved app")
# Hide producer-consumer build outputs and the SDK itself while executing.
file(RENAME "${work}/consumer" "${work}/consumer hidden")
file(RENAME "${work}/sdk moved" "${work}/sdk hidden")
verify_run("${work}/moved app/PackageApp" 41 TRUE)
file(RENAME "${work}/sdk hidden" "${work}/sdk moved")
file(RENAME "${work}/consumer hidden" "${work}/consumer")
set(build_app "${work}/consumer/application output")
file(MAKE_DIRECTORY "${build_app}/vinc_data/sdk-package-app/ui/plugins/user.example")
file(WRITE "${build_app}/vinc_data/sdk-package-app/ui/plugins/user.example/keep.txt" "user-owned")
file(WRITE "${build_app}/vinc_data/sdk-package-app/params.toml" "user-owned")
file(REMOVE "${build_app}/licenses/open-vinc/NOTICE")
execute_process(COMMAND "${work}/moved app/PackageApp/PackageApp${suffix}" -S forbidden.tcl
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
if(NOT result STREQUAL "2" OR NOT errors MATCHES "fixed embedded startup")
    message(FATAL_ERROR "Embedded application accepted a script override")
endif()

# Editing a cross-directory leaf must rebuild both Module consumers and official
# typed integrations. Recover from invalid schema without accepting stale output.
file(WRITE "${work}/source/late_messages/AppLeaf.msg" "uint32_t revision = 42;\nuint32_t value;\n")
file(READ "${work}/source/startup.tcl" startup)
string(REPLACE "puts \"APPLICATION_FLOW_OK\"" "puts \"EMBEDDED_EDIT_OK\"\n    puts \"APPLICATION_FLOW_OK\"" startup "${startup}")
file(WRITE "${work}/source/startup.tcl" "${startup}")
file(WRITE "${work}/source/late_messages/Extra.msg" "uint64_t value;\n")
run_checked("${CMAKE_COMMAND}" --build "${work}/consumer" --target PackageApp --parallel 2)
if(NOT EXISTS "${build_app}/licenses/open-vinc/NOTICE")
    message(FATAL_ERROR "Rebuild did not repair missing deployed resources")
endif()
file(READ "${build_app}/vinc_data/sdk-package-app/ui/plugins/user.example/keep.txt" preserved)
if(NOT preserved STREQUAL "user-owned")
    message(FATAL_ERROR "Rebuild modified a user UI plugin")
endif()
run_checked("${CMAKE_COMMAND}" --install "${work}/consumer" --prefix "${work}/updated app")
if(EXISTS "${work}/updated app/PackageApp/vinc_data/sdk-package-app/params.toml"
        OR EXISTS "${work}/updated app/PackageApp/vinc_data/sdk-package-app/ui/plugins/user.example")
    message(FATAL_ERROR "Installation copied live user data from the build directory")
endif()
verify_run("${work}/updated app/PackageApp" 42 TRUE)
file(READ "${work}/updated app/PackageApp/test-output.txt" output)
if(NOT output MATCHES "EMBEDDED_EDIT_OK")
    message(FATAL_ERROR "Embedded script edit did not rebuild the application")
endif()
file(REMOVE "${work}/source/late_messages/Extra.msg")
file(WRITE "${work}/source/late_messages/AppLeaf.msg" "NotDefined value;\n")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${work}/consumer" --target PackageApp --parallel 2
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 90)
if(result STREQUAL "0" OR EXISTS "${work}/consumer/vinc_generated/include/vinc_messages/AppFrame.hpp")
    message(FATAL_ERROR "Invalid schema accepted stale generated headers: ${output}\n${errors}")
endif()
file(WRITE "${work}/source/late_messages/AppLeaf.msg" "uint32_t revision = 42;\nuint32_t value;\n")
run_checked("${CMAKE_COMMAND}" -S "${work}/source" -B "${work}/consumer" ${configuration} -DEMBED_STARTUP=OFF)
run_checked("${CMAKE_COMMAND}" --build "${work}/consumer" --target PackageApp --parallel 2)
if(EXISTS "${work}/consumer/vinc_generated/include/vinc_messages/Extra.hpp")
    message(FATAL_ERROR "Deleted schema left a stale header")
endif()
run_checked("${CMAKE_COMMAND}" --install "${work}/consumer" --prefix "${work}/plain app")
verify_run("${work}/plain app/PackageApp" 42 FALSE)
foreach(case IN ITEMS DUPLICATE_MODULE MISSING_MESSAGES)
    if(case STREQUAL "DUPLICATE_MODULE")
        set(expected "Duplicate ModuleType")
    else()
        set(expected "Message directory does not exist")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${work}/source" -B "${work}/reject-${case}"
        ${configuration} "-DTEST_${case}=ON" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors
        TIMEOUT 45)
    if(result STREQUAL "0" OR NOT errors MATCHES "${expected}")
        message(FATAL_ERROR "Invalid composition not rejected as expected: ${case}\n${output}\n${errors}")
    endif()
endforeach()

# Official Modules are no longer removable App features. Selection cleanup and
# per-App ownership are exercised by sdk.multi_app.
message(STATUS "Installed application: relocation, custom Bus/Bag/UI, incremental messages and rejection checks passed")
