# Prepare an isolated upstream source copy for NMAKE's in-source helper generation.
# The submodule checkout stays untouched; only the staging copy receives generated metadata.

file(COPY "${SOURCE}/" DESTINATION "${STAGE}" PATTERN ".git" EXCLUDE)
file(READ "${CMAKE_CURRENT_LIST_DIR}/revision.txt" revision)
string(STRIP "${revision}" revision)
# This is the same generated manifest the upstream NMAKE build normally creates.
# The staging directory is not a Git checkout; use the tracked dependency pin.
file(READ "${SOURCE}/win/gitmanifest.in" prefix)
file(WRITE "${STAGE}/manifest.uuid" "${prefix}${revision}\n")
