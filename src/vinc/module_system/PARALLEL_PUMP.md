# Parallel pump change contract / 多執行緒 pump 改動契約

Date / 日期: 2026-11-10.

## Status and responsibility / 狀態與文件責任

**Stages 1–4 implemented; Windows/WSL integration and cost runs recorded.** Native
macOS source and fresh SDK-only functional acceptance is recorded below; cost
measurement remains open. The serial Context investigation identified a Host teardown
race; its repair and current regression status are recorded in the
[shutdown validation](CONTEXT_SHUTDOWN_VALIDATION.md). The owner approved concurrent
Topic/control callbacks, callback drain before `on_stop()`, and the Python GIL
limitation. This document records the target behavior and implementation plan;
writing it does not authorize later stages or establish performance evidence.
`PumpOptions` and `topic_workers` are now the stage-1 C++ API spellings.

**第一至四階段已實作並完成 Windows／WSL 驗收；Mac 來源及乾淨 SDK 功能已補驗，成本待補。** Context 調查已定位並修正 Host 收尾競爭，本次回歸狀態見上述驗收紀錄。使用者同意控制工作與 Topic callback 並行、
先結束框架 callback 再執行 `on_stop()`，以及 Python 的 GIL 限制。
本文件是本次改動的主契約；SDK／搬移及成本結果見第四階段，不能宣稱三平台全部驗收完成或所有已知問題均已解決。

