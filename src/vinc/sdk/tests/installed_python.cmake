cmake_minimum_required(VERSION 3.20)
function(run_checked label)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors
        TIMEOUT 240)
    file(WRITE "${work}/${label}.log" "${output}\n${errors}")
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "${label} failed (${result}): ${output}\n${errors}")
    endif()
endfunction()

# This fixture owns only a fixed child of the configured SDK build directory.
get_filename_component(work "${SDK_BUILD}/sdk-python-test" ABSOLUTE)
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}/unrelated cwd")
run_checked(install_sdk "${CMAKE_COMMAND}" --install "${SDK_BUILD}" --component VincSDK --prefix "${work}/original SDK")
file(RENAME "${work}/original SDK" "${work}/moved SDK")
file(COPY "${FIXTURE}/" DESTINATION "${work}/source")
set(configuration -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${COMPILER}"
    "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    "-Dvinc_DIR=${work}/moved SDK/lib/cmake/vinc" "-DPython_EXECUTABLE=${PYTHON}"
    -DCMAKE_DISABLE_FIND_PACKAGE_Git=ON)
run_checked(configure "${CMAKE_COMMAND}" -S "${work}/source" -B "${work}/consumer" ${configuration})
run_checked(build "${CMAKE_COMMAND}" --build "${work}/consumer" --target PythonApp --parallel 2)
set(build_app "${work}/consumer/application output")
if(WIN32)
    set(suffix .exe)
endif()
function(verify_app root revision extra)
    file(MAKE_DIRECTORY "${root}/data/module/PythonPeer")
    file(COPY "${work}/source/python/PythonPeer.py" DESTINATION "${root}/data/module/PythonPeer")
    run_checked("import-${revision}-${extra}" "${PYTHON}" -B "${work}/source/check_import.py" "${root}" "${revision}" "${extra}")
    file(READ "${work}/source/startup.tcl" script)
    string(REPLACE "@REVISION@" "${revision}" script "${script}")
    file(WRITE "${work}/run.tcl" "${script}")
    execute_process(COMMAND "${root}/PythonApp${suffix}" --shell-port 0 --python "${PYTHON}"
        -S "${work}/run.tcl" WORKING_DIRECTORY "${work}/unrelated cwd"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 35)
    file(WRITE "${work}/flow-${revision}.log" "${output}\n${errors}")
    if(NOT result STREQUAL "0" OR output MATCHES "INSTALLED_PYTHON_FAILED|INVALID_PYTHON_PAYLOAD"
            OR NOT output MATCHES "PYTHON_FLOW_REVISION=${revision}"
            OR NOT output MATCHES "INSTALLED_PYTHON_OK"
            OR NOT output MATCHES "NATIVE_ALIVE"
            OR NOT output MATCHES "NATIVE_SELF_HOST_OK"
            OR NOT output MATCHES "All module processes exited")
        message(FATAL_ERROR "Installed Python flow failed: ${output}\n${errors}")
    endif()
    string(REGEX MATCHALL "PID=[0-9]+" pids "${output}")
    list(REMOVE_DUPLICATES pids)
    list(LENGTH pids process_count)
    if(NOT process_count EQUAL 3)
        message(FATAL_ERROR "Expected distinct central/native/Python processes: ${output}")
    endif()
    foreach(file _build.py licenses/open-vinc/NOTICE licenses/pybind11/LICENSE)
        if(NOT EXISTS "${root}/python/vinc/${file}")
            message(FATAL_ERROR "Incomplete Python package: ${file}")
        endif()
    endforeach()
endfunction()

run_checked(install_app "${CMAKE_COMMAND}" --install "${work}/consumer" --prefix "${work}/original app")
file(RENAME "${work}/original app" "${work}/moved app")
file(RENAME "${work}/moved SDK" "${work}/hidden SDK")
file(RENAME "${work}/consumer" "${work}/hidden consumer")
verify_app("${work}/moved app/PythonApp" 41 no)
file(RENAME "${work}/hidden consumer" "${work}/consumer")
file(RENAME "${work}/hidden SDK" "${work}/moved SDK")

