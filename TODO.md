# TODO

本檔只保留尚未解決或已決定延後的事項；已完成項目的契約與驗證紀錄由所屬元件 README 維護。

2026-11-19 macOS 原生驗收已完成。Debug／Release 各通過來源功能 89/89、
來源 Python 7/7、乾淨 SDK 與搬移部署 5/5。Recorder、中央控制喚醒、
Topic 登記回收與 parallel pump 成本取樣見[本輪報告](src/vinc/sdk/MACOS_RECORDER_CONTROL_VALIDATION.md)。
Windows／WSL 驗收依各元件原報告追溯。

## Bag Recorder：通用映像錄製

- [ ] UI 型別清單編譯成本另案審查：本輪 137 classes 的外部 App 在 MSVC 19.51 Release 的 `topic_monitor.cpp` 觸發 C1128，A/B fixture 兩組均加 `/bigobj` 才可完成大型清單建置。正式 UI 未改；見 [UI 待辦](src/vinc/ui/README.md#large-typed-catalogs)。這不是 Recorder runtime RAM 問題。

- [ ] Recorder 真實負載成本追蹤：Windows 1 KiB paced A/B 的 generic CPU 中位數較高但範圍重疊，WSL 未見同方向；原因未定位，24 次錄製內容／loss／退出驗證均通過。建議後續分段 profiling，不能把編譯節省外推成普遍 runtime 節省；見 [Bag 待辦](src/vinc/bag/README.md#驗證與待處理事項)。

## Module System：中央控制接收喚醒

- [ ] 釐清 WSL Debug 兩個 Bus 測試執行檔及對應 object 變成 0 bytes 的原因，並評估執行前原生產物檢查或明確完成證據（尚未實作）。原 70/70、95.21 s 含空檔誤判，不作完整驗收依據；重建後同版相關套件已實測 70/70、132.54 s，Windows Debug 另驗 23+1 項。功能補驗已完成，此項只追蹤產物完整性／驗收流程；不能把先前建置中斷直接認定為根因。原始紀錄與補驗邊界見[PR #35 更正](src/vinc/module_system/CONTROL_WAKEUP_VALIDATION.md#pr35-review-correction)。

- [ ] 外部應用另核對 callback 容量及 pin，保留原 491 筆／30 秒 Ready 條件，不以框架局部通過替代完整應用驗收。

## Parallel pump：成本後續觀察

Windows／WSL 與 Mac 的驗收、成本及限制見 [pump 契約](src/vinc/module_system/PARALLEL_PUMP.md)
與 [Mac 原生報告](src/vinc/sdk/MACOS_RECORDER_CONTROL_VALIDATION.md)。

- [ ] 成本後續評估（提案，未授權改排程）：profile `Context::dispatch_parallel()` 空 Topic batch 重建與 `TopicScheduler::finish()` 全員喚醒的成本；若要量化 Windows 閒置增量，需更長的獨立取樣。不能因此直接取消 10ms fallback、縮弱快照／同 Topic 序列／停止契約。實測方法與限制見 [成本報告](src/vinc/module_system/PUMP_COST_VALIDATION.md)。

## Installed SDK：UI 測試埠隔離

- [ ] 追蹤 2026-11-17 Windows Debug consumer／搬移 App 首輪的 20 s／35 s guard 逾時；原期限乾淨重跑通過，但根因尚未定位。如再現先取得階段時間與堆疊，不直接延長期限；見 [SDK 待辦](src/vinc/sdk/README.md#待處理事項)及[驗收報告](src/vinc/module_system/CONTROL_WAKEUP_VALIDATION.md)。

- [ ] `installed_application` 固定使用 31411；Windows Release 首輪曾出現 UI BindFailed／HTTP 401，listener 身分未確認，埠釋放後原設定完整重跑通過。後續拆開預設 port 驗證與 HTTP 流程隔離，避免連到非預期服務；詳見 [SDK 待辦](src/vinc/sdk/README.md#待處理事項)。正式 UI 與 RPC 行為本輪未改。

## Tcl：WSL 掛載來源的並行資源部署

- [ ] 評估共用目的地的重複部署工作及慢速來源下的鎖等待政策。

`9f0ec0b` 的 Windows／Linux 退出修復補驗已完成，結果見
[跨平台驗收](src/vinc/osal/WINDOWS_LINUX_EXIT_VALIDATION.md)。其中 WSL 使用 `/mnt/d`
來源並行建置時，Tcl resource deployment 的 60 秒鎖等待逾時；改用同版來源的 WSL
原生檔案系統後，Debug／Release 建置及驗收通過。原掛載來源問題仍未修復，
細節與建議方向見 [Tcl 待辦](library/tcl/README.md#待處理事項)，不改動執行期退出契約。

## OSAL：FileLock 解鎖失敗後的原生持鎖狀態

- [ ] 定義原生 unlock 失敗後的鎖狀態、資料存取限制及清理／重試／故障政策。

**狀態（2026-10-05）：使用者決定先記錄，暫不處理。** 保留 guard 與本地 occupied
不等於已確認 native lock 仍被持有；目前是契約證據缺口，尚未重現失敗後已解鎖的情境。
2026-10-08：Concurrency 已改用共享原子鎖，不再需要原生解鎖移出簿記鎖方案
（見 [Concurrency](src/vinc/components/concurrency/README.md)），不代表 FileLock 問題已修復。
詳細分析及後續決策統一維護於
[OSAL README 待處理事項](src/vinc/osal/README.md#待處理事項)。

## Socket Shell：背景輸出期間的遠端輸入列重繪

- [ ] 改善遠端輸入尚未送出時，OutputHub 背景輸出與輸入列交錯的顯示問題。

**狀態：** 已確認延後處理。Socket Shell 第一版採簡單的整行輸入，不支援遠端自動完成；
使用者接受背景輸出可能打亂輸入列顯示。Socket Shell 第一版見 [操作文件](src/vinc/shell/SOCKET.md)。
本項重繪尚未實作，亦不構成後續動工授權。

- [ ] Socket Shell 密碼登入；目前依已確認範圍免登入，後續另行設計。

### 問題與第一版邊界

- 使用 PuTTY Raw TCP，開啟 Local echo／Local line editing 時，尚未按 Enter 的文字只存在客戶端，伺服器尚未收到。
- 模組 stdout／stderr 經 OutputHub 傳到該連線時，可能插入正在顯示的輸入列；這是顯示交錯，不代表命令內容遭修改。
- 此問題與 Tab 自動完成不同，也不依賴 Vinc Log；目前的 OutputHub 模組輸出即可觸發。
- 第一版先保留整行輸入及模組輸出開關，不加入遠端逐鍵編輯或輸入列重繪。

### 後續候選方向（尚未選定實作）

- 評估客戶端關閉 Local echo／Local line editing，改由遠端 Session 接收按鍵、管理編輯狀態與回顯。
- 顯示背景輸出前清除提示列，顯示後還原尚未送出的文字與游標。
- 確定終端能力與控制序列支援範圍，保留簡單整行客戶端的相容模式。
- 另行討論 UTF-8、退格、換行、終端寬度與長行的行為；遠端自動完成不自動納入此項範圍。

### 驗收條件

- [ ] 背景輸出抵達時，尚未送出的命令內容與游標位置能正確保留及重繪。
- [ ] 連續輸出、中文與跨區塊 UTF-8 不會破壞輸入；按 Enter 後只執行一次完整命令。
- [ ] 明確記錄支援的 PuTTY 設定與相容模式，完成實際連線驗證。
