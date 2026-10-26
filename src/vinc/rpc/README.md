<a id="rpc"></a>

# RPC / 具型別非同步請求／回覆

RPC is asynchronous typed Module-to-Module request/response through inherited Context. The native official UI Module can use it; a browser-plugin RPC bridge remains future work.

RPC 提供 Module 間非同步、具型別的 request / response，使用繼承自 BaseModule 的 Context。
官方 UI Module 可使用同一入口；瀏覽器 UI plugin RPC bridge 留到後續接線。

<a id="公開-api"></a>

## Public API / 公開 API

```cpp
context.register_rpc<Request, Response>(service, options, handler);
context.call_rpc<Request, Response>(target_module, service, request, call_options, completion);
context.rpc_stats();
```

- Request/Response must be generated messages; registration fixes two schema hashes, checked centrally at call time.
- register_rpc returns `Result<void>` and waits for central registration, usually in on_start. Services live until Module stop with no unregister. Local registration locking installs handlers before Invoke; a concurrent pump may wait for registration. Registration inside callbacks is allowed; Host receiver does not use this lock.
- call_rpc success means local acceptance only; remote QueueFull arrives through completion. It snapshots the request, so the caller may mutate/release the original afterward.
- Handler is RpcStatus(const Request&, Response&), with call-scoped references. Framework checks/serializes/copies the mutable local response after return.
- Completion is void(`Result<RpcReply<Response>>`); response unique_ptr transfers independent ownership, not a shared-slot lease.
- RpcStatus code 0 means application success; nonzero application codes and optional reason may still include a valid typed response. Framework Result errors remain distinct.
- Target and service names are separate. Service uses nonempty ASCII letters/digits/_/-. Duplicate service names are forbidden within a Module but allowed across Modules.

- Request／Response 必須是 Ribosome 生成訊息；註冊固定兩個 schema hash，呼叫時中央核對。
- register_rpc 回傳 Result<void>，同步等待中央登記。通常在 on_start 呼叫；服務存活至 Module 停止，
  不提供取消註冊。本地登記鎖保證 handler 先安裝才派送第一筆 Invoke；並行的 pump 可能
  等待這次登記結束。可在 pump callback 內登記，Host receiver 不使用這把鎖。
- call_rpc 回傳 Result<void>；成功只表示本地接受排送，遠端 QueueFull 等結果由 completion 回報。
  呼叫會複製 request 快照，返回後可修改或釋放原訊息。
- Handler 為 RpcStatus(const Request&, Response&)；引用只在 handler 期間有效。
  response 是本地可變訊息，返回後框架檢查容量、建立映像並複製到共享槽位。
- Completion 為 void(Result<RpcReply<Response>>)。Response 以 unique_ptr 交付獨立所有權，
  可 move 保存，不占住共享槽位。
- RpcStatus.code == 0 表示應用成功；其他值是應用自訂狀態，reason 可省略。
  應用失敗仍附帶合法 typed Response。框架失敗使用 Result／Error，兩者分開。
- 服務名與目標 Module 名分開傳入；服務名限 ASCII 字母、數字、_、-，不能空白。
  同一 Module 不接受重複服務名，不同 Module 可註冊同名服務。

See the [complete example](../../../examples/rpc/README.md).

完整範例見 [examples/rpc](../../../examples/rpc/README.md)。

<a id="pump佇列與失敗"></a>

## pump, queues, and failures / pump、佇列與失敗

- Handlers/completions run only on the pump caller. A pass consumes the Host RPC control events ready at its start; events arriving during callbacks wait for another pass, not every occupied slot.
- Each service dispatches in central Submit-receive order, not global call-start FIFO. Handlers follow received Invoke order; no global inter-service chronology.
- QueueFull means not accepted and no handler. Shared-slot/local-pending/dispatch limits may cause it. Retry later explicitly; no automatic retry.
- No synchronous-result API. Handlers may issue async calls but must not wait for results requiring the same pump. Slow callbacks delay the pass, without preemption/hard real-time guarantees.
- Deadline starts at call and is checked by pump; no pump means no timeout callback. Even an arrived-but-unconsumed reply may time out with unknown outcome. Notify once.
- Unsubmitted grants may release; submitted handlers may continue. No public cancel. Timed-out bookkeeping/slots remain until safe completion/confirmed exit; late replies only reclaim. Timeout does not prove nonexecution or exactly-once through crashes.
- Confirmed target exit with unprocessed completion gives TargetExited; exit/completion races need not preserve the response. Heartbeat timeout cannot reclaim live leases. Stop closes admission and can fail pending calls.
- Escaping user exceptions follow Module fault policy; central/peers continue. Bad protocol closes the offending Session; existing supervision stops it.

