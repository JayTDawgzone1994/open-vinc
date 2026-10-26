<a id="共用-socket-控制通訊"></a>

# Shared Socket control channel / 共用 Socket 控制通訊

`vinc::control` is shared by central and child runtimes and links OSAL. It depends on no BaseModule, Shell, Bus, or restart policy, and does not use Pipe.

`vinc::control` 是中央與子程序共用的通訊元件，連結 `vinc::osal`。
不依賴 `BaseModule`、Shell、Message Bus 或程序重啟政策，也不使用 Pipe。

<a id="建置待處理事項"></a>

## Resolved build issue / 已解決建置問題

**Reproduced and fixed, 2026-10-06:** an untyped add_library inherited BUILD_SHARED_LIBS=ON, becoming a Windows DLL without exports while other runtimes remained static. An isolated Release build with tests/apps off failed vinc_module_example linkage with LNK1181/missing vinc_control.lib (build/cmake-review-shared-build.log). Explicit STATIC fixed the same case; Debug control.channel/control.faults passed **2/2**. That particular test preceded Host/Plugin migration and did not verify Linux/macOS; production ownership is described below.

- **已重現（2026-10-06 CMake 重審）：** `CMakeLists.txt:3` 的 `add_library(vinc_control ...)`
  未指定類型。父專案設定 `BUILD_SHARED_LIBS=ON` 時，本元件變成 DLL，其餘主要 runtime
  仍為 STATIC；現有介面沒有 Windows DLL 匯出設定。以獨立目錄配置 Release、關閉測試與
  中央 app，再建置 `vinc_module_example`，在最終連結得到 LNK1181：缺少 `vinc_control.lib`。
  紀錄位於 `build/cmake-review-shared-build.log`。
  **已修正：** 明確採 STATIC，不受父專案選項影響。原 Release／BUILD_SHARED_LIBS=ON
  的 `vinc_module_example` 重現案例已成功連結；Debug 的 `control.channel` 與
  `control.faults` 各通過（2/2）。上述歷史案例發生在 ModuleHost／Plugin 遷移之前；目前接線見下方 Plugin 建置邊界。該案例未驗證 Linux／macOS。

<a id="功能範圍"></a>

## Scope / 功能範圍

Service accepts already-connected OSAL stream sockets; Modules currently use local stream endpoints. Framework-to-framework connection, authentication, and routing remain unimplemented.

`Service` 可接收已連接的 OSAL 串流 Socket；目前 Module 使用本機串流端點。
將來框架對框架的連線、驗證與路由尚未實作。

Deadlines use `<vinc/time.hpp>` and `vinc::Deadline`, with no separate alias here. control::Clock remains steady_clock; timing/heartbeat behavior is unchanged. Deadlines are local wait limits and are never serialized directly.

等待期限共用 `<vinc/time.hpp>` 的 `vinc::Deadline`；本元件不再另定義 Deadline。
既有 `control::Clock` 仍是 `std::chrono::steady_clock`，原有等待與 heartbeat 時序不變。
Deadline 僅用於本地等待，不直接寫入控制通道封包。

<a id="責任與執行緒"></a>

## Responsibilities and threads / 責任與執行緒

- Each Service has one I/O thread and SocketPoller shared by all Sessions.
- Session owns message send/receive, handshake, heartbeat, failures, and closure state.
- No user callback runs on the I/O thread. Public Session calls may be concurrent; object destruction needs external synchronization.
- Socket ownership transfers to Service; only its I/O thread sends/receives, registers/unregisters, and closes.
- Service destruction closes Sessions and joins I/O, without waiting for application delivery.
- Upper ModuleManager/runtime owns process termination/restart.

- 每個 `Service` 使用一個 I/O 執行緒與一個 OSAL `SocketPoller`，共用於其所有會話。
- `Session` 提供具所有權的訊息收發、握手、heartbeat、錯誤與關閉狀態。
- 使用者 callback 不在 I/O 執行緒執行。Session 的公開操作可並行呼叫；物件本身的析構需要外部同步。
- Socket 所有權移交給 Service，只有 I/O 執行緒收發、註冊、解除註冊及關閉。
- Service 析構會關閉會話並 join I/O 執行緒；不等待應用訊息完成交付。
- 程序終止與重啟由上層 ModuleManager／模組執行環境處理。

