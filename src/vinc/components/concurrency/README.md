<a id="concurrency共享記憶體跨程序鎖"></a>

# Concurrency: shared-memory interprocess mutex / Concurrency：共享記憶體跨程序鎖

This component composes OSAL SharedMemory and SharedWord into the framework interprocess mutex. It has no Module System, Param, Context, or Message Bus dependency and starts no worker. Module services still use the inherited Context; this tool grants no independent framework authority. It is a cooperative synchronization protocol, not a sandbox against malicious native processes under the same account.

本元件以 OSAL SharedMemory 與 SharedWord 組合框架內部的跨程序互斥鎖。
不依賴 Module System、Param、Context 或 Message Bus；不建立背景執行緒。
Module 的正常服務仍經繼承的 Context，不能把這個工具當成可自行取得框架權限的入口。
它是合作式同步協定，不是抵抗同帳號惡意 native 程序的安全沙箱。

<a id="實作範圍2026-10-08"></a>

## Implementation scope (2026-10-08; status updated) / 實作範圍（2026-10-08；狀態已更新）

The FileLock backend has been replaced, retaining `InterprocessMutex`, `InterprocessMutexGuard`, and target `vinc::concurrency`; old file-path create/open overloads are not retained. Module System now provides central supervision, MID delivery, private Host/Context integration, cancellation, and resource epochs. See [Instance](../../module_system/INSTANCE.md). Param now uses the mutex for A/B storage and TOML-backed service; see [Param](../../param/README.md). Unit-test parents act as supervisors; integration tests use actual Manager/Host/Plugin. OSAL FileLock remains independent, including its unresolved native unlock-failure contract.

已替換 FileLock 後端，保留 `InterprocessMutex`／`InterprocessMutexGuard` 與 `vinc::concurrency` target，不保留舊路徑 create/open overload。Module System 已接上中央監督、MID、Host 私有 Context、取消與資源世代，見 [Instance](../../module_system/INSTANCE.md)。正式 Param A/B 儲存與 TOML 服務也已實作，見 [Param](../../param/README.md)。工具單元測試父程序扮演監督者，整合測試使用真實 Manager／Host／Plugin。OSAL FileLock 維持獨立，其原生 unlock 失敗契約仍待處理。

<a id="相依與實作入口"></a>

## Dependencies and implementation entry points / 相依與實作入口

```text
Module System 框架服務／監督者
    → concurrency：互斥、發布位元、受控接管
        → OSAL SharedMemory：名稱、映射、原生資源
        → OSAL SharedWord：原生 64-bit SC 原子操作
        → common：Result／Error、OwnerId 與編號邊界
```

- `include/vinc/concurrency/interprocess_mutex.hpp`: lock, Guard, and ordinary operations.
- `src/interprocess_mutex.cpp`: named creation/opening, mapping/layout helpers, atomic acquisition and release.
- `detail/mutex_recovery.hpp`: designated supervisor integration; not included directly by the SDK umbrella.
- `detail/interprocess_mutex_state.hpp`: implementation/layout-test internals, not an integration interface.

- `include/vinc/concurrency/interprocess_mutex.hpp`：鎖、Guard 與一般操作。
- `src/interprocess_mutex.cpp`：具名的資源建立／開啟、映射、布局接合 helper，原子取得與釋放。
- `detail/mutex_recovery.hpp`：指定中央監督接線，不由 SDK 總標頭直接引入。
- `detail/interprocess_mutex_state.hpp`：僅供元件實作與布局測試，不是接線介面。

<a id="身分與共享布局"></a>

## Identities and shared layout / 身分與共享布局

The shared numeric contract is in `<vinc/owner_id.hpp>`:

共用數值契約位於 `<vinc/owner_id.hpp>`：

| Value | Meaning |
| --- | --- |
| 0 | No owner |
| 1 through 2^63−2 | Module MID/OwnerId range |
| 2^63−1 | Central-only OwnerId, never assigned to Modules |

