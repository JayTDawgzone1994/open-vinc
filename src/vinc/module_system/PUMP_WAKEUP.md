# Pump wakeup / pump 喚醒契約

## Status and ownership / 狀態與文件責任

**Implemented through Bus routing, Host pump waiting, recovery and UI diagnostics.**
This is the primary cross-component contract. Native primitive details remain in
[Notification](../osal/NOTIFICATION.md), resource ownership in [Instance](INSTANCE.md),
and the Bus wire format in [Bus design](../message_bus/DESIGN.md). Validation below
identifies the environments actually exercised; it is not a hard real-time guarantee.

**已接通 Bus 路由、Host pump 等待、故障恢復及 UI 診斷。** 本文件維護跨元件契約；
原語、資源生命週期與控制格式分別由上述文件維護。實際驗證環境見文末，不承諾硬即時。

## Behavior / 行為

| Area / 範圍 | Contract / 契約 |
| --- | --- |
| `pumpOnce()` | Still executes one caller-initiated bounded batch and returns. It does not gain idle waiting, self-execution, or a worker thread. 仍由呼叫端主動執行一批後返回，不新增閒置等待、自行執行或執行緒。 |
| `pumpLoop()` | Keeps dispatching; when idle, blocks in an OS wait that a work notification can release. 沒工作時交給 OS 阻塞等待，有通知即可解除等待。 |
| Callbacks | Use the configured pump dispatch model: serial by default, optional Topic workers per [parallel pump](PARALLEL_PUMP.md). 不由通知端或 heartbeat 執行使用者 callback；同 Topic 維持序列。 |
| Data path | Payloads remain in shared memory; notifications carry no message payload and do not replace RingBuffer state. 資料仍走共享記憶體，通知不是訊息內容或訊息筆數。 |
| Bus semantics | Preserve batch ceilings, ordering, cursors, loss accounting, capacity, reader protection, and collision waiting. 不改變既有 RingBuffer 與批次處理語意。 |
| Manual mode | The caller still chooses when to invoke `pumpOnce()` or enter `pumpLoop()`. A notification cannot make absent pump calls happen. 不替使用者自訂迴圈自動呼叫 pump。 |
| Scheduling | No hard real-time promise; OS scheduling and application callbacks still affect latency. 喚醒不等於立刻取得 CPU。 |

## Manual wait API (2026-11-09) / Manual 等待介面

`Result<void> Context::waitForWork(Deadline deadline = Deadline::max())` exposes
the existing Host wait to Manual loops. It never dispatches callbacks, copies
messages, advances cursors, or creates a thread/endpoint. Success only means the
caller may check again; it does not prove work exists. `pumpOnce()` still runs one
bounded batch; `pumpLoop()` shares the internal wait without reacquiring admission.

`waitForWork()` 讓 Manual 迴圈使用既有 Host 通知等待；不派送 callback、不複製訊息、
不推進游標、不建立 thread／端點。成功只表示可以重新檢查，不保證有工作。
原有 pumpOnce 批次契約不變，pumpLoop 共用內部等待。

- Deadlines use steady_clock. Wait ends on a hint, framework stop, or the earlier
  of the supplied deadline and `now + module_pump_idle_interval` (10 ms).
  `Deadline::max()` removes only the caller's extra deadline, not the fallback.
  An expired deadline returns without consuming hints or entering native wait.
  OS scheduling/native operations can exceed the requested interval.
- Hint, ordinary timeout and framework stop return success; inspect `living()`.
  Unbound Context returns `ResourcesUnavailable`; active pump/wait returns `Busy`.
  Both errors use operation `WaitForWork` (71).
- All three APIs share single-caller admission. Concurrent or recursive calls
  fail before consuming hints, including callback calls with an expired deadline.
  No permanent thread affinity is added. Host quiescence includes admitted waits;
  application-created threads must still be joined before resource teardown.
- Native wait failures retain existing diagnostics, stop-aware fallback and
  receiver reopen policy, rather than becoming fatal public wait errors. Recovery
  may allocate under the existing exception policy. Reopen does not promise to
  recover hints lost while no receiver was available.