<a id="程式碼閱讀順序"></a>

### Implementation reading order / 程式碼閱讀順序

- protocol.hpp/cpp: v1 fields, encoding, incremental stream decoding, argument strings.
- channel.hpp: Service/Session operations, waiting, lifetime.
- channel.cpp: create_engine prepares poller/state; start_worker runs after Service ownership. prepare_peer builds state before admission/handoff.
- advance receives frames, checks deadlines, sends frames, then updates poller interest. Helpers share one peer lock rather than releasing it between stages.
- Engine mutex protects sessions/stopping; Peer mutex protects one session/queues. If both are needed, acquire Engine before Peer.

- `protocol.hpp`／`protocol.cpp`：v1 封包欄位、編碼、串流分段解碼及參數字串。
- `channel.hpp`：Service／Session 的公開操作、等待與生命週期契約。
- `channel.cpp`：`create_engine()` 準備 poller 與狀態，Service 取得所有權後才由
  `start_worker()` 啟動執行緒；`prepare_peer()` 準備會話，通過容量檢查後才交付 I/O 執行緒。
- I/O 執行緒透過 `advance()` 依序接收（`receive_frames()`）、檢查期限
  （`check_session_deadlines()`）、傳送（`send_frames()`），最後更新 poller 關注事件。
  這些階段共用同一次 peer lock，不在 helper 邊界釋放鎖。
- Engine mutex 保護會話清單與停止狀態；Peer mutex 保護單一會話狀態與佇列。
  需要同時持有兩者時，先取得 Engine mutex，再取得 Peer mutex。

Public failures use common Result/Error. `<vinc/control/error.hpp>` owns ErrorDomain::ControlChannel enums; a separate provider owns static text. OSAL failures are wrapped with bounded causes. Timeout is a wait deadline; HandshakeTimeout/HeartbeatTimeout close Sessions. encode/encode_arguments/put_string return Result: PayloadTooLarge for limits, Protocol for malformed data, not length_error for expected failure. Ordinary allocations can still throw. Unexpected I/O-worker exceptions close with WorkerFailed, not ordinary Close.

公開失敗介面已統一為 `vinc::Result<T>`／`vinc::Error`。`<vinc/control/error.hpp>`
維護 `ErrorDomain::ControlChannel` 的 enum；獨立 metadata provider 提供靜態文字表。
OSAL 失敗經 `wrap_error()` 轉成本層分類並保留原因鏈。
`Timeout` 是等待期限；`HandshakeTimeout`／`HeartbeatTimeout` 是會關閉 session 的失聯故障。
`encode()`、`encode_arguments()`、`put_string()` 也回傳 Result；超限回報 `PayloadTooLarge`，
格式錯誤回報 `Protocol`，不以 length_error 表示預期失敗。一般配置仍可能拋例外。
I/O worker 邊界的非預期例外以 `WorkerFailed` 關閉 session；不再被偽裝為一般 Close。

<a id="v1-封包"></a>

## v1 frame / v1 封包

Integers are big-endian; never transmit C++ structs, pointers, or ABI objects directly.

所有整數採 big endian，不直接傳輸 C++ struct、pointer 或 ABI 物件。

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic VINC |
| 4 | 2 | Frame version 1 |
| 6 | 2 | Kind: Hello=1, Heartbeat=2, Request=3, Response=4, Event=5 |
| 8 | 2 | Domain: Session=0, Lifecycle=1, Command=2, MessageBus=3, Param=4, Log=5; RPC owns application domain 6 |
| 10 | 2 | Operation defined by domain |
| 12 | 8 | Request ID: nonzero for requests/responses, zero for events |
| 20 | 4 | Payload length, maximum 16 KiB |

| Offset | 長度 | 欄位 |
| --- | --- | --- |
| 0 | 4 | Magic `VINC` |
| 4 | 2 | 協定版本，v1 固定為 1 |
| 6 | 2 | Kind：Hello=1、Heartbeat=2、Request=3、Response=4、Event=5 |
| 8 | 2 | Domain：Session=0、Lifecycle=1、Command=2、Message Bus=3、Param=4、Log=5；RPC 自行定義應用 domain 6 |
| 10 | 2 | Operation，依 Domain 定義 |
| 12 | 8 | Request ID；請求與回覆非零，事件為零 |
| 20 | 4 | Payload bytes，上限 16 KiB |

