# Open-VINC (VINC)

Open-VINC is a lightweight, high-performance C++23 framework designed for modular message-passing systems, robotics, and distributed application architectures. It emphasizes low-latency communication, explicit module dependencies, and automated message generation.

## Core Architecture

The new [OSAL Process layer](src/vinc/osal/README.md) exposes one C++23 API for
Windows, Linux, and macOS backends. Windows has been built and tested; Linux and
macOS still require native validation. The new [Module runtime](src/vinc/module/README.md)
starts each registered instance in its own process, with centrally allocated names,
bidirectional heartbeat, bounded shutdown, and configurable restart limits.
The legacy AxonBus test module is retained separately pending its existing Bus fixes.

[OSAL Pipe](src/vinc/osal/PIPE.md) provides general-purpose one-way byte streams
with typed read/write ends, bounded asynchronous I/O, and separate process
handoff helpers. It does not yet implement heartbeat,
Shell command forwarding, or the shared-memory message bus.

[OSAL Socket](src/vinc/osal/SOCKET.md) provides general-purpose IPv4/IPv6 TCP and
UDP, local streams, typed options, and readiness polling through one public API.
The shared [control channel](src/vinc/control_channel/README.md) adds framing, sessions,
and heartbeat above OSAL; the Module layer dispatches virtual command hooks. Windows is tested;
Linux and macOS backends still require native validation.

The [Ribosome generator](src/vinc/ribosome/README.md) now produces message classes
backed by [message storage](src/vinc/message_storage/README.md), with custom-message
arrays, nested arrays, value copies, and a relocatable v2 image. Framework-private
binding supports fixed-capacity construction, sealing without a payload copy, and
copying received images into local messages. The [shared-memory Bus](src/vinc/bus/README.md)
connects these paths to Module Publishers and Subscribers: separate per-Publisher rings,
multiple Publishers per topic, local push, direct shared loans, next/latest reads, and
reader-lease cleanup after confirmed process exit. Windows is tested; Linux and macOS
still require native validation. See the Bus guide for the runnable `busExample` and ordering limits.

The project is structured around several biologically inspired core components:

*   **Message Bus (`bus`)**: Shared-memory publication and copied local reception of generated messages across Module processes. Socket carries registration and topology only. The older **Axon Bus (`axon_bus`)** remains as legacy code and is not the new transport.
*   **Ribosome (`ribosome`)**: An automated message synthesizer and code generator. It reads simplified `.msg` definition files (located in `src/messages/`) and compiles them into type-safe C++ header files for the C++23 build (including SHA256 hashing for structural integrity validation).
*   **Module Framework (`module`)**: The foundational building block for all system components. Developers extend `base_module` to create independent, modular logical units that are managed by the `module_manager`.
*   **Stem Shell (`stem_shell`)**: An [interactive control interface](src/vinc/stem_shell/README.md) with bounded asynchronous command dispatch, quoted arguments, history and completion. Ctrl+C and `exit` wait for managed Module processes and Bus cleanup before restoring the terminal. The older script interpreter remains available separately.

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

For independently running modules, use `BaseModule` and `vinc_add_process_module` as
shown in the [Module guide](src/vinc/module/README.md). The framework supplies the
process entry point, Socket session, and heartbeat. `exampleModule` demonstrates this path.
The older in-process registration example below describes the retained legacy build helper.

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
through `axon_bus`. The generated headers use the `vinc_message_storage` library
supplied transitively by the `vinc_messages` interface target. Create local messages
with `vinc::make_message<MessageType>()`; see the [Ribosome guide](src/vinc/ribosome/README.md)
for field types, copy semantics, schema validation, and v2 format compatibility.

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
using only the `vinc_messages` target's usage requirements. Nested messages use the
production template and are instantiated with field access, copying, serialization,
and deserialization. CTest also includes schema/hash checks and generated-message
behavior tests, including OSAL shared-memory transfer between independent processes.