- Do not clear hints after checking work. Earlier, coalesced, stale and spurious
  hints are allowed. After returning, check lifecycle and pump again; repeated
  waits alone never advance callbacks. This is not a timer or message counter.
- No external cancellation token, user wake method, periodic scheduler or
  parallel callback executor is introduced.

期限取呼叫端與既有 10ms 補查中較早者；省略期限仍有補查，已到期則不消耗通知。
通知、逾時與正常停止皆成功返回，使用 living() 判斷是否繼續。未接線回
ResourcesUnavailable；並行／遞迴回 Busy／WaitForWork，包含 callback 內的零等待。
三個 API 共用 admission 與收尾保護；不新增永久 thread 綁定。原生故障維持診斷、
備援及低頻重建，不把降級視為 Module 必須退出，也不承諾補回無接收端時遺失的提示。
不能在檢查後清空通知再睡眠。這輪不增加使用者取消／喚醒、週期排程或並行 callback。

```cpp
while (context.living()) {
    // Run application maintenance when due, before starting the next batch.
    auto batch = context.pumpOnce();
    if (!batch) {
        return std::unexpected(batch.error());
    }

    if (batch->callbacks == 0 && batch->commands == 0 && !batch->stopped) {
        auto waited = context.waitForWork(); // Or pass the next maintenance deadline.
        if (!waited) {
            return std::unexpected(waited.error());
        }
    }
}
```

Python provides `context.waitForWork(timeout=None)` in seconds. None uses the
default policy; zero does not block. Negative/non-finite/unrepresentable values
raise `VincError(InvalidArgument, WaitForWork)` before waiting. Context lifetime
admission remains active; the binding releases the GIL during C++ waiting and
restores it before return/error conversion. It invokes no Python callback.

Python timeout 單位為秒；None 使用框架政策，0 不阻塞。負值、NaN、無限大及不可表示值
在等待前明確拒絕。等待期間釋放 GIL，返回或轉換例外前恢復；Context 存活保護維持。

