# OutputHub：中央輸出管理層

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

## 使用與所有權

標頭：`<vinc/output_hub/output_hub.hpp>`，連結入口為 CMake alias `vinc::output_hub`，
實際 library target 為 `vinc_output_hub`。

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

Module 命名核准前以 `starting:<id>` 標示建構／命名階段輸出；核准後改用實例名稱。

Hub 的一般操作可並行；subscription 的 take／set_enabled 由內部同步。
Hub／subscription 的移動、close 或解構，以及 ProcessService 的生命週期，
須由呼叫端與使用者同步。已取出的 Batch 屬呼叫端記憶體責任，不計入 Hub 佇列預算。

## 資料與身分

每份 Event 帶有不可變 Source（來源 ID、名稱、generation、stdout／stderr）、
中央接收序號、steady-clock 接收時間、原始 bytes，以及事件種類：

| 種類 | 意義 |
| --- | --- |
| `Data` | 已讀取的位元組，沒有行或文字編碼保證 |
| `End` | Pipe 已排空且 EOF |
| `Truncated` | 收尾期限到期，不能確認輸出已完整排空 |
| `Failed` | I/O 提交或完成失敗，保留可用的 OS 錯誤 |
| `Renamed` | 結束舊名稱下的文字片段，後續事件使用新名稱 |

接收時間不是模組產生文字的時間；兩條串流之間不承諾實際寫入的總排序。
stderr 也不自動等於某種 log 等級。來源 ID 在同一 Hub 中不重用，重新啟動取得
新來源身分；舊程序的輸出保留原 generation，即使新程序已開始執行。

## 容量、排程與遺失政策

預設值可透過 `Limits`／`QueueLimits` 配置：

預設容量、准入上限、讀取批次、等待期限與文字顯示設定集中於
`<vinc/config.hpp>` 的 `output_hub_*` 常數；目前為編譯期設定。
`Limits`／`QueueLimits` 保留原有呼叫端配置能力。固定的 stdout／stderr 索引、
來源 ID 對應方式與 UTF-8 編碼規則仍由本元件維護。

| 項目 | 預設 |
| --- | --- |
| 同時擷取程序數（含尚未收尾的舊世代） | 128 |
| 接收者數量 | 8 |
| 每個接收佇列 | 256 KiB、128 events |
| 所有接收佇列總預算 | 2 MiB、1024 events |
| 每條串流讀取區塊 | 4 KiB |
| 退出後收尾期限 | 2 秒 |

佇列同時檢查 bytes 與事件數；容量不足時丟棄該接收者最舊的待顯示事件。
單筆事件若大於接收者的 byte 上限，清除先前待顯示資料後略過該事件。
略過的 bytes／事件數獨立累計，下次 take 一次取回，不會隨佇列溢位一起丟失。
停用期間屬於主動不接收，不累計那些資料的 bytes；停用／啟用仍標記不連續。

同一份 Event 由多個接收者共享，每個目的地獨立前進。額度按各佇列容量保守保留；
資料事件與 metadata 同時受事件數上限約束。讀取失敗／收尾截斷另有 Hub 累計統計，
不依賴目的地是否還保留事件。事件配置失敗時清除所有啟用接收者各自的待取佇列，
將額外丟棄及本次失敗事件一併計入略過統計，繼續讀 Pipe。這讓缺口固定出現在
下一批資料之前，避免跨缺口拼接；代價是也會捨棄部分仍有效的舊輸出。
停用接收者不累計該事件；清除與統計本身不配置記憶體。

Hub 使用一條收集執行緒，另有 OSAL 的共用 Pipe worker；不為每個模組新增讀取執行緒。
每條串流最多一筆 I/O，每批最多取 32 個完成事件；收集控制檢查間隔最多約 20ms，
不是硬即時保證。本地 Shell 每輪最多取 16 個 events，避免無限排空擠掉鍵盤處理。

慢速接收者不會讓 Hub 等待其輸出。但 Pipe 容量仍有限；OS 排程、CPU 滿載或收集端
跟不上時，模組 stdout 寫入仍可能阻塞。此版本不承諾端到端無損或硬即時輸出。

## 本地文字顯示

`TextRenderer` 位於顯示側；Hub 永遠保留原始 bytes。

