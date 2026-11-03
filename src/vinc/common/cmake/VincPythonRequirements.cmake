include_guard(GLOBAL)

# Build policy only: including this file never searches for or links Python.
function(vinc_python_minimum output)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/python_minimum.txt")
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/python_minimum.txt" value)
    string(STRIP "${value}" value)
    if(NOT value MATCHES "^3\\.[0-9]+$")
        message(FATAL_ERROR "Invalid Open-VINC Python minimum version policy")
    endif()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

# A macro preserves FindPython's variables and imported targets in caller scope.
# REQUIRED errors are formatted here; QUIET probes remain optional.
macro(vinc_find_python package)
    cmake_parse_arguments(_vinc_python "REQUIRED;QUIET" "" "COMPONENTS" ${ARGN})
    vinc_python_minimum(_vinc_python_minimum)
    set(_vinc_python_requested "${${package}_EXECUTABLE}")
    find_package(${package} ${_vinc_python_minimum}...<4 QUIET
        COMPONENTS ${_vinc_python_COMPONENTS})
    if(NOT ${package}_FOUND AND _vinc_python_REQUIRED)
        set(_vinc_python_path "${${package}_EXECUTABLE}")
        if(_vinc_python_requested)
            set(_vinc_python_path "${_vinc_python_requested}")
        endif()
        set(_vinc_python_found "${${package}_VERSION}")
        if(EXISTS "${_vinc_python_path}")
            execute_process(COMMAND "${_vinc_python_path}" -c
                "import sys; print('.'.join(map(str, sys.version_info[:3])))"
                OUTPUT_VARIABLE _vinc_python_probe OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE _vinc_python_status TIMEOUT 10)
            if(_vinc_python_status STREQUAL "0")
                set(_vinc_python_found "${_vinc_python_probe}")
            endif()
        endif()
        if(NOT _vinc_python_found)
            set(_vinc_python_found "unavailable (see discovery reason below)")
        endif()
        if(NOT _vinc_python_path OR _vinc_python_path MATCHES "-NOTFOUND$")
            set(_vinc_python_path "not found")
        endif()
        set(_vinc_python_advice "Prepare recommended dependencies from an Open-VINC checkout: tools\\setup.bat --recommended (Windows) or bash tools/setup.sh --recommended (Linux/macOS). Select a compatible interpreter and matching development files with -D${package}_EXECUTABLE=/path/to/python.")
        if(NOT EXISTS "${_vinc_python_path}")
            set(_vinc_python_advice "Prepare dependencies from an Open-VINC checkout: tools\\setup.bat (Windows) or bash tools/setup.sh (Linux/macOS). Then select -D${package}_EXECUTABLE=/path/to/python.")
        endif()
        if("${package}" STREQUAL "Python")
            set(_vinc_python_advice "Select System/Custom Module Python in App Kconfig and install its matching development files. Python_EXECUTABLE is not a public setting. Dependency guidance: tools/setup.bat --recommended or bash tools/setup.sh --recommended.")
        endif()
        message(FATAL_ERROR "Open-VINC Python requirements are not satisfied.\nRequired: Python >= ${_vinc_python_minimum} and < 4; ${_vinc_python_COMPONENTS}\nFound: ${_vinc_python_found}\nPath: ${_vinc_python_path}\nReason: ${${package}_NOT_FOUND_MESSAGE}\n${_vinc_python_advice}")
    endif()
endmacro()
