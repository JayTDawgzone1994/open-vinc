# Pump wakeup change contract / pump 喚醒改動契約

Date / 日期: 2026-10-28.

## Status and ownership / 狀態與文件責任

**Implemented through Bus routing, Host pump waiting, recovery and UI diagnostics.**
This is the primary cross-component contract. Native primitive details remain in
[Notification](../osal/NOTIFICATION.md), resource ownership in [Instance](INSTANCE.md),
and the Bus wire format in [Bus design](../message_bus/DESIGN.md). Validation below
identifies the environments actually exercised; it is not a hard real-time guarantee.

**已接通 Bus 路由、Host pump 等待、故障恢復及 UI 診斷。** 本文件維護跨元件契約；
原語、資源生命週期與控制格式分別由上述文件維護。實際驗證環境見文末，不承諾硬即時。

## Motivation and evidence / 問題與證據

Before this change, `Context::pumpLoop()` calls `pump_batch()` and waits when it dispatches no callbacks or commands. Host `wait()` uses `module_pump_idle_interval` (10 ms); its predicate covers stop, commands, and RPC readiness, not shared-memory Bus publication. A publication does not itself wake this wait.

修改前，沒有 callback 或命令時，`pumpLoop()` 使用預設 10 ms 等待；Bus 發布到共享記憶體不會直接喚醒它。

The 2026-10-28 investigation used source `91d26bca95d80a6354323c58bea3f5fab9f9fe2c`, Windows 10.0.26200 x64, Ryzen 7 5700X3D, MSVC 19.51, and Release. Each case used 1 MiB, eight slots, 64 warmups, and 1,000 measured publications at a requested 100 Hz. Across twelve cases, all 12,000 measured messages arrived without collisions, overwrites, or integrity errors. Default pump P99 was approximately 16 ms; continuous `pumpOnce()` reduced measured P99 below 0.7 ms but consumed approximately one logical core. Instrumented default waits had medians of 15.2–15.4 ms. These are environment-specific observations, not hard latency bounds or measurements of the proposed implementation.

調查重現約 16 ms 的尾延遲；持續 pump 可降低延遲，但約占一個邏輯核心。這支持改進等待策略，不代表已驗證新通知方案，也不是三平台的固定時間保證。舊基準的方法與限制見 [benchmark](../message_bus/benchmarks/README.md)。

## Approved behavioral boundary / 已同意的行為邊界

| Area / 範圍 | Contract / 契約 |
| --- | --- |
| `pumpOnce()` | Still executes one caller-initiated bounded batch and returns. It does not gain idle waiting, self-execution, or a worker thread. 仍由呼叫端主動執行一批後返回，不新增閒置等待、自行執行或執行緒。 |
| `pumpLoop()` | Keeps dispatching; when idle, blocks in an OS wait that a work notification can release. 沒工作時交給 OS 阻塞等待，有通知即可解除等待。 |
| Callbacks | Stay on the pump caller's thread, with existing ownership, ordering, exception, and reentrancy contracts. 不由通知端或 heartbeat 執行使用者 callback。 |
| Data path | Payloads remain in shared memory; notifications carry no message payload and do not replace RingBuffer state. 資料仍走共享記憶體，通知不是訊息內容或訊息筆數。 |
| Bus semantics | Preserve batch ceilings, ordering, cursors, loss accounting, capacity, reader protection, and collision waiting. 不改變既有 RingBuffer 與批次處理語意。 |
| Manual mode | The caller still chooses when to invoke `pumpOnce()` or enter `pumpLoop()`. A notification cannot make absent pump calls happen. 不替使用者自訂迴圈自動呼叫 pump。 |
| Scheduling | No hard real-time promise; OS scheduling and application callbacks still affect latency. 喚醒不等於立刻取得 CPU。 |

## Confirmed first-release policy / 已確認的第一版政策

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

OSAL implements Windows named auto-reset Events and Linux/macOS named FIFOs with nonblocking writes and readiness waits. They supply the same hint semantics, not identical token counts. Native validation for this new primitive is currently Windows and WSL Linux; macOS is pending. Backend details and remaining limitations are owned by the OSAL document.

已實作 Windows 命名 auto-reset Event 與 Linux/macOS 非阻塞 FIFO；新通知功能目前驗證 Windows 與 WSL Linux，macOS 待驗。後端細節與限制以 OSAL 文件為準。

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

## Implementation sequence / 實作步驟

