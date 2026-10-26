<a id="outputhub中央輸出管理層"></a>

# OutputHub: central output management / OutputHub：中央輸出管理層

`vinc::output_hub` captures child stdout/stderr into independent bounded subscriber queues. It depends on OSAL/Common defaults, not Module, Shell, Tcl, or Bus. ModuleManager supplies name/startup generation; Shell consumes and displays events.

`vinc::output_hub` 接收子程序 stdout／stderr，提供獨立、有界的接收佇列。
本庫依賴 OSAL 與 Common（提供設定預設值），不依賴 Module、Shell、Tcl 或 message bus；ModuleManager
負責傳入模組名稱與啟動 generation，Shell 負責取出資料並顯示。

```text
任意子程序 stdout／stderr
  → OSAL PipeService（非同步讀取）
  → OutputHub（共用收集執行緒）
  → 每個目的地自己的 Subscription
  → 本地 Shell／Socket Shell
```

<a id="使用與所有權"></a>

## Usage and ownership / 使用與所有權

Include `<vinc/output_hub/output_hub.hpp>`, link alias `vinc::output_hub` (library vinc_output_hub).

標頭：`<vinc/output_hub/output_hub.hpp>`，連結入口為 CMake alias `vinc::output_hub`，
實際 library target 為 `vinc_output_hub`。

- subscribe creates a receiver; subscribe before spawn to receive initialization output.
- spawn(processes, options, name, generation) reserves sources/buffers before OSAL launch, returning CapturedProcess{process, capture}. It always captures both stdout/stderr, overriding those options; stdin remains unchanged.
- Caller observes Process exit, then calls exited(capture) **after OS-confirmed exit** to start tail-drain timing. Manager already integrates this.
- wait_until waits for current sources, neither stopping processes nor closing admission. Stop new launches first. Success means reads ended, potentially with recorded failure/truncation, not that every destination displayed them.
- service_error returns the first PipeService infrastructure error even without active captures or if its Failed event dropped. Historical failure does not change drain-success semantics; later spawn returns SpawnFailed, without auto-recreation.
- set_name retains ID/generation; Renamed ends unfinished text under the old name before new events. Published identities remain immutable.
- Destruction stops collection/closes Readers/asks OSAL to clean up, not terminate processes. Closing Readers can cause broken pipes/SIGPIPE; normally stop/drain children first.
- Subscription take(max_events) transfers shared immutable events and loss counts. Hub invokes no user callback and performs no terminal/file/network output.
- set_enabled(false) clears queued display data and stops enqueueing, while Pipes keep draining. Reenable gets only new data, with discontinuity markers resetting parsers.
- Subscription close/destruction detaches it; with no subscribers, Hub still drains/discards bytes.

- `OutputHub::subscribe()`：建立接收者；須在啟動子程序前訂閱，才能收到初始化輸出。
- `spawn(processes, options, name, generation)`：先預留來源與讀取 buffer，再透過
  OSAL 啟動並擷取 stdout／stderr，回傳 `CapturedProcess{process, capture}`。
  此框架介面固定擷取兩路，覆蓋 options 的 stdout／stderr 模式；stdin 保持原設定。
- 呼叫端持有 Process 並監看退出。**確認 OS 退出後**呼叫 `exited(capture)`，啟動
  輸出收尾期限。ModuleManager 已完成這項接線。
- `wait_until(deadline)`：等待目前來源完成，不自動停止或終止程序，不關閉啟動入口。
  呼叫端應先停止新啟動。成功代表讀取已結束，可能包含有明確紀錄的截斷或失敗；
  不代表所有目的地已顯示資料。
- `service_error()`：查詢第一筆 PipeService 基礎設施故障，回傳 `optional<Error>`。
  即使沒有 active capture 或 Failed 事件已丟棄，診斷仍保留。歷史故障不改變
  `wait_until()` 的收尾成功語意；失效後的新 spawn 回報 `SpawnFailed`，不自動重建服務。
- `set_name(capture, name)`：保留來源 ID／generation，更新後續事件的名稱；`Renamed` 事件讓文字目的地先結束舊名稱下的未完成輸出列，已發布事件的來源資料不變。
- Hub 解構會停止收集、關閉 readers，並要求 OSAL 完成清理。它不會要求 OS 殺程序，
  但關閉 reader 可能令寫入者遇到斷管／SIGPIPE。因此正常流程應先停止程序並收尾。
