# Parallel pump / 多執行緒 pump 契約

Different Topics may run concurrently; the same Topic stays serial within a Context.
預設不開 worker；需要並行時明確指定數量。本文是現行契約，不是待實作計畫。
API 與生命週期入口見 [Module System](README.md)。

## Purpose and exclusions / 目的與非目標

- Allow independent Topics to progress concurrently without requiring application
  authors to build a work queue merely to use callbacks.
- Preserve the default serial execution model and the Manual APIs.
- Do not promise lossless reception, real-time execution, unlimited throughput,
  fixed worker affinity, or automatic synchronization of application state.
- Do not parallelize one Topic, add general callback groups, resize pools at runtime,
  or make Recorder/System Modules opt in automatically in this change.
- Do not redesign shared-memory layouts, central scheduling, RPC transport or OSAL.

不同 Topic 可並行，單一 Topic 不因此加速。保留直接在 callback 做事的便利性，
不強迫使用者轉移到自己的 thread；也不宣稱這一定比單執行緒快。
第一版不新增 callback group／動態調整 worker，也不擅自替 Recorder 開啟多執行緒。

## Configuration and admission / 設定與進入限制

Configuration: `PumpOptions{.topic_workers = N}`.

| Setting / 設定 | Behavior / 行為 |
| --- | --- |
| Omitted / `0` | Existing serial dispatch on the pump caller; no additional workers. 預設維持單執行緒。 |
| `N > 0` | Exactly N additional Topic workers; the pump caller coordinates and executes control callbacks. 新增 N 條 Topic worker，呼叫端維護控制工作。 |
| `N = 1` | Topic callbacks remain mutually serial, but may overlap control callbacks. 不等同預設單執行緒模式。 |
| Invalid/excessive count | Explicit Result failure before starting dispatch; no silent clamp or fallback. 非法設定明確失敗。 |

The count excludes existing heartbeat/control-service threads. FrameworkLoop
accepts configuration at Module construction; Manual supplies it when entering
`pumpLoop(options)`. Both use the same scheduler. FrameworkLoop's constructor is
`BaseModule(std::string name = {}, PumpOptions options = {})`.
`config::module_topic_worker_limit` is 32 additional workers: an admission ceiling,
not a recommendation or performance guarantee. Each turn delivers at most one
message and retains the unfinished batch. Pool size stays fixed for the active loop.

這是「新增 Topic worker 數量」，不是 Module 的總 thread 數。
FrameworkLoop 在建構時設定；Manual 於進入 loop 時提供，兩者共用實作。
上限與排程預算須具名並說明用途；不按 CPU 核心數偷偷建立大量 thread。

Only one public pump/wait owner remains active per Context. Concurrent or recursive
`pumpOnce()`, `pumpLoop()` and `waitForWork()` return Busy, including calls made
inside worker callbacks. Internal workers are not additional public pump callers.
`pumpOnce()` keeps its synchronous finite batch and creates no pool;
`waitForWork()` waits without dispatching. Serial and parallel loops use the shutdown ordering below.

同一 Context 仍只有一個公開 pump／wait 執行者。不能由使用者開多條 thread
同時呼叫 pump；worker 內遞迴呼叫也回 Busy。`pumpOnce()` 不啟動背景工作，
`waitForWork()` 不派送 callback。

## Topic scheduling and ownership / Topic 排程與所有權

1. Serialization is keyed by the exact Topic name within one Context. Two
   registrations for the same Topic share admission, while retaining independent
   cursors and callback lifetimes. Separate Modules are not mutually serialized.
2. At most one worker owns a Topic's copy/validation/callback work at a time.
   Callback completion precedes the next callback for that Topic. A later turn
   may use another worker; thread-local state is not a Topic storage mechanism.
3. Each subscription retains existing source ceilings, cursor/loss accounting and
   multi-Publisher candidate ordering. Multiple subscriptions do not gain a common
   cursor or a new total callback order. Different Topics have no relative order.
4. Parallel loop batches advance independently by Topic. Capture finite boundaries
   before processing each batch; publications/new subscriptions beyond that boundary
   belong to a later batch. Do not impose a barrier waiting for all Topics to finish
   before a ready Topic can progress. `pumpOnce()` keeps its existing global batch.
