# OSAL Shared Memory

`<vinc/osal/shared_memory.hpp>` 提供具名、固定大小的跨程序原始記憶體，連結 `vinc::osal`。
目標為 Windows、Linux、macOS 的 64 位元程序。此元件沒有 Ring Buffer、物件池、
atomic 協調、commit、訊息格式或 Message Bus 政策，也不啟動背景執行緒。

## API 與所有權

| 操作 | 契約 |
| --- | --- |
| `SharedMemory::create(name, byte_size)` | 排他建立；同名回報 `AlreadyExists`，不沿用、清空或調整舊物件 |
| `SharedMemory::open(name, access)` | 只開啟既有物件；找不到回報 `NotFound` |
| `memory.map(byte_size, access)` | 從 offset 0 映射指定前綴；唯讀或讀寫，沒有 executable／copy-on-write |
| `mapping.bytes()` | 回傳本機唯讀 byte span；空／已解除的 mapping 回傳空 span |
| `mapping.writable_bytes()` | 讀寫映射才成功，唯讀回報 `PermissionDenied`，空 mapping 回報 `Closed` |
| `mapping.unmap()` | 解除本機映射；重複呼叫成功，所有借用的 span／pointer 失效 |
| `memory.close()` | 釋放此 handle 物件的所有權；重複呼叫成功，既有 mapping 仍有效 |
| `SharedMemory::unlink_name(name)` | POSIX 移除名稱；不存在回報 `NotFound`。Windows 回報 `Unsupported` |

兩種物件皆可移動、不可複製。Mapping 在程序內保留底層 handle 的共享所有權，所以
`close()` 不會破壞既有映射；最後一位持有者釋放才關閉原生 handle。
這個本機引用計數不是共享記憶體中的跨程序引用計數。
移動建構轉移所有權，不搬動映射；移動賦值會先釋放目標原有資源，其原有 spans 失效。

正常析構會盡力解除映射／關閉 handle，不拋出例外；需要錯誤結果時使用明確的
`unmap()`／`close()`。存在其他 mapping 時 `close()` 僅釋放本機引用，不會提前關閉底層 handle。
POSIX 的 `close()` 錯誤後不重試可能已被重用的 fd。原生清理失敗不能宣稱資源已回收。

同一物件的使用、移動、關閉與析構由呼叫端同步。不同物件可以各自操作，但資料讀寫仍需
呼叫端協調。OS 操作是同步呼叫，不提供硬即時期限或取消；C++ 記憶體配置失敗可能拋出
`std::bad_alloc`。一般 OS 失敗以 `vinc::Result<T>`／`vinc::Error` 回報，包含操作與原生錯誤碼。

## 名稱、大小與權限

- 邏輯名稱限 1～24 個 ASCII 英數字、`_` 或 `-`；不接受 NUL、斜線或平台命名空間前綴。
  Windows 映射到 `Local\ov_<name>`，POSIX 映射到 `/ov_<name>`，保留 macOS 名稱長度餘裕。
- 上層負責產生不重用的名稱，並在 create 成功與初始化完成後才交付給其他程序。
  排他建立不是使用者資料的 Ready 通知；POSIX 建立名稱與設定大小也不是單一原子步驟。
