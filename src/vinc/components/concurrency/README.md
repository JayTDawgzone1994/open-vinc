# Concurrency：並行控制工具

`vinc::concurrency` 將通用 OSAL 能力組合成框架需要的同步機制。它不依賴 Param、Module、Context、
Message Bus 或中央服務，也不含任何原生平台 handle、平台標頭或條件後端。
Open-VINC 不將此 Mutex 暴露在 Module／Context 使用者介面。

本元件位於 `src/vinc/components/concurrency/`，由使用它的元件建立及管理生命週期，
不自行啟動背景執行緒或中央服務。原 `sync` 名稱已改為 `concurrency`；
搬移與後續可讀性整理皆保留原有行為。

## 程式碼閱讀順序

`InterprocessMutex::try_lock()` 先檢查同 thread 重入，再嘗試一次取得本地簿記鎖，
在同一範圍內呼叫 OSAL FileLock 的非等待式取得並交付 guard。失敗不建立本地名額，
成功保存 guard 時也不重新取得簿記鎖。

`lock_until()` 透過 `reserve_local_slot_until()` 保留本地 thread 名額，再等待 FileLock；
`PendingReservation` 負責失敗時的名額回收，`retain_file_guard()` 保存取得的原生 guard。
此等待路徑仍在取得／回收簿記鎖時使用一般 mutex，限制見下方待處理事項。

`unlock()` 先驗證持鎖 thread，再釋放 FileLock；成功後才清除本地名額並喚醒等待者。
測試依所有權、guard 生命週期、取消、FileLock 組合、thread 競爭及程序故障回收分成具名案例。

```text
Param／其他框架功能（尚未接線）
    → vinc::concurrency::InterprocessMutex
        → vinc::osal::FileLock
            → Windows／POSIX 原生檔案鎖
```

## InterprocessMutex

標頭 `<vinc/concurrency/interprocess_mutex.hpp>`，連結入口為 CMake alias `vinc::concurrency`，
實際 library target 為 `vinc_concurrency`。
此元件移自 OSAL；舊 `<vinc/osal/interprocess_mutex.hpp>` 不再保留。
它只呼叫 [FileLock 抽象](../../osal/FILE_LOCK.md)，附加以下規則：

- 一個取得操作先保留本地 thread 名額，再透過 FileLock 取得跨程序占用。
- 同一物件的同 thread 重入回報 `Busy`；其他 thread 可等待，`try_lock` 回報 `WouldBlock`。
- `try_lock` 遇到內部簿記競爭亦回 `WouldBlock`；同 thread 重入的 `Busy` 優先於此情況。
  底層 `std::mutex::try_lock()` 允許假性失敗，因此 `WouldBlock` 不證明另一個 guard 必定存在。
- 取得與釋放必須在同一 thread，guard 移動不改變持有者。
- 取得／釋放加入共享記憶體存取所需的屏障；資料提交與復原仍由上層負責。
- 本地名額失敗／取消／例外會釋放並喚醒等待者，不遺留假占用。

| 操作 | 行為 |
| --- | --- |
| `create(path)`／`open(path)` | 透過 FileLock 建立／開啟資源 |
| `try_lock()` | 成功回傳 `InterprocessMutexGuard`，不等待本地簿記鎖、本地或外部持有者 |
| `lock_until(deadline, token)` | 同一期限與取消訊號涵蓋本地等待及 FileLock 等待 |
| `guard.unlock()` | 同 thread 釋放；失敗保留 guard，成功後重複呼叫無作用 |
| `mutex.close()` | 關閉公開物件；既有 guard 保留資源 |

不承諾 FIFO、無飢餓或硬即時期限。逾時／取消不會搶走持有者的鎖。
不同物件即使 open 同一路徑，仍視為一般跨 handle 競爭，不偵測這種遞迴。
同一 Mutex 的取得操作可並行；close／移動／解構須與其他操作同步。
Guard 的移動／解構／unlock 也不可競爭。錯 thread 明確 unlock 回報 `InvalidArgument`，
原 thread 可繼續正常釋放。RAII 解鎖失敗（含錯 thread 解構）採 `std::terminate()`，
需要處理失敗時應明確呼叫 unlock。

