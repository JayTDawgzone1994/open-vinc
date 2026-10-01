# Build graph for the framework. Message generation is a build dependency,
# while process-module registration collects launch metadata for the central Shell.

cmake_minimum_required(VERSION 3.20)
project(vinc LANGUAGES CXX)

# =================================================================
# Require C++23 consistently across framework targets.
# =================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Source files contain UTF-8 text; do not depend on the Windows code page.
if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
endif()

# Initialize the retained legacy in-process module registry.
set_property(GLOBAL PROPERTY VINC_MODULES_LIST "")

# =================================================================
# Define the message generation target before its consumers.
# =================================================================
# The ribosome target is defined later; its build dependency is added below.
add_custom_target(generate_messages
    COMMAND $<TARGET_FILE:ribosome>
    COMMENT "🧬 Ribosome is synthesizing messages from inputs..."
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    VERBATIM
)

# =================================================================
# Legacy module registration; new process modules use vinc_add_process_module.
# =================================================================
# Preserve the old helper and its AxonBus linkage for existing legacy targets.
function(vinc_add_module TARGET_NAME)
    # Append the target to the legacy registry.
    get_property(current_list GLOBAL PROPERTY VINC_MODULES_LIST)
    list(APPEND current_list ${TARGET_NAME})
    set_property(GLOBAL PROPERTY VINC_MODULES_LIST "${current_list}")
    
    # Supply module framework symbols.
    target_link_libraries(${TARGET_NAME} PRIVATE module)
    
    # Preserve the legacy AxonBus dependency.
    # Linking the target also establishes build ordering.
    target_link_libraries(${TARGET_NAME} PRIVATE axon_bus)
    
    # axon_bus publicly supplies vinc_messages, including generation ordering.
    
    message(STATUS "[Holon] Registered module: ${TARGET_NAME}")
endfunction()

# =================================================================
# Declare runtime libraries before application targets.
# =================================================================
# Explicit target dependencies below govern message generation.
add_subdirectory(ribosome)       # Build-time message compiler
add_subdirectory(osal)           # Independent OS process primitives
add_subdirectory(message_storage) # Relocatable arrays; independent of Bus and Ribosome
add_subdirectory(control_channel) # Shared framed Socket sessions
add_subdirectory(bus)             # Shared-memory publish/subscribe
add_subdirectory(vinc_messages)  # Generated-header usage requirements
add_subdirectory(axon_bus)       # Retained legacy in-process Bus
add_subdirectory(module)         # Process-module runtime and supervisor

# Examples register their module targets before Shell catalog generation.
add_subdirectory(vinc_test)
add_subdirectory(module_example)
add_subdirectory(bus_example)
add_subdirectory(stem_shell)

# =================================================================
# Finish the explicit generation dependency.
# =================================================================
# All referenced targets now exist.

# Rebuild the generator before invoking it to regenerate message headers.
add_dependencies(generate_messages ribosome)

# axon_bus and its consumers inherit generation ordering through vinc_messages.