Length-prefixed strings use u32 byte length plus UTF-8 bytes. Arguments use u32 count plus strings, at most 256. The generic channel treats application payload as bytes; each feature validates content/string semantics. Decoder handles partial headers/payloads and concatenated frames, checking size before payload allocation.

長度前綴字串使用 u32 byte length + UTF-8 bytes。參數為 u32 count + 字串序列，上限 256 個。
通訊核心把應用 payload 視為 bytes，具體功能負責其內容與字串語意驗證。
Decoder 處理部分標頭、部分 payload 及多則連續訊息，在配置 payload 前檢查大小。

Hello contains role(u32), generation(u64), token(string), heartbeat interval(u32 ms), timeout(u32 ms), Hello version(u32=2), and expiry-enabled(u32 0/1). Roles must be opposite and other fields identical. Missing/unknown versions, invalid flags, or supervision mismatch are rejected. Hello v2 is inside frame v1; old Hello is incompatible and central/Host must rebuild together. Current applications include Lifecycle, Command, [Bus control](../message_bus/DESIGN.md), Param, Log, and RPC. Payload transport remains component-owned; the generic channel does not decode those features. Evolve protocols through explicit versions/negotiation, never by silently changing v1 semantics.

Hello payload 依序為 role(u32)、generation(u64)、token(string)、heartbeat interval(u32 ms)、
heartbeat timeout(u32 ms)、Hello version(u32，固定 2)、heartbeat expiry enabled(u32，0 或 1)。
雙方必須角色相反，其他欄位完全相符；缺少版本、未知版本／旗標及監督政策不一致均拒絕。
此為 Hello v2，外層 frame 保留 v1；不接受舊 Hello payload。所有中央／Host 必須一起重建。
目前應用亦包含 Param、Log 與 RPC；Module 支援 Lifecycle、Command 與 [Message Bus 註冊／拓樸協定](../message_bus/DESIGN.md)。
Message Bus payload 留在 shared memory；共用控制層仍不解讀上層 Message Bus 內容。
後續協定演進需明確增加版本或協商欄位，不能直接修改既有 v1 欄位語意。

Heartbeat payload is a strictly increasing u64 sequence starting at 1. Only valid heartbeats refresh liveness, not business traffic. Local steady_clock avoids clock synchronization. Heartbeat indicates communication-runtime responsiveness, not complete application health.

Heartbeat payload 是從 1 開始、嚴格遞增的 u64 序號。只有有效 heartbeat 更新失聯期限，
一般業務訊息不代替 heartbeat。使用本機 steady clock，不依賴兩端時鐘同步。
heartbeat 只表示通訊執行環境有回應，不表示所有應用功能正常。

Options::heartbeat_timeout_enabled defaults true and is fixed/agreed at Session creation. False disables only expiry checks/deadlines; sending/validation, EOF, malformed-data checks, bounded queues, and handshake deadlines remain. This layer neither detects debuggers nor selects Module policy.

`Options::heartbeat_timeout_enabled` 預設 true，建立 Session 時固定，雙方須明確同意。
false 只停止 heartbeat 到期檢查與相關 poll deadline，仍發送／驗證 heartbeat、處理 EOF、
格式錯誤與有界佇列；handshake 期限不變。此層不偵測 debugger，也不決定 Module 政策。

<a id="容量排程與交付"></a>

## Capacity, scheduling, and delivery / 容量、排程與交付

- Each direction defaults to 64 application frames/256 KiB including headers and active partial sends.
- Full send returns Backpressure immediately. Full receive defaults to closing with Backpressure; the explicit local DropNewest policy is described below.
- Heartbeat has reserved space and priority at the next frame boundary, without unbounded overdue-heartbeat accumulation.
- Per-peer work is bounded per pass. A partial frame and bytes already in OS buffers cannot be overtaken; no hard real-time heartbeat under congestion is promised.
- Successful send means local acceptance, not remote delivery/execution.
- If send throws bad_alloc, encoding/deque allocation has not committed the frame. Post-commit counts/wake allocate nothing, so callers may remove unsent pending records. Exceptions while waiting after successful queueing do not have that guarantee.
- Applications allocate/match request IDs; no automatic resend/reconnect.
- Closed Sessions cannot reopen; each process start uses fresh generation/token.
- Unknown application domains may pass through; receiving features decide unsupported/rejection.