5. Use bounded, deduplicated ready-work metadata and finite scheduling turns.
   A hot Topic must yield admission to other ready Topics. Do not enqueue every
   message or pre-copy a complete batch. Preserve a partially processed batch's
   boundary/cursor when yielding; do not restart it and duplicate delivery.
6. Workers perform the native copy and validation as well as the callback. Reader
   protection covers only the existing bounded copy, never user code or queue waits.
7. Each delivered message owns independent writable local storage. Moving its
   `unique_ptr` transfers ownership without another payload copy. Framework never
   overwrites storage still owned by the application. Application handoff queues
   require their own synchronization, bounds and full-queue policy.

同 Topic 序列化範圍包含本機複製、驗證與 callback，但沒有 thread affinity 保證。
不同 Topic 可各自推進下一批，避免所有工作重新被最慢 Topic 拖住。
不保證同 Topic 多份訂閱之間的全域 callback 順序，也不改原本多發布者排序契約。
排程只排工作描述、不堆訊息正文；仍未讀取的資料可能被 RingBuffer 覆蓋，沿用遺失統計。

A close prevents future admission, not an already-running callback's execution.
Keep subscription/callback storage alive through in-flight work; release captured
user objects outside scheduler/registry locks. Self-close must not join its own
worker. A newly registered same-name Topic must not bypass an older still-active
Topic owner. Preserve application synchronization requirements for handle
move/close/destruction and borrowed message fields/proxies.

close 不是 callback join；自行退訂不可等待自己。重建同名 Topic 時也不能讓舊工作
與新工作重疊。callback 的擷取物件要存活到執行結束，且不能持排程鎖銷毀使用者物件。

## Param, Shell and RPC coordination / 控制工作的協調

| Work / 工作 | Execution / 執行方式 |
| --- | --- |
| Topic callbacks | Topic workers when enabled; per-Topic serialization |
| Param cache refresh and callback | Pump caller, serialized with other control callbacks |
| Module Shell command and completion hooks | Same pump caller |
| RPC service handlers and asynchronous completions | Same pump caller; existing service order and finite batches |
| Heartbeat, transport and stop supervision | Existing independent paths; never execute user callbacks |

**Control callbacks may overlap Topic callbacks.** They remain mutually serial;
no global pause/drain of Topics is required for each Param/Shell/RPC operation.
A slow control callback can delay other control callbacks. This is not a promise
of immediate commands or parameter delivery. It can also delay discovery of new
Topic batches; workers can continue existing finite batches, but there is no
separate always-responsive coordinator thread.

**已同意：控制 callback 可與 Topic callback 並行，彼此仍序列化。**
Param 更新不必先等待所有影像工作完成；但慢 RPC handler 仍可能延遲 Shell／Param。
控制 callback 也可能延遲協調端探索新批次；worker 可繼續既有批次，不承諾無限推進新資料。

Only the control dispatcher mutates Param's pump cache. Its `const ParamMap&`
callback borrow remains valid during that invocation, not as a cross-thread retained
snapshot. Other threads use copying Param APIs or application-owned copies.
The framework protects its own metadata, not arbitrary Module fields.
Applications must synchronize state shared across Topics or with control callbacks.
Existing same-Publisher/Subscriber/RpcClient restrictions remain; enabling workers
does not make every SDK handle freely concurrently usable.

Param map 的借用引用不能移交給別條 thread 長期使用；要複製需要的內容。
例如 Param callback 寫 `this->exposure`、影像 callback 讀它，使用者須自行同步。
建議短暫持鎖取得設定副本，再於鎖外執行耗時處理；不要用大鎖包住整個 callback。

## RPC guard and notification ownership / RPC 防護與通知所有權

- Track callback execution on every framework dispatcher/worker. Host, Plugin and Python must observe the same
  authoritative state; unrelated DLL-local flags are insufficient.
- Reject synchronous RPC in the same Context's dispatched callback chain before
  submission or slot reservation. Async RpcClient calls remain allowed. Application
  threads outside dispatch retain synchronous calls and existing deadline semantics.
