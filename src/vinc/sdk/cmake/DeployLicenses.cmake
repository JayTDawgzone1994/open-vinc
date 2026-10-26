cmake_minimum_required(VERSION 3.20)

if(NOT IS_ABSOLUTE "${SOURCE_ROOT}" OR NOT IS_ABSOLUTE "${DESTINATION}")
    message(FATAL_ERROR "License deployment requires absolute source and destination paths")
endif()
foreach(name IN ITEMS LICENSE NOTICE THIRD_PARTY_NOTICES.txt)
    if(NOT EXISTS "${SOURCE_ROOT}/${name}")
        message(FATAL_ERROR "Missing Open-VINC license input: ${SOURCE_ROOT}/${name}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${DESTINATION}")
get_filename_component(destination "${DESTINATION}" REALPATH)
# Multiple targets may share an output directory. Serialize these build-time
# copies so parallel builds never leave a partially replaced attribution file.
set(license_copy_timeout_seconds 60)
file(LOCK "${destination}/.deploy.lock" GUARD PROCESS
    TIMEOUT ${license_copy_timeout_seconds} RESULT_VARIABLE locked)
if(NOT locked STREQUAL "0")
    message(FATAL_ERROR "Cannot lock license deployment: ${locked}")
endif()

foreach(name IN ITEMS LICENSE NOTICE THIRD_PARTY_NOTICES.txt)
    configure_file("${SOURCE_ROOT}/${name}" "${destination}/${name}" COPYONLY)
endforeach()