- `Subscription::take(max_events)`：取走一批共享、不可變事件與略過統計。
  Hub 不會呼叫使用者 callback，也不會執行終端、檔案或網路 I/O。
- `set_enabled(false)`：清除該接收者的待顯示資料，之後不再排入資料；Pipe 仍持續讀取。
  重新開啟只接收新資料，不重播歷史。切換會報告資料不連續，讓文字解析器重設狀態。
- `close()` 或解構 subscription：解除該接收者。沒有接收者仍繼續讀取並釋放資料。

Before name approval, construction/naming output is labeled starting:<id>; afterward it uses the approved instance name.

Module 命名核准前以 `starting:<id>` 標示建構／命名階段輸出；核准後改用實例名稱。

Ordinary Hub calls can be concurrent; take/set_enabled synchronize internally. Synchronize Hub/Subscription move/close/destruction and ProcessService lifetime against callers. Returned Batches belong to caller memory budgets, not queued Hub budgets.

Hub 的一般操作可並行；subscription 的 take／set_enabled 由內部同步。
Hub／subscription 的移動、close 或解構，以及 ProcessService 的生命週期，
須由呼叫端與使用者同步。已取出的 Batch 屬呼叫端記憶體責任，不計入 Hub 佇列預算。

<a id="資料與身分"></a>

## Events and identity / 資料與身分

Each Event retains immutable Source (ID, name, generation, stdout/stderr, PID), central receive sequence, monotonic and wall receive times, raw bytes, and kind:

每份 Event 帶不可變 Source（ID、名稱、generation、stdout／stderr、PID）、中央接收序號、單調與日曆接收時間、原始 bytes 及事件種類：

| Kind | Meaning |
| --- | --- |
| Data | Raw bytes without line/encoding guarantees |
| End | Pipe drained to EOF |
| Truncated | Drain deadline expired; complete drain unconfirmed |
| Failed | I/O submission/completion failure with available OS error |
| Renamed | End old-name text fragments; future events use the new name |

| 種類 | 意義 |
| --- | --- |
| `Data` | 已讀取的位元組，沒有行或文字編碼保證 |
| `End` | Pipe 已排空且 EOF |
| `Truncated` | 收尾期限到期，不能確認輸出已完整排空 |
| `Failed` | I/O 提交或完成失敗，保留可用的 OS 錯誤 |
| `Renamed` | 結束舊名稱下的文字片段，後續事件使用新名稱 |

Receive time is not production time. Two streams have no total original-write order; stderr is not automatically a severity. Source IDs are not reused in one Hub; restart gets a new identity and old output retains its generation.

接收時間不是模組產生文字的時間；兩條串流之間不承諾實際寫入的總排序。
stderr 也不自動等於某種 log 等級。來源 ID 在同一 Hub 中不重用，重新啟動取得
新來源身分；舊程序的輸出保留原 generation，即使新程序已開始執行。

<a id="容量排程與遺失政策"></a>

## Capacity, scheduling, and loss / 容量、排程與遺失政策

Limits/QueueLimits can override defaults:

預設值可透過 `Limits`／`QueueLimits` 配置：

Compile-time output_hub_* defaults in `<vinc/config.hpp>` cover admission, queues, read batches, waits, and rendering. Limits/QueueLimits retain caller configuration. Fixed stream indices, source-ID mapping, and UTF-8 rules stay here.

預設容量、准入上限、讀取批次、等待期限與文字顯示設定集中於
`<vinc/config.hpp>` 的 `output_hub_*` 常數；目前為編譯期設定。
`Limits`／`QueueLimits` 保留原有呼叫端配置能力。固定的 stdout／stderr 索引、
來源 ID 對應方式與 UTF-8 編碼規則仍由本元件維護。

| Item | Default |
| --- | --- |
| Concurrent captures including draining generations | 128 |
| Subscribers | 8 |
| Per queue | 256 KiB / 128 events |
| Total queues | 2 MiB / 1024 events |
| Per-stream read block | 4 KiB |
| Post-exit drain timeout | 2 s |