- Windows 使用目前程序使用者的 DACL 與非繼承 handle，限相同登入工作階段；不使用 `Global\`。
  POSIX 使用 `0600`，仍受 umask 影響；`shm_open` 的 descriptor 設有 close-on-exec。
  不支援另行配置跨帳號分享；此能力不是抵抗同帳號惡意程序的沙箱。
- 新建區域初始內容為零，size 必須大於零且可由本機大小型別表示。沒有 resize、部分 offset 映射、
  native handle 接管或一般檔案映射 API。
- `map()` 的長度是要求存取的範圍，不是查詢原始邏輯容量。create 回傳的 handle 保留建立大小，
  超出即拒絕；POSIX 開啟的 handle 另外以 `fstat` 檢查目前大小。
- **Windows 重新 open 後，使用 MapViewOfFile 的原生範圍檢查，可能允許存取最後一頁的取整尾端。**
  OSAL 不加隱藏格式標頭，也不把頁面大小當成訊息長度。開啟端所需長度及訊息界線由可信上層
  描述交付與驗證；本 API 不保證辨識每個「大於原建立長度、但仍在取整頁面內」的要求。
- 建立／映射成功不保證所有頁面已在實體記憶體內，也不承諾無缺頁或預留所有後續存取成本。
  POSIX backing store 耗盡、外部程式截短物件等情況，可能在存取時產生 SIGBUS，無法透過 span
  的讀寫回傳 `Result`。本 API 不截短既有物件；容量預算及外部程序行為需要上層管理。

## 跨程序位址與同步

兩個程序的映射位址不必相同。span 是本程序的借用視圖，不能當成跨程序資料傳送。
共享內容若需要關聯其他位置，應由上層使用已驗證的 offset；OSAL 不在 bytes 中建立 C++ 物件。

Shared Memory 不等於同步：此元件沒有 lock、atomic、Ready／Published 或讀者計數。
資料發布、記憶體順序、同時讀寫與跨程序物件生命週期，必須使用另行定義並驗證的同步協定。
普通變數、`volatile` 或「已傳送一則通知」不會由此 API 自動變成資料安全保證。
`unmap()`／`close()` 也不是提交操作，不需要也不提供以 flush 代替同步的功能。

## 名稱清理與程序退出

- Windows 最後的 handle／view 引用釋放後回收物件。此實作的 mapping 同時保留 handle。
  不提供假的跨平台 unlink 行為。
- POSIX 析構只關閉本機資源，不自動 `shm_unlink`。指定的建立者／管理者負責明確移除名稱。
  名稱移除後新程序不能再用舊名稱 open，但已開啟的 handle 與 mapping 仍然有效。
- unlink 後重建同名物件是另一份儲存，舊 mapping 不會被換成新物件。上層不要重用名稱，
  也不要讓延遲清理誤刪之後的物件；unlink 操作依名稱，不是依既有 handle 的身分。
- 程序強制退出時 OS 釋放其 handle／mapping；其他程序的存活引用不受影響。
  POSIX 名稱可能遺留，需要上層管理者清理。本元件不監看程序，不自行掃描或刪除名稱。

## 使用示意

```cpp
using namespace vinc::osal;

constexpr std::size_t mapping_bytes = 4096;
auto memory = SharedMemory::create("example_unique_01", mapping_bytes);
if (!memory) {
    // Handle memory.error() before attempting to map the object.
} else {
    auto mapping = memory->map(mapping_bytes, SharedMemoryAccess::ReadWrite);
    if (mapping) {
        auto bytes = mapping->writable_bytes();
        // The caller coordinates initialization, access and name handoff.
    }
}
// On POSIX, the designated owner must also call unlink_name at the proper time.
```

另一程序使用 `open(name, ReadOnly)` 與 `map(length, ReadOnly)`。不可把只讀存取當成
資料不會被其他寫者修改的承諾。映射權限限制本 view；寫入者可能仍持有另一個可寫 view。

## 驗證

`osal.shared_memory` 使用獨立 OSAL 子程序；測試以 Pipe 進行分階段交接，不在 payload 上輪詢
未同步的普通旗標。涵蓋：

- 非法名稱／大小／權限、同名排他建立、原生錯誤資訊、零初始化、前綴映射。
- handle 關閉後 view 存活、多 view、移動與重複釋放。
- 不同映射位址的 2 MiB＋17 bytes 跨程序讀寫。
- 子程序蓄意對唯讀映射寫入，由 OS 拒絕；此故障測試不在主測試程序執行。
- 讀取者異常退出、強制終止，以及建立者未執行解構就退出後的映射存活。
- Windows 名稱隨最後引用消失、handle churn；POSIX unlink 後存活與同名新物件隔離。

2026-10-01：Windows x64、MSVC 19.51.36257、SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2，
完整專案 Debug／Release 建置成功，各八套 CTest 全數通過，包含新增的 `osal.shared_memory`。
此次完整執行紀錄位於本機 `build/shared-memory-verification.log`（建置產物，不納入版控）。
目前三平台原生結果見 [OSAL 平台驗證](README.md#current-platform-validation)：
Windows Debug／Release、macOS Debug 與 Ubuntu 24.04／WSL 2 Debug 的已記錄套件
均包含 `osal.shared_memory`。Linux 另追蹤到真實 `EMFILE` 後的名稱回滾，
並以隔離副本驗證漏清理／誤刪既有名稱會被測試攔下。
這些結果不屬於效能量測，也不增加跨程序 atomic 的契約。
