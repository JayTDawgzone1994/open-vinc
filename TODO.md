# TODO

本檔只保留尚未解決或已決定延後的事項；已完成項目的契約與驗證紀錄由所屬元件 README 維護。

## App output 目錄：macOS 原生驗收

- [ ] 驗證來源版與乾淨 installed SDK 的 App 皆位於 `<build>/output/<App>/<組態>/`。
- [ ] 來源版內建 App 的工作資料位於 `<build>/intermediate/vinc/`；重新配置後測試及部署正常，乾淨 build 不產生舊 `apps/vinc/` 工作目錄。

涵蓋不同子目錄宣告、多 App、Debug／Release 分離、C++／Python Module、UI／Tcl／授權
資源、搬移部署、正常關閉及自訂輸出位置；確認舊目錄資料未被搬移或刪除。
契約與本輪平台結果見 [SDK 安裝指南](src/vinc/sdk/INSTALL.md#app-output-validation)。

## Module 分類部署：macOS 原生驗收

- [ ] 同一分支驗證 `add_python_module()`、`modules/native`／`modules/python` 的來源與乾淨 installed SDK。

涵蓋各 App 的 Kconfig 選擇、共用 Python Module 各自部署、入口／資源檔案、搬移套件、
增量更新及取消選用清理、保留手動 Module、大小寫撞名、舊 `data/module` 不再搜尋，
以及 C++／Python 通訊與正常退出。契約及本輪平台結果由
[Python README](src/vinc/module_system/python/README.md#module-deployment-validation) 維護。

## 檔案 Log Param：macOS 原生驗收

- [ ] 同一分支驗證三項 Log Param 的來源與乾淨 installed SDK。

涵蓋 Limited／App、最後合法容量修正、save／重啟、reopen 不套用新 Param、停用時
無檔案操作／假遺失計數且 raw／structured 顯示仍可用、設定讀取失敗及檔案故障隔離。
案例、契約與 Windows／WSL 證據見 [檔案 Log 參數](src/vinc/log/FILE_PARAMS.md)。

## 服務網路 Param：macOS 原生驗收

- [ ] 同一分支驗證 Shell IP／port、UI port 的來源與乾淨 installed SDK。

涵蓋新預設 31410／31411、CLI 與保存優先序、非法 IP 修正、port 0、IPv6、
埠衝突保留設定、現有連線不中斷及 UI disable／enable 套用新值。
契約與 Windows／WSL 證據見 [服務網路參數](src/vinc/shell/NETWORK_PARAMS.md)。

## Publisher 預設容量 Param：macOS 原生驗收

- [ ] 在同一分支完成 macOS Debug／Release 原生與乾淨 installed SDK 驗收。

本輪為 Bus v4／Plugin entry 14，先前 Param wire v3／entry 13 的 Mac 報告不能替本輪背書。
驗收涵蓋兩項預設、部分／完整覆寫、最後有效值修正、原有 Publisher 不變、真實預算耗盡、
Python None／整數選項及安裝後自訂訊息 App。案例與平台紀錄見
[Publisher 預設契約](src/vinc/message_bus/PUBLISHER_DEFAULTS.md#validation--驗證)。

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