| Step / 步驟 | Deliverable / 交付 | Acceptance gate / 階段驗收 |
| --- | --- | --- |
| 1. OSAL notification | Finalize primitive contract; implement common API/native backends, error catalog, tests, and OSAL documentation. 定義與實作通用通知原語。 | Cross-process signal-before-wait/during-wait, timeout, multiple producers, bounded coalescing, failure cleanup, and platform-specific signal/handle/FD tests. |
| 2. Host resource lifetime | Add receiver ownership, instance resource records, endpoint identity exchange, and stop cleanup. 補上 Host／中央生命週期。 | Repeated starts/stops, same machine different instances, crash leftovers, stale generations, and no extra worker. |
| 3. Bus notification routes | Extend control protocol for callback destinations, registration handover, deduplication, and post-publication signals. 補上 Bus 目標與提交後通知。 | Both push/loan, dynamic subscribe/unsubscribe, multiple Publishers/Modules, no per-publication central relay, failure after commit without duplicate resend. |
| 4. Context wait integration | Connect pump idle wait and relevant local work/stop notifications; preserve fallback. 整合 pump 等待與備援。 | pumpOnce batch/threading contract unchanged; no lost wake between check/wait; RPC/Param/Shell and shutdown regression tests; bounded behavior during notification floods/failures. |
| 5. Integration and performance | Rebuild consumers, verify protocol/ABI rejection where changed, run correctness and latency/resource comparisons, update docs. 整合、效能與文件驗收。 | Actual platform reports; benchmark before/after under identical conditions; no claimed completion for unrun checks. |

Each step is reviewable and testable; completing a primitive alone does not mean Bus wakeup works. Public documentation must keep the whole feature marked incomplete until end-to-end checks pass. Preserve unrelated work and use a dedicated topic branch; publication follows AGENTS only when requested.

每一步都可獨立驗收，但原語完成不等於 Bus 接通。需端到端驗證後才能宣稱功能完成；提交／推送另依 AGENTS 與使用者指示。

Control protocol and Host/Plugin detail ABI changes require explicit version handling and rebuilding connected C++ SDK consumers and Python native adapters. Do not select version numbers from historical README passages; inspect current constants during implementation. No user-facing Python pump API redesign is included.

控制協定與 Host／Plugin detail ABI 若改變，須更新相容性辨識並重建 SDK／Python 原生接線；實際版號以當時程式為準。本輪不重設 Python 公開 pump API。

## Final validation / 最終驗證

- Deterministically exercise publication before wait, during wait admission, during dispatch, and immediately after fallback expiry. Repeated success alone does not establish absence of a lost-wakeup race.
- Inject post-publication notify failures and receiver-wait failures; check publication success, continued fallback, diagnostics, other destinations, and unchanged message counts.
- Cover hot/cold topics, several Publishers and callback subscriptions, get-last-only readers, route installation/removal races, slow callbacks, and stop during waiting/flooding.
- Repeat comparable paced benchmarks for 1–128 MiB and add genuinely small payloads. Keep payload, capacity, rate, build, and CPU conditions recorded. Report achieved rate, P50/P90/P99, send cost, collisions/loss, idle/active CPU, and subscriber fan-out cost. Notification coalescing does not prove zero per-publication system calls.
- Verify Windows, Linux (including the available Ubuntu 24.04 WSL environment), and macOS on the same implementation revision. Earlier platform validation remains valid for its tested scope; it does not validate this new feature.
- Do not change priority, affinity, global timer resolution, signal disposition, or unrelated application settings to make latency results look better.

驗收同時涵蓋競爭、故障、生命週期與實際資源成本；不可只看 P99 變漂亮。三平台既有驗證保留，新功能另附同版本證據。

## Integration status / 接線狀態

Steps 1–4 are implemented. Publisher `push` and loan `commit` signal a retained route snapshot
after successful RingBuffer publication. Destinations are deduplicated per Publisher and Module
generation; several callback subscriptions in the same Module share a destination. Get-last-only
subscriptions create no notification route. Route installation and callback registration send
catch-up hints. In-flight snapshots keep retired senders alive; no shared pending bit can lose the
only hint when a producer crashes. Cost remains O(destination Modules) per publish, with native
calls and short per-destination locks; this is not a wait-free or constant-cost broadcast.

第一至四步已接通。push／commit 成功發布後通知，每個 Publisher 對同一 Module 世代只留一個
目標；純 get_last 不加入路由。路由及 callback 安裝後補送提示；進行中快照保住 sender 生命週期。
每次發布成本仍隨目標 Module 數量增加，包含 OS 呼叫與短期鎖，不宣稱 wait-free 或固定成本廣播。

Bus control is v3, Module bootstrap v16, Plugin entry 10, Module query v2, notification report v1,
and graph HTTP v2. Rebuild central, Hosts, Plugins, SDK consumers and Python native adapters together.
Old formats are rejected. RingBuffer/message layouts and public Python pump syntax are unchanged.

