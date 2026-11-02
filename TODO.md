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

## Python 建置整理：後續變更的 macOS 原生驗證

- [ ] 補驗 Python Publisher 身分回歸：`module.python_services` 的同號／異號、錯誤身分、
  callback 故障隔離與收尾。Windows／WSL 六個情境已通過；見
  [Python 測試修正紀錄](src/vinc/module_system/python/README.md#publisher-identity-regression)。

- [ ] 補驗統一的建置 ABI 探測：`sdk.python_selection`、來源／installed SDK Python binding，
  包含逾時、無效輸出、多 App／venv、cache 保護、patch 更新與 `_build.py` 資料一致性。
  Windows／WSL 已測範圍及契約見 [Python 紀錄](src/vinc/module_system/python/README.md#build-abi-probe)。

- [ ] 補驗 System Python 搜尋一致性：`osal.path`、`sdk.python_selection`、
  `module.python_shell`，涵蓋不可執行檔、失效連結、空 PATH、明確相對目錄、venv
  與選定後失敗不得回退。參考 [本輪紀錄](src/vinc/module_system/python/README.md#system-python-path-consistency)。

先前單一執行檔、Kconfig 選擇與 Runtime／SDK 的 Mac 驗收已完成，見
[原生驗證紀錄](src/vinc/module_system/python/README.md#macos-snapshot-repair-validation)。
上列僅追蹤本地後續搜尋規則、ABI 探測與測試修正，不重新開啟已完成的舊驗收。

## OSAL：FileLock 解鎖失敗後的原生持鎖狀態

- [ ] 定義原生 unlock 失敗後的鎖狀態、資料存取限制及清理／重試／故障政策。

**狀態（2026-10-05）：使用者決定先記錄，暫不處理。** 保留 guard 與本地 occupied
不等於已確認 native lock 仍被持有；目前是契約證據缺口，尚未重現失敗後已解鎖的情境。
2026-10-08：Concurrency 已改用共享原子鎖，不再需要原生解鎖移出簿記鎖方案
（見 [Concurrency](src/vinc/components/concurrency/README.md)），不代表 FileLock 問題已修復。
詳細分析及後續決策統一維護於
[OSAL README 待處理事項](src/vinc/osal/README.md#待處理事項)。

## OSAL：POSIX fd 建立與 spawn 的並行競爭窗口

- [x] 完成第四階段 Windows／WSL Debug、Release 來源與安裝式 SDK 整合驗收。
- [ ] 補驗第二階段 POSIX FD／spawn 修復的 macOS 原生行為，以及 macOS 第四階段跨元件／SDK 驗收。

**第四階段結果（2026-11-03）：** Windows Debug／Release 各 122 項、WSL 各 130 項
均有通過證據；Debug 的 App fixture 參數遺漏已修正並定點重驗，詳見
[驗收紀錄](src/vinc/osal/FD_SPAWN_PLAN.md#stage-4-acceptance)。沒有遺留本輪測試子程序。
macOS 仍須對同一 topic branch／commit 補驗來源 App／Host／Plugin、共用協調器、
乾淨 installed SDK 與搬移 App、自訂訊息通訊、舊 Plugin entry 拒絕、啟動失敗隔離、
重複啟停及回收。先前 Mac 基線與其他功能的 SDK 驗收不能代替本次 FD 修復驗收；
Mac 完成前不將第四階段標記為三平台完成。

**狀態（2026-11-02）：第二階段修復已實作並通過 Windows／WSL 驗證；macOS 新驗收待補。**
Linux Debug／Release 的受控誤繼承、來源失效／重用與 Pipe EOF 延後案例均已通過修復後驗證。
Windows HANDLE 路徑維持並通過回歸；不同機制的結果不能代替 macOS 原生驗證。
2026-10-27 已修正 `osal.descriptor` 對繼承 fd 編號的測試假設，並完成 Ubuntu 24.04／WSL 2
驗證；該修正未改動正式 OSAL 實作，不代表本項並行競爭已解決。
修訂設計已確認：資源建立同步、標準串流副本，以及同 Host／Plugin 的 OSAL 共用協調器。
使用者已接受初始化與第三方操作邊界；最多三個副本是固定串流數量，不是可調參數。
證據與後續驗收方向見 [跨平台 FD 驗證報告](src/vinc/osal/FD_RACE_VALIDATION.md)。
已確認契約、驗收要求與四階段進度統一維護於
[FD／spawn 修訂計畫](src/vinc/osal/FD_SPAWN_PLAN.md)。共用協調器、Host／Plugin
接線與原生 FD／spawn 已接通；第三階段 Mac 退出觀察及第四階段 Mac 原生驗收仍待執行。
新增協調測試的原生 macOS 執行待補，不撤銷既有 Mac 基線證據。

## OSAL：macOS 標準串流與退出觀察

**狀態（2026-11-02）：使用者決定本輪先跳過，保留待辦。** 延後第二階段修復的
macOS 原生驗收與第三階段退出觀察調查；不代表問題已解決，也不撤銷既有 Mac 重現證據。
恢復時使用同一 topic branch／commit 驗證 Debug／Release，記錄工具鏈與測試結果。

- [ ] 原生驗證第二階段 owned-stdio 修復是否排除 macOS Inherit＋CLOEXEC 串流消失。
  Mac Debug／Release 各 69/144 組重現；Linux 對應矩陣各 144/144 通過。
- [ ] 追查 macOS 子程序已為 zombie，`wait_until()` 仍逾時、`try_wait()` 才取得退出結果。
  Mac 未修改 OSAL 的正常啟停控制組已重現；Windows／Linux 本輪各組態 3,000 次未重現。
  尚未確認根因，不先用延長逾時或輪詢掩蓋。

這兩項與 FD 競爭分開追蹤；Mac 為使用者提供的原生證據，本機不重複宣稱已執行 Mac 測試。
詳細證據與原始碼位置見 [OSAL 驗證報告](src/vinc/osal/FD_RACE_VALIDATION.md)；
macOS **退出觀察**根因調查及修復驗收為[修訂計畫](src/vinc/osal/FD_SPAWN_PLAN.md)第三階段。

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
