include_guard(GLOBAL)

# Shared by source builds and the installed package. Only native MSVC is
# classified here; clang-cl's emulated _MSC_VER is not an MSVC version.
function(vinc_check_toolchain out_error)
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/msvc_minimum.txt" minimum)
    string(STRIP "${minimum}" minimum)
    if(NOT minimum MATCHES "^[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "Invalid Open-VINC MSVC minimum version policy")
    endif()

    foreach(language IN ITEMS C CXX)
        if(CMAKE_${language}_COMPILER_LOADED AND
           CMAKE_${language}_COMPILER_ID STREQUAL "MSVC")
            set(version "${CMAKE_${language}_COMPILER_VERSION}")
            if(version STREQUAL "" OR version VERSION_LESS minimum)
                set(${out_error}
                    "Open-VINC requires MSVC ${minimum} or newer; ${language} compiler is '${version}'. Select a supported toolset and a compatible build directory."
                    PARENT_SCOPE)
                return()
            endif()
        endif()
    endforeach()
    set(${out_error} "" PARENT_SCOPE)
endfunction()

function(vinc_require_toolchain)
    vinc_check_toolchain(error)
    if(error)
        message(FATAL_ERROR "${error}")
    endif()
endfunction()
