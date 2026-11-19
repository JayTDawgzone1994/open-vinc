# RPC client redesign contract / RPC Client 改動契約

Date / 日期: 2026-11-06.

## Scope / 範圍

This document owns client ownership, waiting and callback contracts. The
[RPC README](README.md) owns current signatures, Python adapters and validation.
[NAMING.md](../../../NAMING.md), [Module System](../module_system/README.md) and
[Message Storage](../components/message_storage/README.md) own their shared contracts.
本頁說明 Client 的所有權、等待與 callback 邊界；操作與驗收由 RPC README 維護。

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

## API example / 介面範例

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
recognized. Host and Plugin share the authoritative execution state; duplicated
DLL-local markers are insufficient. The rejection is `ErrorCode::BlockingCallInPump`
in the RPC catalog.

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

## Validation / 驗證

Current APIs, Python signatures and acceptance records are maintained in the
[RPC README](README.md#integration-acceptance). Later native Mac source and clean SDK
coverage is recorded in the [Mac report](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md).
Historical failures remain scoped to their recorded revisions.