- 顯示 `[name#generation stdout]` 或 `[name#generation stderr]` 前綴。
- 按來源獨立處理跨區塊 UTF-8、換行與未完成文字。
- 行上限 4 KiB，超長行分段；未換行內容在 100ms 後顯示。
- 不完整 UTF-8 會等後續片段；若到顯示期限／EOF 仍不完整，則轉義顯示。
- ESC、其他 C0／C1 控制字元與無效 bytes 轉成可見 `\xNN`；不執行 ANSI 序列。
  CRLF 當換行，其他 CR 轉義，不提供終端進度條效果。
- 最多暫存 256 個來源的未完成行；滿時先顯示並釋放一筆舊來源狀態。
- 佇列發生資料缺口時，提示略過數量並重設未完成文字，避免把前後片段誤接。

只有 Shell 執行緒會寫自己的 ostream：清提示列、顯示一批資料、再還原輸入列。
命令回覆與 Tcl 輸出沿用原本路由，不經可丟棄的模組輸出佇列。
本地 ostream 自己若阻塞，該 Shell 仍可能暫停；Hub 與模組監看不等待它。

## Module／Shell 關閉

ModuleManager 擁有 Hub，生命週期不依附某個 Shell。模組在啟動時即開始擷取，
不用等 Ready；啟動失敗回收本次來源。重啟不必等待舊世代輸出排空。

正常關閉順序為：關閉啟動入口 → 要求模組停止 → 確認受管理程序退出 →
Message Bus 清理與有期限的輸出收尾 → Shell 取完剩餘佇列並顯示結尾。
2 秒期限只限制已退出程序的輸出收尾，不減少等待受管理程序退出的要求。
若孫程序仍持有 writer，到期後標記 Truncated、關閉讀取端，不永久等待。
Shell 關閉時另列出累計截斷／讀取失敗數，即使個別通知曾被佇列丟棄仍有摘要。

## 驗證範圍

`output_hub.integration` 測試大型雙路資料、慢接收者、總額度、停用／恢復、無接收者、
啟動失敗、來源世代、收尾期限、UTF-8、控制字元、長行與資料缺口。
收尾期限單元測試以仍持有 writer 的 helper 模擬退出通知後的管道狀態，並自行終止清理。

`output_hub.faults` 使用獨立編譯的測試版本，注入事件配置失敗、factory 邊界配置失敗、
monitor 診斷及 I/O 完成結果；正式 library 不包含注入入口。確認各接收者丟棄統計、
分批取出不跨缺口拼接、遺失改名事件、Failed 事件遺失仍保留計數、無來源時仍能查詢
服務故障、原因鏈、獨立讀取失敗不被覆蓋、EOF 優先及失敗後仍可完成收尾。
這是 Hub 邊界的確定性故障測試，不代表已觸發各 OS 的真實 monitor 故障或所有配置失敗點。

`shell.integration` 使用真實模組驗證初始化、命令、崩潰／重啟與 on_stop 尾端資料；
`shell.console` 在 Windows 隱藏終端驗證背景輸出後的部分命令重繪與繼續執行。
Linux／macOS 尚待實機建置與執行。Socket Shell 已接上 OutputHub，見
[Socket Shell 文件](../shell/SOCKET.md)。結構化 Vinc Log 與檔案保存尚未實作。

## 程式閱讀順序

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

此次可讀性整理保留公開 API、錯誤型別、例外邊界、容量數值與同步政策。
UTF-8 測試期望值與測試負載留在測試，不直接引用系統設定作為答案。

### OutputHub 命名統一驗證（2026-10-03）

目錄、命名空間與 target 統一為 `output_hub`，主標頭／實作使用
`output_hub.hpp`／`output_hub.cpp`；類別仍為 `OutputHub`。
Module、Shell、SDK、測試與文件引用已同步更新，不保留舊路徑或 target 別名。
本輪只改名；比對 11 個受影響 C++ 檔案，確認差異僅為名稱與引用路徑。

- Windows x64 MSVC Debug：完整建置成功，OutputHub、Module、Shell、SDK 相關測試 9/9 通過。
- Windows x64 MSVC Release：相關獨立測試目標與 SDK 標頭編譯成功，相關測試 7/7 通過。
- Socket Shell 測試在上述兩種組態均通過；Console 測試本輪僅在 Debug 執行。
- Linux／macOS 本輪未驗證。