- Handler／completion 只在 pump 呼叫執行緒執行。一輪先取出當時已到達 Host 的 RPC 控制事件；
  callback 期間新增的事件留到下一輪。處理的是本輪開始時已就緒的工作，不等待每個已占用槽位。
- 每服務依中央接收 Submit 次序排送；不同 caller 的本地 call 開始時間不構成全域 FIFO。
  Handler 依 Invoke 收到順序執行，不承諾所有服務間的時間排序。
- QueueFull 表示未被接受，不執行 handler。共享槽位、本地 pending 或排送預算不足均可能回報。
  使用者可稍後重試，框架不自動重試。
- 沒有同步等待結果 API。handler 可再發 async RPC，但不能阻塞等待需要同一 pump 才能完成的結果。
  慢 callback 延後整輪，沒有搶占或硬即時保證。
- Deadline 從 call 開始，在 pump 檢查。未 pump 不會自行執行逾時 callback；即使回覆已到達但
  尚未被 pump 消費，也可能回報 TimeoutOutcomeUnknown。到期只通知一次。
- 尚未提交的 grant 可釋放；已提交的 handler 可能繼續。第一版沒有公開取消 API。
  timeout 保留簿記／槽位到安全完成或確認程序退出；晚到結果只回收，不再次通知。
  逾時不等於未執行，不承諾跨程序崩潰的 exactly-once。
- Target 確認退出且完成尚未處理時回報 TargetExited；退出與完成競爭不保證取得結果。
  heartbeat timeout 不足以回收存活對端的 lease。正常停止封住新工作，未完成呼叫也可能失敗。
- 逃出 handler／completion 的例外依 Module 故障政策處理，中央與其他 Module 繼續。
  非法協定回報 Protocol／關閉違規 Session，由既有監督負責停止與退出。

<a id="共享槽位與所有權"></a>

## Shared slots and ownership / 共享槽位與所有權

Central creates a fixed pool at registration: each slot has request/response regions plus two SharedWords, total (align8(request_bytes)+align8(response_bytes)+16)*slots. Capacity includes complete images/metadata, not only arrays.

中央在服務登記時配置固定池，每格包含 request 區、response 區和兩個 OSAL SharedWord。
大小為 (align8(request_bytes) + align8(response_bytes) + 16) * slots。
容量包含完整訊息映像與 metadata，不只是陣列內容。

```text
Free → Granted (caller writes request)
     → Queued  (callee reads request and writes response)
     → Reply   (caller copies response)
     → Free    (caller sends Release)
```

Socket carries schema, MID-bound identity, slot descriptors, status, and notifications; all message payload uses shared memory. Mapping checks bounds/generation; SharedWord publication precedes Storage layout validation. This RPC copies data, not a user-editable shared loan.

Socket 只傳型別、經 MID 認證的身分、槽位描述、狀態及通知；所有 message payload 都走共享記憶體。
每次映射檢查邊界／世代，以 OSAL SharedWord 確認映像發布後再由 Message Storage 驗證布局。
這是有複製的 RPC，不是使用者直接編輯共享區的 loan API。

Scoped request/service IDs and slot tokens increase without wrap; restarted Modules receive new MIDs. Slots are never overwritten and duplicate notifications do not rerun handlers. Central names/resource records govern cleanup. A live peer lease prevents early reuse even if the other peer dies; reclaim dead ownership only after OS exit. Central death follows existing child-liveness policy, not a sandbox against malicious shared-memory writes.

各範圍 request ID、service ID、slot token 單調增加且不回繞；新 Module 使用新 MID。
槽位不覆寫，重複通知不重跑 handler。共享名稱沿用中央資源紀錄產生及回收。
存活對端持有讀寫 lease 時，即使另一端已死也不提早重用；中央確認 OS 退出才回收死亡方。
中央死亡依既有生存契約結束子程序，不承諾對惡意改寫共享記憶體的 Module 提供沙箱。

<a id="資源與執行緒"></a>

## Resources and threads / 資源與執行緒

Settings stay in vinc/config.hpp, without Param migration:

設定集中於 vinc/config.hpp，本輪未遷入 Param：

| Setting | Default/use |
| --- | --- |
| rpc_request_bytes / rpc_response_bytes | 64 KiB each, ServiceOptions overrides |
| rpc_slots / rpc_slot_limit | 8 per service, range 1–256 |
| rpc_shared_bytes | 256 MiB centrally across pools |
| rpc_local_bytes | 64 MiB outbound snapshots per Context |
| rpc_pending_limit | 64 unreclaimed calls including timeout tombstones |
| rpc_service_limit | 256 services including lease-draining services |
| rpc_active_limit | 1024 granted slots centrally |
| rpc_control_limit | 4096 per central/Host/peer queue |
| rpc_name_bytes / rpc_reason_bytes | 128 / 256 bytes |
| rpc_call_timeout / rpc_max_call_timeout | 5 s default / 24h max; default for registration |
| rpc_control_retry | 5 ms dispatch/recovery checks |