| 值 | 意義 |
| --- | --- |
| 0 | 沒有持有者 |
| 1 ～ 2^63−2 | Module 可用的 MID／OwnerId |
| 2^63−1 | 中央專用 OwnerId，不發給 Module |

`MID` remains uint64_t; `mid.hpp` declares the Module range and the central allocator rejects exhaustion. Concurrency knows only OwnerId, includes no Module header, and owns no MID registry. A process must use the same OwnerId throughout one resource epoch; dead identities must not be reassigned.

`MID` 仍為 uint64_t，Module 的正式範圍在 `mid.hpp` 宣告，中央發配器在源頭拒絕耗盡。
Concurrency 只認識 OwnerId，不反向 include Module 標頭、不維護 MID registry。
同一程序在同一資源世代必須使用同一 OwnerId；死亡 OwnerId 不可再發配。

The native, same-layout control area is 24 bytes: magic(u64), layout version(u32), bytes(u32), SharedWord(u64). The high 63 control bits are owner; the low bit is the publication selector. An unowned word may be 0 or 1. There is no separate locked flag that can drift from owner state. Layout version is 1.

控制區為原生同布局的 24 bytes：magic(u64)、layout version(u32)、bytes(u32)、SharedWord(u64)。
control 高 63 bits 為 owner，低 1 bit 為發布選擇；空閒 word 可以是 0 或 1。
不另有容易與 owner 更新脫節的 locked flag。布局版本目前為 1。

The creator fully initializes the object before distributing its name. Open never initializes; it checks magic/version/size. Peers must not guess/open names during initialization; format validation is not a concurrent initialization protocol. Each central startup uses fresh names rather than taking over an old epoch. The caller supplies names; production Module System integration uses Instance allocation.

建立者先完成物件初始化，再把名稱交付給其他程序。open 不初始化，會檢查 magic／版本／大小。
禁止其他程序在初始化期間自行猜名稱 open；格式驗證不構成並行初始化協定。
共享區每次中央啟動使用全新名稱，不以名稱重用接管舊世代。名稱由呼叫端提供；Module System 正式接線使用 Instance 發配。

Control mappings are ReadWrite because the Windows atomic load currently uses an Interlocked RMW. This does not require writable Param payloads; payload permissions belong to the upper layer. Shared fields use only SharedWord, not process-local std::atomic fallback or wait/notify. Same-host 64-bit compatible native layout/atomic backends are required; no cross-architecture/version persistent ABI is promised.

控制區一律 ReadWrite：現有 OSAL Windows atomic load 使用 Interlocked RMW。
這不代表參數 payload 必須可寫；Param 資料權限由上層另行設計。
跨程序控制欄位只經 SharedWord 存取，不使用 std::atomic 的程序內 fallback 或 wait/notify。
同機、64-bit、相容 native layout／原子後端是前提；未承諾跨架構／跨版本持久 ABI。

<a id="api-與生命週期"></a>

## API and lifetime / API 與生命週期

| Operation | Contract |
| --- | --- |
| `create(name, owner)` | Exclusively create a region, selector initially 0; OSAL SharedMemory naming rules apply |
| `open(name, owner)` | Open an initialized region, validate owner/layout; never create a missing resource |
| `try_lock()` | One local reservation/shared CAS attempt; contention returns WouldBlock, without spinning |
| `lock_until(deadline, stop)` | Retry with cancellable local sleeps; return a Guard on success |
| `owner()` | Atomic diagnostic observation, not proof of liveness or permission to recover |
| `guard.publication()` | Read the opaque selector while holding the lock |
| `guard.unlock()` | One CAS clears owner and retains selector; repeated successful unlock is harmless |
| `guard.publish_and_unlock()` | One CAS clears owner and flips selector, not two stores |
| `guard.recovered()` | Whether the supervisor death-recovery entry acquired this Guard |
| `mutex.close()` | Detach this handle; Guards/waits retain mappings and occupancy |

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

