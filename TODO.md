# TODO

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
- 此問題與 Tab 自動完成不同，也不依賴未來的 Vinc Log；目前的 OutputHub 模組輸出即可觸發。
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

<a id="ribosome-訊息模板補齊反序列化介面"></a>

## Ribosome deserialization API (resolved) / Ribosome 反序列化介面（已解決）

- [x] Generated messages provide both `deserialize(buffer)` and `deserialize(buffer, offset)` through `message::GeneratedMessage<Derived>`, returning `Result<void>`.
- [x] 生成訊息透過 `message::GeneratedMessage<Derived>` 提供單參數及帶 `offset` 的 `deserialize()`，皆回傳 `Result<void>`。

**Status checked on 2026-10-26:** the old missing-override/abstract-class description is obsolete.
Ribosome generates classes inheriting GeneratedMessage, which implements both BaseMessage overrides.
The old raw-helper/integer-return guidance no longer applies. Detailed contracts and historical validation
are maintained in [Message Storage: deserialization closeout](src/vinc/components/message_storage/README.md#解碼介面收尾2026-10-05).

**狀態核對於 2026-10-26：** 原有「缺少 override、生成類別仍為抽象類別」的描述已過時。
Ribosome 生成的 class 繼承 GeneratedMessage，由該基礎類別實作 BaseMessage 的兩個入口；
舊 raw helper 與整數回傳值的處理建議亦不再適用。完整契約與歷史驗證統一維護於上述 Message Storage 章節。

Existing regression cases cover BaseMessage virtual dispatch, concatenated images/nonzero offsets,
malformed input, and preservation of destination payload/binding/capacity and offset on failure.
This update checks source and test coverage only; it does not rerun builds/tests.
The separate [isolated-generation-script TODO](src/vinc/ribosome/README.md#待處理事項) remains open.

既有回歸案例涵蓋 BaseMessage 虛擬派送、連續映像／非零 offset、非法輸入，以及失敗時保留
目的 payload、binding、容量與 offset。本次僅核對實作及測試內容，未重新建置或執行測試。
獨立生成測試腳本的過時假設仍列於上述 Ribosome 待辦，本項關閉不代表該問題已修復。