- 每個方向的應用佇列上限 64 則／256 KiB（含標頭）；傳送容量包含正在部分寫入的封包。
- 傳送滿載立即回報 Backpressure；接收佇列滿載預設關閉會話並保留 Backpressure 原因；明確選用的 DropNewest 政策見下方。
- heartbeat 使用獨立保留空間，在下一個封包邊界優先發送，不無限堆積過期 heartbeat。
- 每次迴圈限制各 peer 的收發工作量。已部分寫入的封包不能插入 heartbeat，
  OS socket buffer 裡的資料也不能被超越；此方案不提供壅塞下的硬即時 heartbeat 保證。
- `send()` 成功只表示接受進本機佇列，不代表對端收到或執行。
- `send()` 內部配置失敗拋出 `std::bad_alloc` 時，此 frame 尚未排入佇列；
  編碼與 deque 配置都在提交前完成，提交後的計數更新與 wake 不配置。
  上層可據此撤銷「未送出」的 pending 紀錄；成功排入後的等待例外不具有這項保證。
- Request ID 的配置與配對屬於使用此元件的功能協定；Session 不自動重送或重新連線。
- 會話關閉後不能重用。不同程序啟動使用新的 generation 與 token。
- 未知 application Domain 可由共用層傳送；接收功能決定回報 unsupported 或拒絕。

<a id="安全與限制"></a>

## Security and limits / 安全與限制

Launch tokens bind one process startup and avoid stale-session confusion, not malicious same-account processes. Central currently passes them through process arguments. No TLS, remote authentication, or secret storage is provided; do not expose this channel directly to networks. ModuleManager owns local Socket directories, owner-only on POSIX.

Module 的啟動 token 用來綁定本次程序啟動，避免誤接舊會話；不是針對同帳號惡意程序的安全沙箱。
目前 token 由中央透過程序參數傳入。Session 不提供 TLS、遠端認證或機密儲存，不應將此通道直接公開到網路。
本機 Socket 目錄由 ModuleManager 建立與回收；POSIX 目錄設為 owner-only。

control.channel covers segmentation/coalescing, version/length rejection, bidirectional heartbeat, bad handshakes, disconnect/backpressure, and heartbeat timeout with a live Socket. Fixed v1 bytes check compatibility, NUL/UTF-8 strings, truncated/trailing data, wait timeout, admission, and Session-outliving-Service closure.

`control.channel` 測試封包分割／合併、版本及長度拒絕、雙向 heartbeat、錯誤握手、
Socket 斷線、佇列背壓與連線仍存活時的 heartbeat 逾時。
另以固定 v1 bytes 驗證封包相容性，涵蓋字串內嵌 NUL／UTF-8、截斷與多餘資料拒絕，
以及等待逾時、會話容量拒絕和 Session 比 Service 長壽時的關閉行為。

Recorded 2026-10-04 Module error integration: Windows x64 full Debug/Release builds and **33/33** CTests each, including fixed bytes and real Sockets. No Linux/macOS validation in that round.

2026-10-04 ModuleSystem 錯誤遷移整合驗證：Windows x64 Debug／Release 完整建置成功，
各 33/33 套 CTest 通過，包含上述 `control.channel` 固定 v1 bytes 與真實 Socket 驗證。
Linux／macOS 未於本輪驗證。

<a id="2026-10-05-重審修正容量邊界與故障診斷"></a>

## Capacity/failure review fixes (2026-10-05) / 2026-10-05 重審修正：容量邊界與故障診斷

Three confirmed findings were fixed with regressions:

本輪重審的三個問題均已修正，並補齊對應回歸測試：

