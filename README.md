# Open-VINC (VINC)

Open-VINC is a lightweight, high-performance C++23 framework designed for modular message-passing systems, robotics, and distributed application architectures. It emphasizes low-latency communication, explicit module dependencies, and automated message generation.

## Component Layout and SDK

Each component owns its `include`, `src`, tests and CMake targets. Production headers live under
`include`, with internal contracts in `detail`; implementation files live under `src`.
The [SDK guide](src/vinc/sdk/README.md) lists targets, component boundaries and source-consumer setup.

```cpp
#include <vinc/vinc.hpp>
```

```cmake
target_link_libraries(my_app PRIVATE vinc::sdk)
```

The aggregate header does not include generated application messages.
Components remain individually usable. `apps/vinc` assembles the central executable;
`examples/module` and `examples/message_bus` contain the process examples.

## Parameters

Modules use their inherited `context.param` for typed registration, watched local
caches and pump callbacks. The central Shell provides `param list/get/set/save/status`.
Values are shared through protected A/B snapshots and saved explicitly to TOML under
`vinc_data/<VINC_NAME>/params.toml` beside Vinc. See the [Param guide](src/vinc/param/README.md).
Existing framework configuration is not automatically migrated to this service.

## Core Architecture

The new [OSAL Process layer](src/vinc/osal/README.md) exposes one C++23 API for
Windows, Linux, and macOS backends. Windows has been built and tested; Linux and
macOS still require native validation. The new [Module runtime](src/vinc/module_system/README.md)
starts each registered instance in its own process, with centrally allocated names,
bidirectional heartbeat, bounded shutdown, and configurable restart limits.

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
backed by [message storage](src/vinc/components/message_storage/README.md), with custom-message
arrays, nested arrays, value copies, and a relocatable v2 image. Framework-private
binding supports fixed-capacity construction, sealing without a payload copy, and
copying received images into local messages. The [shared-memory Message Bus](src/vinc/message_bus/README.md)
connects these paths to Module Publishers and Subscribers: separate per-Publisher rings,
multiple Publishers per topic, local push, direct shared loans, next/latest reads, and
reader-lease cleanup after confirmed process exit. Windows is tested; Linux and macOS
still require native validation. See the Message Bus guide for the runnable `messageBusExample` and ordering limits.

The project is structured around several biologically inspired core components:

*   **Message Bus (`message_bus`)**: Shared-memory publication and copied local reception of generated messages across Module processes. Socket carries registration and topology only.
*   **Ribosome (`ribosome`)**: An automated message synthesizer and code generator. It reads simplified `.msg` definition files (located in `vinc_messages/messages/`) and compiles them into type-safe C++ header files for the C++23 build (including SHA256 hashing for structural integrity validation).
*   **Module Framework (`module`)**: The foundational building block for all system components. Developers extend `base_module` to create independent, modular logical units that are managed by the `module_manager`.
*   **Shell (`shell`, executable `Vinc`)**: An [interactive control interface](src/vinc/shell/README.md) with bounded asynchronous command dispatch, quoted arguments, history and completion. Standard Tcl startup scripts use `Vinc -S file.tcl`; `srun file.tcl` runs scripts from the Shell. Ctrl+D cancels the foreground script; Ctrl+C and `exit` wait for managed Module processes and Message Bus cleanup. The legacy script interpreter and `-sc` option have been removed.

## Build Requirements

*   **CMake** (Version 3.20 or higher; newer toolchains may require a newer CMake)
*   A compiler and standard library supporting the **C++23** features used by the project (GCC, Clang, MSVC, etc.)
*   Ninja; Windows uses MSVC and NMAKE from a Developer Command Prompt. Linux/macOS also need make and zlib development files for Tcl.

CMake 3.20 introduced support for the `CXX_STANDARD` value `23`. The build requires this standard level and disables compiler-specific language extensions; it does not silently fall back to an older standard. This setting alone does not verify support for every C++23 feature.

The minimum Linux deployment target is Ubuntu 22.04, with a separately installed, suitable toolchain permitted. Its default GCC 11 is not the project's C++23 baseline. Exact compiler and standard library minimum versions, and Windows/macOS minimum versions, remain to be validated. Deployment must also account for runtime library compatibility, including glibc and libstdc++ on Linux.

## Build Instructions

VINC relies on a strict CMake dependency chain to ensure that the Ribosome message generator is built first, which in turn generates the required message headers before compiling targets that consume generated messages.

```bash
git submodule update --init --recursive
cmake -S src -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Use a separate build directory with `-DCMAKE_BUILD_TYPE=Release` for Release.
The Tcl integration currently requires a single-configuration generator; see the
[Tcl integration guide](library/tcl/README.md) for platform and deployment details.

## Creating a New Module

For independently running modules, use `BaseModule` and `add_module` as
shown in the [Module guide](src/vinc/module_system/README.md). The framework supplies the
process entry point, Socket session, and heartbeat. `exampleModule` demonstrates this path.
The helper builds a Plugin library. The framework's `ModuleHost` supplies main/wmain,
loads that Plugin in its own process, and owns the control session and heartbeat.
Plugin and Host require the same toolchain, standard library, build configuration,
layout settings and Windows CRT; this is not a cross-toolchain binary SDK.
The central manager uses a separate `vinc::module_manager` target. For the aggregate
include, also link `vinc::sdk`.

## Message Generation and Failure Handling

Use `#include <vinc_messages/name.hpp>` for an individual message or
`#include <vinc_messages/all_messages.hpp>` for the complete set. CMake consumers
link `vinc_messages` to receive both the generated-header and `BaseMessage`
include paths, the C++23 requirement, and the generation dependency:

```cmake
target_link_libraries(my_target PRIVATE vinc_messages)
```

Modules created with `add_module()` receive this dependency automatically.
Other CMake targets explicitly link `vinc::messages`. External schema directories
can be registered with `add_message_dir(path)` after adding Open-VINC; see the
[SDK CMake functions](src/vinc/sdk/README.md#公開-cmake-functions).
The generated headers use the `vinc_message_storage` library
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
causes an explicit failure. Output now lives in each build directory under
`generated/include/vinc_messages`; concurrent builds must not share the same output directory.

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


### Vinc Log

Module 使用 context.log.info("value = {}", value)，既有 printf 同時由中央保存。
預設路徑為執行檔旁 vinc_data/<VINC_NAME>/logs/<日期>/，單檔 16 MiB、總量 128 MiB。
log status 查狀態，log show on 開啟目前 Shell 的結構化顯示。
見 [Log README](src/vinc/log/README.md)。

## Bag Recorder

可用 `bagRecorder record session01 --all` 啟動錄製，再用 `recorder status`／`recorder finish` 管理。
只錄指定 topic、容量設定、SQLite 檔案及未知型別限制見 [Bag README](src/vinc/bag/README.md)。
Player 尚未實作。