| 項目 | 預設 |
| --- | --- |
| 同時擷取程序數（含尚未收尾的舊世代） | 128 |
| 接收者數量 | 8 |
| 每個接收佇列 | 256 KiB、128 events |
| 所有接收佇列總預算 | 2 MiB、1024 events |
| 每條串流讀取區塊 | 4 KiB |
| 退出後收尾期限 | 2 秒 |

Queues check bytes/count together, dropping the oldest events for that receiver. An oversized single event clears prior queued data and is skipped. Independent lost-byte/event counters survive overflow and return with the next take. Intentional disable does not count disabled bytes, but toggling marks discontinuity.

佇列同時檢查 bytes 與事件數；容量不足時丟棄該接收者最舊的待顯示事件。
單筆事件若大於接收者的 byte 上限，清除先前待顯示資料後略過該事件。
略過的 bytes／事件數獨立累計，下次 take 一次取回，不會隨佇列溢位一起丟失。
停用期間屬於主動不接收，不累計那些資料的 bytes；停用／啟用仍標記不連續。

Events are shared across independent receivers; queue budgets are conservatively reserved per queue. Event counts bound metadata too. Hub read-failure/truncation totals survive dropped notifications. Event-allocation failure clears every enabled receiver queue and counts both cleared/current events before continuing Pipe reads, positioning gaps before the next batch to avoid accidental concatenation, at the cost of discarding valid older output. Disabled receivers do not count it; clearing/counting allocate nothing.

同一份 Event 由多個接收者共享，每個目的地獨立前進。額度按各佇列容量保守保留；
資料事件與 metadata 同時受事件數上限約束。讀取失敗／收尾截斷另有 Hub 累計統計，
不依賴目的地是否還保留事件。事件配置失敗時清除所有啟用接收者各自的待取佇列，
將額外丟棄及本次失敗事件一併計入略過統計，繼續讀 Pipe。這讓缺口固定出現在
下一批資料之前，避免跨缺口拼接；代價是也會捨棄部分仍有效的舊輸出。
停用接收者不累計該事件；清除與統計本身不配置記憶體。

One Hub collector plus the shared OSAL Pipe worker handles all Modules, not one reader thread each. Each stream has one outstanding I/O; at most 32 completions/batch, with control checks about every 20 ms, not hard real-time. Local Shell takes at most 16 events/pass to preserve keyboard work.

Hub 使用一條收集執行緒，另有 OSAL 的共用 Pipe worker；不為每個模組新增讀取執行緒。
每條串流最多一筆 I/O，每批最多取 32 個完成事件；收集控制檢查間隔最多約 20ms，
不是硬即時保證。本地 Shell 每輪最多取 16 個 events，避免無限排空擠掉鍵盤處理。

Slow destinations do not make Hub wait, but finite Pipe capacity, OS scheduling, CPU saturation, or a slow collector can still block Module stdout writes. No end-to-end lossless or hard real-time guarantee is made.

慢速接收者不會讓 Hub 等待其輸出。但 Pipe 容量仍有限；OS 排程、CPU 滿載或收集端
跟不上時，模組 stdout 寫入仍可能阻塞。此版本不承諾端到端無損或硬即時輸出。

<a id="本地文字顯示"></a>

## Local text rendering / 本地文字顯示

TextRenderer belongs to the display side; Hub retains original bytes.

`TextRenderer` 位於顯示側；Hub 永遠保留原始 bytes。

