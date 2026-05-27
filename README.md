# Open-VINC (VINC)

Open-VINC is a lightweight, high-performance C++20 framework designed for modular message-passing systems, robotics, and distributed application architectures. It emphasizes low-latency communication, explicit module dependencies, and automated message generation.

## Core Architecture

The project is structured around several biologically inspired core components:

*   **Axon Bus (`axon_bus`)**: The core communication backbone. It provides robust Publish/Subscribe (`publisher.hpp`, `subscriber.hpp`) mechanics, high-throughput work queues, and memory-efficient object pooling to handle message routing with minimal overhead.
*   **Ribosome (`ribosome`)**: An automated message synthesizer and code generator. It reads simplified `.msg` definition files (located in `src/messages/`) and compiles them into type-safe C++20 header files (including SHA256 hashing for structural integrity validation).
*   **Module Framework (`module`)**: The foundational building block for all system components. Developers extend `base_module` to create independent, modular logical units that are managed by the `module_manager`.
*   **Stem Shell (`stem_shell`)**: An interactive terminal guard and script interpreter that provides a command-line interface to inspect, control, and interact with running VINC modules.

## Build Requirements

*   **CMake** (Version 3.15 or higher)
*   **C++20** compatible compiler (GCC, Clang, MSVC, etc.)

## Build Instructions

VINC relies on a strict CMake dependency chain to ensure that the Ribosome message generator is built first, which in turn generates the required message headers before compiling the Axon Bus and other downstream modules.

```bash
mkdir -p build
cd build
cmake ../src
cmake --build .
```

## Creating a New Module

The build system exposes a convenient CMake macro `vinc_add_module` to register new modules. This automatically links your target to the core `module` framework, connects it to the `axon_bus`, and ensures all message headers are generated before compilation.

```cmake
# Example CMake configuration for a new module
add_executable(my_custom_module src/main.cpp)
vinc_add_module(my_custom_module)
```
