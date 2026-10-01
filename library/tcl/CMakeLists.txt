# Build the pinned, unmodified Tcl dependency with its upstream build system.
# Keep artifacts in the build tree and copy runtime scripts/licenses beside each consumer.

include(ExternalProject)
if(CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "Tcl integration uses one build configuration per directory; select Ninja and CMAKE_BUILD_TYPE")
endif()
set(tcl_source "${CMAKE_CURRENT_SOURCE_DIR}/upstream")
if(NOT EXISTS "${tcl_source}/generic/tcl.h")
    message(FATAL_ERROR "Initialize the pinned Tcl submodule: git submodule update --init --recursive")
endif()
if(EXISTS "${tcl_source}/.git")
    find_package(Git REQUIRED)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tcl_source}" rev-parse HEAD
        OUTPUT_VARIABLE tcl_actual_revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/revision.txt" tcl_expected_revision)
    string(STRIP "${tcl_expected_revision}" tcl_expected_revision)
    if(NOT tcl_actual_revision STREQUAL tcl_expected_revision)
        message(FATAL_ERROR "Tcl submodule differs from library/tcl/revision.txt; restore the pin or explicitly update the dependency metadata")
    endif()
endif()
set(tcl_build "${CMAKE_CURRENT_BINARY_DIR}/build")
file(MAKE_DIRECTORY "${tcl_build}")
if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "The Windows Tcl integration currently requires MSVC/NMAKE")
    endif()
    find_program(TCL_NMAKE nmake REQUIRED)
    set(tcl_options "static,noembed")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        string(APPEND tcl_options ",symbols")
    endif()
    set(tcl_archive "${tcl_build}/out/tcl91s.lib")
    set(tcl_stub_archive "${tcl_build}/out/tclstub.lib")
    file(TO_NATIVE_PATH "${tcl_build}/out" tcl_out_native)
    file(TO_NATIVE_PATH "${tcl_build}/obj" tcl_tmp_native)
    # Upstream NMAKE requires its source win/ as cwd and writes helper files there.
    # Build an unmodified staging copy, keeping the pinned checkout untouched.
    ExternalProject_Add(vinc_tcl_build SOURCE_DIR "${tcl_source}" BINARY_DIR "${tcl_build}/source/win"
        DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
        CONFIGURE_COMMAND ${CMAKE_COMMAND} "-DSOURCE=${tcl_source}" "-DSTAGE=${tcl_build}/source" -P "${CMAKE_CURRENT_SOURCE_DIR}/stage.cmake"
        BUILD_COMMAND ${CMAKE_COMMAND} -E env "CL=/utf-8" "VSLANG=1033" "${TCL_NMAKE}" -nologo -f makefile.vc
            "OPTS=${tcl_options}" "OUT_DIR=${tcl_out_native}" "TMP_DIR=${tcl_tmp_native}" core "${tcl_out_native}\\tclstub.lib"
        INSTALL_COMMAND "" BUILD_BYPRODUCTS "${tcl_archive}" "${tcl_stub_archive}")
    add_library(vinc_tcl_stub STATIC IMPORTED GLOBAL)
    set_target_properties(vinc_tcl_stub PROPERTIES IMPORTED_LOCATION "${tcl_stub_archive}")
    add_dependencies(vinc_tcl_stub vinc_tcl_build)
    set(tcl_system_libs vinc_tcl_stub netapi32 user32 userenv ws2_32 advapi32)
else()
    find_program(TCL_MAKE NAMES gmake make REQUIRED)
    find_package(Threads REQUIRED)
    find_package(ZLIB REQUIRED)
    set(tcl_archive "${tcl_build}/libtcl9.1.a")
    ExternalProject_Add(vinc_tcl_build SOURCE_DIR "${tcl_source}" BINARY_DIR "${tcl_build}"
        DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
        CONFIGURE_COMMAND "${tcl_source}/unix/configure" --disable-shared --disable-zipfs
        BUILD_COMMAND "${TCL_MAKE}" libtcl9.1.a
        INSTALL_COMMAND "" BUILD_BYPRODUCTS "${tcl_archive}")
    set(tcl_system_libs Threads::Threads ZLIB::ZLIB ${CMAKE_DL_LIBS} m)
    if(APPLE)
        list(APPEND tcl_system_libs "-framework CoreFoundation")
    endif()
endif()
add_library(vinc_tcl STATIC IMPORTED GLOBAL)
set_target_properties(vinc_tcl PROPERTIES IMPORTED_LOCATION "${tcl_archive}"
    INTERFACE_INCLUDE_DIRECTORIES "${tcl_source}/generic"
    INTERFACE_COMPILE_DEFINITIONS STATIC_BUILD
    INTERFACE_LINK_LIBRARIES "${tcl_system_libs}")
add_dependencies(vinc_tcl vinc_tcl_build)
# Deploy beside each executable; no dependency on the source tree or system Tcl.
function(vinc_deploy_tcl target)
    set(destination "${CMAKE_CURRENT_BINARY_DIR}")
    if(ARGC GREATER 1)
        set(destination "${ARGV1}")
    endif()
    # One resource-copy target per destination prevents different executable builds
    # from writing the same Tcl resource directory concurrently.
    string(MD5 key "${destination}")
    if(NOT TARGET vinc_tcl_resources_${key})
        add_custom_target(vinc_tcl_resources_${key}
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${tcl_source}/library" "${destination}/lib/tcl9.1"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${tcl_source}/license.terms" "${destination}/lib/tcl9.1/license.terms"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}/lib/tcl9.1/licenses"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${tcl_source}/utf8proc/LICENSE.md" "${destination}/lib/tcl9.1/licenses/utf8proc.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${tcl_source}/libtommath/LICENSE" "${destination}/lib/tcl9.1/licenses/libtommath.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${tcl_source}/compat/zlib/LICENSE" "${destination}/lib/tcl9.1/licenses/zlib.txt"
            VERBATIM)
    endif()
    add_dependencies(${target} vinc_tcl_resources_${key})
endfunction()
set(tcl_source "${tcl_source}" CACHE INTERNAL "Pinned Tcl source directory")
