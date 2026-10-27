# OSAL FileLock

`<vinc/osal/file_lock.hpp>`，連結 `vinc::osal`。本元件只封裝專用鎖檔的排他鎖與
原生資源生命週期，不認識 Module、Param、Context 或上層 Mutex。
框架的「同一 thread 取得／釋放、非遞迴」共享鎖另由以下元件提供，目前已不再包裝 FileLock：
[共用同步層 InterprocessMutex](../components/concurrency/README.md)。

## 原生能力與範圍

- Windows 使用 `LockFileEx` 鎖住專用檔案的第一個 byte（可超出空檔案 EOF）；
  Linux／macOS 使用 `flock` 排他鎖。每個獨立 `open` 都必須使用同一資源。
- 本介面只承諾合作使用者之間的排他協調，不承諾一般檔案 I/O 或 memory mapping
  會被鎖強制阻擋，也不提供任意 byte-range 或共享讀鎖。
- 鎖檔是專用同步資源，並非資料檔；不依據「檔案存在」推斷持鎖、不寫入 Param 資料。
- 原生持有者是 handle／open file description；**不是 thread**。Guard 可經同步交付後
  在同一 process 的另一條 thread 解鎖或解構。不可跨 process 移交 C++ guard。
- FileLock 不附加共享記憶體資料布局、提交或復原協定；Mutex 的記憶體屏障在上層。

## 操作契約

| 操作 | 行為 |
| --- | --- |
| `FileLock::create(absolute_path)` | 排他建立，不建立父目錄；已存在為 `AlreadyExists` |
| `FileLock::open(absolute_path)` | 只開啟既有資源；找不到為 `NotFound` |
| `try_lock()` | 一次原生非阻塞嘗試，其他 handle 占用為 `WouldBlock` |
| `lock_until(deadline, stop_token)` | 等待另一 handle 釋放，回傳 guard、`Timeout`、`Cancelled` 或原生錯誤 |
| `guard.unlock()` | 在任意 thread 釋放；成功後重複呼叫無作用 |
| `file.close()` | 關閉公開物件，已有 guard 保留原生資源與占用 |

每個 handle 同時只容許一個取得操作或一個 guard。**同一 handle 已有待完成取得或 guard
時，再次取得一律回報 `Busy`，與是哪條 thread 無關**；不悄悄重入、不產生兩個可互相解鎖的 guard。
內部 gate 只保護此資源簿記，沒有 thread owner 判斷或 Mutex 排程政策。
需要本地多 thread 等待同一 mutex，應使用上層 `InterprocessMutex`。

空／已關閉物件回報 `Closed`。期限採單調時鐘；已取消優先於已到期；已到期不嘗試取得。
原生忙碌時每次最多休眠 5 ms，可由取消喚醒，不忙碌旋轉、不建立額外 thread。
成功原生嘗試是取得的判定點；同時發生取消可能仍回傳 guard，但不會回報錯誤卻隱藏持鎖。
逾時／取消只結束等待，不強制解鎖。OS 呼叫與排程可能延遲，不保證硬即時期限或 FIFO。

FileLock、guard 都可移動不可複製。Guard 的移動、解鎖、解構須由呼叫端同步；
公開 FileLock 的 close、移動、解構不得與該物件其他操作競爭。
解構／覆蓋式移動賦值會 RAII 解鎖；明確 `unlock` 失敗保留 guard，可重試。
RAII 解鎖失敗採 `std::terminate()`，不默默丟棄所有權。錯誤保留 OSAL 分類、操作與原生原因；
OSAL 不自行印日誌。配置記憶體等 C++ 例外仍可能傳播。

## 建立、退出與清理

路徑必須位於既有私有目錄、本機檔案系統。本版不支援 NFS／SMB，也不自動辨識全部掛載類型。
後端拒絕最末層 symlink／reparse point 與非一般檔案；父路徑不得由不可信任程序替換。
Windows 建立時使用目前使用者 DACL、非繼承 handle、不分享刪除；POSIX 使用 `0600`
（受 umask 影響）及 `O_CLOEXEC`，開啟時要求目前有效 UID 所有且無 group／other 權限。
本版不支援跨帳號分享，亦不是同帳號惡意程序的安全沙箱。

所有參與者各自 open，不傳遞／複製原生 handle，也不可 fork 後繼續使用既有 FileLock。
額外繼承的 file description 可能保持死去持有者的占用或解除其鎖。
在這個契約下，程序退出後由 OS 回收占用；**不提供統一 OwnerDied 或資料完整性判斷**。
程序仍活著但應用不釋放 guard 時，不會強制回收；OS 清理也沒有硬即時期限。

create 成功後才交付路徑。一般工作資源可每輪使用新名稱；用於單例 admission 的鎖檔
則保留同一個檔案身分、重複 open，不能在不同啟動之間自行刪除重建。close／解構不刪除鎖檔。
擁有者必須停止新的 open，確認全部參與者、pending 操作與 guard 都關閉，再移除檔案。
尤其 POSIX 不可在仍有 handle 時刪除重建同一路徑，否則會產生兩個不同鎖資源。

## 驗證

`osal.file_lock` 僅連結 OSAL，涵蓋獨立開啟競爭、同 handle 忙碌、跨 thread guard
交付／解構、pending 取消、期限、guard 保留 handle，以及真實子程序退出／強制終止後回收。
目前三平台原生結果見 [OSAL 平台驗證](README.md#current-platform-validation)：
Windows Debug／Release 的 23 項回歸、macOS 的 OSAL／OutputHub 24 項回歸，
以及 Ubuntu 24.04／WSL 2 的四項回歸均包含 `osal.file_lock`。
Linux 的 descriptor 修正另有真實 `EMFILE`／回滾追蹤及故意破壞清理的負向對照。

2026-10-02：Windows x64／MSVC 完整專案 Debug／Release 各 25 套 CTest 全通過。
另以 `src/vinc/osal` 單獨配置、建置並通過 `osal.file_lock`，確認不依賴 VINC 上層同步庫。

參考：[Windows LockFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex)、
[Linux flock](https://man7.org/linux/man-pages/man2/flock.2.html)、
[Apple flock](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/flock.2.html)。