Only the acquiring thread may release a Guard; moving it does not change thread ownership. Moving/destructing one object must not race. Same-thread reentry through one handle returns Busy; other threads receive WouldBlock. Different handles do not detect thread reentry, but still contend for the same control word and cannot establish two owners.

Guard 只能在取得它的 thread 釋放。移動不改變 thread 所有者；同一物件的移動／析構不可競爭。
同一 mutex handle 的同 thread 重入回 Busy，其他 thread 競爭回 WouldBlock。
不同 handle 不偵測 thread 重入；仍會競爭同一共享 control，不會建立兩個持有者。

Thread affinity uses a process-local atomic TLS token, never shared memory or a common bookkeeping mutex. Ordinary operations do not scan Modules, query PIDs, or steal live ownership. Death of one thread while its process remains alive is not automatically recoverable; supervisor recovery requires confirmed exit of the entire process.

本地 thread-affinity 只用程序內 atomic TLS token，不放入共享區，也沒有共用簿記 mutex。
一般操作不掃描 Module、不查 PID、不自行搶走活著的持有者。個別 thread 異常退出而程序
仍存活時，不承諾自動回收；監督回收依據必須是整個程序確定退出。

Destruction and overwriting move assignment perform ordinary unlock, never publish candidate data. Local shared ownership keeps mappings alive; synchronize close/move against calls on the same public mutex object. Close proves no peer exit and does not unlink POSIX names; the designated creator unlinks when safe. Failed creation/exception unwinding best-effort unlinks only the new, undistributed name, never an opened resource. Failed unlink can leave a name behind.

Guard 析構／覆蓋式 move assignment 只做普通解鎖，不自動發布候選內容。
映射由本地共享所有權保持，close/move 必須與對同一公開 mutex 物件的呼叫同步。
close 不表示其他程序退出，也不自動移除 POSIX 名稱；指定建立者須在安全時機 unlink。
create 失敗／例外離開時，對本次新建且未交付的名稱做 best-effort unlink；不移除 open 的資源。
unlink 本身失敗可能留下名稱，不能宣稱所有清理都已成功。

<a id="等待取消與成本"></a>

## Waiting, cancellation, and cost / 等待、取消與成本

`config::concurrency_retry_interval` defaults to 5 ms: retry interval only, with no holding-time limit or forced unlock. Each wait owns a local condition_variable_any/mutex for stop_token notification, sleeping without shared ownership or a common bookkeeping lock. Stop wakes local sleep. Creating wait tools can allocate/throw STL exceptions under existing policy. The ordinary acquired-to-release core path allocates nothing and calls no user callbacks.

`config::concurrency_retry_interval` 預設 5ms，僅為重試間隔；沒有持鎖時間上限或強制清鎖。
每個等待呼叫使用自己的本地 condition_variable_any／mutex 接收 stop_token，
不持有共享所有權或共用簿記鎖休眠。停止訊號可以喚醒本地休眠。
等待工具的建立可能配置／拋出標準庫例外；不改變全專案的例外政策。
正常取得成功後到釋放的核心路徑不配置、不呼叫使用者 callback。

Checks run Closed, Cancelled, Timeout, then acquisition; an expired deadline does not attempt acquisition. A successful CAS racing with later cancellation/expiry may still deliver a Guard, rather than hide ownership behind a failure. Release does not wake other processes; they observe it on a subsequent scheduled retry. No hard real-time, FIFO, fairness, or starvation guarantee is made. This is a mutex, not a lock-free algorithm merely because it uses CAS.

檢查順序為 Closed、Cancelled、Timeout，再嘗試取得；已過期不嘗試。
檢查後同時發生取消／到期時，成功 CAS 仍可交付 Guard，不回報失敗卻藏住占用。
本版釋放不跨程序喚醒等待者，其他人最多等到下一次排程重試。
OS 排程與標準庫等待不是硬即時；不承諾 FIFO、公平性或無飢餓。
整體是 mutex，不能因 CAS 是原子操作而稱為 lock-free 演算法。

<a id="發布與資料責任"></a>

## Publication and payload responsibility / 發布與資料責任

