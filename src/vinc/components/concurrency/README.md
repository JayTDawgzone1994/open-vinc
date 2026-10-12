# Concurrency：共享記憶體跨程序鎖

本元件以 OSAL SharedMemory 與 SharedWord 組合框架內部的跨程序互斥鎖。
不依賴 Module System、Param、Context 或 Message Bus；不建立背景執行緒。
Module 的正常服務仍經繼承的 Context，不能把這個工具當成可自行取得框架權限的入口。
它是合作式同步協定，不是抵抗同帳號惡意 native 程序的安全沙箱。

## 實作範圍（2026-10-08）

已替換原 FileLock 後端，保留 `InterprocessMutex`／`InterprocessMutexGuard` 名稱及
`vinc::concurrency` target。舊的檔案路徑 create/open 不提供相容 overload。
中央監督、MID 交付、Host 私有 Context 接線、停止取消與資源世代已接入 Module System。
契約見 [Module System INSTANCE.md](../../module_system/INSTANCE.md)；Param 與 TOML 尚未實作。
工具單元測試的父程序扮演監督者，整合測試另使用實際 Manager／Host／Plugin。
OSAL FileLock 保留其獨立功能；其原生 unlock 失敗契約並未因本輪而修復。

## 相依與實作入口

```text
Module System 框架服務／監督者
    → concurrency：互斥、發布位元、受控接管
        → OSAL SharedMemory：名稱、映射、原生資源
        → OSAL SharedWord：原生 64-bit SC 原子操作
        → common：Result／Error、OwnerId 與編號邊界
```

- `include/vinc/concurrency/interprocess_mutex.hpp`：鎖、Guard 與一般操作。
- `src/interprocess_mutex.cpp`：具名的資源建立／開啟、映射、布局接合 helper，原子取得與釋放。
- `detail/mutex_recovery.hpp`：指定中央監督接線，不由 SDK 總標頭直接引入。
- `detail/interprocess_mutex_state.hpp`：僅供元件實作與布局測試，不是接線介面。

## 身分與共享布局

共用數值契約位於 `<vinc/owner_id.hpp>`：

| 值 | 意義 |
| --- | --- |
| 0 | 沒有持有者 |
| 1 ～ 2^63−2 | Module 可用的 MID／OwnerId |
| 2^63−1 | 中央專用 OwnerId，不發給 Module |

`MID` 仍為 uint64_t，Module 的正式範圍在 `mid.hpp` 宣告，中央發配器在源頭拒絕耗盡。
Concurrency 只認識 OwnerId，不反向 include Module 標頭、不維護 MID registry。
同一程序在同一資源世代必須使用同一 OwnerId；死亡 OwnerId 不可再發配。

控制區為原生同布局的 24 bytes：magic(u64)、layout version(u32)、bytes(u32)、SharedWord(u64)。
control 高 63 bits 為 owner，低 1 bit 為發布選擇；空閒 word 可以是 0 或 1。
不另有容易與 owner 更新脫節的 locked flag。布局版本目前為 1。

建立者先完成物件初始化，再把名稱交付給其他程序。open 不初始化，會檢查 magic／版本／大小。
禁止其他程序在初始化期間自行猜名稱 open；格式驗證不構成並行初始化協定。
共享區每次中央啟動使用全新名稱，不以名稱重用接管舊世代。名稱由呼叫端提供；Module System 正式接線使用 Instance 發配。

控制區一律 ReadWrite：現有 OSAL Windows atomic load 使用 Interlocked RMW。
這不代表參數 payload 必須可寫；Param 資料權限由上層另行設計。
跨程序控制欄位只經 SharedWord 存取，不使用 std::atomic 的程序內 fallback 或 wait/notify。
同機、64-bit、相容 native layout／原子後端是前提；未承諾跨架構／跨版本持久 ABI。

## API 與生命週期

| 操作 | 契約 |
| --- | --- |
| `create(name, owner)` | 排他建立共享區，初始發布位元 0；name 遵守 OSAL SharedMemory 規則 |
| `open(name, owner)` | 開啟已初始化區域，驗證 owner 與布局，不建立不存在資源 |
| `try_lock()` | 一次本地保留及共享 CAS 嘗試；競爭回 WouldBlock，不循環忙等 |
| `lock_until(deadline, stop)` | 反覆嘗試與可取消的本地休眠，成功回傳 Guard |
| `owner()` | 原子觀察持有者，僅供診斷，不證明存活或允許回收 |
| `guard.publication()` | 持鎖時取得不透明的選擇位元 |
| `guard.unlock()` | 同一 CAS 清除 owner 並保留選擇位元；成功後重複呼叫無作用 |
| `guard.publish_and_unlock()` | 同一 CAS 清除 owner 並翻轉選擇位元；不是兩次獨立 store |
| `guard.recovered()` | 此 Guard 是否由中央死亡接管入口取得 |
| `mutex.close()` | 放棄此公開 handle；既有 Guard／等待保留映射，不解除其占用 |

