# 中央名稱、資源世代與共享鎖接線

本文件維護第二輪中央／Module 接線契約（2026-10-08）。Concurrency 的原子操作與
發布位元契約見 [Concurrency README](../components/concurrency/README.md)。
第三輪已接上 Param 資料、TOML、通知與 callback；最新契約見 [Param README](../param/README.md)。

## 中央名稱與 admission

- CMake `VINC_NAME` 預設 `open-vinc`，建置時指定，例如 `-DVINC_NAME=camera-station`。
  限 1～48 個小寫 ASCII 字母、數字、`_`、`-`；非法值在 configure 時拒絕。
- `ModuleManager::create()` 使用本次建置的名稱，先取得 OSAL FileLock，再建立其他服務。
  同名占用回 `ModuleSystem::InstanceRunning`；不同名稱可並存。Socket Shell port 仍須另行區分。
- `detail::ManagerAccess::create(name, ...)` 只供框架組裝及測試，讓測試不爭用正式中央名稱。
  它不是讓 Module 選擇中央身分的公開服務。
- 狀態根目錄：Windows 目前使用者的 temp 下 `open-vinc-state-v1`；POSIX 使用
  `$HOME/.open-vinc-state-v1`，要求絕對 HOME，私有目錄限制 group／other 存取。
  範圍是同一使用者、同一狀態目錄與 OS 命名範圍，不承諾跨帳號／跨 session 全機唯一。
  使用本機檔案系統；不可自行更換目錄或刪除其中仍可能使用的 lock file。
- `instance-<name>.lock` 永久保留檔案身分，只由 OS 鎖的占用判斷存活。
  不憑 PID 檔、heartbeat 或時間戳刪除並重建鎖檔。程序退出由 OS 釋放占用。
- Instance 由所有使用其名稱服務的中央資源共同保有；因此其 admission guard 不會早於資源
  清理失效。Manager 物件析構前必須停止並等待自己的 Module，沿用既有生命週期。

## 建立前記錄、啟動時清理

`detail::Instance` 實作 common 的 `detail::ResourceNames`。Manager 將服務注入 BusManager，
並供共享 mutex 與 fatal report 使用。Bus 不依賴 Module System，沒有全域 runtime registry。

1. 取得同名 admission 後，掃描已完成 owner 紀錄且符合本名稱的前次 epoch。
2. 先驗證該 epoch 的全部 resource 記錄，再逐筆清理。非預期內容回錯誤，不遞迴刪除。
3. POSIX unlink 舊共享名稱，不改寫既有 mapping；Windows 由最後一份 handle/view 釋放資源，
   清理只退役名稱紀錄，不重新 open、初始化或接管舊共享區。
4. 排他建立新的隨機 epoch 前綴保留目錄，完成 owner 紀錄後，才允許 reserve 資源名稱。
5. `reserve()` 先建立空目錄形式的記錄，再把名稱交給 OSAL create。即使 create 中途崩潰，
   下一次啟動仍知道要檢查哪個名稱。`retire()` 在 unlink 成功／已不存在或 Windows 名稱退役後移除記錄。

新名稱由 epoch＋不回繞的序號組合，符合 OSAL 的 24 個 ASCII 字元限制。
同一狀態目錄的已用 epoch 前綴保留，不再次分配，因此舊 Module 尚未退出也不會接觸新世代。
這些持久記錄針對 **process crash**；未提供 fsync／FlushFileBuffers 斷電交易保證。
不掃描共享記憶體 namespace 後按前綴亂刪，也不自動清理舊版本從未留下紀錄的資源。

磁碟操作只在 admission、資源建立／退役；每次持鎖／解鎖不碰檔案系統。
預期檔案／清理失敗回 Result，既有配置例外政策不變。

## Host 與 Plugin 的邊界

### Notification ownership (2026-10-28) / 通知資源所有權

The central journals and creates one notification per Module generation **before
spawn**. The journal uses `r<epoch><sequence>` for existing shared-memory records
and `n<epoch><sequence>` for notifications; both remain 24 characters and consume
the same `instance_resource_limit` budget. `ResourceNames::reserve()` keeps its
shared-memory meaning; only Instance's internal `reserve_notification()` uses
the new kind. Admission validates the entire epoch, rejects unknown kinds, and
dispatches POSIX unlink to the matching OSAL API. Old `r` records remain readable.
Older central builds do not understand `n` records and may reject admission after
a crash; this does not provide journal downgrade compatibility.

