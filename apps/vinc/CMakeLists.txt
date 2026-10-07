# Assemble the central executable and its catalog after examples register.
add_executable(shell src/main.cpp)
set_target_properties(shell PROPERTIES OUTPUT_NAME Vinc)
vinc_deploy_tcl(shell)
target_include_directories(shell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
target_link_libraries(shell PRIVATE vinc::shell)
set_target_properties(shell PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${VINC_BUILD_ROOT}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${VINC_BUILD_ROOT}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${VINC_BUILD_ROOT}"
)
get_property(PROCESS_TARGETS GLOBAL PROPERTY VINC_PROCESS_TARGETS)
get_property(PROCESS_CATALOG GLOBAL PROPERTY VINC_PROCESS_CATALOG)
if(PROCESS_TARGETS)
    add_dependencies(shell ${PROCESS_TARGETS})
endif()

if(BUILD_TESTING AND VINC_BUILD_EXAMPLES)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/module-smoke.tcl"
        "exampleModule\nexampleNode hello\nexampleModule --name secondaryNode\nsecondaryNode status\nexit\n")
    add_test(NAME module.shell COMMAND shell --shell-port 0 -S "${CMAKE_CURRENT_BINARY_DIR}/module-smoke.tcl")
    set_tests_properties(module.shell PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "secondaryNode status")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/message_bus-smoke.tcl"
        "messageBusExample --name messageBusSubscriber sub demo\nmessageBusExample --name messageBusPublisher pub demo\nmessageBusPublisher push 10\nmessageBusSubscriber last\nmessageBusSubscriber last\nmessageBusSubscriber last\nmessageBusPublisher loan 20\nmessageBusSubscriber last\nmessageBusPublisher commit\nmessageBusSubscriber last\nmessageBusPublisher stats\nexit\n")
    add_test(NAME message_bus.shell COMMAND shell --shell-port 0 -S "${CMAKE_CURRENT_BINARY_DIR}/message_bus-smoke.tcl")
    set_tests_properties(message_bus.shell PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "value=20")
endif()

# Generate after collecting all process modules. Generator expressions resolve
# each Plugin library path for this build configuration.
set(catalog_template [=[
#pragma once

// Generated from the registered process-module targets. Edit the CMake sources instead.
#include <vinc/module/host/module_manager.hpp>


namespace vinc {
inline Result<void> register_built_modules(ModuleManager& manager) {
@PROCESS_CATALOG@    return {};
}
} // namespace vinc
]=])
string(CONFIGURE "${catalog_template}" catalog_contents @ONLY)
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/module_catalog.hpp"
    CONTENT "${catalog_contents}"
)

if(MSVC)
    target_compile_options(shell PRIVATE /W4 /utf-8)
else()
    target_compile_options(shell PRIVATE -Wall -Wextra -Wpedantic)
endif()