同 thread 判斷使用程序本地 thread-local 標記的位址及 atomic pointer；不配置 registry，
也不把此位址放入共享記憶體或跨程序傳輸。所有權寫入仍在簿記鎖內完成，atomic 僅讓
`try_lock` 能在鎖外辨識自己的重入，不負責發佈受保護資料。編譯期要求 atomic pointer
為 lock-free，避免身分查詢本身隱藏另一把鎖；這不表示整個 Mutex 是 lock-free 演算法。
Guard／待完成取得必須在取得 thread 結束前釋放，thread 異常終止不自動復原。
OS 呼叫與排程仍可能延遲，不承諾單次 `try_lock` 的硬即時完成時間。

## 錯誤契約

所有可失敗操作使用 `vinc::Result<T>`／`vinc::Error`，不再提供本層 Error payload、
Result alias 或 `os_error` 欄位。`<vinc/concurrency/error.hpp>` 維護本層的
`ErrorCode`／`ErrorOperation`，對應 `ErrorDomain::Concurrency`。
使用 `error.is(ErrorCode::Timeout)`、`error.is_operation(ErrorOperation::Lock)` 判斷目前層，
並以 `vinc::error_to_string(error)` 取得完整診斷文字。

- 本地檢查失敗直接建立本層 Error；內部傳遞不重複包裝。
- FileLock 失敗透過 `wrap_error()` 轉成本層分類與操作，保留 OSAL 原因、操作及原生來源／數值。
- 轉接範圍為 FileLock 使用的 12 個分類；未知或非 FileLock 分類回報本層 `SystemError`，
  原始紀錄仍保留於原因鏈。OSAL 不宣告 InterprocessMutex 操作，也不依賴此庫。
