# Open-VINC Names and Meanings

This document records confirmed project terminology for use in discussion, design, and development. Prefer short, clear names. Confirming a term does not mean that its feature is implemented or that existing code has already been renamed.

This document owns names, meanings, and necessary distinctions. Component documents own complete API, build-integration, and lifecycle contracts. Follow [AGENTS.md](AGENTS.md) for workflow, document-update authorization, and conflict resolution.

## Confirmed Core Names

| Name | Meaning |
| --- | --- |
| `ModuleType` | A launchable module type describing which kind of Module to create. |
| `Module` | An instance created by starting a ModuleType. Use Module in discussion; there is no need to lengthen it to ModuleInstance. |
| `BaseModule` | The C++ base class inherited by users, providing lifecycle and command hooks. Retain this name and `base_module.hpp`. |
| `Context` | A Module's entry point to framework services, replacing the earlier PH/process header terminology. |

One ModuleType may create multiple Modules, such as a camera type started as `cameraFront` and `cameraRear`. The ModuleType continues to exist after creating a Module and can start additional Modules. Types and instances are distinct concepts.

Calling an instance Module after startup distinguishes it from its type. It does not require the instance to reach Running before being called Module, nor does stopping it turn it back into a ModuleType.

## Module System

**Module System** is the component responsible for Module execution and lifecycle. Its directory is `src/vinc/module_system/`, jointly maintaining `BaseModule`, the subprocess Runtime, and the central `ModuleManager`.

Its unified error domain is `ErrorDomain::ModuleSystem`. Module still means a launched module instance.

Headers use `<vinc/module/...>`. See the [Module System README](src/vinc/module_system/README.md) for Runtime, Manager, and subprocess-entry targets and error contracts.

## Identities

| Name | Meaning |
| --- | --- |
| `PID` | The process ID assigned by the operating system. |
| `MID` | Module ID, assigned to a Module by the central VINC framework. |
| `VID` | Reserved for future VINC-to-VINC identity. Its semantics, allocation, and lifecycle remain undecided and unimplemented. |

PID and MID are separate identity spaces and must not substitute for each other. MID follows the current framework ID lifecycle: every start or restart receives a new ID, with no reuse during one central-process lifetime. Uniqueness across central-process restarts or machines is not promised. Module names are managed separately; complete names registered at the same time must be unique.

MID remains uint64_t. `mid.hpp` defines its valid range as 1 through 2^63−2. Zero is invalid; 2^63−1 is reserved for the central shared-lock OwnerId and is not assigned to Modules. Exhaustion returns an error rather than reusing or wrapping IDs. The shared numeric boundary lives in common's `owner_id.hpp`; Concurrency does not depend on Module System.

Module identity uses `get_mid()`, `ModuleSnapshot.mid`, and `mid.hpp`.

## Message Bus

The cross-process message-exchange component is **Message Bus**, using the directory `message_bus` and namespace `vinc::message_bus`. See the [Message Bus README](src/vinc/message_bus/README.md) for entry points and integration.

Publisher, Subscriber, and Client retain their existing responsibility-based names. The ring buffer is `RingBuffer`, and its shared control header is `RingBufferHeader`. Both are internal to Message Bus. RingBuffer includes the Publisher read/write protection protocol; it does not belong to the general container layer. See the [Message Bus design](src/vinc/message_bus/DESIGN.md).

The central manager is `BusManager`. It manages topics, Publisher/Subscriber registration, shared-resource budgets, topology notifications, and exit reclamation. Message payloads do not pass through it. It is a framework integration interface in `vinc::message_bus::detail`.

<a id="topic-與-param-名稱分隔符"></a>

## Topic and Param Name Separators

