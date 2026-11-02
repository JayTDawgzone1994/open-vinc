# TODO

本檔只保留尚未解決或已決定延後的事項；已完成項目的契約與驗證紀錄由所屬元件 README 維護。

## App 組裝統一：macOS 原生驗證

- [ ] 使用同一 topic 內容驗證內建 App 與來源／安裝式 SDK 共用 `vinc_add_app()`。
  涵蓋各 App 產物目錄、完整 SDK UI 能力、Module 選擇／清理、自訂訊息增量建置、
  內嵌腳本、搬移部署、Python 及 System Module 生命週期；記錄工具鏈與組態。

2026-11-02 實作與 Windows Release／WSL Ubuntu 24.04 來源驗證已完成；Windows
另通過來源 SDK、安裝式 Python SDK 與多 App 驗收。契約與完整範圍維護於
[App README](apps/vinc/README.md#app-composition-unification--app-組裝統一)。
本輪未重跑 Debug 或 WSL installed SDK 全套，macOS 需另行原生驗證，不以其他平台結果替代。

## App／SDK：單一執行檔的 macOS 原生驗證

- [ ] 補驗 Python Publisher 身分回歸：`module.python_services` 的同號／異號、錯誤身分、
  callback 故障隔離與收尾。Windows／WSL 六個情境已通過；見
  [Python 測試修正紀錄](src/vinc/module_system/python/README.md#publisher-identity-regression)。

- [ ] 補驗統一的建置 ABI 探測：`sdk.python_selection`、來源／installed SDK Python binding，
  包含逾時、無效輸出、多 App／venv、cache 保護、patch 更新與 `_build.py` 資料一致性。
  Windows／WSL 已測範圍及契約見 [Python 紀錄](src/vinc/module_system/python/README.md#build-abi-probe)。

- [ ] 補驗 System Python 搜尋一致性：`osal.path`、`sdk.python_selection`、
  `module.python_shell`，涵蓋不可執行檔、失效連結、空 PATH、明確相對目錄、venv
  與選定後失敗不得回退。參考 [本輪紀錄](src/vinc/module_system/python/README.md#system-python-path-consistency)。

- [ ] 補驗 Kconfig Module Python 選擇：System 的部署 PATH／venv、Custom 絕對路徑、
  `--python` 覆寫、來源與 installed SDK、多 App 一致性、缺少 Python／ABI 不符的 Module
  失敗隔離，以及切換環境的快取拒絕。使用同一 topic 內容執行 `sdk.configuration`、
  `sdk.python_selection`、Python Module 與安裝式／多 App 測試；Windows／WSL 證據不代替
  macOS。契約與本輪結果見 [設定工具](tools/configure/README.md#module-python-selection--module-python-選擇)。

- [ ] 在 macOS 使用同一份來源版本，驗證中央與原生 Module Host 共用 App 執行檔的完整流程。

**狀態（2026-10-31）：實作與 Windows Debug／Release、WSL Ubuntu 24.04 驗證已完成；
本次改動仍缺 macOS 原生證據。** 既有 Mac 驗證紀錄保留，但不能代替這次新入口的驗證。
需涵蓋角色分流、錯誤啟動／token 拒絕、生命週期與故障隔離、debug gate、服務回歸，
以及乾淨 SDK 安裝、搬移部署、產物清單與 Python／C++ 混合執行；記錄 OS、CPU 架構、
工具鏈、組態及實際結果。詳細契約、測試指令與既有結果統一維護於
[單一執行檔計畫：macOS 交接](apps/vinc/SINGLE_EXECUTABLE.md#remaining-native-macos-handoff--macos-原生待驗證)。

## Python／SDK：Runtime 建置政策的 macOS 原生驗證

- [ ] 由專案擁有者在 macOS 驗證本輪 Runtime 政策與 installed SDK 改動。

**狀態（2026-11-02）：Windows 來源版與 installed SDK 的靜態 CRT 接線已實作並驗證；
macOS 本輪尚未驗證，使用者安排後續驗收。** macOS 保留系統 Runtime，不套用 Windows
`/MT`，也不要求重現 Windows DLL 情境。此項與上方單一執行檔驗證分開追蹤。

驗收範圍：

- 來源版 Python 綁定與原生 C++ 回歸，確認 CMake 政策未引入 Windows 專用旗標或庫。
- SDK 製作者關閉 Python，安裝及搬移 SDK 後，使用端再啟用 Python、加入自訂訊息。
- 搬移 App 並避開來源／建置路徑，驗證 Python 與 C++ Module 的 Bus／RPC／Param、
  正常退出，以及 ABI 拒絕、生成檔更新、部署補檔與停用清理。
- 檢查擴充的動態相依與載入路徑；記錄同一 topic commit、macOS 版本、CPU 架構、
  編譯器／標準庫、Python、CMake、組態與實際測試結果，不推論未測架構亦通過。

契約與結果統一維護於 [Python Runtime 政策](src/vinc/module_system/python/README.md#runtime-policy-rollout)
及 [SDK 整合驗收紀錄](src/vinc/sdk/INSTALL.md#runtime-final-acceptance)。既有 macOS
測試結果仍有效於其原範圍，但不能代替本輪建置與交付變更的驗證。

## OSAL：FileLock 解鎖失敗後的原生持鎖狀態

- [ ] 定義原生 unlock 失敗後的鎖狀態、資料存取限制及清理／重試／故障政策。

**狀態（2026-10-05）：使用者決定先記錄，暫不處理。** 保留 guard 與本地 occupied
不等於已確認 native lock 仍被持有；目前是契約證據缺口，尚未重現失敗後已解鎖的情境。
2026-10-08：Concurrency 已改用共享原子鎖，不再需要原生解鎖移出簿記鎖方案
（見 [Concurrency](src/vinc/components/concurrency/README.md)），不代表 FileLock 問題已修復。
詳細分析及後續決策統一維護於
[OSAL README 待處理事項](src/vinc/osal/README.md#待處理事項)。

## OSAL：POSIX fd 建立與 spawn 的並行競爭窗口

- [ ] 修復標準串流關閉時，內部 fd 建立／搬移與 spawn 繼承取樣之間的競爭。

**狀態（2026-10-05）：使用者決定暫緩處理。** 已確認程式交錯路徑，尚未以受控排程
重現或修復；既有 fd 搬移及 Windows 測試不能證明此窗口已排除。
2026-10-27 已修正 `osal.descriptor` 對繼承 fd 編號的測試假設，並完成 Ubuntu 24.04／WSL 2
驗證；該修正未改動正式 OSAL 實作，不代表本項並行競爭已解決。
共用 mutex＋標準串流快照仍為未核准提案，鎖成本及宿主／第三方責任邊界尚待決策。
後續須在 Linux、macOS 分別建置並執行受控競爭與回收測試；Windows 僅作回歸驗證。
詳細問題、提案、待決策與驗證需求統一維護於
[OSAL README 待處理事項](src/vinc/osal/README.md#待處理事項)。恢復工作須另行確認範圍及取得開工授權。

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
