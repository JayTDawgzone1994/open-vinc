# TODO

本檔只保留尚未解決或已決定延後的事項；已完成項目的契約與驗證紀錄由所屬元件 README 維護。

## Param 重啟標記：macOS 驗收

- [ ] macOS：重啟標記版本的乾淨 installed SDK consumer／application／python 三套驗收。
- [ ] macOS 重建 Plugin entry 13／Param wire v3，跑 Param 三套及 Plugin／Python 四套測試，
  驗證預設 Limited + None、跨程序標記、callback、舊協定拒絕、重新註冊與 TOML 不保存標記。
- [ ] macOS 補跑 Shell unit／integration／socket／tcl 與新版 C++／Python 範例 smoke，
  包含重啟提示、非同步診斷交錯、啟動才套用 label、明確保存及未保存修改的 App 重啟行為。

Windows／WSL Release 第三階段已通過全新 SDK 建置及三套 installed 驗收，各 **3/3**。
本次契約、結果與 Mac 步驟統一見 [Param restart hints](src/vinc/param/README.md#restart-hints-stage-three)。
既有 Mac 基線早於本次 ABI 變更，不代替新版 Mac 驗收。

## Param 重設計：macOS 同版原生驗收

- [ ] 在同一 topic branch／commit 重建中央、Plugin、Python binding，驗證 Limited／Public、read_current、啟動覆寫與保存契約。
- [ ] 重跑來源 Param／Plugin／Python 測試、新版範例 smoke，以及乾淨 installed SDK 的 consumer／application／python 三個整合案例。

驗收步驟、環境要求及結果統一維護於 [Param 第四階段紀錄](src/vinc/param/README.md#param-redesign-stage-four)。
Windows／WSL 結果不能代替 Mac；先前 macOS SDK 通過紀錄仍保留，但不涵蓋本次 Param 變更。

<a id="osalposix-fd-建立與-spawn-的並行競爭窗口"></a>
<a id="osal-macos-標準串流與退出觀察"></a>
<a id="osal-macos-退出事件後的回收進度"></a>

## Windows／Linux：退出修復與測試修訂的原生補驗

**狀態（2026-11-03）：macOS 本輪工作已完成；同一份修復的 Windows／Linux 驗證尚未完成。**
Mac 結果與歷史失敗的區分見 [驗收總結](src/vinc/osal/MACOS_EXIT_VALIDATION.md#current-acceptance-status)。
既有 FD／spawn 的三平台紀錄不等於本輪退出修復已通過三平台，不能沿用為新變更的結案證據。

- [ ] Windows Debug／Release：建置同一份退出修復，執行共用退出進度契約、Process／OSAL 原生回歸及 App／Python／SDK 整合。
- [ ] Linux Debug／Release：完成上述同一範圍，驗證 Linux monitor／waitpid 路徑；記錄實際發行版、編譯器、標準函式庫及 runtime 相容範圍。
- [ ] 兩平台驗證修訂後的 `module.plugin`、`module.failure_order`、`app.plugin`：中央先保存的主要故障不被覆寫，失敗路徑先回收再刪檔，無法確認時保留檔案。
- [ ] 兩平台重跑修正後的 `examples.user_module`，確認 Plugin／UI 資源搬移、官方 Module Ready、C++／Python 範例與正常收尾。

每組紀錄需包含相同來源 commit／必要的工作樹差異、OS／CPU／工具鏈／Python 版本、
實際命令、通過／失敗／略過數與程序清理。各平台只執行適用的原生測試；
`osal.process_exit_macos` 是 Mac 專用，不能要求 Windows／Linux 執行，也不能用共用假後端測試代替原生證據。
保留失敗與重跑的不同輪次，不將定點補驗改寫為完整套件重跑。

契約及驗證主檔：

- [退出進度修復](src/vinc/osal/PROCESS_EXIT_PROGRESS.md)與 [FD／spawn 計畫](src/vinc/osal/FD_SPAWN_PLAN.md)。
- [Plugin 分類及順序測試](src/vinc/module_system/README.md#macos-plugin-classification-follow-up)。
- [範例部署測試](examples/userExampleModule/README.md#macos-deployment-fixture-gap)。

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