Current behavior remains in the [Module System README](README.md#pump-與-callback-契約).
[Pump wakeup](PUMP_WAKEUP.md) owns notification waiting;
[Message Bus](../message_bus/README.md) owns copies, ordering and reader protection;
[Param](../param/README.md) owns cache/version semantics;
[RPC](../rpc/CLIENT_CONTRACT.md) owns calls and completion lifetime.
This contract changes dispatch and shutdown ordering only within the scope below.

目前行為仍以 README 的實作紀錄為準。通知、Bus、Param、RPC 的原有責任維持，
本文件集中描述此次新增的派送與停止契約，不把提案回寫成已完成能力。

## macOS 來源補驗（2026-11-11）

[第二階段主報告](MACOS_SOURCE_FUNCTIONAL_VALIDATION.md)補齊 entry-19 原生 Debug／Release
來源 C++／Python、GIL、Manual drain、故障隔離及雙 Topic 教學互通證據。
先前 Mac 待驗敘述的來源部分由此取代；後續[第三階段](../sdk/MACOS_SDK_DEPLOYMENT_VALIDATION.md)
已補乾淨 SDK-only、搬移與公開標頭。成本矩陣依決定未執行。
教學 status 加上 flush 的原因及首輪失敗均在主報告，未改派送／停止契約。

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

Stage-1 configuration: `PumpOptions{.topic_workers = N}`.

| Setting / 設定 | Target behavior / 目標行為 |
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
`waitForWork()` waits without dispatching. Serial-loop behavior is unchanged except
for the explicitly approved shutdown ordering below.

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
教程須示範短暫持鎖取得設定副本，再於鎖外執行耗時處理，不建議包住整個 callback 的大鎖。

## RPC guard and notification ownership / RPC 防護與通知所有權

- Extend callback-execution tracking from the current single pump owner to every
  framework dispatcher/worker. Host, Plugin and Python must observe the same
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

Approved target order:

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
這是對現有 Manual「on_stop 在 pump quiesce 前」順序的明確變更，不能只改 FrameworkLoop。
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

## Implementation steps / 開工步驟

These are work boundaries, not separate claims of shippable intermediate releases.
Implementation requires explicit authorization. Newly discovered major trade-offs
must be discussed before changing the affected part.

| Stage / 階段 | Scope / 範圍 | Required evidence / 驗證 |
| --- | --- | --- |
| 1. Scheduler and C++ entry | Finalize option spelling/limits; bounded Topic ownership and fair turns; FrameworkLoop/Manual wiring; keep serial defaults and public Busy admission | Controlled overlap/non-overlap, duplicate subscriptions, hot/slow Topics, partial creation rollback, serial regression |
| 2. Control, faults and stop | Param single-writer cache; serialized control dispatch; all-worker RPC guard; notification ownership; pre-on_stop quiescence in both modes | Real Plugin guard, control/Topic overlap, self-close/re-register, exception/stop/deadline/teardown cases; no callback after cleanup |
| 3. Python and examples | Matching Python options, GIL-safe dispatch/join/lifetimes; native and Python tutorials for ownership and shared-state synchronization | Python stop/fault/retained-message tests, native/Python exchange; no unsupported CPU-scaling claim |
| 4. Integration and measurement | ABI version/rejection if changed; installed SDK exports/headers; clean relocated consumers; final docs and performance comparison | Windows Debug/Release and WSL; native macOS checklist with actual evidence/status; regression and cost report |

預期不改 Bus／RPC wire 或共享訊息布局；若內部接線調查顯示必須改動，先提出原因與範圍。
Host／Plugin ABI 有改動就明確升版、拒絕不相容版本並一起重建，不把舊測試視為新版證據。

## Acceptance checklist / 驗收清單

- [ ] Default zero-worker mode, pumpOnce boundaries, waitForWork and Busy behavior retained. Final regressions pass, but the stage-4 Debug serial Context intermittency remains under investigation.
- [x] Controlled gates prove cross-Topic overlap and same-Topic exclusion, including
      multiple subscribers/publishers and close/re-register while work is active.
- [x] Independent Topic progress and fair turns under continuous load; no payload
      backlog hidden in the scheduler; retained-message content remains independent.
- [x] Param borrowed-map lifetime, copied reads and version behavior survive concurrent
      Topic execution; Shell/RPC remain mutually serialized with existing failure rules.
- [x] Sync RPC rejection from every callback thread, async resubmission and ordinary
      application-thread synchronous calls, through actual DLL/Host and Python boundaries.
- [x] Startup rollback, stop while idle/busy, Manual main return with an active pump,
      callback exceptions, no late dispatch, bounded supervision and safe Plugin unload.
- [x] Existing handle misuse returns its documented error; no invented blanket
      thread-safety guarantee. Framework never holds internal dispatch locks in user code.
- [x] Native notification/fallback recovery does not regress; completion/stop cannot
      lose wakeups and idle workers do not spin.
- [x] Python GIL-safe waiting, shutdown and destruction; user-owned retained objects
      remain valid under their documented lifetime.
- [x] Fresh installed SDK, custom messages, relocated deployment and normal cleanup.
- [x] Compare one/many Topics and cheap/expensive callbacks; record delivered/lost
      messages, throughput, P50/P99, CPU, memory and exact environment.
- [x] Record actual Windows/WSL/macOS coverage; pending platforms remain pending.

Checked items mean the documented Windows/WSL cases have been exercised across stages 1–4, not native macOS validation or a guarantee of defect absence. The unresolved serial Context failure is deliberately left open.
勾選代表已有各階段 Windows／WSL 覆蓋，不代表 Mac 已驗或保證沒有缺陷；序列 Context 的偶發失敗刻意保留未結案。

## Stage 1 evidence / 第一階段驗收

2026-11-10: C++ entry points, TopicScheduler, finite per-Topic turns and minimal
all-worker synchronous-RPC rejection are implemented. Each scheduler entry retains
one current batch and at most one replacement metadata snapshot; repeated hints
coalesce. Capacity is `module_callback_limit` (256 Topic entries, including queued
or active work awaiting retirement); saturation reports CapacityExceeded instead
of growing a payload queue. No production Module automatically enables workers.

Plugin entry is **18** because Context/FrameworkLoop layout changed. Rebuild Host,
Plugins and bindings together. Bootstrap v16, Bus v4, RPC wire v1 and shared
message layout remain unchanged.

C++ 排程核心已接線；256 個名額包含尚未退役的工作，滿載明確失敗，不堆積訊息正文。
worker 身分已納入同步 RPC 拒絕。未替 Recorder 等元件啟用多執行緒。
Context／FrameworkLoop 布局變更，Plugin entry 升至 18，相連產物須一起重建。

| Environment / 環境 | Suites / 測試 | Result / 結果 |
| --- | --- | --- |
| Windows x64, MSVC 19.51.36257, CMake 4.3.2, Ninja, Debug | module.topic_scheduler, module.parallel, module.context, module.context_wait | 4/4, 14.70 s |
| Same Windows toolchain, Release | Same four suites | 4/4, 12.96 s |
| WSL2 Ubuntu 24.04 x64, GCC/libstdc++ 13.3.0, CMake 3.28.3, Release | Same four suites | 4/4, 8.19 s |
| Native macOS | Not run | Pending; root TODO retains required native acceptance |

- `module.topic_scheduler`: production scheduler with a test-only thread-creation
  checkpoint; controlled gates verify overlap/exclusion, coalescing 1,000 replacement
  submissions, one-worker hot/cold fairness, bounded capacity, partial creation
  rollback and first-failure admission closure.
- `module.parallel`: real FrameworkLoop Plugin, Host/central and shared-memory Bus;
  two Topics, three subscriptions, 12 deliveries. Checks per-subscription sequence,
  retained copies, same-Topic exclusion, cross-Topic overlap, recursive pump/wait
  Busy, worker synchronous-RPC rejection, control-thread identity and normal shutdown.
- `module.context`: existing serial batch/cursor/loss/topology/fault/stop cases.
  `module.context_wait`: existing wait contracts plus invalid worker counts and a
  Manual positive-worker loop's Busy admission, empty native wait and stop.
- Final runs above include scoped callback identity tracking: worker thread IDs
  are registered only during the callback and removed on normal/exceptional exit,
  rather than retained until pool destruction and exposed to OS thread-ID reuse.
- Initial sandbox tests could not create Socket/notification resources (WinSock
  10013 / Win32 5). Ordinary-user native reruns passed; initial failures are retained.

使用 gate／狀態同步證明並行，不用 sleep 猜測。真實 Plugin 驗證兩個 Topic、三個訂閱
共 12 次交付。沙箱首次 OS 資源權限失敗另留紀錄，未假裝首輪全部通過。
本輪不是效能基準、完整專案測試或 installed SDK 驗收。

Local ignored logs: `build/parallel-stage1-debug-tests.log` (sandbox),
`build/parallel-stage1-{debug,release}-final.log`, and WSL
`/home/vinc-test/python-module-deploy-20261105/parallel-stage1-tests.log`.

The stage-1 on_stop-before-quiesce gap is resolved by stage 2 below. Python
configuration/GIL, fresh installed SDK, native macOS, multi-Publisher parallel
stress and cost measurement remain later stages.

第一階段遺留的 on_stop 先於 quiesce 問題由下方第二階段修正；Python、乾淨 SDK、
Mac、多發布者並行壓力與效能成本仍待後續驗收。

## Stage 2 evidence / 第二階段驗收

2026-11-10: C++ control dispatch and pre-on_stop drain are implemented. Stop
admission, Host wait cancellation and the existing deadline precede drain; the
lifecycle thread holds no inbox lock while waiting. Active pumps join their
Topic pools before releasing admission. on_stop can then join application-owned
pump threads. A failure arriving during drain is retained without overwriting
an already selected primary failure; cleanup faults remain separate.

已實作停止 admission／喚醒、開始原有期限、等待 pump 與 Topic worker、再執行 on_stop。
等待時不持 inbox 鎖；on_stop 負責 join 使用者 thread。排空期間新增的 callback 故障
不會被正常完成吞掉，也不覆寫已選定的主要故障。

### Cross-image drain correction / 跨動態庫等待修正

The first WSL real-Plugin run exposed a concrete wait bug that monolithic tests
could not reproduce: libstdc++ 13 used different internal waiter pools for Host
and Plugin `atomic_flag::wait/notify_all`. The flag became false, but the Host
waiter stayed blocked. An isolated executable/shared-library probe printed
different pool addresses; Plugin notify left completion false, and a Host-side
notify released it. The production Host had no dynamic export of that pool.
This is evidence for this build, not a claim that every Linux binary behaves so.

Context now owns a condition variable, paired with its existing pump-owner mutex.
Flag clearing and the quiescence predicate share that mutex; notification uses
the same condition-variable object across images. No thread, IPC endpoint, retry
timer or polling interval is added. **Plugin entry is 19** because Context layout
changed; bootstrap v16, Bus v4, RPC v1 and Param v3 remain unchanged. Rebuild all
connected native Plugins and bindings. Stage-1 entry-18 evidence remains historical.

WSL 真實 Plugin 測出 Host／Plugin 的 atomic_flag 等待池不同，旗標已清除但 Host 不醒。
隔離 probe 亦重現，從 Host 補一次 notify 才返回。改由 Context 持有同一個 condition
variable，搭配原有 owner mutex，避免依賴標準庫各 image 的內部池。
新增的是每個 Context 的同步狀態，沒有新增背景 thread／端點／輪詢。布局變更升至 entry 19。

### Control and failure coverage / 控制與故障驗證

- Real `module.parallel` Plugin: Param callback, Shell hook, RPC handler and async
  completion all check the control-thread identity and mutual exclusion while a
  Topic callback is held at a bounded gate. Every callback category rejects sync
  RPC before request admission. Worker async self-call and callback resubmission
  succeed. Param cached/current copies remain independent; a concurrent central
  write advances version without mutating the callback's borrowed map.
- Real `module.parallel_stop` Manual Plugin: main returns with an active external
  pump; parent observes Finishing/Stopping and releases the callback independently
  of on_stop. No queued callback starts during cleanup. on_stop joins the app
  thread. Self-close/re-register of the exact Topic cannot bypass the active owner.
  A thrown callback is retained as HookException/SubscriptionCallback even when
  the app ignores pump's Result. A stalled callback is terminated under the original
  stop grace; on_stop never runs early. The Host deadline report or central's
  observed abnormal exit is accepted according to which supervision path wins.
- `module.context_wait`: native wait/fallback/recovery and single-owner Busy,
  plus Param exception after stop (not silently treated as cancellation) and
  injected coordinator bad_alloc. Escaped coordinator/worker exceptions latch
  failure before pool joins, so supervision is already active. Expected Result
  failures retain their component policy; no blanket exception policy is added.
- `rpc.contracts`: stopping during an expired-completion batch suppresses later
  callbacks in that batch. Existing RPC suites retain synchronous calls, shared
  transport routing, cancellation/target exit, ownership and recovery behavior.

真實 Plugin 以有期限的 gate 驗證控制／Topic 重疊，不靠 sleep 當並行證明。
Param 保持單一快取寫入者，callback map 不跨 thread 留存。同步 RPC 防護與非同步重送
均通過；停止測試覆蓋 main 返回、同名 Topic 重建、callback 例外與卡死監督。
不要求跨平台必須同一退出碼，也不把 Host 回報被中央終止搶先視為正常完成。

| Environment / 環境 | C++ regression / 回歸 | Result / 結果 |
| --- | --- | --- |
| Windows x64, MSVC 19.51.36257, CMake 4.3.2, Ninja, Debug | Six Module, three Param, nine RPC suites | 18/18, 48.77 s |
| Same Windows toolchain, Release | Same 18 suites | 18/18, 46.50 s |
| WSL2 Ubuntu 24.04 x64, GCC/libstdc++ 13.3.0, CMake 3.28.3, Release | Same 18 suites | 18/18, 30.59 s |
| Native macOS | Not run | Pending in root TODO |

The full-group results above precede one final test-only assertion: a Manual main
thread makes a synchronous call while a Topic is active and receives the ordinary
remote NotFound result, not BlockingCallInPump. The updated lifecycle fixture is
rebuilt and separately repeated below; production code is unchanged.

| Final lifecycle fixture / 最後版本停止測試 | Repeats / 重複 | Total time / 總時間 |
| --- | --- | --- |
| Windows Debug | 3/3 passed | 14.50 s |
| Windows Release | 3/3 passed | 14.22 s |
| WSL Release | 3/3 passed | 10.14 s |

Logs: `build/parallel-stage2-{debug,release}-stop-final.log` and WSL
`/home/vinc-test/python-module-deploy-20261105/parallel-stage2-stop-final.log`.
The nine repetitions include normal drain, self-close/re-register, late callback
failure and deadline termination on every invocation. They establish regression
evidence, not exhaustive schedule exploration or performance measurements.

Initial failures are retained: Windows fixture waited for Running after the short
main had already entered Finishing; the test now uses explicit callback/stop gates.
The first stall assertion incorrectly looked only for primary failure, then only
for a Host deadline report; actual supervision also permits central termination
to win. WSL's overly broad first selection additionally ran an old Python fixture
and correctly rejected its old Plugin ABI. Final stage-2 selection excludes Python;
Python configuration/GIL/rebuild acceptance remains stage 3, not a relaxed ABI check.

首輪失敗保留於本機 log，包含測試等待 Running 的錯誤、停止故障欄位／回報順序假設、
真正的跨 image atomic wait 缺漏，以及誤帶舊 Python fixture 的 ABI 拒絕。
不是完整專案、Python 或 SDK 驗收，不宣稱效能改善；Mac 待原生驗證。

Ignored local logs: `build/parallel-stage2-debug-complete.log`,
`build/parallel-stage2-release-abi19.log`; WSL
`/home/vinc-test/python-module-deploy-20261105/parallel-stage2-complete.log`.
The isolated waiter-pool probe is under `build/parallel-stage2-atomic-probe/`
and WSL `atomic-probe/` beside the validation build, not shipped as framework code.

## Stage 3 evidence / 第三階段驗收

2026-11-10: Python exposes immutable `PumpOptions(topic_workers=0)`.
FrameworkLoop accepts `pump_options` in the BaseModule subclass declaration;
PythonModule copies it after construction while holding the GIL, and native main
uses that copy. Manual passes options to `context.pumpLoop(options)`. Supplying
class-level options for Manual is rejected rather than silently ignored. The
native Context remains the single owner of admission/worker limits/scheduling.
Plugin entry **19**, bootstrap 16 and existing Bus/RPC/Param protocols are unchanged.
The binding and native fixtures were rebuilt together; no ABI guard was relaxed.

Python 設定同步接入既有排程器。FrameworkLoop 在 class 宣告設定，Manual 於呼叫
pumpLoop 時傳入；省略維持零 worker。設定在建構完成時複製，不由執行中的 native worker
讀取 Python class 屬性；本輪沒有變更既有 C++ ABI／wire，也沒有放寬相容性檢查。

GIL review retained the existing `PythonCallback` acquire/copy/invoke/retire
boundary and ContextCall lifetime. Manual pumpLoop releases the GIL through
native waits, worker joins and exception unwinding; FrameworkLoop runs directly
inside the already-released `_run` boundary. Runtime quiescence precedes Python
on_stop, then Context revocation drains entered calls before GIL-held resource
clearing. No additional interpreter, callback executor or language-specific
scheduler was introduced. This does not promise parallel pure-Python CPU work.

GIL／生命週期保護沿用原接線，重點是等待與 join 不占住 GIL、callback 與引用銷毀取得
GIL，以及先 drain 再 on_stop／撤銷／清理。實測不代表支援 free-threaded CPython，
也不是純 Python 多核心效能的證據。

`module.python_parallel` uses actual Python subprocesses and native echo Plugins:

- Event gates prove cross-Topic overlap and exact-Topic exclusion with duplicate
  subscribers. Every retained sample is checked after later deliveries and at stop;
  mutating one delivery leaves the other subscription's copy and next sample intact.
- Topic callbacks overlap Param, Shell and asynchronous self-RPC handler/completion.
  Control callbacks stay on one coordinator; recursive pumps/waits return Busy,
  and blocking RPC is refused on both coordinator and Topic workers.
- Callback registration/close/reference retirement occurs inside a worker. Bound
  callback cycles release during revoked-Context destruction with a checked marker.
- FrameworkLoop and Manual complete normal exchange and stop while idle. Manual main returns
  with an active callback on an application-owned pump thread: a parent-controlled
  gate proves on_stop cannot precede drain. on_stop joins the application thread.
- A callback fault during that drain retains HookException/SubscriptionCallback
  with PythonException cause. An unreleased callback is terminated by existing
  supervision; an unrelated native Module remains Running. A count above the
  native worker ceiling is rejected as MainHook failure with InvalidArgument/Pump cause.
- The same test compiles the actual C++ tutorial and loads the actual Python
  tutorial, verifies both receive four messages from the two Publishers, and checks
  normal shutdown. This checks safe exchange, not a latency or configuration-barrier guarantee.

受控案例包含不同 Topic 重疊、同 Topic 多訂閱序列、保留訊息與獨立修改、控制接線／RPC
防護、worker 退訂／引用釋放、兩種模式的忙碌／空閒停止、Manual 提前返回、例外及停止
期限。最後直接執行 [C++／Python 教學範例](../../../examples/userExampleModule/PARALLEL_PUMP.md)，
不是另外維護一份不會被使用者讀到的範例副本。

| Environment / 環境 | Regression / 回歸 | Result / 結果 |
| --- | --- | --- |
| Windows x64, MSVC 19.51.36257, CMake 4.3.2, Ninja, CPython 3.13.12, Debug | module.python, python_messages, python_runtime_metadata, python_runtime, python_services, python_parallel; rpc.python | **7/7, 51.00 s** |
| Same Windows toolchain, Release | Same seven suites | **7/7, 47.80 s** |
| WSL2 Ubuntu 24.04 x64, GCC/libstdc++ 13.3.0, CMake 3.28.3, Ninja, CPython 3.12.3, Release | module.python_shell, python, python_messages, python_services, python_parallel; rpc.python | **6/6, 34.20 s** |
| Native macOS | Same source/binding rebuild and native Debug/Release tests | Pending; root TODO updated |

The seven Windows suites include the unchanged dynamic/static Runtime policy
checks. Existing zero-worker Python lifecycle, Bus, Param, synchronous/async RPC
and waitForWork tests passed against the rebuilt extension. No full-project or
fresh installed SDK acceptance, new performance claim, or automatic Recorder
parallelism is implied by this stage. Those remain stage 4 or outside scope.

Windows 另含既有 Runtime policy 檢查，未放寬任何 DLL／ABI 防護。預設零 worker、
既有生命週期、Bus／Param／RPC／waitForWork 均回歸通過；不代表完整專案或 installed SDK
驗收。效能／成本、乾淨 SDK／搬移與 Mac 原生留後續，Recorder 不自動啟用多執行緒。

Initial fixture failures are preserved: the native echo's default two-slot ring
overwrote a four-sample burst, so the fixture now explicitly provisions 16 slots;
the invalid-options assertion originally omitted the legitimate MainHook wrapping.
The initial C++ fixture also omitted stop_module's deadline argument; its build
failure was corrected before execution. No production behavior was changed to
make these assertions pass. The separate Windows test entry declares its repository
root explicitly; its first incomplete configure was not a Runtime failure.

Local ignored logs: `build/parallel-stage3-debug-tests.log`,
`build/parallel-stage3-release-tests.log`; build/initial-failure logs use the same
`parallel-stage3-` prefix. WSL logs are under
`/home/vinc-test/python-module-deploy-20261105/parallel-stage3-tests.log` and the
same prefix. Windows Python-enabled test trees are `build/parallel-python-debug`
and `build/parallel-python-release`; formal App Python defaults were not edited.

<a id="stage-4-acceptance"></a>

## Stage 4 acceptance / 第四階段整合與成本驗收

2026-11-10. No production dispatch, protocol, worker default or Recorder behavior
was changed in this stage. Plugin entry remains **19**; source/SDK/bindings were
rebuilt with matching headers and libraries. The previous-entry Plugin rejection
case passed as part of `module.plugin`. The public first-include smoke list now
includes `vinc/module/pump_options.hpp`. Stale current-entry summaries in the SDK
and Python README were corrected; historical evidence remains labeled historical.

本階段只補驗收、探針、測試診斷與文件；不修改排程策略、預設 worker、Recorder 或 ABI。
entry 19 的整套來源／SDK／binding 已重建，舊 entry 拒絕測試亦通過。

### Fresh installation and relocation / 乾淨安裝與搬移

Three new SDK-only producer directories used `VINC_INSTALL_SDK=ON`,
`VINC_BUILD_APPS=OFF`, `BUILD_TESTING=OFF`. The maintained CMake scripts behind
`sdk.installed_consumer`, `sdk.installed_python`, `sdk.installed_application` were
then executed directly against each producer, not against an older installed SDK.
Each uses its own copied fixture and moved installation; these were not CTest
invocations of the three scripts. Their existing timeouts/assertions were retained.

- The C++ consumer checks moved targets/headers, message generation, execution,
  ABI/profile rejection and protected installation ownership.
- The Python fixture now opts its native Plugin and Python peer into two workers.
  Bounded gates require two Topics to enter simultaneously on different threads;
  a serial implementation would time out. Native/Python retain owned deliveries
  through `on_stop`; Python mutates one retained message and checks the other.
- Before the first App run, both the consumer build tree and moved SDK are renamed
  out of their configured locations; App starts from an unrelated working directory.
  Custom schema Bus exchange, synchronous/asynchronous RPC, Param and orderly
  shutdown still pass. The fixture then tests schema additions/removal/recovery,
  ABI/startup failure isolation, static Python CRT guards on Windows and owned-file
  repair/cleanup. No incompatible environment was made acceptable by the test.
- The application script independently checks moved C++ App packaging, built-in
  Bag/UI, scripts, custom messages, Log settings, incremental rebuilds and inventory.

三個新 SDK producer 沒有沿用先前的 SDK 副本。實際執行既有三套 CMake 驗收腳本，
並強化多 worker 路徑；原 SDK 與 consumer 路徑隱藏後仍能跑搬移 App。正常通訊之外，
也保留錯誤 ABI 拒絕、訊息增刪與壞 schema、部署修復及使用者檔案保留驗證。

| Environment / 環境 | Fresh SDK scripts / 乾淨 SDK 腳本 | Consumer / Python / App time |
| --- | --- | --- |
| Windows 11 x64, MSVC 19.51.36257, CMake 4.3.2, Ninja, CPython 3.13.12, Debug | **3/3 passed** | 45.00 / 769.70 / 347.10 s |
| Same Windows environment, Release | **3/3 passed** | 54.74 / 824.55 / 355.94 s |
| WSL2 Ubuntu 24.04 x64, GCC/libstdc++ 13.3.0, CMake 3.28.3, Ninja, CPython 3.12.3, Release | **3/3 passed** | 33.55 / 656.54 / 169.20 s |
| Native macOS Debug/Release | Both configurations passed the three fresh SDK scripts and parallel gates; see the [2026-11-11 report](../sdk/MACOS_SDK_DEPLOYMENT_VALIDATION.md) | Recorded in that report |

The long Python script includes multiple extension rebuilds; these times are
validation durations, not runtime performance or SDK installation benchmarks.

### Regression and unresolved failure / 回歸與未解偶發失敗

Windows Debug and Release built `sdk_header_smoke`, including the new public
header; WSL Release did likewise. Focused CTest groups cover topic scheduler,
parallel dispatch/stop, Context/wait, Runtime faults, previous-entry Plugin
rejection, all three Param tests and nine native RPC tests. WSL also includes
`rpc.python` in the same selected group.

| Environment | Final group result |
| --- | --- |
| Windows Debug | **19/19, 61.41 s** |
| Windows Release | **19/19, 57.62 s**; diagnostic-only Context rebuild additionally passed **1/1, 13.43 s** |
| WSL Release | **20/20, 33.85 s**; diagnostic-only Context rebuild additionally passed **1/1, 6.52 s** |

**Preserved failure:** the first Debug group was **18/19**, with `module.context`
returning ModuleNotFound/WaitRunning during the serial `loss` case. The unchanged
isolated rerun passed; after adding diagnostic output, ten consecutive runs passed
(151.83 s), followed by the final group above. No assertion, timeout or production
behavior was relaxed. The original log lacks the primary cause; this remains an
[open investigation](README.md#outstanding-issues--待處理事項), not a fixed defect or
proof that parallel dispatch caused it. Stage-3 Python regression results remain
applicable to the unchanged binding/Runtime; this stage additionally rebuilt and
ran the installed Python extension. No full-project CTest or native macOS claim.

Debug 首輪失敗沒有抹掉；補診斷後十次及最終整組通過，仍不等於查清根因。
目前只能說本輪整合案例已跑完，不能宣稱所有歷史偶發問題都已修復。

### Measured costs / 實測成本

The opt-in [installed-SDK cost probe](tests/pump_cost/README.md) compares native
Manual pumpLoop with 0/1/2/4 additional workers, 1/4 Topics, cheap/CPU-heavy callbacks,
three repetitions each. Windows and WSL ran sequentially after builds/tests ended.
**48/48 cases per platform**, **245,760 deliveries per platform, zero loss/collision**.
The [cost report](PUMP_COST_VALIDATION.md) retains methodology, all 96 numeric rows,
throughput, P50/P99, process CPU, idle sampling, RSS and limitations.

Four-Topic heavy throughput improved about 3.9x with four workers in both tested
environments. One Topic remains serial and can slow down with extra workers;
cheap callbacks do not generally benefit. WSL shows measurable idle overhead;
Windows short CPU windows are too coarse/noisy for a precise worker cost. Peak
RSS is not isolated worker allocation or virtual stack reservation. These results
do not establish Python CPU scaling, real-world throughput or hard-real-time bounds.

四 Topic 重工作有明確收益；單 Topic／輕 callback 不應盲目增加 worker。資源成本與
測量限制完整保留，沒有因此調整零 worker 預設，也沒有替 Recorder 自動套用。
後續空 batch／喚醒最佳化僅列為待 profile 的候選，未取得新實作授權。

### Evidence and Mac handoff / 證據與 Mac 接手

Windows ignored artifacts use `build/parallel-stage4-*`: fresh SDK producers
`parallel-stage4-sdk-debug` / `parallel-stage4-sdk-release` contain each acceptance
log and moved test package; regression and the original failure logs remain at
build root. WSL evidence is under `/home/vinc-test/python-module-deploy-20261105/`
with the same prefix. `PUMP_COST_VALIDATION.md` links the tracked numeric data.

Initial harness failures were retained: the Windows runner initially selected
MSYS Python 3.14 instead of the intended official CPython 3.13, causing the foreign
venv fixture lookup to fail; it was rerun with the explicit official interpreter.
WSL initially omitted the existing Node directory from PATH. The new cost fixture
initially missed the required module_interface override; its build was corrected
before any measurements. None required a Runtime/ABI guard change.

Mac entry-19 source/binding Debug/Release regression is now recorded in the
[2026-11-11 source report](MACOS_SOURCE_FUNCTIONAL_VALIDATION.md), including the
initial failures and their isolated portability fixes. The three fresh SDK-only
scripts and relocated parallel consumers subsequently passed in the
[deployment report](../sdk/MACOS_SDK_DEPLOYMENT_VALIDATION.md). Native Release cost
measurement is separate and was explicitly excluded from this functional round.
Prior Mac FD/Param and Windows/WSL results are not substituted for this evidence.