- Exact 64-frame admission counts active partial application frames; the complete frame consumes bytes until the final byte sends. Hello/heartbeat do not consume application budgets; priority remains.
- Send retains outer Closed/Send and wraps existing heartbeat/protocol/I/O/OSAL causes without changing the saved failure. Repeated send does not grow layers. Incomplete handshake, explicit close, or normal Service destruction return one Closed/Send layer; peer EOF Closed/Receive remains a cause. Validation order remains.
- Worker saves Poller failure, sets stopping under Engine lock, and cleans the current session list, including attachments after the prior snapshot. Earlier Session failures win. Attach after stop returns Closed/Attach; unexpected exceptions remain WorkerFailed/Close, without new catch/reconnect policy.

- **精確限制 64 則：** `Session::send()` 將尚未送完的 active application frame
  計入則數；256 KiB byte 預算仍計整筆封包，直到最後一個 byte 送出才釋放。
  Hello／heartbeat 不占用應用則數與 byte 預算，既有優先排程不變。
- **保留 send 失敗原因：** 外層維持 `Closed／Send`；若會話已有 heartbeat、協定或 I/O
  故障，使用 `wrap_error()` 保留原故障與 OSAL／native 診斷。此操作不改寫保存的原故障，
  重複 send 不增加原因層數。未完成握手、明確 `close()` 或 Service 正常析構仍回單層
  `Closed／Send`；對端 EOF 的 `Closed／Receive` 則保留為原因。輸入驗證順序不變。
- **統一套用 worker 終止原因：** `run()` 保存 Poller 故障，於 Engine lock 下先設定
  stopping，再對目前清單的所有會話清理，涵蓋最後一次快照之後加入的會話。
  各會話原本已有的主要故障／關閉原因仍優先保留；新 attach 在停止後仍回 Closed／Attach。
  worker 非預期例外仍為 WorkerFailed／Close，未修改 catch 邊界或引入重連。

<a id="驗證方式"></a>

### Validation method / 驗證方式

- Real-library/Socket channel tests add handshake timeout, zero/repeated/decreasing heartbeat sequences, receive count/byte exhaustion, draining received frames after close, and send failure/normal-close semantics. Healthy sessions in the same Service remain usable after a malformed peer closes.
- control.faults compiles the same sources into a test-only target with VINC_CONTROL_CHANNEL_TESTING. channel_test_hooks intercepts send/wait; production control has no test state/API/branch. Shared fixtures add no public API.
- Partial-send tests actually send one byte, inject WouldBlock, and pause. Frame 64 and exactly 256 KiB are accepted; frame 65/one extra frame rejected. Resumed transmission checks order/data/budget release.
- Poller tests pause after snapshot, attach a new Session, then inject synthetic OSAL/native failure. Old/new sessions retain Io/Poll causes; explicit prior closure is not overwritten. Worker exception injection checks WorkerFailed and attach rejection. This is not real OS exhaustion or a proof over all races.
- Test deadlines and stop wakeups bound injected waits; CTest also has a total timeout.

- `control.channel` 連結正式 library，使用真實 Socket。除原有 v1 固定 bytes、分段／合併、
  雙向 heartbeat、等待與斷線案例外，新增握手逾時、零／重複／倒退 heartbeat 序號、
  接收則數與 byte 滿載、關閉後依序排空已接收訊息，以及原故障／正常關閉的 send 語意。
  畸形會話關閉後，同 Service 的健康會話仍能傳送應用訊息。
- `control.faults` 以同一份 channel／protocol 原始碼建立獨立測試程式；編譯期
  `VINC_CONTROL_CHANNEL_TESTING` 僅供此 target 使用。`tests/channel_test_hooks.hpp`
  攔截 send／wait 邊界，正式 `vinc::control` 不包含此入口、狀態或執行期分支。
  `channel_test_support.hpp` 共用 Socket fixture 與斷言，沒有增加公開 API。
- 部分傳送案例先由真實 Socket 送出第一個 byte，再注入 WouldBlock 並暫停 worker。
  驗證包含 active frame 的第 64 則可接受、第 65 則拒絕、精確 256 KiB 可接受、
  再加一筆拒絕；恢復傳送後核對資料／順序，以及則數／byte 容量正常釋放。