中央在 spawn 前，先為每個 Module 世代登記並建立一個通知資源。既有共享記憶體記錄
使用 `r` 前綴，通知使用 `n`；都維持 24 字元並共用 `instance_resource_limit`。
新增通知每個世代多占一筆記錄。舊 `r` 記錄仍可讀，未知類型拒絕；POSIX 依類型 unlink。
舊中央不認識 `n` 記錄，降版後可能拒絕帶有崩潰遺留記錄的 admission；不承諾 journal 降版相容。

Bootstrap **v15** carries the reserved notification name. After control-channel
authentication, native and Python Hosts open a receiver and a local sender before
the debug gate, Plugin/Python source preparation, or user hooks. Host never creates
or unlinks the endpoint. Existing Ready confirms handoff; central closes its
temporary creator receiver and never waits on it. Plugin entry remains **9**:
no Context vtable or RuntimeServices table change is involved. Rebuild central,
ModuleHost and Python native adapters together; old bootstrap markers reject.

bootstrap v15 攜帶通知名稱；Host 在控制連線驗證後、debug gate 與使用者程式載入前，
開啟接收端及本機發送端。Host 不建立、不 unlink。中央收到既有 Ready 後關閉交接用
handle，中央不等待此通知。Plugin entry 仍為 9；Context 與 RuntimeServices 介面不變。

Preparation failures use `ModuleSystem / NotificationFailed / PrepareNotification`
with the original cause chain. Only that Module fails startup; central and peers
continue, and existing restart policy applies. A failed exclusive create does not
unlink the pre-existing endpoint. Shutdown closes Host endpoints after hooks return;
exceptional teardown uses RAII and OS process cleanup. Central retires the name only
after OS-confirmed exit, or when no child was spawned. Cleanup failures remain
separate from primary failures and retain the journal record for later admission.

準備失敗只讓該 Module 啟動失敗，保留原因鏈並沿用既有重啟政策。獨占建立失敗不刪除
既有資源。中央只有確認程序退出，或本來就沒有 spawn 子程序，才清理名稱；回收失敗
保留記錄與獨立 cleanup error，不蓋掉主要故障。heartbeat timeout 不等於退出證據。

Each restart reserves a new name. A replacement central unlinks recorded POSIX
names without modifying retained endpoints; a delayed old Host's open cannot
recreate them. Windows retirement follows handle lifetime. Old handles can still
refer to old objects, but never to a new generation. Instance admission and epoch
reservations continue to isolate different central names.

本輪只完成資源交接與回收，**沒有 Bus route，也沒有通知等待者或新增 thread**。
Host 的端點保持到 hooks 結束；後續 pump 接線必須先取消／等待本機使用者離開，再 close。
目前 pumpOnce／pumpLoop 行為不變，完整計畫見 [PUMP_WAKEUP.md](PUMP_WAKEUP.md)。

#### Phase-2 validation / 第二步驗證

Validated on 2026-10-28, working tree based on `91d26bc` plus the notification
phases (not yet committed):

| Environment / 環境 | Checks / 檢查 | Result / 結果 |
| --- | --- | --- |
| Windows x64, MSVC 19.51.36257, toolset 14.51.36231, CMake 4.3.2, CPython 3.13 | Debug `^(module\.|osal\.)`, including C++/Python Shell startup | 40/40 passed, 81.27 s |
| Same Windows toolchain / 同上 | Release, same selection / 相同範圍 | 40/40 passed, 80.19 s |
| WSL Ubuntu 24.04, GCC/libstdc++ 13.3, glibc 2.39, CMake 3.28.3, CPython 3.12.3 | Debug `^module\.`, native Host and Python; apps/UI/examples disabled in this build | 18/18 passed, 50.38 s |
| macOS | New notification ownership integration / 新通知接線 | Not run in this phase; earlier unrelated platform reports remain valid / 本輪未執行，不否定先前其他功能驗證 |