| 設定 | 預設與用途 |
| --- | --- |
| rpc_request_bytes / rpc_response_bytes | 各 64 KiB，ServiceOptions 可覆寫 |
| rpc_slots / rpc_slot_limit | 每服務預設 8 格，範圍 1～256 |
| rpc_shared_bytes | 每中央 256 MiB，全部服務配置總和 |
| rpc_local_bytes | 每 Context 64 MiB，保留 outbound request 快照總額 |
| rpc_pending_limit | 每 Context 64 個未回收 call，含逾時 tombstone |
| rpc_service_limit | 每中央 256 個服務，含等待 lease 回收的服務 |
| rpc_active_limit | 每中央 1024 個已借出槽位 |
| rpc_control_limit | 中央 inbox、各 Host／peer 控制佇列各 4096 筆 |
| rpc_name_bytes / rpc_reason_bytes | 128 / 256 bytes |
| rpc_call_timeout / rpc_max_call_timeout | 預設 5 秒，最大 24 小時；登記等待採預設值 |
| rpc_control_retry | 中央 worker 排送重試／退出補查間隔 5 ms |

These are not RSS limits: handlers, serialized temporaries, Python copies, and retained responses add memory. Pools are fixed; each transfer opens/maps them without a mapping cache.

這些不是程序整體記憶體上限：本地 handler 訊息、序列化暫存、Python copies 及使用者保留的
Response 額外占空間。共享池固定，每個 transfer 開啟／映射該池，尚無 mapping cache。

One central RpcManager worker handles allocation/dispatch/recovery outside lifecycle supervision. Host retains its sole watchdog/control receiver; Plugins own no second service or RPC-callback thread. Idle wakeups and runtime scheduling exist; no fixed latency or guaranteed advantage over Param is claimed.

中央增加一個 RpcManager worker 處理配置、排送、回收，避免共享區配置卡住生命週期監督。
ModuleHost 沿用唯一 watchdog／控制 receiver；Plugin 沒有第二套控制服務，也沒有 RPC callback thread。
worker 有少量閒置喚醒，傳送受現有 runtime 排程影響，不宣稱固定延遲或必然比 Param 快。

rpc_stats reports local pending/request_bytes/queue_full/timeouts/requests/completions. Requests counts decoded/handler-stage work including layout/capacity failures, not application success. Completions counts notifications once, including timeout but not late replies. These are not central-wide traffic totals.

rpc_stats 返回本地 pending、request_bytes、queue_full、timeouts、requests、completions。
requests 計入服務解碼／handler 階段已處理的請求（含布局／容量失敗），不代表應用成功次數；
completions 是通知次數，timeout 計一次、晚到回覆不重算。不是全中央流量統計。

<a id="python"></a>

## Python / Python 介面

```python
context.register_rpc(Request, Response, "service", handler, options)
context.call_rpc(Request, Response, "target", "service", request, completed, timeout_ms=5000)
```

Handlers receive two local messages and return vinc.rpc.RpcStatus; local objects may be retained. Completion completed(reply, error) gets (status, response)/None on success, or None/VincError for framework failure. Immediate admission failure raises VincError. Completion must return None; escaping exceptions retain Python diagnostics and fault only that Module. Types are compiled-in; no dynamic schema interpreter. Stats match C++ fields.

handler 收到兩個本地 Python 訊息，回傳 vinc.rpc.RpcStatus；可保留本地物件。
completion 為 completed(reply, error)：成功 reply 是 (status, response)，error 為 None；
框架失敗 reply 為 None，error 為 VincError。立即 admission 失敗直接拋 VincError。
completion 必須回傳 None；未處理例外由既有 Python 故障路徑保留診斷並終止該 Module。
使用已編譯 Python message 型別，沒有動態 schema 直譯器。rpc_stats 與 C++ 提供相同欄位。

<a id="相依協定與-abi"></a>

## Dependencies, protocol, and ABI / 相依、協定與 ABI

- vinc::rpc supplies Context facade/image validation/read/write using Storage, Common, OSAL, control codecs.
- vinc::rpc_host owns Host Session integration and is not linked into Plugins as a control service.
- vinc::rpc_manager owns central worker/services/pools.
- ErrorDomain::Rpc=14 has a build-composed static catalog; numeric errors/operations/causes are encoded, never pointers/layouts. Preserve unknown codes; reject malformed/incompatible formats.
- Application domain 6, transfer/admission v1; error envelope versions are separate.
- Plugin entry 8/bootstrap v13 require matched Host/Plugin/SDK rebuilds and toolchain/stdlib/config/layout/CRT.
- RPC does not publish topics or fill Bus publisher/count/timestamps. Applications needing those concepts define payload fields instead of borrowing read-only Bus metadata.