- Poller 案例在 worker 已取得會話快照後暫停，建立新會話，再注入帶 native 診斷的
  合成 OSAL 錯誤。新舊會話皆保留相同 Io／Poll 原因鏈；先前已明確 close 的會話不被覆蓋。
  另注入 worker 例外，驗證既有 WorkerFailed 政策及停止後拒絕 attach。
  注入不是實際 OS 資源耗盡，也不是所有排程的競爭偵測證明。
- 注入等待受測試期限限制且可由 stop 喚醒；CTest 另有總期限。

Public signatures, frame v1, identifiers, deadlines, and allocation-exception policy were unchanged. Linux/macOS require native tests; Windows success does not prove cross-platform verification.

公開 API 簽章、Socket v1、既有錯誤碼、等待期限與配置例外政策未變。
Linux／macOS 仍需原生建置與執行，不能由 Windows 通過推論跨平台已驗證。

<a id="本輪驗證結果"></a>

### Recorded review results / 本輪驗證結果

Windows x64/MSVC 19.51.36257, SDK 10.0.26100.0, CMake 4.3.2, Ninja 1.13.2: related Debug/Release rebuilds had no compiler warnings; channel/faults, Bus error_process/module, and Module process passed **5/5** each (25.23 s / 20.77 s). Build commands confirmed injection define only on control_channel_fault_tests. Other suites and Linux/macOS were not run.

Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
Debug／Release 相關 target 重新建置通過，建置輸出未出現編譯警告。
`control.channel`、`control.faults`、`message_bus.error_process`、`message_bus.module`、
`module.process` 各 **5/5** 通過（Debug 25.23 秒、Release 20.77 秒）。
已核對兩種組態的建置指令，測試注入 define 僅出現在 `control_channel_fault_tests`。
未重跑全專案其餘測試；Linux／macOS 未建置或執行。

<a id="plugin-建置邊界2026-10-06"></a>

## Plugin build boundary (2026-10-06) / Plugin 建置邊界（2026-10-06）

Static vinc::control_protocol contains only protocol.cpp codecs and Common, with no session state, Socket, or worker. Static vinc::control provides channel.cpp plus protocol/OSAL. Host and central own control; default Plugins link only protocol. Frame bytes, queues, heartbeat, waiting, and faults remain unchanged.

`vinc::control_protocol` 僅靜態提供 protocol.cpp 的編解碼，依賴 Common，沒有服務狀態、
Socket 或 worker。`vinc::control` 靜態提供 channel.cpp 並相依 protocol／OSAL。
ModuleHost 與中央連入 control；Plugin 的預設連結集合只帶入 control_protocol。
Socket v1 bytes、佇列、heartbeat、等待與故障語意不變。

Recorded Hello-v2 validation (2026-10-06): Windows Debug/Release control.channel passed old/unknown/malformed/mismatched Hello rejection, disabled-expiry silence/recovery, real EOF, and normal heartbeat. Full scope is in [Module debugging validation](../module_system/README.md#除錯支援驗證).

Hello v2 監督政策驗證（2026-10-06）：Windows Debug／Release 的 `control.channel` 通過，
含舊 payload／未知版本／非法旗標／雙方政策不一致拒絕，停用 expiry 後的靜默及恢復、
真實 EOF 與既有正常 heartbeat。全套驗證見 [ModuleSystem](../module_system/README.md#除錯支援驗證)。

<a id="log-專用局部佇列政策2026-10-08"></a>

## Local queue policies for Log (2026-10-08) / Log 專用局部佇列政策（2026-10-08）

Options::receive_overflow defaults CloseSession; Log explicitly chooses DropNewest. Session::queue_stats retains cumulative dropped frames/bytes outside the application queue. reserved_send_frames/bytes default zero; normal send preserves reserves, while send(frame, true) may consume them. Existing FIFO/Hello/heartbeat remain. Policies are local, not wire fields; invalid enums/reserves are rejected. See [Log](../log/README.md).

Options::receive_overflow 預設 CloseSession；Log 明確選用 DropNewest。
Session::queue_stats() 保存接收丟棄 frames／bytes 累計，不經應用 frame 排隊。
reserved_send_frames/bytes 預設 0，普通 send 保留此容量，send(frame, true) 可使用。
不重排已有 frames，不改 Hello／heartbeat；政策是本地選項，不是 wire 欄位。
非法 enum／預留容量被拒絕。見 [Log](../log/README.md)。
