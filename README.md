# Open-VINC (VINC)

Open-VINC is a lightweight, high-performance C++23 framework designed for modular message-passing systems, robotics, and distributed application architectures. It emphasizes low-latency communication, explicit module dependencies, and automated message generation.

## Core Architecture

The new [OSAL Process layer](src/vinc/osal/README.md) exposes one C++23 API for
Windows, Linux, and macOS backends. Windows has been built and tested; Linux and
macOS still require native validation. Existing modules are not yet migrated to
separate processes.

[OSAL Pipe](src/vinc/osal/PIPE.md) provides general-purpose one-way byte streams
with typed read/write ends, bounded asynchronous I/O, and separate process
handoff helpers. It does not yet implement heartbeat,
Shell command forwarding, or the shared-memory message bus.

[OSAL Socket](src/vinc/osal/SOCKET.md) provides general-purpose IPv4/IPv6 TCP and
UDP, local streams, typed options, and readiness polling through one public API.
It has no Module-specific connection or command protocol. Windows is tested;
Linux and macOS backends still require native validation.

The project is structured around several biologically inspired core components:

*   **Axon Bus (`axon_bus`)**: The core communication backbone. It provides robust Publish/Subscribe (`publisher.hpp`, `subscriber.hpp`) mechanics, high-throughput work queues, and memory-efficient object pooling to handle message routing with minimal overhead.
*   **Ribosome (`ribosome`)**: An automated message synthesizer and code generator. It reads simplified `.msg` definition files (located in `src/messages/`) and compiles them into type-safe C++ header files for the C++23 build (including SHA256 hashing for structural integrity validation).
*   **Module Framework (`module`)**: The foundational building block for all system components. Developers extend `base_module` to create independent, modular logical units that are managed by the `module_manager`.
*   **Stem Shell (`stem_shell`)**: An interactive terminal guard and script interpreter that provides a command-line interface to inspect, control, and interact with running VINC modules.

## Build Requirements

*   **CMake** (Version 3.20 or higher; newer toolchains may require a newer CMake)
*   A compiler and standard library supporting the **C++23** features used by the project (GCC, Clang, MSVC, etc.)

CMake 3.20 introduced support for the `CXX_STANDARD` value `23`. The build requires this standard level and disables compiler-specific language extensions; it does not silently fall back to an older standard. This setting alone does not verify support for every C++23 feature.

The minimum Linux deployment target is Ubuntu 22.04, with a separately installed, suitable toolchain permitted. Its default GCC 11 is not the project's C++23 baseline. Exact compiler and standard library minimum versions, and Windows/macOS minimum versions, remain to be validated. Deployment must also account for runtime library compatibility, including glibc and libstdc++ on Linux.

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

## Message Generation and Failure Handling

Use `#include <vinc_messages/name.hpp>` for an individual message or
`#include <vinc_messages/all_messages.hpp>` for the complete set. CMake consumers
link `vinc_messages` to receive both the generated-header and `BaseMessage`
include paths, the C++23 requirement, and the generation dependency:

```cmake
target_link_libraries(my_target PRIVATE vinc_messages)
```

Modules registered with `vinc_add_module` already receive these requirements
through `axon_bus`. The `vinc_messages` target is header-only.

`generate_messages` runs on every build that depends on it, even when no `.msg`
file has changed. CMake supplies the template path from this checkout:
`src/vinc/ribosome/data/message_sample.txt`.

Each generation attempt removes the previous generated-header directory before
reading the template or message inputs. A missing or empty template, invalid
message, or I/O failure returns a nonzero status and stops dependent build steps;
old headers are not used as a fallback. If the OS prevents removal, generation
also fails and reports the filesystem error.

New headers are written to the sibling `vinc_messages.ribosome-stage` directory
and moved into the output directory only after every file has been written and
closed successfully. Ordinary failures clean up staging files. A forced process
termination can leave the staging directory behind; after confirming no generator
is running, remove that directory before retrying. An existing staging directory
causes an explicit failure. Concurrent builds sharing the source output directory
are not supported.

Run the generation regression checks from the repository root in a configured
compiler environment. Use an absolute, previously nonexistent directory:

```sh
cmake -DTEST_ROOT=/absolute/path/to/new-test-directory -DTEST_GENERATOR=Ninja -P tests/ribosome_generation.cmake
```

On Windows, use an x64 MSVC developer environment, a path such as
`C:/work/open-vinc/build/ribosome-test-1`, and add `-DTEST_CXX_COMPILER=cl`.
The checks use a separate source copy and verify regeneration, missing/empty
templates, missing inputs, invalid messages, recovery, and a full build. They
also compile every production message header and the umbrella header separately,
using only the `vinc_messages` target's usage requirements. Nested include routing
is compiled with a minimal test template; the production template's missing
`deserialize(buffer, offset)` implementation remains a separate known issue.
