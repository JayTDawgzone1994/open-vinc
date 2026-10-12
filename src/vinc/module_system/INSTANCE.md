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