These are confirmed naming decisions. Topic separator rules are validated by both Client and central code; implementation and test records are in the [Message Bus README](src/vinc/message_bus/README.md#topic-分隔符驗證2026-10-08). For Param, see the [Param README](src/vinc/param/README.md).

| Name kind | `.` | `/` | Examples |
| --- | --- | --- | --- |
| Topic | Forbidden | Allowed | `camera/frames`, `frames` |
| Param | Separates the owning Module from the parameter name | Forbidden | `camera.exposure` |

Using `category/name` for a Topic is a recommendation. A Topic need not contain `/`, and its prefix need not equal the publishing Module's name. Users may choose names for their purpose within these separator rules. Multiple Modules may still publish to one Topic. Topic segments do not create child Modules or child Contexts, and Context does not automatically prepend the Module name.

A Param's complete name is `module.param`. A Module may register its own parameter using a short name, and Context supplies ownership from that Module's identity. A complete name has exactly one dot; each segment permits only ASCII letters, digits, `_`, and `-`. The complete name, including Module and dot, is limited to 256 bytes. Module names likewise contain neither dots nor slashes, and `vinc` is reserved for the central process. String parameter contents and file paths still support UTF-8. The Param README owns type, permission, and cache contracts.

Use the existing `publisher_name()` to identify a received message's actual publisher. Retain this API name; do not rename it to `get_publisher()`. The framework fills publisher metadata, so users need not fill it themselves, and the source is not inferred from the Topic name. `publisher_instance()` is a Publisher ID, not a Module MID.

## Message Containers and Access

| Name | Meaning |
| --- | --- |
| `MessageArray` | The array container used in message fields. It can also be used independently within a process, but using it does not make its contents valid for cross-process transmission. |
| `RingBuffer` | The Message Bus queue maintained for a Publisher, retaining multiple messages. This is distinct from array fields inside a single message. |
| proxy | A lightweight interface borrowing the original data. Copying a proxy retains the borrowing relationship, does not create an independent content copy, and does not elevate read-only permissions. |

Proxy validity depends on the contracts for its element kind, node, and borrow lease. See the [Message Storage README](src/vinc/components/message_storage/README.md) for invalidation conditions and copying behavior.

## OutputHub

The central output-management component is **OutputHub**, using `output_hub` as its directory and namespace. The complete class name is `vinc::output_hub::OutputHub`.

Types such as `TextRenderer` and `Subscription` retain their responsibility-based names and remain owned by this component. See the [OutputHub README](src/vinc/output_hub/README.md) for entry points and output contracts.

## Shell

The central operator interface is **Shell**, located in `src/vinc/shell/`. Its internal framework class is `vinc::Shell`, with lifecycle managed by the central process. It is not a user-facing API for starting and stopping framework services.

Supporting types remain in `vinc::shell`. Names such as `SocketShell` and `CommandRouter` retain their specific responsibilities. The central program defaults to `Vinc.exe`/`Vinc`. Applications may configure its output name through `vinc_configure_app` without changing the central identity `VINC_NAME`. See the [Shell README](src/vinc/shell/README.md) for entry points and build integration.

## Context and pump Naming Status

`Context` is the service entry point approved by the central process and bound to BaseModule by the Host. Users access the inherited `context`. `pumpOnce()` processes one bounded batch of events; `pumpLoop()` repeatedly dispatches until the framework stops. `living()` queries whether the local runtime permits continued execution.

Choose `RunMode::FrameworkLoop` or `RunMode::Manual` explicitly when inheriting BaseModule. See the [Module System README](src/vinc/module_system/README.md#context-與執行模式) for complete API, threading, and stop contracts.

## Confirmed Lock Layers

Shared utilities live in `src/vinc/components/`. Concurrency tools live in `components/concurrency/`, using namespace `vinc::concurrency`. This category currently provides an interprocess mutex whose users manage its lifecycle.

| Name | Layer and meaning |
| --- | --- |
| `FileLock` | OSAL's dedicated exclusive file-lock abstraction, wrapping native resources, waiting, and release. |
| `FileLockGuard` | The held-lock object obtained after acquiring FileLock, responsible for releasing that acquisition. |
| `InterprocessMutex` | The shared synchronization layer's interprocess mutex, using atomic shared-memory ownership and a publication selector bit, with takeover after the central process confirms owner-process exit. |
| `InterprocessMutexGuard` | The held-lock object obtained after acquiring InterprocessMutex, responsible for releasing that acquisition. |

A lock object is used to acquire a lock. A Guard represents one successful acquisition and releases it automatically at the end of its lifetime (RAII), or earlier through an explicit unlock operation. These are paired names for the lock and its held-lock object; the lock class is not being renamed to Guard.

FileLock and InterprocessMutex now use different implementations; the latter no longer wraps the former. See [OSAL FileLock](src/vinc/osal/FILE_LOCK.md) and [Concurrency](src/vinc/components/concurrency/README.md) for Guard contracts.

## API Naming and Boundaries

API names must distinguish ModuleType from Module. For example, `register_module_type()` registers a type, while `get_active_module_names()` queries instances. Retain existing getter conventions such as `get_mid()` and `get_module_name()`. See the [Module System README](src/vinc/module_system/README.md) for complete API contracts.

Completed renames do not retain compatibility aliases for old names or paths. Naming work must not also change behavior or delete features.

Functions that actually create objects may still be called factories, such as `ModuleFactory`. Nodes in message storage also retain their original meaning. Shell displays use ModuleTypes/Modules and MID/PID.

`generation` identifies a generation within its owning component and must not be indiscriminately renamed MID. The Module System README owns the relationship between ModuleSnapshot and MID. The general Control Channel, OutputHub, and Message Bus slots each retain their own generation semantics. Publisher, Subscriber, request, Socket session, and OSAL resource IDs retain their distinct meanings. Global spelling conventions for classes, functions, variables, and files have not been separately established.

Only user-confirmed names are recorded here as decisions. Clearly mark proposals. Follow AGENTS for the update process.

## Vinc Identity

`VINC_NAME` is the local central-process name specified at build time. It identifies exclusive startup of the same name and ownership of shared-resource records. It is distinct from Module names, MID, and the reserved VID. See [Module System INSTANCE.md](src/vinc/module_system/INSTANCE.md) for scope and lifecycle.

## Message Publication Times

`timestamp`/`get_message_timestamp()` is Unix nanoseconds for tracing calendar time. `monotonic_timestamp`/`get_message_monotonic_timestamp()` is the same-host monotonic publication time used for Bus ordering. Both are uint64_t and filled by the framework. See [Message Bus](src/vinc/message_bus/README.md#雙時間發布-metadata2026-10-13) for sampling, formatting, and version contracts.

## RPC

RPC is the typed request/response service used by Modules through Context. RpcManager centrally manages service registration, fixed shared slots, and reclamation. RpcStatus is the application result, separate from framework Error. Service names and target Module names are passed separately. See the [RPC README](src/vinc/rpc/README.md) for complete contracts.