Guard 只能在取得它的 thread 釋放。移動不改變 thread 所有者；同一物件的移動／析構不可競爭。
同一 mutex handle 的同 thread 重入回 Busy，其他 thread 競爭回 WouldBlock。
不同 handle 不偵測 thread 重入；仍會競爭同一共享 control，不會建立兩個持有者。

本地 thread-affinity 只用程序內 atomic TLS token，不放入共享區，也沒有共用簿記 mutex。
一般操作不掃描 Module、不查 PID、不自行搶走活著的持有者。個別 thread 異常退出而程序
仍存活時，不承諾自動回收；監督回收依據必須是整個程序確定退出。

Guard 析構／覆蓋式 move assignment 只做普通解鎖，不自動發布候選內容。
映射由本地共享所有權保持，close/move 必須與對同一公開 mutex 物件的呼叫同步。
close 不表示其他程序退出，也不自動移除 POSIX 名稱；指定建立者須在安全時機 unlink。
create 失敗／例外離開時，對本次新建且未交付的名稱做 best-effort unlink；不移除 open 的資源。
unlink 本身失敗可能留下名稱，不能宣稱所有清理都已成功。

## 等待、取消與成本

`config::concurrency_retry_interval` 預設 5ms，僅為重試間隔；沒有持鎖時間上限或強制清鎖。
每個等待呼叫使用自己的本地 condition_variable_any／mutex 接收 stop_token，
不持有共享所有權或共用簿記鎖休眠。停止訊號可以喚醒本地休眠。
等待工具的建立可能配置／拋出標準庫例外；不改變全專案的例外政策。
正常取得成功後到釋放的核心路徑不配置、不呼叫使用者 callback。

檢查順序為 Closed、Cancelled、Timeout，再嘗試取得；已過期不嘗試。
檢查後同時發生取消／到期時，成功 CAS 仍可交付 Guard，不回報失敗卻藏住占用。
本版釋放不跨程序喚醒等待者，其他人最多等到下一次排程重試。
OS 排程與標準庫等待不是硬即時；不承諾 FIFO、公平性或無飢餓。
整體是 mutex，不能因 CAS 是原子操作而稱為 lock-free 演算法。

## 發布與資料責任

Concurrency 只維護一個選擇位元，不擁有 A/B payload、版本、timestamp 或 Param schema。
上層把它解釋成 A/B 時，所有讀者必須持同一把鎖，複製完才釋放；callback 在鎖外執行。

1. 取得鎖時，選擇位元仍指向上次完整版本。
2. 只修改非作用中的候選 slot，不破壞有效 slot。
3. 普通解鎖或析構保留原位元，候選更新作廢。
4. 明確 publish_and_unlock 的 SC CAS 同時發布完整候選與解除占用。
5. 在 CAS 前死亡：中央接管時保留舊位元；CAS 後死亡：新位元與新資料已發布。

新舊 payload 的寫入順序由 OSAL SC 原子操作同步。
這不是「一般原地修改可以自動回滾」；若使用者元件直接修改有效資料，需自己提供恢復協定。
再次重用候選 slot 前，上層必須完整建立候選內容，不能把前一次半成品當成有效起點。
版本號是上層完整快照的欄位，選擇位元本身不是單調版本或更新通知。

## 受控死亡接管

`detail::MutexRecovery::acquire_after_exit(mutex, dead_owner)` 僅供中央監督整合：

- 呼叫端必須已由 OSAL 確认該程序退出；timeout、heartbeat 失聯、送出 kill 都不足以證明。
- mutex 綁定須為 central_owner；不能以此入口回收中央，也不能回收 no_owner。
- 核對目前 owner 後，以 CAS 將 dead_owner 改成 central_owner，保留發布位元。
- 成功回傳 recovered Guard，讓中央在排他範圍丟棄候選或執行上層必要清理。
- 沒有相符占用或競爭回 WouldBlock；不任意重試並搶其他 owner。
- 保留正常 thread-affinity 規則，同 handle 重入回 Busy。
- 接管不是資料已修復的證明，不能直接讓一般 Module 偽裝中央呼叫。