Concurrency maintains only one selector bit, not A/B payloads, versions, timestamps, or Param schemas. If the upper layer interprets it as A/B, every reader holds the same mutex until its copy is complete; callbacks run outside the lock.

Concurrency 只維護一個選擇位元，不擁有 A/B payload、版本、timestamp 或 Param schema。
上層把它解釋成 A/B 時，所有讀者必須持同一把鎖，複製完才釋放；callback 在鎖外執行。

**English**

1. On acquisition, the selector still points to the last complete version.
2. Modify only the inactive candidate slot, preserving the active slot.
3. Ordinary unlock/destruction retains the selector and abandons candidate changes.
4. Explicit publish_and_unlock uses one SC CAS to publish the complete candidate and release ownership.
5. Death before CAS leaves the old selector for recovery; death after CAS leaves the new data published.

**繁體中文**

1. 取得鎖時，選擇位元仍指向上次完整版本。
2. 只修改非作用中的候選 slot，不破壞有效 slot。
3. 普通解鎖或析構保留原位元，候選更新作廢。
4. 明確 publish_and_unlock 的 SC CAS 同時發布完整候選與解除占用。
5. 在 CAS 前死亡：中央接管時保留舊位元；CAS 後死亡：新位元與新資料已發布。

OSAL SC atomics synchronize payload writes. This is not automatic rollback of arbitrary in-place mutation; a component overwriting active data needs its own recovery protocol. Fully reconstruct a candidate before reusing its slot, rather than assuming an old partial candidate is valid. The upper-layer snapshot owns the version; the selector is neither a monotonic version nor an update notification.

新舊 payload 的寫入順序由 OSAL SC 原子操作同步。
這不是「一般原地修改可以自動回滾」；若使用者元件直接修改有效資料，需自己提供恢復協定。
再次重用候選 slot 前，上層必須完整建立候選內容，不能把前一次半成品當成有效起點。
版本號是上層完整快照的欄位，選擇位元本身不是單調版本或更新通知。

<a id="受控死亡接管"></a>

## Controlled recovery after owner exit / 受控死亡接管

`detail::MutexRecovery::acquire_after_exit(mutex, dead_owner)` is only for central supervision integration:

`detail::MutexRecovery::acquire_after_exit(mutex, dead_owner)` 僅供中央監督整合：

- The caller must already have OSAL confirmation of process exit. Timeout, heartbeat loss, or issuing kill is insufficient.
- The mutex must be bound to central_owner; this API cannot recover central_owner or no_owner.
- Check the current owner, then CAS dead_owner to central_owner while retaining the selector.
- Success returns a recovered Guard for exclusive candidate discard or required upper-layer cleanup.
- No matching ownership/contention returns WouldBlock; do not retry indiscriminately and steal another owner.
- Normal thread-affinity rules remain; same-handle reentry returns Busy.
- Takeover does not prove payload repair and must not let ordinary Modules impersonate the supervisor.

- 呼叫端必須已由 OSAL 確认該程序退出；timeout、heartbeat 失聯、送出 kill 都不足以證明。
- mutex 綁定須為 central_owner；不能以此入口回收中央，也不能回收 no_owner。
- 核對目前 owner 後，以 CAS 將 dead_owner 改成 central_owner，保留發布位元。
- 成功回傳 recovered Guard，讓中央在排他範圍丟棄候選或執行上層必要清理。
- 沒有相符占用或競爭回 WouldBlock；不任意重試並搶其他 owner。
- 保留正常 thread-affinity 規則，同 handle 重入回 Busy。
- 接管不是資料已修復的證明，不能直接讓一般 Module 偽裝中央呼叫。

Never reusing dead MIDs prevents delayed recovery from confusing a new owner. Even if an idle selector cycles back, successful CAS acquires only the current free state; read payload according to the selector after acquisition, not a stale observation. Central death invalidates the epoch; old Modules exit under independent supervision and a new central never takes old locks. Host watchdog failure/stop signals are wired to cancellation; this tool does not detect central liveness itself.