Plugin entry **17** changes `ContextRuntime::wait(Deadline)`; rebuild Host, Plugins,
SDK consumers and Python native extensions together. Bootstrap v16, Bus v4 and
RPC wire v1 are unchanged. Recorder integration is a separate change documented
in the [Bag README](../bag/README.md#元件與相依); its evidence is not supplied by
the Context-only validation below.

Plugin entry 17 須整套重建；bootstrap／Bus／RPC wire 不變。Recorder 後續接線與
驗收由 Bag README 維護，下列 Context 驗收不代替 Recorder 負載驗證。

## Delivery and failure policy / 交付與失敗政策

The following policies were approved for this integration. / 以下政策已獲本輪實作授權。

1. **Keep the existing 10 ms fallback scan.** Notifications normally release the wait early. Expiry still causes a pump, preserving the existing opportunity to check RPC deadlines and Param revisions. This duration is a requested wait, not a scheduling or delivery bound. Do not introduce an infinite wait or silently increase this interval in this change.
   **保留目前 10 ms 補查。** 正常通知可提前喚醒；到期仍 pump，保留 RPC 期限與 Param 版本補查。不得在這輪自行改成無限等待或增加間隔。
2. **Publication success survives a later notification failure.** Before publication, ordinary failures return `Result` errors. After the RingBuffer publication point, do not turn an accepted message into an apparent failed publish or invite an automatic resend. Record bounded diagnostics and use fallback scanning. A full notification buffer that already represents pending work is coalescing, not publication failure.
   **提交成功後，通知失敗不改判發布失敗。** 留下有界診斷並靠補查；通知緩衝已滿且已有待處理提示，屬於合併，不是發布失敗。
3. **No new background thread.** One receiving wake endpoint per Module, shared by its pump work. Host owns it; Plugin does not create another process service. Central manages lifetime and routes, but does not relay every publication wakeup.
   **不新增背景執行緒。** 每個 Module 共用一個 Host 持有的接收入口；中央管理生命週期與關係，不逐筆轉送喚醒。

Fallback avoids permanent sleep after a missed hint; it does not guarantee that retained data survives overwrite. This integration does not turn Bus into a reliable delivery queue. Retaining fallback also means this version does not promise reduced idle wake frequency compared with the current implementation.

補查避免漏通知後永久睡眠，不保證訊息在等待期間不被覆寫，也不改成可靠交付佇列。保留補查代表第一版不承諾降低目前的閒置喚醒頻率。

## Layer responsibilities / 分層責任

| Layer / 層級 | Change / 改動 |
| --- | --- |
| OSAL | Generic named notification creation/opening, signalling, deadline-based waiting, native errors, and lifetime rules; no MID/topic/Context knowledge. 通用具名通知原語，不認識框架身分。 |
| Module System | Host-owned receiver, authenticated endpoint association, shared pump wait, local work/stop wakeups, diagnostics, and shutdown. Host 持有接收端並整合既有生命週期。 |
| Message Bus | Callback subscription notification targets, direct post-publication signals, deduplication by destination generation, and topology changes. 管理接收目標，發布後直接通知。 |
| Central resource owner | Reserve unique names before creation; retire resources and clean recorded leftovers using existing instance ownership. 沿用既有資源世代與清理紀錄，不靠猜測 PID 存活回收。 |
| Common/config | Existing Result/error contracts and named policy constants only. No dependency back into runtime components. 僅保留共用錯誤契約與具名設定。 |

Use existing component targets and designated detail interfaces. A new Concurrency wrapper, target, service registry, or general executor is not required by this design; justify any such addition separately.

沿用現有 target 與指定 detail 接線；本設計不要求新增 Concurrency 包裝層、一般執行器或 runtime registry。

## Notification primitive / 通知原語

The OSAL API uses `NotificationReceiver` / `NotificationSender`, replacing the provisional `WakeEvent` name. Its implemented API, ownership, cancellation, native backends and validation are maintained in [Notification](../osal/NOTIFICATION.md). The requirements below guide the later cross-component integration.

OSAL 使用 `NotificationReceiver`／`NotificationSender`，取代原先候選名稱 `WakeEvent`；詳細 API、生命週期、後端與測試由[通知文件](../osal/NOTIFICATION.md)維護。以下是後續跨元件整合的要求。

Required semantics:

- Many producers may notify one receiver; at most one active waiter per receiver.
- A notification before waiting remains observable. Duplicate and spurious wakeups are allowed; exact notification counts are not promised.
- The receiver checks work after consuming a notification. There is no application-facing reset operation that can erase a racing notification before sleep.
- Notify does not wait for the receiver to consume a token or process a message. This is not a wait-free execution guarantee for OS calls.
- Storage and draining work are bounded. Do not drain an endlessly replenished notification stream forever before dispatching work or observing stop.
- Do not suppress the only native notification through a shared pending bit whose owner can die between setting the bit and signalling.
- The caller keeps resources alive through concurrent operations; closing/unlinking is not an undocumented cancellation primitive.

必要語意：多通知者、單等待者；先到的通知可保留；允許合併與多餘喚醒；先消耗通知再檢查工作；通知不等待接收端處理；儲存與清理有界；避免共享旗標持有者死亡造成永久漏喚醒；資源生命週期必須明確。

OSAL implements Windows named auto-reset Events and Linux/macOS named FIFOs with nonblocking writes and readiness waits. They supply the same hint semantics, not identical token counts. The initial validation covered Windows and WSL Linux; native macOS Release verification was added on 2026-11-02. Backend details and remaining limitations are owned by the OSAL document.

已實作 Windows 命名 auto-reset Event 與 Linux/macOS 非阻塞 FIFO；初次驗證 Windows 與 WSL Linux，2026-11-02 補齊 macOS Release 原語驗證。後端細節與限制以 OSAL 文件為準。

References: [Windows Events](https://learn.microsoft.com/en-us/windows/win32/sync/event-objects), [Linux Pipe/FIFO](https://man7.org/linux/man-pages/man7/pipe.7.html), [Apple mkfifo](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/mkfifo.2.html). These describe OS facilities; Open-VINC implementation evidence is recorded separately in the OSAL document.

## Publication, topology, and lifetime / 發布、拓樸與生命週期

1. Create the receiving endpoint before advertising its identity. Endpoint names use the existing central epoch/resource namespace plus the Module generation association; never route by reusable PID alone.
2. Track callback subscriptions separately from get-last-only subscriptions. Only callback work requires Bus-triggered pump wakeups. Deduplicate a Publisher's destinations by Module generation while retaining subscription reference counts.
3. Publish the complete payload/metadata and RingBuffer head using the existing protocol **before** notification. OS signalling does not replace shared-memory ordering or reader protection.
4. Resolve/open sender resources during topology setup, not by reopening names for every publication. Retain endpoint lifetimes during sends; do not hold topology locks across callbacks or blocking control requests.
5. Close the subscription-installation race: installing a new destination must trigger a catch-up wake/check after the route becomes usable. Publications preceding installation must not wait forever for a later publication to generate a signal. Test registration acknowledgements and route ordering explicitly.
6. Removing a subscription removes its route reference; removing the last reference retires that destination. Late notifications may be harmless extra wakes for the old generation, never work for a replacement Module.
7. Stop seals new work and wakes blocked pump waits. Keep resources alive until local waiters/users are finished; reclaim cross-process resources using confirmed lifecycle state. A heartbeat timeout alone is not proof all access stopped. Extend the instance resource journal if the new native resource kind requires it.

流程依序為：先建立入口再發布身分、登記 callback 接收關係、先提交後通知、預先開好通知端、補上註冊交接喚醒、退訂引用回收、停止與世代隔離。相關舊共享資料回收政策維持不變。

## Pump and failure integration / pump 與錯誤接線

The existing pump batch remains the source of truth. A notification only requests another check; it does not extend a frozen batch or invoke a callback inline. Retained stale hints may cause an extra empty batch. Local Shell commands, RPC readiness, Param hints, topology changes, and stop must signal the same pump receiver when applicable. Preserve the separate startup/registration condition-variable uses rather than replacing every inbox wait indiscriminately.

通知只要求重新檢查，不擴大已固定的 batch，也不直接執行 callback。Shell、RPC、Param、拓樸與停止接入相同 pump 喚醒；原有啟動／註冊等 condition variable 用途保留。

Implemented failure behavior:

| Failure / 失敗 | Response / 處理 |
| --- | --- |
| Endpoint creation/setup fails before Module readiness | Return a contextual Result and clean partial resources; do not terminate a healthy central process. 啟動失敗正常回報與回收。 |
| Payload publication fails | Preserve existing publish Result behavior; do not send a success notification. 維持原發布錯誤語意。 |
| Notification fails after publication | Keep publication successful, retain bounded cause-aware diagnostics, and let fallback scanning progress. 不讓使用者誤以為應重送。 |
| One destination fails | Continue servicing other destinations; retirement and active faults are classified separately. 不連帶阻斷其他接收端。 |
| Wait backend fails | Report degradation and use the existing bounded fallback waiting mechanism; do not spin repeatedly on an invalid handle. 回報降級，不形成忙迴圈。 |
| Publish succeeds, producer dies before notify | Existing retention/exit policy plus fallback applies; no promise to preserve retired Publisher history. 不新增可靠交付保證。 |

Diagnostics distinguish **receiver waits**, **local Host wakes**, and **Publisher destination routes**.
Each bounded summary retains failures, open attempts, recoveries, first failure Unix nanoseconds,
last error with causes, and last affected target MID where applicable. A healthy native wait timeout
is normal, not a failure. Historical errors remain after recovery; retiring the last degraded route
clears its active status without pretending a successful recovery occurred. No per-message text/log output.

診斷區分接收等待、本機喚醒與 Publisher 路由；保留累計失敗／開啟嘗試／恢復次數、首次失敗
Unix ns、最後錯誤原因鏈及適用的目標 MID。正常等待逾時不是故障；恢復後歷史錯誤仍保留。
退役路由不再算活動故障，但不偽造恢復次數；逐筆發布不格式化或寫 log。

| Setting in `vinc::config` | Default | Purpose / 用途 |
| --- | --- | --- |
| `module_pump_idle_interval` | 10 ms | Fallback work/revision/deadline scan; not an OS scheduling bound / 備援補查 |
| `notification_retry_interval` | 1 s | Reopen existing generation handles after repeated send/wait failures / 低頻重新開啟 |
| `notification_send_failures_before_backoff` | 2 | Initial failed send plus one ordinary retry before throttling / 首次失敗後允許再試一次 |
| `notification_report_interval` | 1 s | At most one changed health report per interval / 狀態回報節流 |

A first failed send permits the next ordinary send to try again. Repeated failures suppress native
send attempts until retry time. The existing watchdog reopens senders; only the pump thread reopens
the receiver, between completed waits. Reopening never creates, unlinks, or switches generations.
Invalid receivers use a stop-aware fallback wait instead of busy-spinning. A recovered sender sends
a catch-up hint. Diagnostics use the existing control path; backpressure retries later without
turning a successful publication into failure. No production thread is added.

第一次送出失敗後，下一次正常送出仍可嘗試；連續失敗才節流。既有 watchdog 重新開啟 sender，
receiver 僅由 pump 在兩次等待之間重新開啟，不建立／刪除資源、不切換世代。等待失敗使用
可取消的備援等待；恢復 sender 後補送提示。回報遇背壓稍後重試，不反轉發布成功，也不新增執行緒。

Central exposes the latest authenticated report through `ModuleInfo.notifications` and Module
query v2. The existing manual-refresh Module Graph uses HTTP API v2: green means Running with an
observed report and no active notification fault; yellow means notification degradation; red is
lifecycle failure/restart waiting; gray means inactive/unobserved/unresolved. This is a sampled
status, not a live heartbeat or proof that callbacks/applications are healthy. Reports may lag;
a successful OS signal does not acknowledge receipt or callback execution.

中央以 Module 查詢 v2 提供狀態；既有手動刷新關係圖使用 HTTP v2：綠色為運作中且已收到
無活動故障的回報，黃色為通知降級，紅色為生命週期失敗／等待重啟，灰色為未活動／未觀察／
身分未解析。狀態可延遲，不是即時 heartbeat，也不證明 callback 健康或已收到每筆通知。

## Integration status / 接線狀態

Publisher `push` and loan `commit` signal a retained route snapshot
after successful RingBuffer publication. Destinations are deduplicated per Publisher and Module
generation; several callback subscriptions in the same Module share a destination. Get-last-only
subscriptions create no notification route. Route installation and callback registration send
catch-up hints. In-flight snapshots keep retired senders alive; no shared pending bit can lose the
only hint when a producer crashes. Cost remains O(destination Modules) per publish, with native
calls and short per-destination locks; this is not a wait-free or constant-cost broadcast.

push／commit 成功發布後通知，每個 Publisher 對同一 Module 世代只留一個
目標；純 get_last 不加入路由。路由及 callback 安裝後補送提示；進行中快照保住 sender 生命週期。
每次發布成本仍隨目標 Module 數量增加，包含 OS 呼叫與短期鎖，不宣稱 wait-free 或固定成本廣播。

Control protocols and Plugin entry compatibility follow [Module System](README.md).
Connected Host/Plugin/SDK/Python artifacts must be rebuilt together when those interfaces
change; old incompatible formats are rejected. Notifications do not change message layouts.

## Validation / 驗證

[Windows／WSL and historical Mac follow-up](PUMP_WAKEUP_VALIDATION.md) preserve the
original 16 ms investigation, Manual wait tests and notification benchmarks.
[Later native Mac source/SDK/cost results](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md)
have their own revision and workload. No hard real-time or universal latency guarantee is implied.
