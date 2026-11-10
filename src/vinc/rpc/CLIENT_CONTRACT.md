# RPC client redesign contract / RPC Client 改動契約

Date / 日期: 2026-11-06.

## Status and scope / 狀態與範圍

**Stages 1–4 implemented and accepted on Windows/WSL.** Source and clean installed-SDK
results, the initial Release UI failure and successful retry are recorded in the
[RPC README](README.md#integration-acceptance). Native macOS remains pending.

**第一至四階段已完成 Windows／WSL 驗收。** 來源、乾淨 SDK、Release UI 首輪失敗與
成功重跑見 README；原生 macOS 尚待驗證，不能以其他平台結果取代。

This document owns the change contract and acceptance plan. The [RPC README](README.md)
continues to describe the current implementation until the corresponding work is
completed and verified. [NAMING.md](../../../NAMING.md) owns terminology;
[Module System](../module_system/README.md) owns Context/pump lifecycle;
[Message Storage](../components/message_storage/README.md) owns message access and
ownership. Do not duplicate those contracts here or treat historical test results as
validation of this redesign.

## Goals and exclusions / 目標與排除範圍

- Provide a direct synchronous call and a reusable asynchronous `RpcClient` through
  the inherited Context. `RpcMessages` groups local Request/Response ownership.
- Preserve central RpcManager ownership, Host-owned control transport, fixed shared
  slots, schema validation, bounded resources, and existing fault isolation.
- Do not add threads, nested pumping, dynamic schema interpretation, automatic
  retries, global provider selection, or a browser RPC bridge.
- Do not change Bus/Param semantics, Python Runtime loading policy, or introduce
  general Context services into RuntimeServices.
- Remove the old per-call asynchronous callback entry point and separate-target
  public syntax. No compatibility alias or separate migration guide is required.
  Update maintained examples, tests and current documentation together.

同步呼叫自行推進必要通訊；非同步回覆由 pump 派送。框架拒絕可明確辨識的不允許
呼叫情境，不宣稱能偵測使用者所有 mutex、跨 Module 等待循環或無限迴圈。

## Target API / 目標 API

The following expresses the agreed API shape, not compilable evidence of implementation.
Request and Response must be Ribosome-generated messages. Ordinary message construction
retains the existing allocation/exception policy; no new blanket `noexcept` promise is made.

```cpp
context.register_rpc<Request, Response>(service, handler, options);

auto messages = vinc::make_rpc_messages<Request, Response>();
auto result = context.call_rpc("receiver.get_count", messages, options);

auto created = context.rpc_client<Request, Response>(
    "receiver.get_count",
    [this](auto reply) {
        on_count_reply(std::move(reply));
    },
    options);
```

| Entry / 入口 | Contract / 契約 |
| --- | --- |
| `register_rpc<Request, Response>(service, handler, options)` | Returns `Result<void>`; optional trailing `ServiceOptions`. The Context supplies the registering Module identity. Handler remains `RpcStatus(const Request&, Response&)`. |
| `make_rpc_messages<Request, Response>()` | Creates a local `RpcMessages<Request, Response>` with `request()` and `response()` access. It is not a combined wire image or shared-memory loan. |
| `context.call_rpc(address, messages, options)` | Synchronous `Result<RpcStatus>`; optional trailing `CallOptions`. Types are inferred from `RpcMessages`. |
| `context.rpc_client<Request, Response>(address, callback, options)` | Returns a Result containing a movable, noncopyable `RpcClient`; callback is `void(Result<RpcReply<Response>>)`. Optional trailing `CallOptions` sets client defaults. |
| `client.make_request()` | Creates an owning Request using the existing message construction model; normal use is `auto request = client.make_request();`. |
| `client.call(*request)` | Returns `Result<void>` for local admission, using the client's default options. |
| `client.call(*request, options)` | Overrides options for this call only. |

CallOptions controls caller waiting policy, including timeout. ServiceOptions owns
request/response byte capacities and slot count; a caller cannot resize the service pool.
Existing `rpc_stats()` remains available, with synchronous/async accounting documented
and tested as the implementation is integrated.

`options` 皆位於最後且可省略。Client 設定為後續呼叫的預設值，單次覆寫不修改預設。
不要求填寫虛構的 `reset` 等欄位；使用者只填自己 Request 真正定義的內容。

## Address and identity / 地址與身分

- Public addresses use exactly one dot: `module.service`, for example
  `receiver.get_count`. Each segment follows its existing Module/service name limits
  and ASCII letters/digits/underscore/hyphen rules. Reject empty or malformed segments;
  do not interpret a slash as an alternate separator.
- Registration accepts the local service name only. Context supplies its authenticated
  Module identity; applications cannot register services for another Module.
- A client may be created before the target exists. Each call resolves the target
  and verifies both schema hashes; construction is not a remote registration/probe.
- An admitted request is bound to its actual service and MID. Never retarget an old
  request to a restarted Module. A later new call may resolve the same name again.
- The control protocol may continue encoding target and service separately. A public
  string-format change alone does not require changing the wire representation.

## Synchronous call and messages / 同步呼叫與訊息

- The caller does not need `pumpOnce()`, `pumpLoop()`, or another pumping thread.
  A Manual main loop with no caller pump is a required supported/tested scenario.
  The target still needs its own pump to execute its service handler.
- The calling thread advances only its own request and response transfer. It never
  invokes application callbacks or performs nested pumping.
- Framework failures return `Error`. A valid reply returns `RpcStatus`: code zero is
  application success; a nonzero application status may still carry a valid Response.
- Validate and prepare the full reply before replacing `messages.response()`.
  Framework failure preserves the previous Response. Callers must check the current
  Result before treating the Response as this call's result.
- The same `RpcMessages` cannot be concurrently mutated or used for two calls.
  Different objects may be used independently. Borrowed fields/proxies follow Storage
  invalidation rules when the owning message content is replaced.
- Direct synchronous calls to the caller's own Module are rejected before submission;
  asynchronous self-calls remain supported.

`RpcMessages` 是本機便利容器，不改 Request／Response schema，不把兩筆訊息合成新的
共享格式。非同步回覆不保留此容器的引用，亦不稍後偷偷改寫它。

## Reusable asynchronous client / 可重複非同步 Client

- A client binds its address, two types, callback and default options once.
- One outstanding call per client, until its result begins delivery. Another call
  returns `Busy` without submitting another request. Clear busy state before invoking
  the completion so it may submit the next request.
- Different clients may run concurrently, subject to shared Context/central limits.
  An immediate admission failure produces no later completion for that attempted call.
- Successful admission snapshots the Request; afterward the original may be modified
  or released. Remote failures arrive through the completion on pump.
- Each delivered Response owns independent local storage and may be retained by move.
- Destroying/closing a client prevents future callback dispatch, but does not imply
  remote cancellation or safe immediate slot reuse. Framework cleanup outlives the
  application callback registration when necessary.
- An already-running callback is not interrupted or joined by close. Applications
  synchronize handle operations, move/destruction and captured object lifetimes.
  Context shutdown revokes new calls; it must not leave a dangling usable Context pointer.
- Timeout delivery may free the client for another call while old cleanup bookkeeping
  remains charged against existing limits. Client availability does not promise capacity.

## Pump execution guard / pump 執行環境防護

| Situation / 情境 | Behavior / 行為 |
| --- | --- |
| Async `RpcClient::call()` in a callback | Allowed; completion remains pump-dispatched. |
| Synchronous `context.call_rpc()` in the same Context's pump callback call chain | Immediately return a specific RPC Result error before submission/remote slot acquisition. Do not terminate the Module. |
| Another application thread makes a synchronous call while pump runs | Allowed; no general deadlock-freedom guarantee. |
| Manual main makes a synchronous call without any caller pump | Supported and independently verified. |
| Other Context APIs | Keep their individual contracts; no blanket prohibition. |

Track the current execution context with scoped state that restores the previous state
on normal or exceptional exit. Indirect calls through application helpers must still be
recognized. Host and Plugin must use the same authoritative execution state; duplicated
DLL-local thread-local markers cannot establish correctness. Test through a real Plugin,
not only a monolithic unit-test executable. Do not publish an arbitrary numeric error code
in this plan; allocate the new typed error/operation in the existing RPC catalog.

此防護依據目前呼叫執行緒，而非「Context 有 pump 在跑就禁止其他執行緒同步呼叫」。
它是明確的 API 使用限制，不是一般死鎖偵測器。

## Progress, deadlines and reclamation / 推進、期限與回收

- Host retains the sole control transport and routes request-specific events to the
  correct waiter. Synchronous waiters and pump must not consume the same event twice
  or steal each other's events. Register routing before a reply can arrive.
- The synchronous calling thread performs its shared-memory transfer and reply
  validation. Heartbeat/watchdog must not execute user code or large payload processing.
- Keep fixed pump batches for asynchronous handlers/completions. Waiting must release
  locks needed by control reception, other calls, supervision and shutdown.
- Retain existing config-backed resource limits and the five-second default call timeout.
  Use one deadline for a call rather than restarting its budget at each protocol phase.
  This is not a hard real-time return bound under OS scheduling or allocation delays.
- Local stop/channel failure/target exit must wake synchronous waiting with the
  appropriate error. Central loss still follows the process termination contract;
  do not promise a returned Result before forced termination.
- A timeout does not prove nonexecution. Do not retry automatically or reclaim a live
  peer's lease because the caller stopped waiting. Late replies reclaim safely and
  never cause a second result delivery.
- Transfer abandoned synchronous-call cleanup to framework-owned bounded bookkeeping
  without a handoff race. Later caller pump calls must not be required for cleanup.
  Confirm ownership release or process exit before reusing protected storage.
- Preserve per-service submission ordering, schema/layout validation, cause chains,
  callback exception policy and failure isolation. No public cancellation API is added.

## Component changes / 元件改動邊界

| Component | Work |
| --- | --- |
| RPC | Public client/messages helpers, address parsing, synchronous progress, bounded wait routing, errors, cleanup and async lifecycle. |
| Module System | Context entry points, pump execution state, stop wakeups and active-call lifetime integration. |
| Host/Plugin integration | Designated Access interface changes and matching ABI identity updates; rebuild all connected artifacts. |
| Python binding | Equivalent client/messages/synchronous behavior using existing Python error conventions; release GIL during native waits, acquire it for Python object work. No Python execution on watchdog. |
| SDK/examples/docs | Public header export/smoke tests, C++/Python examples, installed consumers, current documentation and validation records. |

Reuse existing component targets and ownership boundaries. Update protocol versions only
where wire changes actually require it; reject incompatible layouts rather than reading
them as old formats. Python-facing spelling/adapters must be documented during stage 3;
the Python signatures and GIL/lifetime behavior are specified in the RPC README.

## Implementation stages / 開工步驟

Stages 1–4 are complete for the Windows/WSL scope; native macOS remains a follow-up.
Intermediate stages need not be independently shippable; final correctness and the
acceptance evidence below are required.

1. **Contract and API foundations:** implement address parsing, RpcMessages and RpcClient
   foundations; add type, ownership and malformed-address tests. Keep documentation clear
   about any paths not yet connected.
2. **Synchronous transport and cleanup:** integrate Host routing/waiting, pump guard,
   stop/deadline handling, late-event recovery and native Plugin ABI wiring. Prove progress
   and cleanup without caller pump before claiming the synchronous API works.
3. **Async and language integration:** complete reusable-client callback lifetimes,
   Python bindings and teardown/GIL behavior; replace old call sites and examples, then
   update current API documentation. Preserve bounded async pump dispatch.
4. **Integration acceptance:** Windows Debug/Release, WSL Linux, clean installed SDK
   with custom messages and cross-language RPC. Record native macOS follow-up in root
   TODO and relevant component records; no unexecuted platform is marked passed.

New major dependency/lifecycle trade-offs discovered during implementation require a pause
and owner agreement, even inside an authorized stage. Record unresolved concerns with
evidence and next steps in the owning component documentation.

## Acceptance checklist / 驗收條件

Checked items refer to the Windows/WSL evidence and limitations in the
[coverage map](README.md#integration-acceptance), not universal platform or race-freedom guarantees.
勾選僅代表所列 Windows／WSL 案例通過；不宣稱跨平台已全驗或任意並行程式皆安全。

- [x] Normal replies and nonzero application status with valid typed Response.
- [x] Invalid addresses, unavailable service, schema mismatch, large payloads and capacities.
- [x] Client Busy, local/remote QueueFull and repaired/repeated calls without automatic retry.
- [x] No-pump synchronous success, timeout, late Grant/Reply cleanup and repeat capacity reuse.
- [x] Concurrent synchronous callers plus async pump without event theft or duplicate delivery.
- [x] Real Plugin pump guard, indirect helper calls, scope restoration, async self-call and
      synchronous self-call rejection before submission.
- [x] Callback-driven resubmission, handle move/close/destruction and shutdown races.
- [x] Target crash/restart/MID isolation, channel failure and local stop wakeups.
- [x] Duplicate/late events, lease recovery and retained Response on framework failure.
- [x] Only pump executes user callbacks; heartbeat remains independent of waiting/transfers.
- [x] Python native waits release GIL; callback exceptions, lifetime and C++/Python exchange.
- [x] SDK public headers, fresh installed consumer, generated custom messages and normal exit.
- [x] Record actual platform/toolchain/configuration, commands, results and skipped coverage.

- [ ] Native macOS source and clean installed-SDK acceptance with matching rebuilt
      Host/Plugins/Python; tracked in root TODO.

Stage-specific evidence belongs to the [RPC README](README.md#integration-acceptance).
The controlled restart-identity test is paired with real target-death tests, not claimed
as an additional automatic-restart stress run. Existing handle concurrency and application
thread-join responsibilities remain unchanged. Historical pre-redesign validation does
not automatically satisfy this checklist.
