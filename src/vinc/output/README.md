# 中央輸出管理層

`vinc::output` 接收子程序 stdout／stderr，提供獨立、有界的接收佇列。
本庫只依賴 OSAL，不依賴 Module、Shell、Tcl 或 message bus；ModuleManager
負責傳入模組名稱與啟動 generation，Shell 負責取出資料並顯示。

```text
任意子程序 stdout／stderr
  → OSAL PipeService（非同步讀取）
  → OutputHub（共用收集執行緒）
  → 每個目的地自己的 Subscription
  → 本地 Shell／未來的 Socket 傳送流程
```

## 使用與所有權

標頭：`<vinc/output/hub.hpp>`，連結目標：`vinc::output`。

- `OutputHub::subscribe()`：建立接收者；須在啟動子程序前訂閱，才能收到初始化輸出。
- `spawn(processes, options, name, generation)`：先預留來源與讀取 buffer，再透過
  OSAL 啟動並擷取 stdout／stderr，回傳 `CapturedProcess{process, capture}`。
  此框架介面固定擷取兩路，覆蓋 options 的 stdout／stderr 模式；stdin 保持原設定。
- 呼叫端持有 Process 並監看退出。**確認 OS 退出後**呼叫 `exited(capture)`，啟動
  輸出收尾期限。ModuleManager 已完成這項接線。
- `wait_until(deadline)`：等待目前來源完成，不自動停止或終止程序，不關閉啟動入口。
  呼叫端應先停止新啟動。成功代表讀取已結束，可能包含有明確紀錄的截斷或失敗；
  不代表所有目的地已顯示資料。
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

接收時間不是模組產生文字的時間；兩條串流之間不承諾實際寫入的總排序。
stderr 也不自動等於某種 log 等級。來源 ID 在同一 Hub 中不重用，重新啟動取得
新來源身分；舊程序的輸出保留原 generation，即使新程序已開始執行。

## 容量、排程與遺失政策

預設值可透過 `Limits`／`QueueLimits` 配置：

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
不依賴目的地是否還保留事件。事件配置失敗也會計入接收者的略過統計，繼續讀 Pipe。

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
Bus 清理與有期限的輸出收尾 → Shell 取完剩餘佇列並顯示結尾。
2 秒期限只限制已退出程序的輸出收尾，不減少等待受管理程序退出的要求。
若孫程序仍持有 writer，到期後標記 Truncated、關閉讀取端，不永久等待。
Shell 關閉時另列出累計截斷／讀取失敗數，即使個別通知曾被佇列丟棄仍有摘要。

## 驗證範圍

`output.hub` 測試大型雙路資料、慢接收者、總額度、停用／恢復、無接收者、
啟動失敗、來源世代、收尾期限、UTF-8、控制字元、長行與資料缺口。
收尾期限單元測試以仍持有 writer 的 helper 模擬退出通知後的管道狀態，並自行終止清理。

`shell.integration` 使用真實模組驗證初始化、命令、崩潰／重啟與 on_stop 尾端資料；
`shell.console` 在 Windows 隱藏終端驗證背景輸出後的部分命令重繪與繼續執行。
Linux／macOS 尚待實機建置與執行。Socket Shell、結構化 Vinc Log 及檔案保存未在此版實作。
