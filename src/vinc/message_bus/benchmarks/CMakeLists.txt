# Keep measurement code and generated schema local to this component.
set(message_root "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
set(message_header "${message_root}/vinc_messages/BenchmarkPayload.hpp")
add_custom_command(
    OUTPUT "${message_header}" "${message_root}/vinc_messages/all_messages.hpp"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${message_root}/vinc_messages"
    COMMAND $<TARGET_FILE:ribosome>
        --input "${CMAKE_CURRENT_SOURCE_DIR}/messages"
        --output "${message_root}/vinc_messages"
        --template "${CMAKE_CURRENT_SOURCE_DIR}/../../ribosome/data/message_sample.txt"
    DEPENDS ribosome "messages/BenchmarkPayload.msg" "../../ribosome/data/message_sample.txt"
    VERBATIM
)
add_custom_target(bus_benchmark_messages DEPENDS "${message_header}")
vinc_add_module_plugin(bus_benchmark_plugin module.cpp)
add_dependencies(bus_benchmark_plugin bus_benchmark_messages)
target_include_directories(bus_benchmark_plugin PRIVATE "${message_root}" "include")
add_executable(bus_benchmark driver.cpp)
target_include_directories(bus_benchmark PRIVATE "include")
target_link_libraries(bus_benchmark PRIVATE vinc::module_manager vinc::module_runtime vinc::errors)
add_dependencies(bus_benchmark bus_benchmark_plugin)
if(WIN32)
    target_link_libraries(bus_benchmark_plugin PRIVATE psapi)
    target_link_libraries(bus_benchmark PRIVATE psapi)
endif()
foreach(target IN ITEMS bus_benchmark bus_benchmark_plugin)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
