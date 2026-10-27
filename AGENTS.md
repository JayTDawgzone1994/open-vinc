# Open-VINC Project Guidelines

Before starting work in this repository, read the required documents listed below. Document updates require authorization. Distinguish confirmed decisions, proposals, and the current implementation state.

## Required Reading and Document Responsibilities

| Document | Responsibility | When to read |
| --- | --- | --- |
| [AGENTS.md](AGENTS.md) | Workflow, authorization boundaries, required reading, precedence, and project-wide rules | Before starting any work |
| [WORKING_STYLE.md](WORKING_STYLE.md) | Design philosophy, aesthetics, trade-off preferences, and expectations for AI output | Before starting any work |
| [NAMING.md](NAMING.md) | Confirmed names, meanings, and the status of naming decisions | Before starting any work |
| README files for the components involved | Component APIs, responsibility boundaries, lifecycle, error and synchronization contracts, validation status, and outstanding issues | Before discussing, reviewing, or modifying a component; read all relevant contracts for cross-component work |

See the Component Documentation Index below for README entry points. If a README links to a DESIGN document, protocol specification, or topic-specific document relevant to the task, read that document as well. Reading AGENTS.md alone does not establish an understanding of component contracts.

Give each rule one primary maintenance location according to the responsibilities above. Other documents should reference it with a brief summary and a link, rather than maintain complete duplicate copies. When updating a decision, check its referring summaries to prevent divergence. Keep shared cross-component contracts here; component documents own their specific APIs and implementation details.

## Document Precedence

- The user's latest explicit decisions take precedence over existing project documents. Proposals, candidate approaches, and TODOs in documents are not approved decisions.
- Follow AGENTS.md for workflow and authorization, WORKING_STYLE.md for design trade-offs, NAMING.md for terminology, and each component's confirmed contracts for its behavior. Explicit contracts take precedence over general design preferences. Do not change existing behavior merely to satisfy a preference.
- If documents materially conflict and the user's latest decisions do not resolve the conflict, explain it and obtain confirmation before changing the affected area. Do not guess which document is newer or infer the intended contract from the current implementation.
- Document contents do not authorize implementation for the current task. The explicit authorization rule below still applies.

## Collaboration: Discuss First; Modify Implementation Only After Explicit Authorization

- **Discussion and read-only inspection are the default.** Before modifying code, build settings, tests, or other implementation files, confirm the current scope and receive an explicit `開工` or `動工` from the user. These are the owner's authorization keywords. A request to add or change something does not by itself authorize implementation.
- Questions such as "What do you think?", "How should we change it?", or "Is that possible?", agreement with a direction, quoting an authorization keyword, or saying not to start do not constitute authorization. Silence is not consent.
- Authorization remains valid throughout the confirmed task scope; do not ask again at every step. Ask first if the scope is unclear or must expand. Once a task is complete, do not extend its authorization to the next task or use it to delete or refactor unrelated features.
- Within an authorized scope, briefly explain the purpose of a local helper and implement it without seeking separate approval for each helper. Discuss and reach agreement first if it introduces an unconfirmed responsibility boundary, dependency, or lifecycle trade-off.
- When the user explicitly asks to write or update memory, specifications, or other documentation, complete that document work. This does not authorize implementation changes.
- **Architectural quality comes first.** Directly and respectfully point out clear errors or significant conflicts in either the user's or the assistant's assumptions, reasoning, or design. Explain the evidence, impact, and alternatives, and distinguish confirmed errors, concerns, and design trade-offs.
- Even with implementation authorization, pause the affected work when a new major architectural issue is discovered. Obtain explicit agreement before implementing that part. Necessary read-only investigation may continue.
- Communicate with the user in Traditional Chinese. Clearly report changes, validation, and limitations. Do not present proposals as decisions or describe a design-only or unverified capability as complete.

## Git Workflow: Topic Branches and the `push` Instruction

- Use a short-lived topic branch for each coherent feature, fix, or documentation change. Keep its implementation, tests, and documentation together; a topic may contain multiple commits. Do not maintain permanent platform development branches such as `mac_develop`. Windows, Linux, and macOS validate the same topic branch and commit.
- The default PR target is `develop`; `main` is the release integration branch. Follow an explicit user-specified target when provided. Never commit or push task changes directly to `main` or `develop`.
- **An explicit instruction to `push` authorizes the complete publication workflow below without further confirmation or a separate `開工`/`動工`.** It publishes the current task's authorized changes; it does not authorize unrelated implementation work, merging, or enabling auto-merge. Discussing or quoting the word `push`, including requests to document this rule, does not trigger publication.

