# TODO

本檔只保留尚未解決或已決定延後的事項；已完成項目的契約與驗證紀錄由所屬元件 README 維護。

## App／Kconfig：統一 App 組裝路徑與其餘重複設定入口

- [ ] Review 並統一 App 組裝路徑，清理其餘重複設定入口。

**狀態（2026-11-02）：App 組裝統一仍只有設計討論，尚未授權實作。**
現有根目錄來源建置仍走舊單 App 路徑，公開 `vinc_add_app()` 則使用各 App 的 Kconfig；
兩者尚未完全統一。本輪另已授權並接上 Kconfig 的 Python 建置／執行選擇、
移除公開 `-DPython_EXECUTABLE`，不代表本項 App 組裝統一已完成。
Python 契約與實測結果統一維護於 [設定工具](tools/configure/README.md#module-python-selection--module-python-選擇)。

已確認的使用需求：

- Kconfig 管理 Module Python 的選擇：預設系統 Python，可選自訂 Python 執行檔。
- 第一版自訂路徑只接受絕對路徑；保留 `--python PATH` 作為本次執行的覆寫。
- 不將 Python 最低版本視為跨 minor ABI 相容保證；既有 ABI 檢查仍須保留。

建議處理順序與待 review 範圍（尚未核准實作細節）：

1. 讓內建 Vinc App 與使用者 App 共用 `vinc_add_app()` 組裝路徑，盤點舊 catalog、
   官方 System Module、啟動腳本、部署及測試接線，保留既有行為。
2. 對真正控制同一項 App 功能的重複入口，只保留 Kconfig。候選包括
   `VINC_BUILD_PYTHON` 的 App 開關與舊 `VINC_BUILD_BAG` 轉接。
   提案是拒絕舊入口並提示遷移，而非默默忽略或保留覆寫規則；
   確切移除清單與舊 cache 處理須在開工前確認。

不能直接按名稱刪除所有 CMake 參數：`VINC_BUILD_UI` 同時涉及 SDK 能力與舊 App 選擇，
須先拆清責任；`VINC_NAME` 與新 `INSTANCE_NAME` 的遷移亦須盤點。
編譯器、Debug／Release、測試、benchmark、SDK 製作／安裝，以及執行設定工具的 Python，
與 App 功能選擇不同；其入口保留或調整應個別決定。CMake 內部變數不等於公開設定入口。
同一 build 目前共用一份 Python binding，不能因新增各 App 設定而默默混用不同 ABI。

驗收應涵蓋來源／installed SDK、多 App 一致性、PATH 與自訂路徑、`--python` 覆寫、
缺少工具／開發檔、ABI 不符、舊參數與快取診斷、部署搬移、設定增量更新及正常退出。
同步修正 `tools/configure/Kconfig` 中 UI／Bag「不自動啟動」的過時說明；目前它們已是
自動啟動的 System Module，Recorder 未收到命令前待命。

目前接線與契約見 [設定工具](tools/configure/README.md)、
[App 組裝](apps/vinc/MULTI_APP.md)、[SDK](src/vinc/sdk/INSTALL.md) 及
[Python Module](src/vinc/module_system/python/README.md)。實作前仍須確認完整範圍並取得開工授權。

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