不重用死亡 MID 是避免延遲回收誤認新持有者的前提。普通取得遇到空閒位元繞回時，
CAS 成功仍只取得當下空閒狀態；資料必須在取得後依當下選擇位元讀取，不靠舊觀察。
中央死亡後，整個資源世代作廢，舊 Module 應由獨立監督退出；新中央不接管舊鎖。
ModuleHost 已將 watchdog 故障／停止接入取消訊號；本工具本身不自行判斷中央存活。

## 錯誤與違約

預期失敗使用 common Result／Error，保留舊錯誤碼，新 InvalidLayout=23。
操作碼新增 InspectOwner=5、Recover=6、Publish=7。共享記憶體失敗包成本層 Error，
保留 OSAL 原因鏈及 native 診斷。錯誤 catalog 仍是 C++ 靜態表，沒有 runtime registry。

明確錯 thread unlock/publish 回 InvalidArgument 並保留 Guard，原 thread 仍可釋放。
錯 thread 析構屬 programmer error，走 contract_violation。
已取得 Guard 的 shared owner 若被非法更動，釋放 CAS 失敗屬嚴重同步契約破壞，
走 contract_violation，不能假裝釋放成功並繼續讀寫。這不是一般 native unlock 失敗。
合法釋放不使用 FileLock，因此沒有原本「OS 解鎖錯誤後 RAII 再次 terminate」的路徑。
OS mapping 被外界截短／破壞等記憶體故障，仍不保證能轉成 Result。

## 驗證

本輪新增／重寫：

- concurrency.errors：共享資源錯誤轉接、原因鏈、穩定編號與靜態 catalog。
- concurrency.interprocess_mutex：create/open、owner 邊界、布局版本、Guard lifetime/move、
  同 thread 重入／錯 thread 明確釋放、取消／逾時、多 thread／多程序 A/B 快照。
- 故障子程序：半筆候選、完整未提交候選、提交後立即退出、OS 終止、錯 thread 析構。
  Pipe handshake 定位持鎖階段；父程序先 wait 確认退出，再驗證未回收前仍被占用及受控接管。
- 延遲重複接管不得清掉新的 live guard，新世代資源可在舊世代仍存在時獨立操作。
- module.mid：實際 Manager 共用的 allocator helper，並行發配最後合法 MID、保留值及耗盡不回繞。

2026-10-08 Windows x64／MSVC：Debug、Release 相關 target 建置成功，各 **6/6** 通過：
`concurrency.errors`、`concurrency.interprocess_mutex`、`module.mid`、`module.process`、
`osal.shared_memory`、`osal.file_lock`。Debug 16.19 秒，Release 15.10 秒。
Release SDK 公開標頭獨立編譯成功，`sdk.consumer` **1/1** 通過。
以上為第一輪驗證；第二輪 Windows Debug／Release 全專案 61 套均已取得通過結果，
包含外部 SDK consumer，詳見 [Module System 驗證](../../module_system/INSTANCE.md)。
Linux／macOS 新鎖仍須各自原生驗證；
之前 Linux 框架建置／Message Bus 成功不代表這份新鎖已驗證。

## 待處理事項

- **第三輪尚未實作：** Param A/B 儲存、完整快照驗證、callback、版本補查與 TOML；
  本輪只有 A/B 測試 payload，不是正式 Param。
- **平台驗證：** Linux／macOS 執行本輪互斥與故障套件，包含不同映射位址。
  同一工具鏈／布局的子程序測試不構成任意 ABI 相容保證。
- **OSAL 獨立待辦：** FileLock 原生解鎖失敗與 POSIX fd 建立/spawn 競爭仍由 OSAL 追蹤。
  新 Concurrency 已無 FileLock／共享本地簿記鎖，不再套用原先的解鎖移出簿記鎖提案。
- **清理限制：** 失敗初始化的 POSIX 名稱僅 best-effort 移除；Module System 已提供建立前記錄／下次啟動清理；獨立使用此工具的呼叫端仍須自行管理殘留。
  不得為了清理而重用名稱，或刪除其他世代仍在使用的資源。
- **歷史測試疑慮尚未定位：** 2026-10-03 舊 FileLock 後端測試曾在並行 Release 建置時
  發生 Pipe handshake Timeout，單獨重跑通過；不足以歸因於負載。
  本輪已重寫 fixture 並提供有界子程序清理，但未重現或定位舊逾時原因，不能宣稱修復。
  若再出現，應補充案例、握手階段及 completion／子程序診斷，不能只放寬 timeout。