- 固定容量原因鏈與截斷規則依 [common](../../common/README.md#bounded-cause-chains)，
  包裝不配置記憶體；文字格式化及既有 STL／配置器例外政策保持不變。
- 本地重入 `Busy`、錯 thread 明確解鎖 `InvalidArgument`、RAII 解鎖失敗 terminate、
  解鎖失敗時保留 guard／本地占用，以及逾時／取消順序皆維持。保留本地占用不代表
  所有原生解鎖失敗都能保證 native lock 仍被持有；此問題見待處理事項。

既有錯誤碼編號保留，已移除的服務／網路分類編號 8、10、13、15–21 不再使用。
原 `Operation` 改為 `ErrorOperation`：Open=1、Lock=2、Unlock=3 不變；
Create 由舊 wrapper 的 0 改為 4，0 現為 common 的 Unspecified。舊 wrapper 不是可沿用的
序列化格式；這次沒有新增錯誤傳輸協定。

`vinc_concurrency_errors` 是只依賴 `vinc::common` 的 metadata target，將 C++ 靜態表註冊至
建置時 catalog；不反向依賴 Mutex runtime，也不新增執行期註冊。
`vinc::concurrency` 透過 OSAL 傳遞 `vinc::errors` 的連結需求。
`detail/error.hpp` 的 FileLock 轉接 helper 只供本元件實作與測試使用；
`detail/error_catalog.hpp` 只供 catalog 整合與測試使用。
`detail/interprocess_mutex_state.hpp` 僅供本元件實作與受控測試使用，其他元件不得依賴。
測試 friend 的定義留在 `tests/`，只用於暫停簿記鎖；沒有正式 runtime hook。

## 資源與故障邊界

路徑、原生占用及程序退出清理由 FileLock 管理，限制見其文件。
擁有者仍須先確認全部參與者與 guard 已關閉，才移除鎖檔；不自動刪除或重建。

程序退出可解除占用，不表示寫到一半的資料已修復。程序仍存活但 thread 卡住，
不會被強制解鎖；個別 thread 非正常終止不在自動復原契約內。
上層應使用版本／A、B slot 等完整提交協定，且只在鎖內做短暫框架工作。
callback、Socket 等待、存檔與其他鎖等待應在鎖外。
本次重構未實作或接入 Param／Context，也未變更 Message Bus 同步。

## 驗證

`concurrency.errors` 驗證 12 個 FileLock 分類的轉接、既有錯誤編號、具 domain 的比對、
未知／非 FileLock 分類的 fallback、既有原因鏈與原生來源保留，以及最終 catalog 的文字查詢。
原生 PermissionDenied／ResourceExhausted 等難以穩定觸發的分類使用合成 Error；
未對實際 OS unlock 失敗進行故障注入，也未新增 RAII terminate 的死亡測試。

`concurrency.interprocess_mutex` 驗證本地 thread 互斥、非遞迴、錯 thread 釋放、guard
生命週期、本地與外部等待取消、FileLock 與 Mutex 直接交互競爭，以及多程序共享快照一致性。
持鎖程序 `_Exit`／OS 終止測試保留，確認上層組合未破壞回收行為。
受控簿記停頓案例確認 `try_lock` 在簿記鎖釋放前回 `WouldBlock`，以及分別經
`try_lock`／`lock_until` 取得的 owner 仍優先收到 `Busy`。停頓有測試專用的有界安全釋放，
斷言依回傳與釋放的先後判斷，不以短時間 sleep 猜測競爭窗口。另覆蓋原生 try 失敗後
再次取得、所有權換 thread，以及 try／wait 混合競爭下的互斥。
`osal.file_lock` 另外驗證原生 handle 所有權，特別確認 FileLock guard 可跨 thread 交付。
Linux／macOS 尚待原生驗證，未做效能量測。

2026-10-02：Windows x64／MSVC Debug／Release 建置成功，各 25 套 CTest 全通過，
包含分開的 FileLock 與跨程序互斥鎖測試。

2026-10-03：搬至 `components/concurrency` 並更新名稱後，Windows x64／MSVC
Debug／Release 建置成功，兩種組態的 `concurrency.interprocess_mutex` 與
`osal.file_lock` 均通過；本輪未重跑其餘測試，Linux／macOS 未驗證。

同日後續可讀性整理：Windows x64／MSVC Debug／Release 的 concurrency 函式庫與
測試 target 建置成功，兩種組態的 `concurrency.interprocess_mutex` 均通過。
本輪未重跑其他元件測試；原有測試情境、逾時與負載數值保留。

2026-10-05 錯誤遷移：Windows x64／MSVC 19.51.36257 的 Debug／Release 完整建置成功，
包含 `concurrency/error.hpp` 與 Mutex 公開標頭的獨立 include 編譯。
兩種組態各 **7/7** 相關 CTest 通過：`common.error`、`common.error_chain`、
`common.error_catalog`、`concurrency.errors`、`concurrency.interprocess_mutex`、
`osal.file_lock`、`sdk.consumer`。本輪未重跑全專案測試，也未驗證 Linux／macOS。
本輪前後的 Mutex 執行流程比對確認，除 Result 型別、操作 enum 及錯誤轉接呼叫外，
取得、等待、回滾與解鎖主體維持原樣；未修改 OSAL 實作。

2026-10-05 簿記競爭修正：Windows x64／MSVC 19.51.36257 Debug／Release 相關 target
重新建置成功，各 **3/3** CTest 通過：`concurrency.errors`、`concurrency.interprocess_mutex`、
`osal.file_lock`。本輪未執行全專案測試，Linux／macOS 未建置或執行。
原生 unlock 呼叫位置、失敗保留狀態及等待／取消政策未變更。

## 待處理事項

### 2026-10-05 重審：內部等待與故障驗證

- **解鎖失敗後的原生持鎖保證待釐清（阻擋下述解鎖重構）：**
  目前 FileLock 失敗分支保留 guard／本地 occupied，但未查證原生鎖仍被持有。
  前次提案的「失敗保留兩層所有權」不能由這些本地狀態推導成立；原生 unlock 失敗
  不應一概視為可安全繼續讀寫受保護資料。Windows UnlockFileEx 與 Linux／macOS flock
  文件未提供涵蓋全部失敗的持鎖不變保證；此為契約證據缺口，尚未重現失敗後已解鎖的情境。
  建議區分尚未呼叫 OS 的本地拒絕，以及原生失敗後無法確認的鎖狀態，再決定故障停用、
  清理與重試政策；這些仍是提案，尚未修改原有公開契約或實作。下述測試不得以注入前置失敗
  作為所有原生失敗均保留持鎖的證明。下層追蹤見 [OSAL 待辦](../../osal/README.md#待處理事項)。
  try_lock 的內部簿記競爭回報 WouldBlock 已另行完成，見下項與驗證紀錄；
  原生解鎖移出鎖範圍及失敗狀態政策暫緩，已列入根目錄 [TODO](../../../../TODO.md)。
- **已修正：try_lock 的簿記等待（2026-10-05）。**
  原本入口、rollback 與 retain 會阻塞式取得 `local_mutex`。目前改為一次 try 取得，
  失敗不保留名額，成功在同一鎖範圍交付 guard；同 thread `Busy` 優先。
  已補受控簿記停頓及原生失敗後再取得測試，結果見上方驗證。
- **仍待處理：lock_until／unlock 的內部簿記等待。**
  `reserve_local_slot_until()` 仍先取得一般 mutex 才檢查 deadline／stop token；
  取得成功後保存 guard、失敗回收名額也需此鎖。`InterprocessMutexGuard::unlock()`
  仍在持有 `local_mutex` 時呼叫 FileLock 原生解鎖，因此已取消的等待者也可能先等待
  簿記鎖。這不是已確認的互斥失效或死鎖，但內部 mutex 等待不能由 stop token 喚醒。
  **暫緩提案：** 評估將原生解鎖移出 `local_mutex`，期間保持 occupied，成功後才釋放名額；
  須先釐清上項原生解鎖失敗契約，再決定等待、回滾與故障狀態，這輪不實作。
- **已確認測試缺口：原生解鎖失敗與 RAII 終止未驗證。**
  目前 `tests/error_tests.cpp` 的合成錯誤只驗證轉接，不會進入 Mutex 的失敗持鎖流程。
  建議補受控 FileLock 解鎖失敗，確認 guard、本地名額、其他等待者與重試成功後喚醒行為；
  以獨立子程序驗證錯 thread 析構及 RAII 解鎖失敗確實終止。測試注入方式尚待設計，
  不新增正式 runtime hook，也不改變既有 terminate 或 STL 例外政策。
- **已確認診斷缺口，根因仍待驗證：** `tests/interprocess_mutex_tests.cpp` 的
  send_token／receive_token 只提供泛用文字，缺少案例／握手階段、子程序身分與 completion
  status／error，無法充分定位下方既有間歇性逾時。建議先補診斷及有界失敗清理，
  再受控重現；不以放寬 timeout 或單次通過作為修復。

初次重審時尚未修改程式或測試，以當時既有 Windows 建置產物執行
`concurrency.errors`、`concurrency.interprocess_mutex`、`osal.file_lock`，Debug／Release
各 **3/3** 通過；未重新建置、未重現間歇性逾時，Linux／macOS 未驗證。
後續只實作已授權的 try_lock 修正，建置及測試結果見上方新紀錄；其餘待辦保留。
OSAL fd 並行建立／spawn 的問題依使用者決策繼續暫緩，見 [OSAL 待辦](../../osal/README.md#待處理事項)。

### 測試工具的間歇性逾時

2026-10-03 元件目錄整理的首次完整 Debug CTest 中，此測試在另一個 Release 建置
同時執行時失敗，輸出 `OSAL code=5`（Timeout）及 `receive token`。
停止並行建置後，單獨重跑通過（0.30 秒），後續 Debug／Release 完整測試亦各 26／26 通過。
本次沒有改動鎖與 Pipe 的實作邏輯，
也沒有放寬測試期限；目前不足以將原因歸於系統負載，仍是待驗證的間歇性逾時。
若再次出現，應先補充 fixture 階段與子程序診斷，定位是哪次 Pipe handshake／等待失敗。

### 已修正：測試的 UTF-8 路徑轉換（2026-10-05）

跨程序鎖測試改用 [OSAL 路徑 helper](../../osal/README.md#utf-8-與原生路徑)，
移除 char-to-char8_t 轉型及本地重複轉換。非法路徑經 Result 回到既有測試斷言邊界。
Windows Debug／Release 含本套件的相關測試各 20/20 通過。
Linux／macOS 尚未驗證。上方間歇性逾時仍為獨立待辦。