When the user instructs you to push:

1. Inspect the working tree, current branch, remote, and existing PR. Identify the current task's changes and preserve unrelated work; do not blindly stage everything.
2. Fetch the target branch. Reuse an existing unmerged topic branch/PR for this task, or create a descriptively named branch from the current remote target, such as `fix/pipe-sigpipe-contract`. Preserve and carry the task's local changes onto it, resolve relevant conflicts, and inspect the resulting diff against the target. Do not discard changes or rewrite published history without authorization. Never reuse a merged topic branch for new work.
3. Run the checks appropriate to the changes and record the actual environment, results, and any missing platform coverage. Existing evidence can be reused when the tested content remains applicable. If unresolved failures prevent readiness, report them and open the PR as a draft with those failures clearly documented.
4. Stage only the task's files or hunks and create clear, logically scoped commits. If the changes are already committed, do not manufacture an empty commit. Push the topic branch and set its upstream as needed; do not force-push by default.
5. Create a GitHub Pull Request (the user's "merge request") targeting `develop`, or update the existing PR for this branch. Describe the problem, final behavior, validation, and remaining verification. Do not create duplicate PRs. Attach the PR to the current task when the app supports it, and return its URL, branch name, commit ID, and validation summary to the user for review. Opening the PR does not authorize posting separate messages or review requests to other people.
6. Leave the PR for the user to review and decide whether to merge. After a user-approved merge is confirmed, the topic branch can be deleted once it has no remaining work; subsequent tasks start from the updated target branch.

If credentials, permissions, or remote state block a step, preserve completed work and report the exact blocker and the remaining steps. Do not claim a push or PR creation succeeded without confirmation from the remote service.

## Project Goal

Open-VINC is a general-purpose software framework that can be reused across projects and operating systems, providing a common foundation for robots, drones, games, and other software. Its core goals are **cross-platform operation, speed, low overhead, maintainability, modern technology, and engineering rigor**.

## Architecture and Toolchain Principles

- Use **C++23**. CMake must require standard level 23, disallow fallback to an earlier standard, and disable language extensions. The declared minimum CMake version is **3.20**. If the selected toolchain requires a newer version, adjust it based on validation and update the documentation.
- The primary target platforms are Windows, macOS, and Linux. The minimum Linux target is **Ubuntu 22.04**; installing a newer toolchain is allowed. Do not use its default GCC 11 as the C++23 support baseline. Exact compiler and standard-library minimums, and minimum Windows/macOS versions, remain subject to validation.
- **Keep OSAL general-purpose and independently usable.** It wraps OS capabilities not provided by the standard library. It may depend on the standard-library-only `common` component, which must accompany it when moved or built independently. OSAL must not depend on Module, Message Bus, or Shell, and should not wrap every API merely for a uniform appearance. Concurrency implements the framework's interprocess mutex using OSAL SharedMemory/SharedWord. Recovery after owner death requires the upper layer to confirm process exit; see the Concurrency README.
- **Modules run in independent processes**, with OSAL managing their lifecycle. The central process and Modules use a shared Socket control channel with bidirectional heartbeat. Handle disconnection and exit according to Module policy; heartbeat does not prove that every application function is healthy.
- **Separate control and data.** Sockets carry commands, lifecycle operations, and control information such as Message Bus registration and topology. Message Bus payloads use shared memory. Each Publisher manages its own RingBuffer, and one topic may have multiple Publishers.
- Shared message layouts must support mapping across processes without process-local pointers. Existing layouts and protocols are implemented, but this does not promise a stable ABI across versions, compilers, or architectures. Synchronization, permissions, and failure recovery still require validation for their intended use.

## Unified Error Returns

- **Classify the failure before choosing how to report it.** Use Result for expected, recoverable runtime failures. Programmer errors that violate explicit preconditions are not recoverable error returns. Where detection is needed, use `vinc::contract_violation()` to terminate the current process; do not add framework exceptions. External input and resource exhaustion must still be validated and reported according to the API contract, rather than arbitrarily classified as contract violations.
- Framework C++ APIs with such recoverable failures use `template<class T> using Result = std::expected<T, Error>;`. Return `T` on success, `Result<void>` when there is no result data, and `std::unexpected(Error{...})` on failure. Infallible getters need not return Result.
- Use `vinc::Error`. Common owns `ErrorDomain` and shared contracts. Each component maintains C++ error/operation enums with `std::int32_t` as the underlying type and static text tables. Error identity is domain plus code. Assign explicit numbers independently of build order, and never reuse published numbers. Success is represented by Result's value state. New framework APIs must not invent separate error payloads or use bare `int`, `bool`, or special IDs as error substitutes. Adapt existing interfaces and external protocols within the authorized scope.
- For cross-component failures, first determine whether the current layer can recover. Otherwise, use `wrap_error()` to translate the failure into this layer's error code and operation while preserving lower-layer causes and native diagnostics. Do not expose a lower-layer classification as this layer's public contract. Cause chains have fixed capacity. When full, retain the current layer and deepest cause, use remaining capacity for the most recent causes, and mark omitted intermediate causes. Passing an existing Error through unchanged does not add a layer. Migrate existing components within their authorized scopes. Programmatic checks inspect the current layer using domain-aware, type-safe comparisons, not bare numbers.
- The sole text-query entry point is `std::string vinc::error_to_string(const Error& error)`. It requires neither `<T>` nor a separate integer-query API. Code should inspect error codes; text is for diagnostics. Preserve numeric values for unknown codes.
- Propagate expected failures through Result; do not use `throw`/`catch` as ordinary control flow. Check a result before accessing its value or `.error()`. Do not rely on `.value()` throwing to handle failure.
- Result does not eliminate exceptions from the standard library, allocators, third-party libraries, or user-defined types. Do not indiscriminately add `noexcept`. `error_to_string()` may allocate and is not `noexcept`. Exception/allocation-failure boundaries and disabling compiler exceptions require separate decisions. Preserve existing policy when not authorized to change it; do not broadly add or remove catch blocks.
- The interface lives in `src/vinc/common/`, and `vinc::common` depends only on the standard library. `Error` and its cause records store domain, code, operation, and native source/value, not process-local pointers or `std::error_code` category identities. Copying and wrapping cause chains must not allocate. Across processes, explicitly encode fields, record counts, and omission flags, and validate the protocol. Do not transmit C++ object layouts directly. Preserve unknown numeric codes, and do not treat a foreign native source as a local category.
- Components define error tables in C++. At build time, integrate them into one static catalog for each final program; do not use runtime registration. Keep error tables separate from component runtime code. Common must not depend back on components or the catalog integration layer. See the [common README](src/vinc/common/README.md) for CMake wiring, targets, and migration status.

## Operators and Message Container Contracts

- **Preserve understandable C++ operator semantics.** `operator=` must not return Result. If an operation can normally fail due to capacity or similar conditions, provide a named Result API such as `assign()` or `copy_from()`. Whether to retain an operator depends on the type's contract; do not apply one rule indiscriminately to every type.
- **MessageArray access:** `[]` is contract-based access. The caller guarantees a valid index. It must not allocate resources on first access or perform a deep copy. Nested elements may return lightweight proxies; do not introduce a wrapper cache merely to preserve `auto&`. `at()` provides checked access and an independent copy. `set()`/`assign()` return Result. Scalar element assignment remains available; whole-array copy/move assignment is disabled.
- **Make proxy semantics explicit.** Copying a proxy preserves its borrowing relationship and must not elevate read-only access. Document invalidation conditions. See the [Message Storage README](src/vinc/components/message_storage/README.md) for detailed APIs, lifecycle, and validation results.

## Temporary Location for System Settings

`src/vinc/common/include/vinc/config.hpp` (`vinc::config`) temporarily centralizes system settings, which will later be managed by Param. Add or move settings incrementally within each task's authorization. See the [common README](src/vinc/common/README.md) for the current state.

Settings that affect type layout remain compile-time constants and must not move to runtime Param. After a change, all connected C++ components and user programs must use the same value and be rebuilt. Each setting's defaults, limits, and purpose belong in its component README.

## Shared Utility Directory Classification

`src/vinc/components/` contains tools and infrastructure used by other framework components. Each utility retains its own build target, and its users manage its lifecycle. `concurrency` contains concurrency utilities, currently an interprocess mutex, and may depend on OSAL. Keep `common` limited to standard-library-only foundational types and contracts; do not place OSAL-dependent utilities there.

`message_storage` also belongs under components and provides message storage and containers. This classification does not make every API private. Each component's contracts define its formal API/detail boundary. See the [Components README](src/vinc/components/README.md).

## Component Dependencies and Interface Boundaries

These are confirmed design principles. Existing components are migrated within each task's authorized scope; this does not imply that migration is complete.

1. **Declare dependencies through targets.** Components declare dependencies through CMake targets. Each target supplies its own header paths and usage requirements. Do not add another component's `src` directly to include paths.
2. **Use interfaces according to their contracts.** General users depend on formal APIs. Framework integration may use only the other component's designated `detail` interfaces, rather than manipulate arbitrary internals. Header visibility does not make every declaration a supported dependency.
3. **Directories and targets need not map one-to-one.** A component may provide multiple targets with distinct responsibilities, such as the Module subprocess Runtime and central Manager, while keeping their files within the same component.

## Component Layout and SDK Entry Points

- Use a component-oriented layout. Each component owns its headers, implementation, tests, CMake, and documentation. Formal `.hpp` files live in `include/`, and `.cpp` files in `src/`. Internal headers live in `include/.../detail/`; do not introduce separate public/private/integration directories. Keep test-only helpers in tests and generated headers in the build directory.
- The convenience entry points are `<vinc/vinc.hpp>` and `vinc::sdk`; individual component APIs/targets remain usable. Do not add internal detail APIs or user-generated messages to the umbrella entry point. A component's headers may include the detail needed for their templates.
- See the [SDK README](src/vinc/sdk/README.md) for the concrete layout, targets, and designated integration contracts. Follow the Module System README for Context/pump modes, dispatch, and lifecycle. General Module services are accessed through the inherited Context.

## Code Readability

1. **Make code easy for maintainers to understand.** Clearly show operation order, branch conditions, and failure handling. Do not compress code merely to reduce line count.
2. **One statement per line.** Do not combine multiple declarations, assignments, function calls, or `return` statements on one line. Declare variables separately; avoid forms such as `int a, b;`. Initialization, condition, and increment may share a `for` header line.
3. **Expand control flow.** Use braces for `if`, `else`, and loops, with bodies on separate lines. An `if` error-handling branch must not share a line with the success path; separate the error branch from the following success path with a blank line. Avoid nested ternary expressions and excessively long conditions. Use named variables to express a condition's meaning when helpful.
4. **Name things by purpose.** Variable and function names should identify their role. Short names such as `i` are appropriate only for clear, narrowly scoped indices.
5. **Name configuration-related magic numbers.** Give counts, capacities, timeouts, and processing budgets names, and explain their purpose and units. Move system settings to `vinc/config.hpp` incrementally when authorized. Protocol fields and encoding constants remain owned by their components.
6. **Separate processing stages with blank lines.** For example: validate input, acquire resources, perform the operation, and update state. Split a function when it has multiple independent responsibilities; do not fragment it merely to shorten it.
7. **Use English comments to explain reasons and limits.** In particular, explain ownership, lock scope, lifecycle, and failure behavior. Avoid comments that merely translate the code into prose.
8. **Preserve behavior during readability work.** Do not also change conditions, execution order, lock scope, or public interfaces. Raise discovered logic problems separately.
9. **Name resource acquisition stages.** If a core resource-management function contains three or more independent acquisition steps, split them into named helpers. Each helper must have clear ownership and failure-cleanup contracts. If splitting would fragment lifecycle, lock scope, or rollback handling, explain the reason and discuss it first.
10. **Restrict `reinterpret_cast` to explicit boundaries.** Use it only at a clear ABI, memory-layout, or native OS API boundary. Add a nearby English comment explaining relevant size, alignment, object-lifetime, aliasing, or native-type assumptions and why the cast is valid. Avoid it in ordinary business logic.

## Engineering and Review Principles

Apply these within the current task's scope. Do not turn a small change into a comprehensive refactor.

- **Contracts and boundaries:** Define behavior, responsibilities, ownership, lifecycle, concurrency/blocking, and failure semantics before public interfaces. Cross-process protocols need versioning, data boundaries, ordering, delivery, timeout, and retry semantics. Record important trade-offs.
- **Validate abstractions through real use:** Separate the general core from application logic. Complete one minimal end-to-end flow, then test the abstraction with a different application. Control dependencies, binary size, CPU, and memory costs. Do not prebuild speculative features, introduce technology without evidence of value, or leave unexplained long-term limitations.
- **Capacity and waiting:** Bound queues, buffers, messages, and pending requests, and define full-capacity behavior. Waiting needs a timeout, cancellation, or an explicitly agreed indefinite-wait policy. For indefinite waits, explain blocking conditions, diagnostics, and failure recovery; do not silently replace the policy with timeout failure.
- **Confirmed Message Bus waiting policy:** A Writer may wait indefinitely for a protected Reader copy to finish, recording collision count and waiting time. Heartbeat timeout alone is insufficient to clear occupancy safely. Reclaim only after confirming that the Reader released protection or its owning process exited. See the [Message Bus design](src/vinc/message_bus/DESIGN.md).
- **Failure isolation and diagnostics:** Carefully review shared memory, ABI, synchronization, and recovery. Process liveness does not prove functional health. Define dependent behavior after disconnection, stale-data handling, restart limits, and invalidation of old resources/requests. Preserve Module identity, operation, exit reason, and OS diagnostics. Keep logging costs controlled and avoid blocking important flows or exposing credentials.
- **Build and validation:** Record OS, CPU, compiler, standard library, and dependency versions. Preserve generation/rebuild instructions and progressively establish CI on all three platforms. Process/communication tests must cover startup failure, abnormal exit, unresponsiveness, timeout, repeated start/stop, and cleanup. Bound failure tests and clean up child processes. Platform support requires native build and behavioral evidence; Linux deployment also needs glibc/libstdc++ compatibility validation. Explicitly identify checks not performed.
- **Performance and compatibility:** Preserve benchmark environments and workloads. Measure throughput, tail latency, idle costs, memory, startup/shutdown, and overload behavior as appropriate. Distinguish API, ABI, and protocol compatibility. Breaking changes need version identification, explicit rejection, or migration. Do not conflate performance, process isolation, security sandboxing, and hard real-time guarantees. Hard real-time support remains undecided.
- **Dependencies and documentation:** Evaluate licensing, maintenance status, platform support, and deployment cost when adding dependencies. Custom implementations carry the same validation responsibility. Keep stable rules here and detailed protocols, progress, and results in component documents. Read relevant documents and code before judging implementation status.
- **Record unresolved concerns:** Proactively record unresolved issues or concerns found during work in the owning README's TODO/outstanding-issues section, and explain them to the user. Briefly identify the relevant files/functions, evidence or reasoning, possible impact, current status, and recommended next step. Distinguish confirmed problems, unverified concerns, and unapproved proposals. Add and maintain these records during work; update resolved items with their status and validation results so stale TODOs do not remain. Recording an issue neither replaces discussion and necessary pauses for major issues nor authorizes implementation beyond the current scope.

## Component Documentation Index

- [Names and meanings](NAMING.md): ModuleType, Module, MID, Context/pump, lock terminology, and decision status.
- [OSAL](src/vinc/osal/README.md), [Components](src/vinc/components/README.md), [Concurrency](src/vinc/components/concurrency/README.md): OS capabilities, shared utilities, and the interprocess mutex.
- [Module System](src/vinc/module_system/README.md), [central identity and shared-lock integration](src/vinc/module_system/INSTANCE.md), [Control Channel](src/vinc/control_channel/README.md): lifecycle, heartbeat, stop/restart policies, and control protocols.
- [Message Bus](src/vinc/message_bus/README.md), [Message Bus design](src/vinc/message_bus/DESIGN.md): shared-memory exchange, ordering, occupancy, and reclamation.
- [Message Storage](src/vinc/components/message_storage/README.md), [Ribosome](src/vinc/ribosome/README.md): message layouts, containers, and generation.
- [Shell](src/vinc/shell/README.md), [Socket Shell](src/vinc/shell/SOCKET.md), [OutputHub](src/vinc/output_hub/README.md): scripts, remote operation, and output management.
- [SDK and component layout](src/vinc/sdk/README.md): integration entry points, targets, and framework wiring contracts.
- [Common](src/vinc/common/README.md): error interfaces and the current temporary configuration header.
- [RPC](src/vinc/rpc/README.md): Context typed request/response, fixed shared slots, pump batches, and failure recovery.
- [Param](src/vinc/param/README.md): typed parameters, Context caching/pump, A/B shared images, permissions, and TOML persistence.
- [Bag Recorder](src/vinc/bag/README.md): the recording Module, SQLite format, capacity, and loss statistics; Player is not yet implemented.
- [Web UI](src/vinc/ui/README.md), [UI Plugin](src/vinc/ui/PLUGIN.md): the official UI Module, browser workspace, and plugin trust/lifecycle; management panels are not yet wired.

## Outdated Documentation

- Proactively flag specific passages in `AGENTS.md`, `WORKING_STYLE.md`, `NAMING.md`, or relevant component documents that are outdated, contradictory, or inconsistent with confirmed decisions or implementation status. Provide evidence and a suggested correction. Follow this document's authorization rules for document updates. Do not replace confirmed contracts with assumptions inferred from implementation. Record unresolved README concerns according to the rule above.