不重用死亡 MID 是避免延遲回收誤認新持有者的前提。普通取得遇到空閒位元繞回時，
CAS 成功仍只取得當下空閒狀態；資料必須在取得後依當下選擇位元讀取，不靠舊觀察。
中央死亡後，整個資源世代作廢，舊 Module 應由獨立監督退出；新中央不接管舊鎖。
ModuleHost 已將 watchdog 故障／停止接入取消訊號；本工具本身不自行判斷中央存活。

<a id="錯誤與違約"></a>

## Errors and contract violations / 錯誤與違約

Expected failures use common Result/Error with retained codes and added InvalidLayout=23. New operations are InspectOwner=5, Recover=6, Publish=7. Shared-memory failures become Concurrency errors retaining OSAL/native causes. The catalog is static C++, without a runtime registry.

預期失敗使用 common Result／Error，保留舊錯誤碼，新 InvalidLayout=23。
操作碼新增 InspectOwner=5、Recover=6、Publish=7。共享記憶體失敗包成本層 Error，
保留 OSAL 原因鏈及 native 診斷。錯誤 catalog 仍是 C++ 靜態表，沒有 runtime registry。

Explicit wrong-thread unlock/publish returns InvalidArgument and retains the Guard for its original thread. Wrong-thread destruction is a contract_violation. If acquired shared ownership is illegally changed, failed release CAS is fatal synchronization-contract corruption, not a normal native unlock failure; continuing as if released is forbidden. Valid release no longer uses FileLock, so the old native-unlock-error/RAII-terminate path does not apply. External truncation/corruption of mappings can still cause memory faults that cannot be guaranteed to become Result.

明確錯 thread unlock/publish 回 InvalidArgument 並保留 Guard，原 thread 仍可釋放。
錯 thread 析構屬 programmer error，走 contract_violation。
已取得 Guard 的 shared owner 若被非法更動，釋放 CAS 失敗屬嚴重同步契約破壞，
走 contract_violation，不能假裝釋放成功並繼續讀寫。這不是一般 native unlock 失敗。
合法釋放不使用 FileLock，因此沒有原本「OS 解鎖錯誤後 RAII 再次 terminate」的路徑。
OS mapping 被外界截短／破壞等記憶體故障，仍不保證能轉成 Result。

<a id="驗證"></a>

## Recorded validation / 驗證

Tests added or rewritten for the shared-memory implementation:

本輪新增／重寫：

- concurrency.errors: shared-resource translation, cause chains, stable identifiers, static catalog.
- concurrency.interprocess_mutex: create/open, owner bounds, layout version, Guard lifetime/move, reentry/wrong-thread explicit release, cancellation/timeouts, multithread/multiprocess A/B snapshots.
- Child faults: partial candidate, complete uncommitted candidate, immediate post-commit exit, OS termination, wrong-thread destruction. Pipe handshakes locate the holding stage; parent confirms exit before checking retained ownership and controlled recovery.
- Delayed duplicate recovery must not clear a new live Guard; new epochs operate independently while old resources exist.
- module.mid: the actual Manager allocator helper, concurrent allocation of final legal IDs, reserved values, and exhaustion without wrap.

- concurrency.errors：共享資源錯誤轉接、原因鏈、穩定編號與靜態 catalog。
- concurrency.interprocess_mutex：create/open、owner 邊界、布局版本、Guard lifetime/move、
  同 thread 重入／錯 thread 明確釋放、取消／逾時、多 thread／多程序 A/B 快照。
- 故障子程序：半筆候選、完整未提交候選、提交後立即退出、OS 終止、錯 thread 析構。
  Pipe handshake 定位持鎖階段；父程序先 wait 確认退出，再驗證未回收前仍被占用及受控接管。
- 延遲重複接管不得清掉新的 live guard，新世代資源可在舊世代仍存在時獨立操作。
- module.mid：實際 Manager 共用的 allocator helper，並行發配最後合法 MID、保留值及耗盡不回繞。

