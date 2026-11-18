# TODO

本檔只保留尚未解決或已決定延後的事項；已完成項目的契約與驗證紀錄由所屬元件 README 維護。

## Bag Recorder：通用映像錄製

- [ ] UI 型別清單編譯成本另案審查：本輪 137 classes 的外部 App 在 MSVC 19.51 Release 的 `topic_monitor.cpp` 觸發 C1128，A/B fixture 兩組均加 `/bigobj` 才可完成大型清單建置。正式 UI 未改；見 [UI 待辦](src/vinc/ui/README.md#large-typed-catalogs)。這不是 Recorder runtime RAM 問題。

- [x] 第四階段 Windows／WSL 完成：Debug、Release 相關元件及乾淨 SDK／雙 App 搬移、正文／metadata／退出驗收通過；兩平台各完成三組編譯及兩種尺寸各三組 runtime A/B。見[本輪結果](src/vinc/bag/IMAGE_RECORDING.md#stage-4-validation)。
- [ ] Recorder 真實負載成本追蹤：Windows 1 KiB paced A/B 的 generic CPU 中位數較高但範圍重疊，WSL 未見同方向；原因未定位，24 次錄製內容／loss／退出驗證均通過。建議後續分段 profiling，不能把編譯節省外推成普遍 runtime 節省；見 [Bag 待辦](src/vinc/bag/README.md#驗證與待處理事項)。
- [ ] macOS 原生同版重建 Debug／Release（Plugin entry 23、Bag v3）：先跑 `message.errors/storage/generated/access`，核對 generic canonical／typed byte parity、壞映像拒絕、未知 hash、型別驗證界線與配置失敗重試；另跑 `message_bus.images`、`module.context`、`module.parallel_images`、`module.parallel_image_stop` 及既有 Bus／Host／Python typed 回歸。跑 `bag.storage/module/loop`、`ribosome.schema/generated/names`，確認未知型別正文與巢狀欄位可由外部 schema 解碼、byte／筆數 queue、worker credit、容量／故障／drain、Bag v2 拒絕及名稱表依賴。每組態建立乾淨 SDK，搬移並隱藏 producer／consumer／SDK 原路徑，同時錄製兩個 App，逐筆驗正文、順序、metadata、finish、真實回收。成本沿用 137 class、相同 ring／queue／兩種尺寸，各三組 A/B；先補 Mac 原生 CPU／RSS 採樣（現有成本工具明確拒絕 Darwin）。記錄工具鏈與初次失敗，不沿用歷史 typed Recorder 證據。見[完整交付清單](src/vinc/bag/IMAGE_RECORDING.md#stage-4-validation)。

## Module System：中央控制接收喚醒

- [ ] macOS 原生以同版重建，Debug／Release 執行 `control.channel`、`control.faults`、`control.receive_wake`、`module.control_wakeup`、`module.control_scheduler`，並重跑 Module 生命週期／故障／退出、Bus、RPC、Param（包含快取收斂測試補強）。核對入睡交界、40 幀公平掃描、RPC 真實滿載下等待／FIFO／一次入列、blocked Module 停止及 confirmed exit；每組態建立乾淨 SDK，驗 consumer／App／Python 及搬移部署。依[第四階段報告](src/vinc/module_system/CONTROL_WAKEUP_VALIDATION.md)重做相同 4／128 筆、每組三次的成本對照；目前 `tests/control_cost/usage.hpp` 僅有 Windows／Linux 採樣，Mac 需補原生 CPU／RSS／執行緒／資源計數，不接受假數值或直接套用 WSL 結果。記錄提交與工具鏈，Windows／WSL 證據不能替代 Mac。
- [ ] 外部應用另核對 callback 容量及 pin，保留原 491 筆／30 秒 Ready 條件，不以框架局部通過替代完整應用驗收。

## Message Bus：Topic 登記回收

- [ ] macOS 原生以同一提交重建 Bus v6／Plugin entry 23，Debug／Release 執行 `message_bus.control_plane.*`、Bus 回歸、`message_bus.module`、`message_bus.release_routes`、`module.context`、`module.shutdown_evidence` 與 Control Channel 測試。涵蓋 `topology-load` 的 1×128／16×128、160 個 Topic、超過心跳期限的派送暫停、無新操作收斂及原生 Host 異常退出。再驗乾淨 installed SDK 的 consumer／application／Python 與搬移部署；用 `tests/topology_cost.py` 記錄原生頁面、記憶體及喚醒成本。Windows／WSL 第五階段已有通過證據，不能替代 Mac。指令、量測邊界與完整矩陣見[控制面修復契約](src/vinc/message_bus/CONTROL_PLANE_REPAIR.md#第五階段整合與成本驗收2026-11-16)。

- [ ] macOS 原生 Release 重建中央、Host 與 Plugin，執行 `message_bus.cleanup`、`message_bus.topic_limit_param`、`message_bus.release_routes`。核對同一個 callback 清除 141 個訂閱後立即重新註冊、對端仍能收發、延後 ACK 與斷線時的 close 結果；記錄提交與工具鏈。

- [ ] macOS 原生 Release 驗證 `vinc.message_bus_max_topics`。執行 `message_bus.topic_limit_param`（預設超過 128 個名稱、動態調整、保存重啟、兩個 App 隔離）、`message_bus.cleanup`、`module.publisher_defaults`、`message_bus.topic_registration_emfile`；需啟用測試並備妥測試用 Python。記錄提交與工具鏈。

- [ ] macOS 原生驗收同名 Topic 的跨 App 崩潰／重啟隔離、FD 耗盡重試與共享名稱回收。使用同一提交的 [第四步驗收指令](src/vinc/message_bus/TOPIC_REGISTRATION_RECOVERY.md#第四步驗收)，記錄實際平台與結果。

## Parallel pump：macOS 與後續觀察

Windows Debug／Release 與 WSL Release 的乾淨 SDK／搬移驗收已完成；兩平台各 48 組成本對照亦完成。證據見下方契約，不代表 Mac 或所有偶發問題已解決；Recorder 未自動啟用 worker。

- [ ] macOS 成本矩陣（本輪依決定不做 benchmark）：用原生 Release 的 `src/vinc/module_system/tests/pump_cost` 分別量測 1／4 Topics、0／1／2／4 workers、輕／CPU-heavy callbacks，每組三次；記錄 latency、吞吐、delivery/loss/collision、process CPU／peak RSS 與實際工具鏈。Windows／WSL 或舊 Mac entry 的結果不能替代這輪原生驗收。

範圍與進度見 [多執行緒 pump 契約](src/vinc/module_system/PARALLEL_PUMP.md)。
Mac Host 收尾與 C++ 回歸已有[第一階段原生證據](src/vinc/module_system/MACOS_CONTEXT_SHUTDOWN_VALIDATION.md)；
來源 Python／GIL、parallel、RPC／Param 已補[第二階段 Debug／Release 證據](src/vinc/module_system/MACOS_SOURCE_FUNCTIONAL_VALIDATION.md)；
SDK-only／搬移／分類部署／輸出位置已有[第三階段證據](src/vinc/sdk/MACOS_SDK_DEPLOYMENT_VALIDATION.md)。
Mac 功能補驗已完成；成本量測仍依使用者決定延後，不能宣稱已測效能。

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