- `module.instance`: real central crash with both shared-memory and notification
  records, reserved-but-not-created names, retained old handles, generation/name
  isolation, different central names, unknown-kind rejection and full validation
  before cleanup. POSIX additionally verifies that delayed open cannot recreate
  an unlinked old endpoint.
- `module.manager_faults`: deterministic native exclusive-create failure, preserved
  OSAL cause, healthy peer commands, no deletion of a conflicting endpoint, actual
  child kill/restart with new MID/name, and native/journal cleanup on shutdown.
- `module.process`: a real Host receives a missing notification name and an invalid
  Plugin path; it reports NotificationFailed/NotFound before loading that Plugin,
  never reaches Ready, and exits. Existing heartbeat/stop/restart tests also pass.
- `module.runtime_faults`: queued hint survives Host open/creator close, local
  sender works, close releases both endpoints, missing open cannot create resources,
  and bootstrap v14 is rejected.
- Existing Context, Plugin/debug, fatal supervision, shared locks, Python services
  and Python message suites passed. No new worker or changed pump waiting was tested
  or claimed. Native close/unlink failure injection is not part of this evidence.

初次 Windows 全 Module 回歸只重建 Module System，`module.shell` 使用舊中央而因
bootstrap 不相容逾時；重新連結中央後，上表 Debug／Release 全數通過。此紀錄保留
版本一致性限制，沒有為了測試放寬驗證。Linux 首次完整設定補裝 zlib／Python 開發套件；
Git safe.directory 僅以該命令的明確 submodule 清單設定，未修改全域 Git 設定。

This validates resource lifetime, not lower Bus latency. Bus routing, local stop/
work wakeups, cancellation/join of an actual pump waiter, and end-to-end latency
measurements remain for later phases.

本次證據是資源生命週期，不是 Bus 延遲改善；通知路由、停止／工作喚醒、實際 pump
等待者的取消與 join，以及端到端延遲量測仍屬後續步驟。

```text
ModuleManager
  ├─ Instance：同名排他、名稱與清理紀錄
  ├─ LockRegistry：中央建立、OS 確認死亡後接管
  └─ 既有控制通道 ── ModuleHost
                       ├─ ModuleLocks：MID 綁定、映射快取、停止取消
                       └─ ContextRuntime → Plugin 的繼承 Context
```

- 私有 `ManagerAccess::create_lock()` 建立 mutex 並給它一個本中央內不重用的資源 ID。
  LockRegistry 保存專用 recovery handle，普通中央操作另開自己的 handle。
- 私有 `ContextLockAccess::lock(context, id, deadline)` 經 Host adapter 取得 lease。
  沒有公開 `Context::lock()`，Module 不傳入自選 owner／MID 或共享名稱。
- 第一次使用某個 ID 時向中央查詢；中央由已登記的控制 session 回覆名稱、資源 ID 和 MID。
  Host 驗證全部欄位後才以自己的 MID open。之後使用 Host 映射快取，不每次 RPC。
- `ContextLock` 為 Host 建立的 virtual lease，取得、publication、釋放與析構皆在 Host 的
  Concurrency 實作執行。避免 Plugin／Host 各自 static library 的 thread token 不相同。
  同 thread 釋放契約仍有效；不能把 lease 留給另一條 thread 析構。
- 每次 lease 在取得共享鎖之前配置一份 Host wrapper；首次映射與請求亦可能配置。
  不宣稱整個 Context 取得路徑零配置；取得後的核心解鎖／發布路徑不配置。
- 此鎖交付層無新 thread、無使用者 callback。控制回覆由既有 watchdog 接收。
  Plugin 不因這個入口加入 Concurrency runtime 或另一套控制服務所有權。
- Stop、失聯／故障及正常 main 完成會取消 Host 的 lookup／持鎖等待，封閉新取得。
  已取得的 Guard 不被強行剝奪，既有停止寬限及 OS 終止政策仍適用。
- 這是合作式框架邊界，不是阻止同帳號惡意 native Plugin 直接呼叫 OS API 的沙箱。

## 死亡回收與發布

Manager 只有在 `Process::try_wait()` **確認退出**後才掃描已登記 mutex。
heartbeat timeout、停止請求、kill 已送出、MID 從名稱表消失都不足以回收。
掃描由既有 monitor 完成；沒有額外定時 thread、PID 查詢或持鎖時間上限。