### 可讀性整理驗證（2026-10-04）

- Windows x64 MSVC Debug：完整建置與 SDK 標頭編譯成功；OutputHub、Module、
  Shell（含 Socket 與 Console）、SDK 相關測試 9/9 通過（30.49 秒）。
- Windows x64 MSVC Release：相關獨立測試目標與 SDK 標頭編譯成功；
  OutputHub、Module、Shell Socket／Tcl／Unit／Integration、SDK 測試 7/7 通過（26.43 秒）。
- 新增的 UTF-8 邊界案例包含於兩種組態的 `output_hub.integration`，均通過。
- 原有診斷／顯示字串比對一致；本輪未執行 Linux／macOS 驗證。

### 重審修正與驗證（2026-10-05）

三項已確認問題已修正：

1. 事件配置失敗採已核准的清除佇列政策，移除缺口前待取事件並累計遺失量。
   `TextRenderer` 原有的批次開頭重設語意得以成立，不新增佇列缺口標記。
2. 保存第一筆 PipeService monitor 故障，新增 `service_error()` 查詢；受影響的讀取
   及後續啟動回報保留原因鏈。沒有 active capture 時亦能診斷，失敗事件被丟棄
   不會抹去服務診斷。維持原生 I/O 完成／回收、EOF 優先及 wait 收尾成功的契約。
3. PipeService 建立移入 factory 原有例外邊界，配置失敗回報
   `ResourceExhausted / Create`，沿用 RAII 回收，不擴大其他 API 的例外處理範圍。

Windows x64（MSVC 19.51.36257、SDK 10.0.26100、CMake 4.3.2、Ninja 1.13.2）
相關目標重新建置；Debug／Release 各 **6/6** 測試通過：`output_hub.integration`、
`output_hub.faults`、`osal.process_output`、`module.process`、`shell.integration`、
`shell.socket`。故障注入範圍與限制見上方「驗證範圍」；Linux／macOS 尚未實機驗證。

### 先前驗證紀錄

- **已解除的驗證限制（2026-10-04）：** 先前因使用者執行中而未重建的 Release
  `Vinc.exe` 與 Module 範例，已在原因鏈工作中完成全量建置；Release Console 測試亦通過。

## Error domain 接線

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

執行中的 monitor 故障保存為 `ServiceFailed / MonitorOutput`，保留 OSAL 原因；
由服務關閉／取消造成的讀取失敗，再包為 `ReadFailed / ReadOutput`。
有獨立原生原因的讀取失敗維持自身原因；已確認 EOF 及收尾期限的判定順序不變。
`create()` 的既有配置失敗邊界包含 PipeService 建立；其他 API 的 STL／配置例外政策未改變。

原因鏈容量及滿載省略政策由 [Common](../common/README.md#bounded-cause-chains) 維護。
一般判斷只檢查 OutputHub 的目前層；`error_to_string()` 會一併顯示保留的 OSAL 原因。

2026-10-04 Windows x64 Debug／Release 完整建置及各 30/30 套件通過；
OutputHub integration 額外確認啟動失敗使用本層 `SpawnFailed`，並保留原生診斷。
Linux／macOS 本輪未建置或執行。

原因鏈工作另於同日完成 Windows Debug／Release 全量建置，兩種組態的
`output_hub.integration` 均通過，新增檢查確認 OSAL 原因、原生值及兩個 catalog
的文字查詢均保留；完整測試及物件大小結果見 Common。


## Log 保存所需的來源資訊（2026-10-08）

Source 增加 PID，在 spawn 成功、開始發布前填入；已發布來源仍不可變。
Event 增加 wall_received，保留 monotonic received。Log 使用獨立 Subscription。
Batch 增加 sources_skipped 與 unattributed：每個 queue event slot 預先配置一格来源損失記錄，
producer loss path 不配置，超過來源格數仍保留 aggregate 與無法歸屬計數。
skipped 仍為總數，不可再把 sources_skipped 加上去。原始 bytes 與丟舊資料政策不變。
完整使用方式見 [Log](../log/README.md)。