- Do not introduce nested pumps, relax self-call rules, or claim general deadlock
  detection. Scoped tracking restores state after normal or exceptional completion.
- Keep one consumer of the existing Host NotificationReceiver. Workers use local
  waiting; they do not independently consume OSAL notifications. Work completion,
  registry changes and stop must wake the appropriate scheduler wait without a
  lost-wakeup window or idle busy loop. Retain existing fallback/recovery diagnostics.

所有 worker 都要有同步 RPC 防護，不能因有其他 worker 就允許阻塞式呼叫。
通知仍由單一協調端消費，worker 不搶同一個 OS 通知；不新增跨程序端點或逐筆控制封包。

## Stop, faults and resource lifetime / 停止、故障與資源生命週期

Required shutdown order:

1. Stop/fault/normal completion closes new callback admission and makes local
   `living()` false. Start/retain the existing supervised stop deadline immediately.
2. Wake framework waits and workers; do not start queued user callbacks. Existing
   operations follow their stop/cancellation contracts; preserve transport cleanup.
3. Let admitted callbacks return, drain dispatch and join framework-owned workers.
4. Invoke `on_stop()` on the lifecycle thread. It cleans user resources and
   stops/joins application-created workers.
5. Finish existing completion/failure reporting, destroy Module and callback
   captures, shut down services and unload Plugin in ownership-safe order.

**先停止派送、等框架 callback 結束並回收 worker，再進入 `on_stop()`。**
FrameworkLoop 與 Manual 都採用這個順序。
仍在其他應用 thread 上執行的 Manual pump 必須先離開派送，on_stop 再 join 該應用 thread。
不能把停止期限延後到 callback 結束後才開始，也不能每個階段重設期限。

A callback must finish independently of `on_stop()`. It may finish its current
bounded work or observe stop; blocking application waits need cancellation or a
finite wait. `living() == false` alone cannot release a user's blocking OS call.
Application workers may outlive dispatch until on_stop joins them, so on_stop still
must synchronize with those workers. No framework thread is detached to escape
shutdown. If callbacks cannot quiesce, existing supervision terminates the Module;
central and unrelated Modules continue according to their contracts.

callback 不可等待只有 on_stop 才會送出的訊號。框架不強制搶占使用者程式，也不靠
detach 隱藏未結束工作；卡死交由既有停止監督處理。使用者自建 worker 不會憑空消失，
on_stop 仍須正確停止／join 它們後再釋放其資源。

Preserve component-specific error/exception policies. A terminal callback fault
latches the Module failure and stops new dispatch, rather than silently dropping a
worker. Preserve the first primary failure and separate cleanup failures. Partial
pool creation failure joins already-created idle workers and returns a typed Error;
no user callback may start before pool startup has committed successfully.

## Python and costs / Python 與成本

Python exposes equivalent opt-in configuration and ordering/stop contracts.
All Python callbacks and object lifetime operations honor the GIL; native waits
and joins must release it where required. GIL-enabled CPython does not promise
parallel pure-Python CPU execution. I/O or native work that releases the GIL may
benefit; free-threaded Python/subinterpreters are outside scope.

Python 功能同步提供，但不承諾純 Python 的多核心加速，不擴大支援 free-threaded ABI。
驗收必須包括 callback／析構取得 GIL、等待／join 釋放 GIL，避免退出時互等。

Costs include worker stacks, local scheduler metadata/synchronization and multiple
simultaneous message copies. Do not quote an unmeasured byte/time overhead. Bound
framework ready entries by registered work and workers; retained application
messages are separately owned. Measure serial versus parallel latency, throughput,
CPU and memory with disclosed workloads; small callbacks may be slower in parallel.

## Validation / 驗證

[Windows/WSL source and SDK evidence](PARALLEL_PUMP_VALIDATION.md),
[cost method and limits](PUMP_COST_VALIDATION.md), and
[macOS source/SDK/cost acceptance](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md).
The discovered Host teardown race was repaired separately; see
[shutdown validation](CONTEXT_SHUTDOWN_VALIDATION.md).

Mac 已有原生驗收，不再列為待辦。空 batch 與喚醒成本仍是 profiling 候選，
不是已證實的缺陷或已授權的排程修改。