- Prefixes are [name#generation stdout/stderr].
- UTF-8, newlines, and partial text are handled per source across chunks.
- Lines are limited to 4 KiB and split; unterminated fragments display after 100 ms.
- Incomplete UTF-8 waits for later fragments; deadline/EOF escapes remaining incomplete bytes.
- ESC, C0/C1, invalid bytes become visible \xNN; no ANSI execution. CRLF is newline; other CR is escaped, without terminal progress-bar behavior.
- At most 256 incomplete source states; on exhaustion display/release an old state.
- Gaps show loss counts and reset unfinished text rather than join unrelated fragments.

- 顯示 `[name#generation stdout]` 或 `[name#generation stderr]` 前綴。
- 按來源獨立處理跨區塊 UTF-8、換行與未完成文字。
- 行上限 4 KiB，超長行分段；未換行內容在 100ms 後顯示。
- 不完整 UTF-8 會等後續片段；若到顯示期限／EOF 仍不完整，則轉義顯示。
- ESC、其他 C0／C1 控制字元與無效 bytes 轉成可見 `\xNN`；不執行 ANSI 序列。
  CRLF 當換行，其他 CR 轉義，不提供終端進度條效果。
- 最多暫存 256 個來源的未完成行；滿時先顯示並釋放一筆舊來源狀態。
- 佇列發生資料缺口時，提示略過數量並重設未完成文字，避免把前後片段誤接。

Only each Shell thread writes its ostream: clear prompt, display batch, restore input. Commands/Tcl keep their original routing outside droppable Module queues. A blocked local ostream can stall that Shell, not Hub/process supervision.

只有 Shell 執行緒會寫自己的 ostream：清提示列、顯示一批資料、再還原輸入列。
命令回覆與 Tcl 輸出沿用原本路由，不經可丟棄的模組輸出佇列。
本地 ostream 自己若阻塞，該 Shell 仍可能暫停；Hub 與模組監看不等待它。

<a id="moduleshell-關閉"></a>

## Module and Shell shutdown / Module／Shell 關閉

Manager owns Hub independently of any Shell. Capture starts at launch before Ready; failed startup cleans that source. Restart need not await old-generation draining.

ModuleManager 擁有 Hub，生命週期不依附某個 Shell。模組在啟動時即開始擷取，
不用等 Ready；啟動失敗回收本次來源。重啟不必等待舊世代輸出排空。

Normal order: close admission → request stop → confirm process exit → Bus cleanup/bounded output drain → Shell final queues/ending. The 2 s bound applies only to output after exit, not to waiting for managed processes. Descendants retaining Writers cause Truncated/Reader close at deadline, never endless drain. Shell reports cumulative truncation/read failures even when notifications dropped.

正常關閉順序為：關閉啟動入口 → 要求模組停止 → 確認受管理程序退出 →
Message Bus 清理與有期限的輸出收尾 → Shell 取完剩餘佇列並顯示結尾。
2 秒期限只限制已退出程序的輸出收尾，不減少等待受管理程序退出的要求。
若孫程序仍持有 writer，到期後標記 Truncated、關閉讀取端，不永久等待。
Shell 關閉時另列出累計截斷／讀取失敗數，即使個別通知曾被佇列丟棄仍有摘要。

<a id="驗證範圍"></a>

## Validation scope / 驗證範圍

output_hub.integration covers large dual streams, slow subscribers, global budgets, mute/resume, no subscribers, launch failure, generations, drain timeouts, UTF-8/controls/long lines/gaps. A helper retaining a Writer simulates post-exit pipe state for timeout tests and is explicitly terminated/cleaned.

`output_hub.integration` 測試大型雙路資料、慢接收者、總額度、停用／恢復、無接收者、
啟動失敗、來源世代、收尾期限、UTF-8、控制字元、長行與資料缺口。
收尾期限單元測試以仍持有 writer 的 helper 模擬退出通知後的管道狀態，並自行終止清理。

output_hub.faults is a separately compiled test variant injecting event/factory allocation failures, monitor errors, and I/O completions; production has no hook. It checks loss accounting, no cross-gap joins, missing rename/failure events, infrastructure errors with no sources, causes, independent read errors, EOF priority, and eventual drain. These are deterministic boundary tests, not real OS monitor failures or every allocation site.

`output_hub.faults` 使用獨立編譯的測試版本，注入事件配置失敗、factory 邊界配置失敗、
monitor 診斷及 I/O 完成結果；正式 library 不包含注入入口。確認各接收者丟棄統計、
分批取出不跨缺口拼接、遺失改名事件、Failed 事件遺失仍保留計數、無來源時仍能查詢
服務故障、原因鏈、獨立讀取失敗不被覆蓋、EOF 優先及失敗後仍可完成收尾。
這是 Hub 邊界的確定性故障測試，不代表已觸發各 OS 的真實 monitor 故障或所有配置失敗點。

Real-Module shell.integration checks initialization/commands/crash/restart/on_stop tails; Windows hidden-console tests check partial-input redraw and continued use. Linux/macOS still require native validation. [Socket Shell](../shell/SOCKET.md) uses Hub; structured logging/file storage are now implemented separately in [Log](../log/README.md), not inside Hub.

`shell.integration` 使用真實模組驗證初始化、命令、崩潰／重啟與 on_stop 尾端資料；
`shell.console` 在 Windows 隱藏終端驗證背景輸出後的部分命令重繪與繼續執行。
Linux／macOS 尚待實機建置與執行。Socket Shell 已接上 OutputHub，見
[Socket Shell 文件](../shell/SOCKET.md)。結構化 Vinc Log 與檔案保存已由獨立 [Log 元件](../log/README.md) 實作，不屬於 Hub 本身。

<a id="程式閱讀順序"></a>

## Implementation reading order / 程式閱讀順序

- output_hub.hpp: events/sources/limits/subscriptions/capture contracts.
- output_hub.cpp: Queue drop/take policy, then Impl::run; advance_captures → wait → dispatch_completion. complete_reader delivers bytes, reacquires buffer, then decides EOF/truncation/failure.
- spawn allocates capture, registers under lock, launches outside lock, and adopts Readers under lock. register_capture/attach_readers/rollback_capture state ownership/cleanup.
- text_renderer.cpp: consume_display_text handles UTF-8/controls; render handles sources/lines/events; flush handles deadlines.
- Tests group fanout/mute, rollback/drain, no receivers, text, plus valid boundaries, overlong encodings, surrogates, out-of-range Unicode, C1, fragments, EOF.

- `output_hub.hpp`：事件、來源、容量限制、訂閱與擷取契約。
- `output_hub.cpp`：先看 `Queue` 的丟棄／取出政策，再看 `Impl::run()`。
  收集迴圈依序執行 `advance_captures()`、等待完成事件與 `dispatch_completion()`；
  `complete_reader()` 維持先交付 bytes、再取回 buffer、最後判斷 EOF／截斷／失敗的順序。
- `spawn()`：配置 capture、在鎖內登記來源、鎖外啟動程序、鎖內接管 readers。
  `register_capture()`、`attach_readers()` 與 `rollback_capture()` 分別註明所有權和回收責任。
- `text_renderer.cpp`：`consume_display_text()` 處理 UTF-8 與控制字元；
  `render()` 處理各來源的行與事件，`flush()` 處理顯示期限。
- `output_hub_tests.cpp`：依 fanout／mute、回滾／收尾、無訂閱者、文字顯示分組，
  額外驗證有效 UTF-8 邊界、過長編碼、代理碼點、超出 Unicode 範圍、C1、斷片與 EOF。

The readability pass preserved APIs/errors/exception boundaries/capacity/synchronization. UTF-8 expected values/workloads remain independent test data, not answers derived directly from config.

此次可讀性整理保留公開 API、錯誤型別、例外邊界、容量數值與同步政策。
UTF-8 測試期望值與測試負載留在測試，不直接引用系統設定作為答案。

<a id="outputhub-命名統一驗證2026-10-03"></a>

### OutputHub naming validation (2026-10-03) / OutputHub 命名統一驗證（2026-10-03）

Directory/namespace/targets use output_hub, main files output_hub.hpp/cpp, class OutputHub. Module/Shell/SDK/tests/docs migrated without old aliases. Eleven affected C++ files were compared; only names/includes changed.

目錄、命名空間與 target 統一為 `output_hub`，主標頭／實作使用
`output_hub.hpp`／`output_hub.cpp`；類別仍為 `OutputHub`。
Module、Shell、SDK、測試與文件引用已同步更新，不保留舊路徑或 target 別名。
本輪只改名；比對 11 個受影響 C++ 檔案，確認差異僅為名稱與引用路徑。

- Windows MSVC Debug: full build and **9/9** related tests.
- Release: related standalone targets/SDK headers and **7/7** tests.
- Socket tests passed in both; Console ran only in Debug for that round.
- No Linux/macOS verification.

- Windows x64 MSVC Debug：完整建置成功，OutputHub、Module、Shell、SDK 相關測試 9/9 通過。
- Windows x64 MSVC Release：相關獨立測試目標與 SDK 標頭編譯成功，相關測試 7/7 通過。
- Socket Shell 測試在上述兩種組態均通過；Console 測試本輪僅在 Debug 執行。
- Linux／macOS 本輪未驗證。

<a id="可讀性整理驗證2026-10-04"></a>

### Readability validation (2026-10-04) / 可讀性整理驗證（2026-10-04）

- Windows MSVC Debug full build/SDK headers, **9/9** related tests including Socket/Console (30.49 s).
- Release related standalone targets/SDK headers, **7/7** OutputHub/Module/Shell Socket/Tcl/Unit/Integration/SDK tests (26.43 s).
- New UTF-8 boundaries passed in output_hub.integration in both configurations.
- Existing diagnostics/display matched. No Linux/macOS run.

- Windows x64 MSVC Debug：完整建置與 SDK 標頭編譯成功；OutputHub、Module、
  Shell（含 Socket 與 Console）、SDK 相關測試 9/9 通過（30.49 秒）。
- Windows x64 MSVC Release：相關獨立測試目標與 SDK 標頭編譯成功；
  OutputHub、Module、Shell Socket／Tcl／Unit／Integration、SDK 測試 7/7 通過（26.43 秒）。
- 新增的 UTF-8 邊界案例包含於兩種組態的 `output_hub.integration`，均通過。
- 原有診斷／顯示字串比對一致；本輪未執行 Linux／macOS 驗證。

<a id="重審修正與驗證2026-10-05"></a>

### Review fixes and validation (2026-10-05) / 重審修正與驗證（2026-10-05）

Three confirmed findings were fixed:

三項已確認問題已修正：

**English**

1. Event-allocation failure clears queued pre-gap events under the approved policy and counts losses, preserving renderer batch-start reset semantics without new queue markers.
2. The first PipeService monitor failure is queryable through service_error even without active captures/delivered events; affected reads/later launches retain causes. Native completion/cleanup, EOF priority, and successful drain semantics remain.
3. PipeService creation moved into the factory allocation boundary: ResourceExhausted/Create with RAII cleanup, without expanding other exception boundaries.

**繁體中文**

1. 事件配置失敗採已核准的清除佇列政策，移除缺口前待取事件並累計遺失量。
   `TextRenderer` 原有的批次開頭重設語意得以成立，不新增佇列缺口標記。
2. 保存第一筆 PipeService monitor 故障，新增 `service_error()` 查詢；受影響的讀取
   及後續啟動回報保留原因鏈。沒有 active capture 時亦能診斷，失敗事件被丟棄
   不會抹去服務診斷。維持原生 I/O 完成／回收、EOF 優先及 wait 收尾成功的契約。
3. PipeService 建立移入 factory 原有例外邊界，配置失敗回報
   `ResourceExhausted / Create`，沿用 RAII 回收，不擴大其他 API 的例外處理範圍。

Windows x64/MSVC 19.51.36257, SDK 10.0.26100, CMake 4.3.2, Ninja 1.13.2: related rebuilds and **6/6** Debug/Release tests (OutputHub integration/faults, process_output, Module process, Shell integration/socket). Injection limits are described above; no native Linux/macOS validation.

Windows x64（MSVC 19.51.36257、SDK 10.0.26100、CMake 4.3.2、Ninja 1.13.2）
相關目標重新建置；Debug／Release 各 **6/6** 測試通過：`output_hub.integration`、
`output_hub.faults`、`osal.process_output`、`module.process`、`shell.integration`、
`shell.socket`。故障注入範圍與限制見上方「驗證範圍」；Linux／macOS 尚未實機驗證。

<a id="先前驗證紀錄"></a>

### Earlier validation record / 先前驗證紀錄

**Prior restriction resolved, 2026-10-04:** Release Vinc/Module examples previously left unrebuilt while in use were fully rebuilt during cause-chain work; Release Console also passed.

- **已解除的驗證限制（2026-10-04）：** 先前因使用者執行中而未重建的 Release
  `Vinc.exe` 與 Module 範例，已在原因鏈工作中完成全量建置；Release Console 測試亦通過。

<a id="error-domain-接線"></a>

## Error-domain integration / Error domain 接線

Result APIs use ErrorDomain::OutputHub, with component enums and separate static metadata. OSAL spawn/capture failures become SpawnFailed/ReadFailed, wrapping bounded OSAL/native causes and inheriting native source/value. Callers branch on this layer; capacity/input/wait errors and event diagnostics use OutputHub codes. OutputHub::create(Limits) returns `Result<unique_ptr<OutputHub>>`: InvalidArgument for options, ServiceFailed for PipeService, WorkerFailed with native detail for thread creation, ResourceExhausted for factory allocation. Failure cleans partial resources; capture/subscription scheduling responsibilities remain.

OutputHub 的既有 `vinc::Result<T>` API 使用 `ErrorDomain::OutputHub`。
`<vinc/output_hub/error.hpp>` 維護本層錯誤／操作 enum，錯誤表由獨立 metadata target
註冊。OSAL 啟動失敗轉為 `SpawnFailed`，擷取失敗轉為 `ReadFailed`，使用 `wrap_error()`
保留 OSAL 的分類、操作與原生診斷作為有限原因鏈；目前層仍繼承原生來源與數值。
呼叫端不必理解 OSAL 的分類。容量、輸入與等待失敗使用本層對應代碼。
事件的診斷 Error 同樣採本層分類。
建立方式已改為 `OutputHub::create(Limits)`，回傳 `Result<unique_ptr<OutputHub>>`。
無效設定回報 `InvalidArgument`，PipeService 失敗包成 `ServiceFailed`，worker 建立失敗
回報 `WorkerFailed` 並保留原生值，factory 配置失敗回報 `ResourceExhausted`。
factory 失敗回收已建立資源；既有 capture／訂閱／輸出排程保持原責任。

Monitor failure saves ServiceFailed/MonitorOutput with OSAL cause. Reads cancelled/closed by that service wrap ReadFailed/ReadOutput; independent read errors retain their own cause. EOF/drain ordering remains. Only the existing factory allocation boundary includes PipeService creation; other STL policies are unchanged.

執行中的 monitor 故障保存為 `ServiceFailed / MonitorOutput`，保留 OSAL 原因；
由服務關閉／取消造成的讀取失敗，再包為 `ReadFailed / ReadOutput`。
有獨立原生原因的讀取失敗維持自身原因；已確認 EOF 及收尾期限的判定順序不變。
`create()` 的既有配置失敗邊界包含 PipeService 建立；其他 API 的 STL／配置例外政策未改變。

Cause capacity/truncation policy belongs to [Common](../common/README.md#bounded-cause-chains). Branch only on current OutputHub classification; error_to_string also displays retained OSAL causes.

原因鏈容量及滿載省略政策由 [Common](../common/README.md#bounded-cause-chains) 維護。
一般判斷只檢查 OutputHub 的目前層；`error_to_string()` 會一併顯示保留的 OSAL 原因。

Recorded 2026-10-04: full Windows Debug/Release builds and **30/30** tests each; OutputHub additionally checks SpawnFailed with native provenance. No Linux/macOS build/run.

2026-10-04 Windows x64 Debug／Release 完整建置及各 30/30 套件通過；
OutputHub integration 額外確認啟動失敗使用本層 `SpawnFailed`，並保留原生診斷。
Linux／macOS 本輪未建置或執行。

Later same-day cause-chain work fully rebuilt both configurations and passed OutputHub integration, checking OSAL causes/native values and both text catalogs. Complete suites/object sizes are in Common.

原因鏈工作另於同日完成 Windows Debug／Release 全量建置，兩種組態的
`output_hub.integration` 均通過，新增檢查確認 OSAL 原因、原生值及兩個 catalog
的文字查詢均保留；完整測試及物件大小結果見 Common。

<a id="log-保存所需的來源資訊2026-10-08"></a>

## Source metadata for Log persistence (2026-10-08) / Log 保存所需的來源資訊（2026-10-08）

Source adds PID after successful spawn before publication and remains immutable after delivery. Event adds wall_received beside monotonic received. Log has an independent Subscription. Batch includes sources_skipped/unattributed: source-loss slots are preallocated per queue-event slot, so producer losses allocate nothing; attribution overflow retains aggregate/unknown counts. skipped is already the total: do not add sources_skipped again. Raw bytes/drop-oldest policy remains; see [Log](../log/README.md).

Source 增加 PID，在 spawn 成功、開始發布前填入；已發布來源仍不可變。
Event 增加 wall_received，保留 monotonic received。Log 使用獨立 Subscription。
Batch 增加 sources_skipped 與 unattributed：每個 queue event slot 預先配置一格来源損失記錄，
producer loss path 不配置，超過來源格數仍保留 aggregate 與無法歸屬計數。
skipped 仍為總數，不可再把 sources_skipped 加上去。原始 bytes 與丟舊資料政策不變。
完整使用方式見 [Log](../log/README.md)。