上述協定／入口已更新；所有相連產物須重建，舊格式明確拒絕。RingBuffer／訊息布局及 Python
公開 pump 語法不變。原有平台驗證紀錄保留；本輪新增行為的證據另記於下方。

## Validation record (2026-10-28) / 本輪驗證

- Windows 11 x64 (10.0.26200), Ryzen 7 5700X3D, MSVC 19.51.36257 / toolset
  14.51.36231, SDK 10.0.26100.0, CMake 4.3.2, Ninja 1.13.2: complete Debug and
  Release builds passed. The 50 selected Common/OSAL Notification/Control/Bus/
  Module/Param/RPC/UI/Shell CTests passed in each configuration. After final
  admission/format/diagnostic refinements, Debug's 11 and Release's 12 focused checks passed; the latter includes the independent external SDK consumer build/deployment test.
- WSL2 Ubuntu 24.04.5 x86_64, GCC/libstdc++ 13.3, glibc 2.39, CMake 3.28.3,
  Ninja 1.11.1: Bus/Module **23/23** passed; the final eight affected tests also
  passed. This is WSL functional evidence, not a native Linux latency benchmark.
- Vue TypeScript checking passed. Chromium **6/6** passed, including yellow
  degradation, retained causes/target MID, recovery to green, red lifecycle
  priority, safe error text and manual refresh. Run browser tests only after the
  matching asset/server build finishes; a mixed-build trust fixture was rejected
  and passed after rerunning with matching artifacts. Trust checks were unchanged.

Windows Debug／Release 完整建置與各 50 項相關回歸通過；最後差異再驗證 Debug 11 項、Release 12 項（含外部 SDK 獨立建置／部署）。
Ubuntu 24.04 WSL 的 Bus／Module 23 項及最後差異 8 項通過。TypeScript 與 Chromium 6 項通過。
WSL 不當作原生 Linux 效能證據；瀏覽器須使用同版資產／伺服器，不能與重建並行混用。

Fault tests cover both publication paths, one failed destination while another
continues, immediate next-send retry, repeated-failure throttling, existing-name
reopening/catch-up, invalid receiver fallback without spinning, stop cancellation,
route deduplication/removal/subscriber-first registration, reader cleanup despite
route-encoding allocation failure, wire-capacity rejection/reuse, invalid identities,
unknown diagnostic codes, full cause chains and truncated/version-mismatched reports.
The existing process/Context tests cover batch/callback ownership, command dispatch,
stop/restart, callback failure, Python, RPC and Param regressions. Primitive pre-wait
retention/concurrent signals/cancellation evidence remains in the OSAL report.

故障測試涵蓋 push／loan、單一目標失敗不阻斷其他目標、重試／節流／恢復、失效 receiver
備援等待、停止取消、路由合併／退訂／先訂閱後發布、通知配置失敗不阻斷回收、容量拒絕後重用、
非法身分、完整錯誤原因鏈與截斷／版本錯誤。既有跨程序測試保留 batch、callback、Shell、
Python、RPC、Param 及生命週期回歸證據。原語的等待前通知與取消證據仍由 OSAL 文件維護。

### Outstanding validation / 待補驗證

Native macOS validation of **this new notification integration** remains pending;
earlier macOS component reports remain valid for their recorded scope. Native Linux
latency, pure idle CPU, very high-rate/topology-churn stress and maximum fan-out
resource limits have not been established by these tests. Retaining the fallback
means idle wake frequency is not promised to decrease. A native signal success is
only a hint acceptance, never proof of callback progress or durable delivery.

本輪新增通知整合尚待 macOS 實機驗證，既有 macOS 元件報告不受影響。原生 Linux 延遲、
純閒置 CPU、高頻拓樸變動壓力與最大扇出資源極限仍待量測；保留補查不承諾降低閒置喚醒頻率。
通知成功不等於 callback 已執行或資料可靠交付。

### Performance evidence / 效能證據

The [measurement report](../message_bus/benchmarks/NOTIFICATION_RESULTS_2026-10-28.md) records 64 B–128 MiB, push/loan, 1,000 measured messages per case, 4 GiB shared budget, raw evidence paths and 0/1/8/32 receiver fan-out. All formal runs were clean; rejected large-payload pilots remain visible. On this Windows host, 1 MiB push/loan P99 was 0.5890/0.3668 ms versus the earlier approximately 16 ms polling observation. Pure idle CPU and native Linux/macOS latency remain outside this report.

量測報告保留所有正式與未合格試跑；正式組均零碰撞／零遺失／無完整性錯誤。此 Windows 主機的 1 MiB push／loan P99 為 0.5890／0.3668 ms，相較先前輪詢約 16 ms；不推廣為跨平台保證，純閒置與原生 Linux／macOS 延遲另待量測。