file(WRITE "${work}/source/messages/SdkPacket.msg" "uint32_t revision = 42;\nuint32_t value;\n")
file(WRITE "${work}/source/messages/AddedPacket.msg" "SdkPacket packet;\n")
run_checked(added_build "${CMAKE_COMMAND}" --build "${work}/consumer" --target PythonApp --parallel 2)
verify_app("${build_app}" 42 yes)
file(REMOVE "${work}/source/messages/AddedPacket.msg")
file(WRITE "${work}/source/messages/SdkPacket.msg" "Missing value;\n")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${work}/consumer" --target PythonApp --parallel 2
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 90)
if(result STREQUAL "0" OR NOT "${output}${errors}" MATCHES "Unknown type")
    message(FATAL_ERROR "Invalid message did not stop Python build: ${output}\n${errors}")
endif()
file(WRITE "${work}/source/messages/SdkPacket.msg" "uint32_t revision = 42;\nuint32_t value;\n")
run_checked(recovery_build "${CMAKE_COMMAND}" --build "${work}/consumer" --target PythonApp --parallel 2)
run_checked(removed_import "${PYTHON}" -B "${work}/source/check_import.py" "${build_app}" 42 no)

# Wrong ABI must reject just the Python Module, with the central still able to
# command another Module and complete its normal shutdown sequence.
set(metadata "${build_app}/python/vinc/_build.py")
file(READ "${metadata}" original_metadata)
file(APPEND "${metadata}" "\nVERSION = (0, 0)\n")
file(WRITE "${work}/bad-python.tcl"
    "NativePeer\nif {[catch {PythonPeer --name rejected}]} {puts PYTHON_REJECTED}\nnative status\nexit\n")
execute_process(COMMAND "${build_app}/PythonApp${suffix}" --shell-port 0 --python "${PYTHON}"
    -S "${work}/bad-python.tcl" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 35)
file(WRITE "${work}/wrong-abi.log" "${output}\n${errors}")
file(WRITE "${metadata}" "${original_metadata}")
if(NOT result STREQUAL "0" OR NOT output MATCHES "PYTHON_REJECTED"
        OR NOT output MATCHES "NATIVE_ALIVE" OR NOT output MATCHES "All module processes exited")
    message(FATAL_ERROR "Python startup failure escaped isolation: ${output}\n${errors}")
endif()

# Rebuilding an up-to-date extension repairs deleted deployment resources.
file(REMOVE "${metadata}")
run_checked(repair "${CMAKE_COMMAND}" --build "${work}/consumer" --target PythonApp --parallel 2)
if(NOT EXISTS "${metadata}")
    message(FATAL_ERROR "Missing Python deployment was not repaired")
endif()
file(WRITE "${build_app}/python/vinc/user-owned.txt" "keep")
file(WRITE "${work}/source/vinc.conf" "CONFIG_VINC_MODULE_NativePeer=y\nCONFIG_VINC_APP_PYTHON=n\n")
run_checked(disabled_build "${CMAKE_COMMAND}" --build "${work}/consumer" --target PythonApp --parallel 2)
run_checked(disabled_install "${CMAKE_COMMAND}" --install "${work}/consumer" --prefix "${work}/moved app")
foreach(root "${build_app}" "${work}/moved app/PythonApp")
    file(GLOB native_files "${root}/python/vinc/_native*")
    if(native_files OR EXISTS "${root}/python/vinc/__init__.py")
        message(FATAL_ERROR "Disabled Python left managed package files")
    endif()
    if(NOT EXISTS "${root}/data/module/PythonPeer/PythonPeer.py")
        message(FATAL_ERROR "Python cleanup deleted user Module source")
    endif()
endforeach()
if(NOT EXISTS "${build_app}/python/vinc/user-owned.txt")
    message(FATAL_ERROR "Cleanup removed untracked file")
endif()
message(STATUS "Installed Python: relocation, custom Bus/RPC/Param, schema incremental changes, ABI isolation, repair and cleanup passed")