Recorded 2026-10-08 on Windows x64/MSVC: related Debug/Release builds passed, with **6/6** each (16.19 s / 15.10 s): `concurrency.errors`, `concurrency.interprocess_mutex`, `module.mid`, `module.process`, `osal.shared_memory`, `osal.file_lock`. Release standalone SDK header compilation and `sdk.consumer` **1/1** passed. These are first-stage results; second-stage Windows suites subsequently passed all 61 tests in both configurations, including external SDK consumers; see [Instance validation](../../module_system/INSTANCE.md). The new mutex still requires native Linux/macOS validation; earlier Linux framework/Bus success does not establish coverage for this implementation.

2026-10-08 Windows x64／MSVC：Debug、Release 相關 target 建置成功，各 **6/6** 通過：
`concurrency.errors`、`concurrency.interprocess_mutex`、`module.mid`、`module.process`、
`osal.shared_memory`、`osal.file_lock`。Debug 16.19 秒，Release 15.10 秒。
Release SDK 公開標頭獨立編譯成功，`sdk.consumer` **1/1** 通過。
以上為第一輪驗證；第二輪 Windows Debug／Release 全專案 61 套均已取得通過結果，
包含外部 SDK consumer，詳見 [Module System 驗證](../../module_system/INSTANCE.md)。
Linux／macOS 新鎖仍須各自原生驗證；
之前 Linux 框架建置／Message Bus 成功不代表這份新鎖已驗證。

<a id="待處理事項"></a>

## Outstanding issues and resolved dependencies / 待處理事項與已完成相依

- **Param integration implemented:** production A/B snapshots, validation, callbacks, version checks, and TOML now belong to [Param](../../param/README.md); they are no longer pending Concurrency work. The mutex itself still owns no payload/schema.
- **Platform validation:** run mutex/fault suites on Linux/macOS, including different mapping addresses. Same-build child tests do not establish arbitrary ABI compatibility.
- **Separate OSAL issues:** native FileLock unlock failure and POSIX fd creation/spawn races remain tracked by OSAL. The new mutex has neither FileLock nor a shared bookkeeping mutex; the old unlock-outside-bookkeeping proposal no longer applies.
- **Cleanup limit:** failed POSIX initialization only best-effort unlinks. Module System records before creation and cleans next startup; independent callers must manage remnants. Never reuse names for cleanup or remove resources still used by other epochs.
- **Unresolved historical test concern:** a 2026-10-03 FileLock-backend Pipe handshake timed out during a concurrent Release build and passed alone; load was not established as the cause. Fixtures were rewritten with bounded child cleanup, but the old timeout was not reproduced/located and is not claimed fixed. If repeated, capture case, handshake stage, completion/child diagnostics rather than only extending timeouts.

- **Param 接線已完成：** 正式 A/B 儲存、完整快照驗證、callback、版本補查與 TOML 已由 [Param](../../param/README.md) 實作，不再列為 Concurrency 待辦；mutex 本身仍不擁有 payload／schema。
- **平台驗證：** Linux／macOS 須執行互斥與故障套件，包含不同映射位址；同工具鏈／布局的子程序測試不保證任意 ABI 相容。
- **OSAL 獨立待辦：** FileLock 原生解鎖失敗與 POSIX fd 建立／spawn 競爭由 OSAL 追蹤。新實作已無 FileLock／共享簿記鎖，舊解鎖移出簿記鎖提案不再適用。
- **清理限制：** 失敗初始化的 POSIX 名稱只 best-effort 移除；Module System 有建立前記錄與下次啟動清理，獨立使用端仍須管理殘留。不得重用名稱清理或刪除其他世代正在使用的資源。
- **歷史測試疑慮未定位：** 2026-10-03 舊 FileLock 測試曾在並行 Release 建置時 Pipe handshake Timeout，單獨通過，不足以歸因負載。Fixture 已重寫且有界清理子程序，但未重現／定位舊原因，不宣稱修復。再發生時須記錄案例、握手階段與 completion／子程序診斷，不只放寬 timeout。