只接管 owner 等於死亡 MID 的鎖；專用 recovery handle 不與正常操作共用。
接管後普通 unlock 保留 publication bit，候選資料仍不對外發布。
`publish_and_unlock()` 已成功的更新則保留，因發布與釋放原本就是同一個 CAS。
鎖交付層不讀寫 Param payload，也不呼叫修復 callback；Param 重用候選前完整重建內容。
不相符占用略過；非競爭回收錯誤存入 Module 的 cleanup failure，不宣稱該鎖已恢復。

中央死亡後整個世代失效；新中央不回收或重設舊世代鎖。舊 Host 依既有 watchdog 退出，
新中央使用新資源世代，MID 數值即使重設也不與舊鎖混用。

## 控制協定與限制

Lifecycle operation `attach_lock=9`，request 為版本 u32＋資源 ID u64。
response 使用既有 Result envelope；成功 payload 為版本 u32、資源 ID u64、MID u64、名稱字串。
拒絕未知版本、截斷、尾端多餘資料、資源 ID／MID 不一致。失敗保存 common error 原因鏈。
逾時或取消的舊 reply 被退役，未知 request ID 拒絕。協定版本目前為 1。
Param 接線後 Plugin entry version 為 4，bootstrap 為 `--vinc-module-v9`，不相容舊 Host／Plugin 明確拒絕。
所有相連的 Host、Plugin 與 SDK 消費端須重新建置。

`config.hpp` 暫存以下上限：

| 設定 | 預設與用途 |
| --- | --- |
| `module_lock_limit` | 每個中央 64 把，登記後保留至 Manager 析構 |
| `module_lock_pending_limit` | 每個 Host 最多 32 個等待名稱交付的請求 |
| `instance_resource_limit` | 每個 epoch 最多 16384 筆尚未退役記錄 |
| `instance_epoch_limit` | 狀態目錄掃描到 65536 筆 epoch 時拒絕新啟動 |
| `instance_name_attempts` | 隨機 epoch 前綴碰撞最多嘗試 32 次 |

## 驗證與待處理事項

2026-10-08，Windows x64／MSVC：Debug／Release 完整建置與公開 SDK 標頭編譯成功。
Debug CTest **61/61**（182.98 秒）；Release 58 套首輪通過，3 套建置型測試因
啟動 CTest 時未載入 MSVC include 環境而失敗，改用 `build/toolchain.cmd` 補跑後 **3/3**，
合計 **61/61** 全部通過。另補跑最終 `module.instance` 通過。
含外部 SDK 原始碼消費端／增量建置驗證。時間是測試耗時，不是效能量測。


- `module.instance`：真實中央角色子程序持鎖、同名拒絕、不同名共存、OS 終止後重新 admission，
  舊 mapping 仍存在時的清理／新世代隔離、其他 Vinc 記錄保留、畸形記錄拒絕。
- `module.locks`：實際 Plugin 經 Context 取鎖，MID 綁定、持鎖 crash、OS 確認後回收、
  新 MID 重啟、發布位元保留、停止取消，及另一持有者不可被誤清。
- `module.lock_protocol`：真實控制 Session 驗證錯誤版本／MID／ID、截斷、尾端資料、
  原因鏈、逾時舊回覆與停止取消。
- **尚待實機驗證：** Linux／macOS 的 admission、process crash 清理及本輪 Host 接線。
- **維護限制：** epoch 前綴保留占用少量持久磁碟 metadata；目前沒有自動壓縮／刪除工具。
  到上限先拒絕啟動，不能為了容量自行刪除仍可能有舊映射的前綴。
  後續可另行設計在全部同帳號中央及 Host 都退出後的離線維護流程。
- **既有 OSAL 疑慮仍在：** FileLock 原生 unlock 失敗契約及 POSIX fd 競爭待辦不在本輪修復範圍。

Phase 3 now connects Bus routes and pump waits. The v15 bootstrap above records the phase-2 validation; current versions and recovery/diagnostic contracts are in [Pump wakeup](PUMP_WAKEUP.md).

第三步已接通；上文 v15 是第二步驗證的歷史版本，目前協定與恢復／診斷契約見上述連結。
