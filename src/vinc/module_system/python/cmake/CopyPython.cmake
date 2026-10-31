cmake_minimum_required(VERSION 3.20)
include("${MANIFEST}")
# Copy only this build's declared resources, never another ABI's stale extension
# or user-created files. Application ownership tracking handles removed entries.
foreach(relative IN LISTS resources)
    get_filename_component(parent "${destination}/${relative}" DIRECTORY)
    file(MAKE_DIRECTORY "${parent}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${source}/${relative}" "${destination}/${relative}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