- vinc::rpc：Context facade、布局驗證／映像讀寫，依賴 Storage、Common、OSAL、控制 codec。
- vinc::rpc_host：Host Session 接線；Plugin 不連入此控制服務實作。
- vinc::rpc_manager：中央 worker、服務與池所有權。
- ErrorDomain::Rpc = 14，靜態 catalog 經 CMake 整合。錯誤／操作及原因鏈明確編碼，不傳送
  C++ pointer 或物件布局。未知錯誤代碼保留數值，損壞／不相容格式拒絕。
- RPC domain = 6，transfer／admission 版本 = 1，錯誤 envelope 另有版本。
- Context 布局已改：Plugin entry = 8、bootstrap = v13，Host、Plugin、SDK 須全部重新建置。
  保留相同工具鏈、標準庫、Debug/Release、布局與 CRT 限制。
- RPC 不作為 topic 發布，不替訊息填 Bus publisher、topic 序號或發布時間；需要時應用須自行
  在 message 資料欄位定義，不借用唯讀 Bus metadata。

<a id="驗證"></a>

## Recorded validation / 驗證

Tests cover real large C++ cross-process payloads, Python↔C++, schemas/application failures/QueueFull/timeouts/late replies/target crash/fixed pump batches/duplicates/capacity/errors/recovery. Submit/Complete allocation faults verify leases do not transfer before control queue acceptance. Recorded 2026-10-14 on Windows x64/MSVC 19.51/CPython 3.13:

tests 包含真實 C++ 跨程序大訊息、Python↔C++、schema mismatch、應用失敗回覆、QueueFull、
timeout／late reply、目標 crash、固定 pump batch、重複通知、容量／錯誤協定與故障回收。
recovery 亦注入控制 payload 配置失敗，確認 Submit／Complete 未成功入列時不提早轉移 lease。
2026-10-14，Windows x64／MSVC 19.51／CPython 3.13：

- Full Debug/Release builds, SDK single-header and C++ examples passed.
- Release full **85/85**; Debug initially **84/85** because concurrent tests launched the same Vinc name and hit exclusivity. Isolated examples.user_module passed; exclusivity was unchanged.
- After final allocation fix, six RPC suites plus Manager faults/example passed **8/8** in both.
- Recovery injection checked reclaiming untransferred Submit/Complete leases.
- Real Shell examples produced expected C++→Python, Python→C++, C++ self-call replies.
- Logs: build/rpc-{debug, release}-tests.log and build/rpc-{debug, release}-final-tests.log.

- Debug／Release 完整建置通過，包含 SDK 單標頭與 C++ 範例。
- 全專案 Release 85/85；Debug 初輪 84/85，唯一失敗為兩組並行測試啟動同名 Vinc，
  觸發既有排他鎖。單獨重跑 examples.user_module 通過，未修改排他規則。
- 最後的配置失敗修正後，RPC 六套＋ModuleManager 故障＋使用範例，兩組態各 8/8 通過。
- RPC recovery 注入 Submit／Complete payload allocation failure，檢查未交接的 lease 可回收。
- 實際從 Shell 啟動 examples/rpc，C++→Python、Python→C++、C++ self-call 均收到預期回覆。
- 驗證紀錄：build/rpc-debug-tests.log、build/rpc-release-tests.log、
  build/rpc-debug-final-tests.log、build/rpc-release-final-tests.log。

<a id="待處理事項"></a>

## Outstanding issues / 待處理事項

- Native Linux/macOS validation remains.
- Browser bridge/authorization/typed UI APIs remain unwired.
- No latency/throughput measurement; mapping-cache value needs evidence.
- Registration timeout can leave a central service without a locally obtained handler identity. No automatic retry/rollback; treat it as initialization failure and recover through Module exit, not assume same-name retries succeed.

- Linux／macOS 尚未執行本輪 RPC 建置與實機測試。
- 瀏覽器 UI bridge、權限與 typed UI API 尚未接線。
- 未量測 RPC 延遲／吞吐量；mapping cache 是否值得增加，待量測。
- 登記逾時可能已在中央建立服務，本地未取得 handler 身分；不自動重試／回滾。
  應視登記失敗為初始化失敗，交由 Module 停止／退出回收，不保證同名重試成功。

Allocation injection covers payload allocations that can throw to callers. MSVC Debug vector default construction is noexcept yet allocates iterator metadata; failure terminates beyond outer catch. Existing STL/allocator policy remains, without a universal OOM-survival promise for central.

配置失敗測試只涵蓋可拋回呼叫端的 payload allocation。MSVC Debug 的 vector 預設建構子
宣告 noexcept，仍會為 iterator proxy 配置記憶體；該處配置失敗會終止程序，無法由外層 catch
復原。本輪維持既有 STL／allocator 例外政策，沒有承諾全域 OOM 仍可保住中央。
