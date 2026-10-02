# 共用同步層

`vinc::sync` 將通用 OSAL 能力組合成框架需要的同步機制。它不依賴 Param、Module、Context、
Bus 或中央服務，也不含任何原生平台 handle、平台標頭或條件後端。
Open-VINC 不將此 Mutex 暴露在 Module／Context 使用者介面。

```text
Param／其他框架功能（尚未接線）
    → vinc::sync::InterprocessMutex
        → vinc::osal::FileLock
            → Windows／POSIX 原生檔案鎖
```

## InterprocessMutex

標頭 `<vinc/sync/interprocess_mutex.hpp>`，連結 `vinc::sync`。
此元件移自 OSAL；舊 `<vinc/osal/interprocess_mutex.hpp>` 不再保留。
它只呼叫 [FileLock 抽象](../osal/FILE_LOCK.md)，附加以下規則：

- 一個取得操作先保留本地 thread 名額，再透過 FileLock 取得跨程序占用。
- 同一物件的同 thread 重入回報 `Busy`；其他 thread 可等待，`try_lock` 回報 `WouldBlock`。
- 取得與釋放必須在同一 thread，guard 移動不改變持有者。
- 取得／釋放加入共享記憶體存取所需的屏障；資料提交與復原仍由上層負責。
- 本地名額失敗／取消／例外會釋放並喚醒等待者，不遺留假占用。

| 操作 | 行為 |
| --- | --- |
| `create(path)`／`open(path)` | 透過 FileLock 建立／開啟資源 |
| `try_lock()` | 成功回傳 `InterprocessMutexGuard`，不等待本地或外部持有者 |
| `lock_until(deadline, token)` | 同一期限與取消訊號涵蓋本地等待及 FileLock 等待 |
| `guard.unlock()` | 同 thread 釋放；失敗保留 guard，成功後重複呼叫無作用 |
| `mutex.close()` | 關閉公開物件；既有 guard 保留資源 |

不承諾 FIFO、無飢餓或硬即時期限。逾時／取消不會搶走持有者的鎖。
不同物件即使 open 同一路徑，仍視為一般跨 handle 競爭，不偵測這種遞迴。
同一 Mutex 的取得操作可並行；close／移動／解構須與其他操作同步。
Guard 的移動／解構／unlock 也不可競爭。錯 thread 明確 unlock 回報 `InvalidArgument`，
原 thread 可繼續正常釋放。RAII 解鎖失敗（含錯 thread 解構）採 `std::terminate()`，
需要處理失敗時應明確呼叫 unlock。

本層使用自己的 `Error`／`Operation`，沿用 OSAL 的錯誤分類並以 `os_error`
保留原始原因；OSAL 不宣告任何 InterprocessMutex 操作，也不依賴此庫。

## 資源與故障邊界

路徑、原生占用及程序退出清理由 FileLock 管理，限制見其文件。
擁有者仍須先確認全部參與者與 guard 已關閉，才移除鎖檔；不自動刪除或重建。

程序退出可解除占用，不表示寫到一半的資料已修復。程序仍存活但 thread 卡住，
不會被強制解鎖；個別 thread 非正常終止不在自動復原契約內。
上層應使用版本／A、B slot 等完整提交協定，且只在鎖內做短暫框架工作。
callback、Socket 等待、存檔與其他鎖等待應在鎖外。
本次重構未實作或接入 Param／Context，也未變更 Bus 同步。

## 驗證

`sync.interprocess_mutex` 驗證本地 thread 互斥、非遞迴、錯 thread 釋放、guard
生命週期、本地與外部等待取消、FileLock 與 Mutex 直接交互競爭，以及多程序共享快照一致性。
持鎖程序 `_Exit`／OS 終止測試保留，確認上層組合未破壞回收行為。
`osal.file_lock` 另外驗證原生 handle 所有權，特別確認 FileLock guard 可跨 thread 交付。
Linux／macOS 尚待原生驗證，未做效能量測。

2026-10-02：Windows x64／MSVC Debug／Release 建置成功，各 25 套 CTest 全通過，
包含分開的 `osal.file_lock` 與 `sync.interprocess_mutex`。
