# Included only by the isolated ribosome_generation.cmake test checkout.
# Each production header must compile as the first include in its own TU.
file(GLOB message_files "${CMAKE_SOURCE_DIR}/messages/*.msg")
set(header_names all_messages)
foreach(message_file IN LISTS message_files)
    get_filename_component(message_name "${message_file}" NAME_WE)
    list(APPEND header_names "${message_name}")
endforeach()
set(smoke_sources)
foreach(header_name IN LISTS header_names)
    set(smoke_source "${CMAKE_CURRENT_BINARY_DIR}/${header_name}.cpp")
    file(WRITE "${smoke_source}" "#include <vinc_messages/${header_name}.hpp>\n")
    list(APPEND smoke_sources "${smoke_source}")
endforeach()
add_library(message_header_smoke OBJECT ${smoke_sources})
# No manual include paths or generation dependencies: test the public target.
target_link_libraries(message_header_smoke PRIVATE vinc_messages)

# An include-only template is used for this fixture to isolate include routing
# from the production template's existing deserialize/abstract-class problems.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nested.cpp" [=[
#include <vinc_messages/nested.hpp>
#include <type_traits>
static_assert(std::is_same_v<decltype(vinc::nested::position), vinc::float3>);
static_assert(std::is_same_v<decltype(vinc::nested::index), vinc::int3>);
]=])
add_library(nested_header_smoke OBJECT EXCLUDE_FROM_ALL "${CMAKE_CURRENT_BINARY_DIR}/nested.cpp")
target_link_libraries(nested_header_smoke PRIVATE vinc_messages)
