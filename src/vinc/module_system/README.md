# Module System

每個模組實例在獨立 OS 程序執行。中央以 OSAL Process 管理生命週期，
中央與子程序使用同一個 `vinc::control` 元件透過 OSAL 本機串流 Socket 溝通。
模組作者繼承 `BaseModule`，無須處理 Socket、封包或 heartbeat。

stdout／stderr 由中央 [OutputHub](../output_hub/README.md) 自啟動起擷取，保留名稱與
啟動 generation。本地 Shell 會顯示訂閱之後的輸出；沒有接收者仍持續讀取，不保留歷史。
`wait_shutdown()` 包含已退出程序的有期限輸出收尾，不保證所有目的地已顯示資料。

## 建置邊界

### ModuleHost／Plugin 接線（2026-10-06）

Module 交付為動態庫，由 `ModuleHost` 在獨立程序中載入。中央只保存庫路徑，
不在中央程序載入 Plugin；一個 Module 實例對應一個 Host 程序。停止後結束 Host，
不在同程序內重載。自動掃描、即時新增、中央版本相容性檢查仍未實作。

`vinc_add_process_module()` 現在建立 MODULE library；名稱仍表示獨立程序執行的 Module。
`REGISTER_MODULE` 匯出唯一的 `vinc_module_plugin` 查詢入口，傳回版本／大小、ModuleType、
initialize、create、destroy、clear 函式。宏本身沒有靜態註冊或建立使用者物件的副作用。
create／destroy 配對均在 Plugin image 執行；Host 使用 Plugin deleter，不直接 delete Module。

第一版是相同工具鏈、標準庫、Debug／Release、CRT 與布局設定的 C++ ABI。
C export 只固定查詢符號名稱；STL、Result、BaseModule hooks 仍是 C++ 邊界，不能混搭組態。
Windows 框架與 helper 使用動態 CRT（Debug `/MDd`、其他組態 `/MD`）；跨庫配置的其他
STL／shared_ptr 物件也依賴此共同建置契約。入口版本／大小驗證不代表完整 ABI 探測。
不支援使用者在單一 image 私自替換 allocator 後跨 image 釋放配置的做法。

責任與 target：

- `vinc::module`：BaseModule 與 Plugin 可用的 Message Bus client，無程序入口或控制服務。
- `vinc::module_runtime`：Host bootstrap、Plugin loader、watchdog、控制服務與 Bus 接線。
- `vinc::module_entry`：框架 Host main／wmain；不再由應用 Module 連結。
- `vinc_module_host`：共用 `ModuleHost` 執行檔；仍由 Module System 元件維護。
- `vinc::module_manager`：中央監督；Control Channel 服務仍靜態連入中央與 Host。

Bus 的 Client／Transport 由 Host 明確建立；Plugin 使用同一個 Client。
Host 將送出／關閉操作注入 Bus 的 `ControlTransport`，watchdog 仍是唯一接收者。
Plugin 僅靜態連入 Bus client、訊息儲存及必要工具；不連入控制服務或中央 BusManager。
`vinc::control_protocol` 提供無 worker／Socket 所有權的 codec。
RuntimeServices 仍僅處理 fatal，沒有加入 Bus、Param 或 Context placeholder。

本元件位於 `src/vinc/module_system/`，共同維護 `BaseModule`、子程序 Runtime 與中央
`ModuleManager`。標頭仍使用 `<vinc/module/...>`，各 target 依下述職責分開提供。

`cmake/VincModule.cmake` 提供應用 Plugin helper，框架 Host 入口由本元件建置。
內部協定與 Plugin 接線放在 `include/vinc/module/detail`。
詳見 [SDK 與目錄說明](../sdk/README.md)。


## 程式閱讀順序

1. [`BaseModule`](include/vinc/module/base_module.hpp)：應用 hook、MID／名稱與 Message Bus 的可用時機。
2. [`ModuleManager`](include/vinc/module/host/module_manager.hpp)：中央登記、啟停、命令與查詢介面。
3. [`module_protocol.hpp`](include/vinc/module/detail/module_protocol.hpp)：中央與子程序共用的操作碼、啟動參數位置與回覆布局。
4. [`module_runtime.cpp`](src/runtime/module_runtime.cpp)：`run_module_process` 串起啟動流程；
   `parse_bootstrap`／`connect_to_central` 驗證並建立連線，`register_module_name` 完成名稱註冊，
   `Context::pumpOnce`／`HostContext::commands` 派送使用者 hook，`watch_central` 獨立接收控制訊息與監督停止期限。
5. [`module_manager.cpp`](src/host/module_manager.cpp)：`launch` 依序重設 generation、準備 listener、
   組裝參數與建立子程序；`tick` 推進生命週期，`receive_control_frames` 處理有限批次訊息，
   `register_name` 在取得 registry lock 後核准名稱。
6. [`module_tests.cpp`](tests/module_tests.cpp)：依序檢查命名、程序隔離、hook、重啟與停止；
   `test_parent_loss` 以原始 Socket 模擬中央失聯，和正常 Manager 情境分開閱讀。

`RuntimeInbox` 只在子程序內供主執行緒與 watchdog 交換狀態，不是跨程序記憶體布局。
其 mutex 保護名稱回覆、停止旗標與待派送請求；使用者 hook 執行時不持有這把鎖。
Manager 的 launch／tick helper 由呼叫端持有 record lock，拆分 helper 不轉移鎖的責任。
啟動中取得的部分資源留在 Record，由既有監督收尾流程釋放。

每個 Module 每輪最多處理 `control_messages_per_pass = 16` 筆控制訊息，避免一個忙碌模組
占滿監督執行緒；子程序的待派送請求上限為 `pending_request_limit = 32`。
設定性的數值在所屬檔案以具名常數表達，本輪未遷移至 `vinc/config.hpp`，未改變既有值。

## 建立模組

```cpp
#include <vinc/module/base_module.hpp>
class Camera : public vinc::BaseModule<vinc::RunMode::FrameworkLoop> {
public:
    using BaseModule::BaseModule;
    vinc::Result<void> on_start(const std::vector<std::string>& args) override {
        return {};
    }

    vinc::Result<std::int32_t> module_interface(std::vector<std::string> args) override {
        return 0;
    }

    vinc::Result<void> on_stop() override {
        return {};
    }
};
REGISTER_MODULE(Camera, "cameraModule")
```

```cmake
vinc_add_process_module(camera
    MODULE_TYPE_NAME cameraModule
    MODULE_BASE_NAME cameraNode
    SOURCES camera.cpp
)
```

框架為 ModuleHost 提供 `main`／Windows `wmain`。`MODULE_TYPE_NAME` 必須與 `REGISTER_MODULE` 一致，
每個 Plugin 只宣告一種 ModuleType；同一庫可由多個 Host 程序建立獨立實例。
CMake 產生中央的模組目錄，Shell 不再把模組實作 whole-archive 連進自身。
生成目錄內是本次建置的 Plugin 絕對路徑；搬移部署時以 `register_module_type(ModuleType)`
提供 library 路徑，並以 `ModuleManager::create(limits, host_executable)` 指定 Host 絕對路徑。
省略 Host 路徑時使用本次建置生成的位置；目前不是可搬移的安裝 manifest。

`exampleModule` 示範獨立程序的生命週期與命令 hook，`exampleNode` 是其預設實例名。

## 應用介面與所有權

- 工廠以預設名稱建構模組，框架以帶有 Plugin destroy 函式的 `unique_ptr` 持有它；此時名稱尚未登記。
- 建構後框架設定 MID，呼叫 `on_name(args)` 取得名稱提案；中央核准後才呼叫 `on_start(args)`，Result 成功才送 Ready。
- `on_name` 可修改參數列表以消化命名選項，剩餘參數傳給 `on_start`。MID 可透過 `get_mid()` 讀取。
- 建構、`on_name`、`on_start`、`main`、`on_stop` 與解構在子程序主執行緒執行。命令／補全與訂閱 callback 在 pump 呼叫端執行；Manual 自建 pump thread 時，應用須同步與主執行緒共享的資料。命名期間已有 heartbeat 與啟動期限保護。
- 命名失敗不執行 `on_start`／`on_stop`，正常例外展開仍會解構物件；不可在命名階段啟動需要 `on_stop` 清理的工作。
- Socket I/O 與 heartbeat 在通訊執行緒執行；另一個框架監督執行緒接收控制事件並監看停止期限。
- 正常停止會呼叫 `on_stop` 並解構物件。使用者自行建立的執行緒必須在這段流程中結束。
- 中央失聯時執行 `std::_Exit(72)`，不保證 callback、解構或緩衝日誌完成。
  正常停止超過清理期限時也會強制退出（73）；中央同時可透過 OSAL 強制終止。
- 初始化例外使程序失敗；命令 callback 例外回報 `HookException`，正常的負數命令結果仍是 value。

BaseModule 由子程序內的框架工廠建立，中央透過程序與控制通道管理它。
應用初始化放在建構子或 `on_start`。尚未啟動的 ModuleType 靜態 autocomplete
目前回傳空清單，執行中 Module的 virtual autocomplete 已透過 Socket 接通。

## MID 數值範圍（2026-10-08）

`mid.hpp` 保留 `MID = uint64_t`，以 `invalid_mid`／`max_mid`／`valid_mid()` 定義
合法 Module 身分為 1～2^63−2；0 無效，2^63−1 保留給中央的共享鎖所有者。
數值邊界由 common `owner_id.hpp` 供 Module System 與 Concurrency 共用。
中央的程序級發配器從 1 單調增加，最後合法值發出後回報 `MidExhausted`，不回繞、
不重用已退出 Module 的 MID；`detail/mid_allocator.hpp` 是 Manager 與邊界測試共用的實作。
中央／Host 已接上共享鎖交付、死亡接管與停止取消；Param 接線見 [Param README](../param/README.md)。
本次 VINC_NAME、資源清理與私有 Context 接線契約見 [INSTANCE.md](INSTANCE.md)。
相關新測試為 `module.mid`，涵蓋並行最後編號、保留值與耗盡時 counter 不變。

## Context 與執行模式

Module 必須明確繼承 `BaseModule<RunMode::FrameworkLoop>` 或 `BaseModule<RunMode::Manual>`，
不提供預設模式。FrameworkLoop 的 `main()` 為 final，直接執行 `context.pumpLoop()`；
Manual 的 `Result<void> main()` 為 pure virtual，由使用者提供。這不是 OS 的 main，
DLL 仍由 ModuleHost bootstrap 載入，框架將主執行緒交給該虛擬入口。

`Context` 放在本元件 `include/vinc/module/context.hpp`，BaseModule 繼承的 protected
`context` 是唯一正常 Module SDK 服務入口。不能自行建立、複製或替換；中央核准 MID／名稱後，
Host 綁定既有認證 Session 的 Client 與本地 Runtime。C++ Context 物件／pointer 不經 IPC 傳輸。
`context.advertise<T>()`／`subscribe<T>()` 從 on_start 起可用，停止後拒絕新註冊；
已取得的 Pub／Sub handle 可直接操作，不用每次另傳 Context。原 `message_bus()` 入口已移除。
這是 API 身分邊界，不是防止惡意 native Plugin 呼叫 OS 的安全沙箱。

### pump 與 callback 契約

- `Result<PumpStats> pumpOnce()`：固定本次訂閱集合與每個來源的已發布序號上限，
  處理仍保留的資料；callback 期間新增的來源／訂閱／發布留到下一輪。各來源界線依序擷取，
  不是全域同時快照。停止／錯誤可提前結束；空批次是成功。
- `Result<void> pumpLoop()`：重複批次；沒有資料／命令時等待，停止要求會喚醒並返回。
  本版資料發布沒有額外 Socket 通知，使用 `config::module_pump_idle_interval`（預設 10ms）輪詢，
  這是閒置等待值，不是 callback 送達時限。
- `bool living()`：本地 Runtime 是否仍允許繼續；停止或已偵測故障後為 false，
  不承諾即時得知遠端中央狀態。
- 同一 Context 只允許一個 active pump（包含整段 pumpLoop），並行／遞迴呼叫回傳 Busy。
  Manual 可自行建立 pump thread；FrameworkLoop 不另開 pump thread。
- `context.subscribe<T>(topic, callback, options)` 的 callback 回傳 void，收到
  `std::unique_ptr<T>` 的本地訊息所有權，可修改或 move 留存。無 callback 的 overload 只供 get_last。
  使用者必須保存 Subscriber handle；close／析構停止後續派送。callback 可自行 close。
- 框架先複製與驗證訊息、釋放共享讀取保護，再呼叫應用；不持有 registry/inbox/read lease 執行 callback。
  同一 handle 的 move／close／析構仍須由應用與其他使用者操作同步；close 不作為正在執行 callback 的 join。
- Shell 命令與補全由 pump 派送，沒有 pump 就不執行；heartbeat 與停止監督仍獨立。
  Shell 原有 Result／HookException 回覆契約保留。訂閱 callback 未處理例外則鎖定 Module 終止故障，
  即使 Manual 忽略 pump 的 Result，也由 watchdog 終止並由中央套用重啟政策。
- `PumpStats` 回報本批次 callback／命令數、可確認覆蓋的遺失筆數及 stopped。
  Subscriber 保有累計覆蓋事件／筆數與目前來源的 Publisher ID 分項；不記錄無界歷史或逐筆洗版。

Context 的 callback 登記與批次 metadata 在本地配置，訊息逐筆複製，不預先複製整批 payload。
登記上限 `config::module_callback_limit` 預設 256，closed 登記在後續註冊／pump 時移除。
Bus 批次接線使用指定的 `message_bus/detail/dispatch.hpp`；Bus 不反向依賴 Module System。
同 topic 多來源維持現有 timestamp／Publisher rank 合併，沒有新增全域線性 FIFO 承諾。
Param 已接入 `context.param`，共享版本補查與 callback 亦由 pump 派送，見 [Param README](../param/README.md)。
timer 註冊與 Message Bus 跨程序資料通知仍未提供。

### 正常完成、故障及收尾

`main()` 成功返回後先停止 pump 派送並開始停止期限，再執行 on_stop，等待 pump 離開，
銷毀 Module 與 callback wrapper；Plugin 在這些物件及 Host 服務結束後才卸載。
應用須在 on_stop 中停止／join 自建執行緒；框架無法安全搶占正在執行的使用者 callback。
若卡住，既有 watchdog 與中央依 stop_timeout 終止程序。

自行完成使用 Lifecycle Finishing Request/Response=7 與 Completed Request/Response=8，
兩者皆為空 payload、request ID=UINT64_MAX，由該 Module 認證 Session 傳遞。
Finishing 啟動中央收尾期限；on_stop／Module 解構完成後才送 Completed，等待確認後退出。
只有完成協定、未記錄故障且 OS 正常退出碼為 0，才記為 Stopped、不自動重啟。
直接 exit(0) 不構成正常完成；主要／收尾故障與 crash 優先，依原有政策重啟。
`wait_started()` 亦接受已確認正常完成，避免短工作在等待者觀察到 Running 前結束而誤報；
`wait_running()` 仍要求目前處於 Running，正常完成後回 NotRunning。
本地 watchdog 以 `config::module_runtime_poll_interval`（預設 20ms）檢查本地主入口停止／故障，
會增加少量閒置喚醒；不新增執行緒，也不保證 OS 排程的硬期限。

Host 經內部非模板 `detail::ModuleObject` 操作兩種公開 BaseModule，維持 Plugin 成對 create/destroy。
Plugin entry version 為 5，bootstrap 標記為 `--vinc-module-v10`；舊 Plugin／Host 明確拒絕，
中央、Host、Plugin 與 SDK 消費端須一同重建。同工具鏈／CRT 的 ABI 限制維持。
RuntimeServices bridge 仍僅供 fatal，未加入一般 Context 服務。

## 中央 API 與命名

`ModuleManager` 透過 `create()` 明確建立，回傳 `Result<std::unique_ptr<ModuleManager>>`，
不提供隱含 singleton 或 `get()` 相容入口。Shell 主程式明確持有並傳入 manager。
`register_module_type` 登記 ModuleType、Plugin 絕對庫路徑、模組基礎名稱與政策。
`start_module` 回傳 `Result<MID>`，成功值為中央配置的非零 `MID`（`uint64_t`），啟動工作由監督執行緒進行；
回傳成功不代表程序建立成功或應用已就緒，使用 `wait_started(mid)`，再以 `inspect(mid)->name` 取得核准名稱。
`ModuleSnapshot` 以 `module_type` 表示種類名稱、`mid` 表示框架身分、`pid` 表示 OS 程序 ID。
`inspect`、等待、命令、停止與移除 API 都接受 MID 或名稱；尚未取得名稱時必須使用 MID。

名稱由 Module 的 `on_name(std::vector<std::string>&)` 提案，預設回傳建構時的名稱。
中央原子地登記名稱；撞名直接回報 `module already exists`，不再產生 `_1`／`_2`。
名稱不得為空、超過 128 bytes、含控制字元，或與 ModuleType、micro-program、保留控制命令衝突。
同一 manager 內最終名稱唯一；多個實例應由模組作者自行解析參數並回傳不同名稱。
`exampleModule` 與 `messageBusExample` 範例提供 `--name`，這是範例自己的選項，並非所有模組都必須支援的框架旗標：

```text
exampleModule --name frontCamera
exampleModule --name rearCamera
frontCamera status
rearCamera status
```

每次建立程序（含自動重啟）取得不同 MID，範圍涵蓋同一中央程序生命週期內的所有 manager；
不承諾跨中央程序重開仍全域唯一，不可把它當持久化設定鍵。generation 與 MID 使用同一數值。
重啟保留已核准名稱（等待重啟期間也保留），但更換 MID、generation 與連線。
若重啟時 `on_name` 改成其他名稱，中央拒絕並停止重試；舊 MID 不會操作新程序。
只有觀察到退出並回收後才可重用名稱；已完成紀錄可用 `forget_module` 移除。
重新啟動相同基礎名也可替換已完成的同名診斷紀錄。
中央最多保留 256 筆紀錄，滿載時要求先釋放已完成紀錄。

`inspect` 保留狀態、最後的 PID、generation、重啟次數、退出碼／signal，以及帶 MID 的主要／收尾 Error。
監督執行緒以最長約 20ms 間隔檢查 OSAL 程序狀態；不承諾硬即時期限。
OS 原生 spawn 本身仍遵循 OSAL 的限制，不具有可取消的硬期限。

## 狀態與政策

Queued → Starting → Running → Stopping → Stopped／Failed。
意外退出或失聯後，先終止並確認舊程序退出，再依政策進入 RestartWaiting → Starting。

| 政策 | 預設 |
| --- | --- |
| heartbeat interval | 1 秒 |
| heartbeat timeout | 5 秒 |
| 啟動至 Ready 期限 | 30 秒 |
| 正常停止寬限 | 3 秒 |
| 最大自動重啟次數 | 0（停用） |
| 重啟間隔 | 1 秒 |

所有期限使用 steady clock。重試次數是該邏輯實例整段生命週期的累計，不因短暫成功而歸零。
未完成正常完成協定的自行退出仍算程序遺失；明確 stop、中央正常關閉及已確認正常完成不觸發重啟。
停止逾時不放棄管理；若 OS 拒絕終止，保留程序紀錄及原生錯誤並繼續監看，不假造已退出。

中央析構要求所有子程序停止，等待各政策停止期限加觀察寬限，必要時要求 OS 強制終止。
OS 終止失敗或程序完全無法排程時，無法提供絕對的退出時限。

完整關閉使用 `begin_shutdown()` 停止接受新工作，再呼叫 `wait_shutdown(deadline)`。
Timeout 時仍持續監督，可以再次等待；成功代表受管理程序已確認退出且 Message Bus 回收佇列已排空。
`snapshots()` 可顯示等待中的模組與 OS 錯誤。互動 Shell 的 Ctrl+C／exit 使用此流程，
等待到完成才退出，不把析構的時間限制當成程序已退出的證明。

## 命令與後續擴充

既有 `module_interface(args)` 與 `module_autoComplete(args)` 的呼叫已由中央代理經 Socket 送到子程序。
中央每個模組同時只接受一個等待回覆的請求；忙碌時回報 `ModuleSystem::Busy`。
逾時表示結果未知，不表示指令沒執行或已取消，不自動重送。
重啟／斷線會結束尚未收到回覆的舊會話等待；已收到的有效回覆仍交付原等待者。逾時後保留未完成請求的 Busy 狀態，直到收到其遲到回覆或程序退出；
遲到回覆不交付已結束的呼叫，但會釋放 Busy。回覆帶 request ID 並核對 domain／operation。
`call_*_interface`、補全與 micro-program 介面均使用 common Result；Shell 顯示統一錯誤文字。
互動 Shell 在工作執行緒等待啟動／命令／補全，鍵盤仍可用；既有腳本入口維持同步。
`wait_started` 依模組啟動期限加一秒等待 Ready；逾時不隱含取消背景啟動。

### Module v3 協定

程序啟動標記為 `--vinc-module-v10`，在 Plugin 路徑之後加入本次啟動的 debug 旗標（0／1）。
中央與 Host 須一同重新建置，不提供舊 bootstrap 轉接；以下 Socket 錯誤 payload 仍為既有 v3 格式。
外層 Control Channel 的 v1 frame 不變；更新的是 Module payload 契約。

- Lifecycle Domain=1：Ready Event=1（名稱字串）、Stop Event=2（空 payload）、
  RegisterName Request/Response=3（request ID=1）。Request 帶提案名稱。
- RegisterName、Execute、Complete 的 Response 先放 u32 tag：0 成功、1 失敗。
  成功 body 分別是名稱字串、i32 應用結果的 u32 位元編碼、參數字串序列。
  失敗 body 為下述數字 Error；目前層必須是 ModuleSystem，底層原因可來自其他 domain。
- Command Domain=2：Execute=1、Complete=2，Request payload 仍是參數序列。
  不支援的操作回報 `UnsupportedOperation`；hook 失敗不會被解讀成應用結果整數。
- Lifecycle ReportFailure Request/Response=4。Request ID 從 2 遞增；payload 是
  u32 role（0 主要故障、1 收尾故障）＋Error。中央保存後回傳同 request ID 的空 Response。
  子程序等待確認最多 `config::module_error_report_timeout`（預設 500ms）；watchdog
  的停止期限仍優先，不會為了診斷延長程序停止寬限。
- Error header 是 u32 version=1、u32 record count（1..16）、u32 flags（0 或 1，1 表示省略）。
  每個 record 是 domain/code/operation/native source（各 32 bits）及 native value（64 bits）。
  所有欄位 big endian；有號值保留位元表示。禁止直接複製 C++ Error 布局。
  截斷／多餘 bytes、未知格式版本／flags、非法筆數均拒絕；未知數字代碼原樣保留。
  接收端依本地容量保留近因與最深原因，必要時標記省略；wire 上限不隨本地設定改變。

[Shared Memory Message Bus](../message_bus/README.md) 已接上：在 `on_start()`／後續工作
使用繼承的 `context.advertise<T>()`／`subscribe<T>()`，建構與 on_name 尚未開放服務。
Module watchdog 統一路由 Message Bus Domain=3 的控制訊息；中央另有 BusManager 執行緒管理註冊與共享區。
大量資料不進 Socket，中央確認子程序退出後才清除其 reader 占用與發布歷史。
`ModuleManager::create(message_bus::Limits)` 可配置 Message Bus 容量；省略 limits 使用預設限制。
`messageBusExample` 提供可操作的跨程序發布／接收範例。
訂閱 callback 由 Context pump 派送；框架間多機通訊尚未實作。

## 驗證

`control.channel`、`module.process` 與 `module.shell` 由根專案 CTest 執行。程序測試涵蓋同時命名、
獨立 PID、virtual 命令與參數補全、Unicode 參數、正常停止 callback、crash 隔離、
命令卡住、初始化卡住、啟動失敗、雙向 heartbeat 中止、中央斷線、重啟上限與名稱重用。
測試透過 OSAL 管理子程序，CTest 有總時間上限；正常及例外路徑都要求回收已啟動子程序。
2026-10-02：Windows x64、MSVC 工具鏈，完整專案 Debug／Release 建置成功，
兩種組態各 25 套 CTest 全通過。程序驗證包含同名競爭只核准一個、保留字／非法名稱拒絕、
拒絕後不進入 `on_start`、命名 hook 卡住／例外／crash、未命名程序以 MID 停止，
以及重啟等待期間保留名稱、新 MID 與舊 MID 隔離。OutputHub 名稱交接另有事件與文字輸出測試。
命名遷移另驗證 ModuleType 啟動入口與 Module 命令／補全入口沒有混用、
種類登記與已啟動模組的查詢，以及 Snapshot 的 `module_type`／`mid`／generation 對應。
Linux／macOS 尚待原生建置與行為驗證；未進行效能量測。

2026-10-03 可讀性整理後：Windows x64／MSVC 的完整 Debug、Release 建置成功，
各 25／25 套 CTest 通過（Debug 79.83 秒、Release 40.44 秒）。
驗證包含 Module 程序生命週期、Control Channel、Message Bus 與 Shell／Socket Shell／Tcl 整合。
本輪未更動測試情境與既有等待值；Linux／macOS 仍未執行。

2026-10-04 中央入口／範例收尾：`cmake/VincModule.cmake` 與 `apps/vinc/CMakeLists.txt`
已改用可閱讀的多行 catalog 模板；Module 範例與本頁 hook 範例也已展開。
註冊順序、命名參數消耗及例外政策保持原樣。Windows Debug／Release 建置通過，
兩種組態各通過 `shell.console`、`module.shell`、`message_bus.shell`；
CLI 前後比對與限制見 [中央程式 README](../../../apps/vinc/README.md)。

2026-10-04 元件命名整理：目錄改為 `module_system`，錯誤分類改為
`ErrorDomain::ModuleSystem = 3`，同步更新 CMake 接線與文件引用。
Windows x64／MSVC Debug、Release 完整建置通過；各組態的 13 項相關 CTest
（Common error、Module、Message Bus 整合、Shell 與 SDK）均通過。
Debug catalog 測試首次因缺少 MSVC 環境失敗，補齊工具鏈環境後重跑通過。
本次未重跑其餘測試；Linux／macOS 未驗證。

## 統一錯誤與建立契約

2026-10-04 本輪驗證：Windows x64／MSVC 完整 Debug、Release 建置成功，公開標頭
獨立編譯通過；完整 CTest 各 **33/33**（Debug 74.25 秒、Release 68.89 秒）。
新增 `module.error_protocol` 驗證數字原因鏈、未知代碼／原生來源、格式拒絕與不同本地容量。
真實子程序測試涵蓋 hook 失敗與負數應用結果的區別、主要／收尾故障分離、上一筆故障 MID
在重啟後保留、送出前／後失敗語意、依賴 factory 失敗及正常停止。
最後將編譯模組工廠註冊改為 Result 後，兩種組態重新建置；Debug 另重跑 4/4 項
Module／Message Bus 程序相關測試（22.66 秒），Release 的上述 33/33 為此調整後的完整驗證。
Linux／macOS 未建置或執行；未注入 OS 執行緒建立失敗及所有記憶體配置失敗點。

```cpp
auto created = vinc::ModuleManager::create();
if (!created) {
    std::cerr << vinc::error_to_string(created.error()) << '\n';
    return;
}
auto manager = std::move(*created);
// Construct Shell with *manager; destroy Shell before manager.
```

Manager 移除預設建構與 `get()` singleton。服務、私有 Socket 目錄與 supervisor
都建立成功後才回傳 owner；失敗時以 RAII 回收已取得資源。Shell 需要明確注入 Manager。
MID 耗盡、名稱衝突、容量不足、關閉中、啟動／連線失敗有各自代碼；不靠字串判斷。
錯誤分類為 `ErrorDomain::ModuleSystem`，enum 位於 `<vinc/module/error.hpp>` 的
`vinc::module_system`；靜態 catalog provider 不依賴 runtime。

BaseModule 的 `on_name`、`on_start`、`on_stop`、`module_interface`、`module_autoComplete`
分別回傳 `Result<string>`、`Result<void>`、`Result<void>`、`Result<int32_t>`、
`Result<vector<string>>`。hook 回傳失敗會包成 `HookFailed` 並保留原因；使用者例外在
hook 邊界轉成 `HookException`，配置失敗轉成 `ResourceExhausted`，不傳送 `what()` 字串。
`on_start` 已被呼叫時，預期失敗／hook 例外後仍執行 `on_stop`；factory 或名稱登記失敗
不會執行尚未開始的生命週期。命令 hook 失敗不會自動終止模組。

`Result<int32_t>` 的整數是應用結果；包含 -111 等負數也可為成功的 value。
送出前的 `Timeout`／`Cancelled` 表示此次未排入請求；送出後則回報
`TimeoutOutcomeUnknown`／`CancelledOutcomeUnknown` 等結果未知分類，不重送、不提早清除 Busy。
命令本身是否執行，不能從等待失敗推斷。已收到有效回覆時，即使同時要求停止仍交付該回覆。

`ModuleSnapshot.last_failure` 與 `cleanup_failure` 各保存 `{mid, error}`：同一 MID
第一次主要故障不被連帶錯誤覆寫；收尾故障另外保存。成功重啟不清掉上一筆故障，
可由診斷的 MID 和目前 MID 區分。OS 退出狀態另外保存在 `last_exit`。
`stop_module`／`wait_shutdown` 的成功表示已確認退出／完成回收，不把歷史 hook 失敗
當成永遠無法關閉；需要查看原因時讀取 snapshot。Shell 的完整關閉仍等待實際程序退出。

## 待處理事項

### Plugin 遷移後的限制

- 相同建置 C++ ABI、自動掃描／熱載入與中央版本檢查的限制見上方建置邊界。
- Build-tree catalog 與預設 Host 路徑仍是絕對路徑，正式可搬移部署 manifest 尚未實作。
- 載入器執行的 static constructor 早於 bridge 注入，只有 fallback termination；
  清除 bridge 後的 static destructor 同樣不保證有 fatal report。
- 原生載入／卸載可能執行任意 Plugin 程式碼，無 portable timeout；中央的啟動／停止
  監督負責終止卡住的 Host。這不構成對惡意 native Plugin 的安全沙箱。

### RuntimeServices bridge 與後續整合（2026-10-06）

使用者提出由 Host 在 Plugin 初始化時注入最小 process runtime service table；
一般 Module 服務走 Context，無 Context 的 contract fatal 路徑只經 bridge 使用
Host 擁有的故障回報狀態，不反向依賴 Context。本輪已完成 bridge 與現有 Runtime 接線，
以及真實雙 DLL 驗證，詳見下方；正式 ModuleHost／Plugin 載入與 Module 動態庫接線已加入，見上方建置邊界及下方驗證紀錄。

**既有行為與分階段處理：** `src/contract_report.cpp` 的
`ContractReportStorage::report_and_exit()` 在另一個 writer 處於 writing 時無限等待，
原意是保住第一筆完整診斷。若第一位 writer 停滯，後續 fatal 呼叫也無法自行終止；
本輪以停滯及同 thread 重入測試重現，並接上中央有界監督，見下方「Fatal 退出監督」。
回報可能不完整是已接受的限制；不能沿用原有 callback 宣稱所有環境都必定回報及退出。
未綁定、失效或 callback 返回的 fallback 已依使用者決策改成 `_Exit(77)`，
不再呼叫使用者 terminate handler；一般初始化失敗仍使用 Result。

**子程序失聯退出仍是獨立限制：** `runtime/module_runtime.cpp::watch_central()` 在
通道失聯時透過 `watchdog_failure()` 退出。中央保留 fatal 寬限，不等於子程序也會等待；
此路徑仍可能中斷尚未提交的 writer。這是來源審查確認的既有政策，本輪未修改，也未以
實機斷線注入驗證。若未來要求失聯後仍保留回報時間，須另行確認子程序的有界等待政策，
同時維持中央真的死亡時不留下無限存活的 Module；不可直接停用 watchdog。

Chromium 對照：Crashpad 將 dump 擷取／寫檔交給獨立 handler process，減少在
受損程序內執行的工作。2026-10-06 查閱的 Windows client 實作中，第一個
exception handler 通知外部 handler 後等待 60 秒，再自行終止；其他同時進入的
thread 等待程序被終止。這是該 Windows 路徑的策略，並非全平台硬期限或必有 dump
的保證，也不表示本專案已決定引入 Crashpad。
參考：[Crashpad 設計](https://github.com/chromium/crashpad/blob/main/doc/overview_design.md)、
[Windows client 原始碼](https://chromium.googlesource.com/crashpad/crashpad/+/refs/heads/main/client/crashpad_client_win.cc)。

- **已確認限制：故障診斷交付不是絕對保證。** `src/runtime/module_runtime.cpp` 已增加
  主要／收尾故障的有界確認回覆。突然 crash、已斷線、OS 強殺、停止期限用盡、配置耗盡
  仍可能來不及交付；中央保留可觀察的通道原因與實際退出狀態，不由 exit 70 猜測根因。
  建立控制連線前的一般 Result 失敗仍以本機 `error_to_string()` 輸出，尚無跨程序 bootstrap
  錯誤備援；已完成 reporter 綁定後的 contract violation 則使用下方專用共享紀錄。
  目前保留此限制；需要更完整故障鑑識時，另討論持久診斷管道與 crash report。
- **已修正的相依問題（2026-10-05）：** Message Bus 的 Detach 通知配置／送出失敗
  已不再中斷退出回收；跨 topic Reader 清理、預算及名額重用已有故障測試。
  範圍與限制見 [Message Bus 重審修正](../message_bus/README.md#2026-10-05-重審修正註冊回滾與退出回收)。

### 2026-10-05 Module System 重審修正

本輪修正四項已確認問題，並同步更新 OSAL 的觀察／控制契約：

1. **觀察失敗仍可安全終止。** `record_failure()` 不再把停止期限一再往後移。
   OSAL 保留可重試的 native owner，觀察恢復後交付真實退出狀態；所有權失去時禁止裸 PID 終止。
   `tick()` 在回收前讀取 `Process::observation_error()`，即使 OSAL 已在兩輪檢查之間恢復，
   仍保存先前原因。主要故障與終止／回收故障維持分開；沒有 ExitStatus 就不清 Bus Reader。
   詳見 [OSAL 所有權與回收](../osal/README.md#所有權與回收)。
2. **命令等待者以作用域管理。** `CommandWaiter` 在正常回傳與例外展開時都解除 waiter。
   `Session::send()` 的 bad_alloc 發生於排入前，清掉 pending；成功排入後的例外／等待失敗
   保留 pending，直到遲到回覆或真正退出才解除 Busy。既有 STL 例外仍可傳播，沒有自動重送。
3. **已收到的回覆屬於原請求。** `retire_command()` 在退出與 generation 重設時保留仍有
   waiter 的完成回覆；新命令須等原 waiter 離開。回覆核對 request ID、operation 與 generation，
   等待端先交付已完成結果，再判斷 generation 變更；舊結果不交給新命令。
4. **停止 hook 不依賴診斷配置成功。** `run_started_hooks()` 管理開始後的生命週期。
   框架 dispatch／Ready 配置例外轉成既有 ResourceExhausted／SupervisorFailed；
   故障回報失敗不覆蓋主因，也不跳過一次 `on_stop()`。主要／收尾報告各自嘗試。
   factory／名稱登記失敗不進入此生命週期；watchdog 的 `_Exit`、OS 強殺、程序 crash 仍可能中斷收尾。

#### 故障驗證邊界

- `osal.process_faults` 將真實 ProcessService 狀態機接到測試 native backend：持續 poll
  失敗、受控終止、退出確認前的 Busy／shutdown timeout、恢復觀察及首次診斷保留；
  另模擬 ObservationLost，確認不再呼叫 native terminate，且不產生虛假的退出結果。
  包含背景重試與非忙迴圈檢查；這不是 Linux／macOS 原生 ECHILD 的實機驗證。
- `module.manager_faults` 使用真實子程序與 Socket，注入送出前／後配置例外，確認
  waiter 清除與遲到回覆釋放 Busy；在保存回覆後先透過 OSAL 終止並確認退出，
  再讓同一 tick 繼續，驗證結果仍能交付。隨後重啟並送不同命令，驗證新舊結果隔離。
  record 級測試另驗證反覆故障不延後停止期限、保留回覆跨 generation 重設與 waiter 例外清理。
- `module.runtime_faults` 執行實際 lifecycle helper，注入故障報告及 Ready 配置失敗，
  驗證 `on_stop()` 恰好一次、主因保留，以及主要／收尾報告分別嘗試。
- Module 故障 target 使用 `VINC_MODULE_TESTING`；正式 library 不包含測試 checkpoint 或 registry。
  測試以私有實作 fixture 驗證生命週期，不提供新的公開測試 API；CTest 均有總時間上限。

#### 本輪驗證結果

Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
相關 Debug／Release target 建置通過；`osal.process_faults`、`osal.process`、
`osal.process_output`、`module.manager_faults`、`module.runtime_faults`、`module.process`、
`module.error_protocol`、`module.shell`、`message_bus.module` 各 **9/9** 通過
（Debug 26.32 秒，Release 21.62 秒）。最後調整 OSAL 退避測試的排程容忍度後，
Debug 上述整組已重跑；Release 另重建並重跑該測試 1/1 通過（0.29 秒）。
已核對兩種組態 compile commands：Module 測試 define 只用於兩個 fault target。
未重跑全專案其餘測試。

Linux／macOS 尚未原生建置或執行；注入結果不代表所有 OS 故障與排程均已驗證。

## 測試路徑轉換收尾（2026-10-05）

`tests/module_helper.cpp` 的啟動／停止 marker 路徑改用
[OSAL 路徑 helper](../osal/README.md#utf-8-與原生路徑)，不再透過 char8_t 指標讀取 char 儲存區。
失敗由 StartHook 的 Result 保留 OSAL 原因。Windows Debug／Release 的 module.process 均通過，
包含在本輪各 20/20 相關測試內。Linux／macOS 尚未驗證。

## 契約故障共享紀錄

2026-10-05：已接受「中央保留已提交的紀錄，回報不受 Socket 鎖與傳送延遲影響」的方向。
這個機制處理框架偵測到的 contract violation，不把任意 segmentation fault、記憶體毀損或
OS 強殺當成可攔截的 C++ 契約失敗；可恢復失敗仍走 Result。

### 責任與生命週期

- ModuleManager 在每次 spawn 前建立排他、唯一命名的 SharedMemory。每個 MID 一份，
  中央既有監督迴圈只讀原子狀態，存活時不讀 payload。`vinc_module_contract` 是元件內部共用 target，讓中央／Runtime
  使用同一份紀錄 codec 與 OSAL owner；Common 不依賴它，也不依賴 OSAL。
- 子程序先驗證 bootstrap v10、開啟／映射紀錄，再綁定 Common 的 RuntimeServices，最後
  建立控制服務與呼叫使用者工廠／hooks。綁定活到 Runtime 服務停止後；解除綁定等待正在
  使用 context 的讀者離開，才解除映射。bootstrap 以前或綁定以前的故障仍沒有這份保證。
- 故障呼叫端直接寫入預先配置的 bytes，發布 committed 狀態後 `_Exit(76)`。
  不取得 RuntimeInbox／Socket／stdio 鎖，不建立字串、封包，不呼叫 `on_stop` 或解構。
  同時故障時第一位 writer 負責紀錄；其他 fatal 呼叫等待提交，不用 500ms 把第一位提前殺掉。
  這個等待只在 fatal 路徑以 yield 發生；既有 watchdog、中央停止政策及外部 OS 強殺仍可能中斷它。
- 中央持有自己的 mapping，子程序退出不會讓紀錄消失。只有取得 OSAL 的真實 ExitStatus 後
  才讀取 payload，保存後再解除映射／移除 POSIX 名稱。重新啟動使用新 MID、新名稱、新的 empty 紀錄。
  尚未 spawn 的部分取得失敗由 owner 回滾；只有成功 create 的中央 owner 可 unlink。
- 明確 `close()` 將清理失敗存入 `cleanup_failure`。POSIX unlink 的名稱配置若耗盡，
  轉成 ResourceExhausted 並繼續本機 unmap／close，不因 destructor 例外終止中央；
  名稱可能遺留，不能宣稱清理成功。析構 fallback 為盡力清理，沒有另建重試執行緒。

### Fatal 退出監督（2026-10-06）

- 中央第一次觀察到 writing、committed 或非法狀態，即鎖存本 MID 的 fatal 狀態，
  轉入 Stopping。`config::module_fatal_exit_timeout` 預設 **5 秒**，從中央首次觀察起算；
  原有 startup／stop 期限，以及之後明確停止或框架關閉所給的期限，取較早者。
- 重複觀察不重設期限。中央後續 Socket／退出觀察失敗保留為 cleanup 診斷，不縮短
  已給定的 fatal 寬限；此政策不改變子程序自身的失聯退出，限制見上方待辦。
- Fatal 後不送 Stop，不接受 Ready／名稱註冊或新 Bus 工作，新命令回報 NotRunning。
  已保存或已在接收佇列中的命令回覆仍依既有規則交付；沒有回覆的在途命令維持結果未知。
- 有效期限到期才要求 OS 終止；失敗保留錯誤，沿用既有重試間隔與程序所有權。
  OS 終止請求不等於已退出。只有確認 OS ExitStatus 後，才讀取診斷並回收 Bus reader leases。
- 使用既有中央監督流程，每個活躍 generation 每輪增加一次原子讀取，鎖存後不再讀。
  未新增 thread 或正常期間的喚醒。既有約 20ms 的迴圈等待不構成硬即時偵測／退出上限；
  OS 排程、中央停滯或 OS 拒絕終止仍可能延遲退出。
- 尚未發布 writing 的故障無法靠這份紀錄偵測。重啟清除 fatal 期限及狀態，
  保留舊 MID 的最後診斷，不將它套用到新 generation。

### 格式、成本與驗證

共享區固定 **1336 bytes／程序 generation**，原生 OS 可能按頁取整；中央另外在 snapshot
保留固定容量的診斷值。建立時配置和映射，fatal 路徑無配置；沒有新增 thread 或定期 watchdog
喚醒。Shell 在既有收集迴圈讀取一個 revision 原子值，只有變更時取得 snapshots。

| Offset | Bytes | 內容 |
| --- | ---: | --- |
| 0 | 8 | OSAL SharedWord：empty=0、writing=1、committed=2 |
| 8 | 8 | 紀錄格式版本 1 |
| 16 | 8 | MID，讀取端必須與中央持有身分一致 |
| 24 | 8 | flags：bit0 有 index／size，bit1 文字被截短 |
| 32 | 8 | 偵測行號，解碼拒絕超過 uint32_t |
| 40 | 8 | index |
| 48 | 8 | size |
| 56 | 256 | reason（含終止 NUL） |
| 312 | 768 | file（含終止 NUL） |
| 1080 | 256 | function（含終止 NUL） |

狀態字遵循 OSAL 同主機 64-bit 原生 shared atomic 的大小、對齊與同步契約；其餘欄位明確
編碼為 little endian bytes，不 memcpy C++ struct 或 pointer。讀取者只解析 committed，
拒絕未知狀態、版本、MID、flag、非法行號與無終止文字；writing 回報 IncompleteFaultReport。
文字超長會截短並帶旗標；偵測位置來自 source_location，不保證是使用者呼叫端行號。
這不是跨版本 ABI 或惡意 Module 的安全沙箱。

### 中央診斷與限制

`ModuleSnapshot.last_contract_failure` 保存最後一次 `{sequence, mid, pid, exit,
termination_requested, exit_deadline_expired, report}`；`report` 是完整紀錄或不完整／格式錯誤的 Result。
MID／PID 來自中央紀錄，退出狀態來自 OSAL，不相信子程序自行提供的 PID。
`termination_requested` 只在 OSAL 回傳 Requested 時成立，不把 AlreadyExited 當成強殺。
它描述中央曾要求終止，不宣稱該要求必然是退出原因。
`exit_deadline_expired` 表示中央在尚未確認退出時觀察到有效 fatal／startup／stop 期限已到；
即使終止要求失敗也可成立，不得以此冒充 OS 已接受終止。

原有 `last_failure` 保持「同 MID 第一筆主要故障」優先；Socket 關閉即使先被觀察到，
也不遮蔽後續共享紀錄。成功重啟保留上一筆 contract 診斷；再次 contract 故障覆寫為較新一筆，
`forget_module` 移除整個 logical Module 的紀錄。這是有界的最後診斷，不是完整歷史日誌或磁碟保存。

Shell 本機／遠端自動顯示已觀察的診斷，`help` 可再次查詢；顯示不受 Module output 開關影響。
Shell 停滯或故障風暴時可能只觀察到每個 logical Module 最後一筆；TCP 接受或畫面輸出不代表
操作者已閱讀。完整提交後，只要中央仍保留該 generation 資源，即使 Socket 已斷仍能取回。
提交前的外部強殺、記憶體損毀、中央／OS／整台機器崩潰仍可能丟失，不能承諾任何故障都送達。

### 共享故障紀錄驗證

本段下方 50/50 為前輪歷史紀錄；本輪 fatal 監督的結果另列於後。

Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2：
Debug／Release 完整建置通過；全專案 CTest 各 **50/50** 通過
（Debug 98.76 秒，Release 89.13 秒）。最後的格式容量具名化與註解整理不改布局或行為；
Release 全套測試包含此整理，Debug 另同步重建產物。

- 新增 `module.contract`：真實子程序 `[]` 越界、另一 thread 持有 stderr 鎖、四個 thread
  同時觸發違約；確認退出後取回完整 index／size／reason／偵測位置與中央持有的 MID／PID。
  檢查不執行 `on_stop`、另一個 Module 仍回應、重啟後新 generation 正常且保留前次診斷，
  以及 `on_start` 故障已具備 reporter。
- 同套測試另以獨立固定 offset fixture 驗證：empty、writing、未知狀態、錯版本／MID、
  未知 flag、無 NUL 文字、同名排他建立、子程序端 owner 釋放不刪父名稱，以及最後清理後
  名稱不可再開啟。格式錯誤測試不假裝是正常 Module 行為。
- `shell.unit` 驗證 C0／ESC 顯示處理、身分、索引、截短提示及不完整紀錄文字；
  `shell.integration` 驗證 revision 不重複通知、help 可重查、本機 Shell 持續運作；
  `shell.socket` 以真實 TCP 驗證 output off 仍收到中央診斷，且之後仍能操作 help。
- 既有 Common catalog clean／缺檔／增量重建、錯誤原因鏈、OSAL、Message Storage
  契約終止、Control Channel、Bus、Module 生命週期、Tcl、Shell 與 SDK 測試均包含於上述全套。

Linux／macOS 未原生建置或執行；未注入 native mapping／unlink 失敗或完整的配置耗盡矩陣，
未量測效能，也未模擬中央／整台機器崩潰。這些結果不能當成 crash dump 或磁碟持久化保證。

### Fatal 監督驗證（2026-10-06）

新增 `module.contract_supervision`，使用正式 Manager／report writer 的測試專用編譯副本，
注入點與終止失敗 adapter 不進入正式 library。測試涵蓋：

- writing 狀態停滯、提交後尚未退出、同 thread 重入，以及延遲後成功提交。
  以真實子程序維持 heartbeat，確認中央仍偵測 fatal、拒絕新命令，另一 Module 可繼續回應。
- 中央首次觀察後的期限只設定一次；重複狀態及次級錯誤不能延後或提前該期限。
  startup、明確 stop 及整體 shutdown 的較早期限優先，不呼叫 fatal Module 的 stop hook。
- 完整／不完整 payload 只在確認退出後收集；OS 終止請求失敗時保留程序所有權及錯誤，
  下一次重試才終止。PermissionDenied 為 adapter 注入，不是假稱實際 Windows 拒絕權限。
- 重啟後新 MID 清除 fatal 狀態，保留舊 MID 診斷；Shell 區分期限已到與 OS 已接受終止請求。

Windows x64／MSVC 的 Debug 完整建置通過。全套初跑 51/52，唯一失敗是新增測試將
restart_delay 設得低於既有 100ms 下限；修正測試設定後該測試通過。
最後校正觀察計時點並重建後，Module／Shell **14/14** 通過（48.77 秒）；
其餘 38 項在前述全套中通過。Release 完整建置與最後版本全套 **52/52** 通過
（115.56 秒）。Linux／macOS 未執行，沒有新增效能或硬期限保證。

## RuntimeServices bridge 接線（2026-10-06）

Common 擁有最小 `detail::RuntimeServices` 契約與每個 binary image 的綁定；
Module System 的 `detail::HostRuntimeServices` 擁有不可移動的 service table，
其中 callback 只轉到 Host 擁有的 `ContractReportStorage::report_and_exit()`。
原有 ContractReporter／ContractReportBinding 已移除，不維護兩套狀態。

```text
common/include/vinc/detail/runtime_services.hpp       # table、借用 view、綁定契約
common/src/runtime_services.cpp                      # image-local pointer／reader 保護
module_system/include/vinc/module/detail/runtime_services.hpp
module_system/src/runtime/runtime_services.cpp       # Host provider、Result 接線
```

相依維持 Module System → Common／OSAL。Plugin 的 bridge 只需靜態 Common，
透過 table 的函式指標呼叫 Host，不直接連結 Host 的 Module Runtime、OSAL 或控制通道。
這些指標只在同一程序內使用，中央仍透過既有共享紀錄取得診斷；共享布局和 bootstrap 版本不變。
一般 Bus／Param 服務保留給 Context，未加入 table。

### 初始化及失效順序

1. Host 建立／映射該 MID 的故障紀錄，接著建立 HostRuntimeServices。
2. `bind_host()` 為本 image 綁定 table，失敗回傳 ModuleSystem ServiceFailed／
   BindRuntimeServices，保留 Common 原因；成功後才建立 Runtime 服務及使用者 Module。
3. Plugin 初始化接收同一份 table，在 Plugin image 內呼叫 Common 的 bind；
   不複製故障紀錄 owner。這輪用兩個真實 DLL fixture 驗證此接線。
4. 停止並 join 使用者／Plugin 工作，逐一清除各 image 的綁定；確認完成後才銷毀
   Host table 及故障紀錄。Host 的析構只管理自己的本地綁定，不能代替 Plugin 的清除。
   呼叫端必須管理這個順序；正式 LoadedPlugin owner 依此流程管理單一 Plugin。

清除保留現有「先撤銷 pointer，再等待已進入的 fatal reader」保護，不能在 callback 仍使用
context 時解除映射。非返回的故障 callback 可能讓清除無法返回，仍由前輪中央 fatal 監督
要求 OS 終止，bridge 不另加 timer／thread。未綁定、清除後或 callback 返回，Common
直接 `_Exit(77)`；已提交紀錄後仍 `_Exit(76)`。退出碼不證明回報已送達，中央以紀錄與 OS
ExitStatus 分別判斷。一般 STL／第三方／使用者例外政策未變更。

服務 table 的版本與大小驗證僅處理這個同建置環境的內部介面，不是中央版本檢查，
也不承諾跨編譯器／CRT 的穩定 ABI。DLL 靜態初始化早於注入時只能 fallback。
此 bridge 歷史輪次尚未實作正式 ModuleHost；後續 Plugin 遷移及驗證見本文末節，熱載入仍未實作。

### 驗證範圍

- `common.runtime_services`：錯版、短 table、缺 callback、重複綁定、清除後禁止重綁，
  錯誤 catalog，三種 fallback，以及無一般 C++ 配置下的 context／索引／來源位置交付。
- `module.runtime_services`：兩個真正 SHARED library 各靜態連結 Common；同一 Host
  注入兩份本地綁定，兩者均寫入 Host 的同一份預配置紀錄。驗證逐一回報、清除 A 後 B／Host
  仍可回報、未注入 B 不會誤用 A／Host 的綁定、清除及 Host teardown 後 fallback，
  以及 ModuleSystem 的初始化 Result 原因鏈。父程序只在 OS 確認退出後解碼，逾時會清理子程序。
- fixture 透過作業系統載入實際 DLL 並明確呼叫初始化；沒有把 fixture 當成正式 Plugin loader。
  沒有測試任意 DLL 卸載競爭、完整配置耗盡矩陣、signal handler 或機器崩潰。

Windows x64／MSVC 19.51.36257：Debug／Release 完整建置通過，全專案 CTest 各
**54/54** 通過（146.39 秒／115.05 秒）。最後消除測試哨兵的 C4722 警告並明確指定
其 C++23／警告選項後，兩種組態重新建置及 `common.runtime_services` 重跑通過；
正式 bridge 實作未因此變動。`dumpbin /exports` 確認 Plugin A 只匯出測試入口。
Linux／macOS 未原生建置或執行；ELF symbol visibility 設定尚待該平台驗證。

## Plugin Host 生命週期與故障隔離

1. Host 驗證 bootstrap v10，映射 fatal storage，綁定自己的 RuntimeServices。
2. 建立 Control Service、Session、Bus Client 與既有 watchdog。
3. 透過 OSAL DynamicLibrary 載入指定庫、驗證入口與種類，再初始化 Plugin bridge。
4. Plugin create 建立 Module；沿用 MID、名稱核准、on_start、命令及 on_stop 流程。
5. Plugin destroy 銷毀 Module；Host 關閉 Bus／Session、join watchdog，釋放 Client 與服務。
6. 清除 Plugin bridge、釋放 loader reference；最後清除 Host bridge 與 fatal storage。

LoadedPlugin 先於服務 owner 宣告，晚於它們析構；Module 物件只存在於 run_hooks 範圍。
使用者工作仍須在 on_stop／析構停止並 join。禁止在卸載後保留 Plugin callback、deleter、
vtable 或 thread。DllMain／loader hooks 不執行 bridge 綁定，靜態建構不執行 REGISTER_MODULE。
正常結束可釋放庫一次，不提供同程序 unload/reload 或 bridge rebind。

載入失敗回 ModuleSystem 的 PluginLoadFailed／InvalidPlugin／IncompatiblePlugin，
初始化失敗保留 Common 原因；OSAL 失敗保留原因鏈。故障透過已建立的通道回報中央，
只有該 Host 退出，中央與其他 Module 繼續運作。Bus lease 仍等確認 OS exit 後才回收。

`module.plugin` 驗證載入／符號查詢／move／close、缺庫、缺入口、入口版本、種類不符、
初始化拒絕、null factory、載入期停滯，以及 Plugin destroy。既有 module、fatal、
Bus、Shell、SDK 測試改用真實 Plugin；silent peer 與故障注入 Host 仍是測試專用執行檔。


## ModuleHost／Plugin 本輪驗證（2026-10-06）

Windows x64／MSVC 19.51（toolset 14.51.36231，compiler 19.51.36257）、Windows SDK
10.0.26100.0；完整 Debug／Release 建置及 SDK 公開標頭 smoke 成功。
最終全套 CTest 兩種組態各 **55/55** 通過，耗時 34.50／78.10 秒（不是效能基準）。
包含真實 DLL 載入、中文／空白路徑、缺庫／缺符號、入口版本／ModuleType 不符、
初始化拒絕、null factory、建構例外、載入停滯、Plugin destroy，以及 Host spawn failure。
既有 Module 啟停／重啟、雙向 heartbeat、fatal report 與有界監督、Bus 發布／訂閱／
退出回收、Shell／Tcl／Socket Shell、獨立 SDK consumer 皆經正式 Host／Plugin 路徑驗證。

Debug 首輪為 54/55，shell.console 的既有 fixture 寫死 .exe 路徑，改為 .dll 後全套通過。
故障注入 fixture 將 report writer 留在專用 Host，Module 本體另編為 Plugin；未把故障
checkpoint 加入正式 Host。Bootstrap v4 不提供相容模式，正式接線使用新增庫路徑的 v5。

檢查 Ninja 實際 link closure：Module 與 SDK Plugin 不含 vinc_control、Module Runtime、
Manager 或 Shell；仍有 vinc_control_protocol、Bus client 及必要工具。
`dumpbin /exports` 確認 Message Bus 範例 DLL 只有 `vinc_module_plugin` 匯出。
Linux／macOS 的載入分支與 PIC／visibility 設定尚未實機建置或驗證。

## Module 除錯支援（2026-10-06）

使用者已確認：中央啟動後附加 debugger；明確選擇本次啟動的除錯模式；自動 heartbeat／
startup／fatal 期限及自動重啟暫停；明確 stop／完整 shutdown 仍保留有界 OS 終止。
詳細操作、符號及限制見 [除錯指南](DEBUGGING.md)。

- `start_module(type, args, ModuleStartOptions{.debug = true})` 仍立即回傳 MID。
  Snapshot 保存 `debug`、`debug_waiting`、`plugin_path` 與既有 PID；建置組態不影響此政策。
- `wait_debug_ready(mid, deadline, stop)` 等 Host 到達載入 Plugin 前的 gate。
  呼叫者逾時／取消只結束等待，不停止背景 Host；以 help／inspect 找到 MID 後再停止。
- `continue_debug(mid)` 將一次性的放行 Event 排入 Session，成功不等於應用 Ready。
  重複放行、正常啟動的 Module、未知 MID 與正在停止／故障的 Module 均回 Result 錯誤。
- Host 建好 Session／Bus client／watchdog 後才送 DebugWaiting（Lifecycle Event=5，空 payload）；
  既有 watchdog 收 DebugContinue（Event=6，空 payload）喚醒主執行緒的條件變數。
  等待中 Stop 優先，不執行 Plugin load／create／hooks。沒有新增 thread 或定時 gate 輪詢。
- 中央在除錯啟動以無限 startup deadline 監督，仍觀察 OS exit；Host 建立連線與握手仍有界。
  必須等 DebugWaiting 後才附加。之後 heartbeat 不判逾時，但格式、EOF、背壓仍按原契約處理。
- fatal 仍標記故障、不再接納新工作，保留共享回報；自動 fatal deadline 為無限。
  明確停止會套用既有 stop_timeout，fault／stop deadline 與實際 OS exit 判讀仍分開。
- 不修改錯誤後的程式執行語意：contract report 完成仍 `_Exit`，一般失敗仍回報並退出。
  debugger 放行 fatal 不代表可回復運作；未附加 debugger 也不會自動觸發 debugger。
- 停止期限、命令及 Bus 請求期限不因除錯全面取消；debugger detach 不自動恢復正常監督，
  需停止並以一般方式重新啟動。共享 reader lease 仍只於釋放或確認 OS exit 後回收。

### 除錯支援驗證

Windows x64／MSVC 19.51.36257：最終 Debug／Release 完整建置成功，CTest 各 **56/56**。
耗時 197.03／189.70 秒，含外部 SDK 重新建置，不是效能量測。

- `module.debug`：正式 Host／Plugin 的載入前 gate、Shell MID／PID、放行／重複放行拒絕、
  掛起超過 startup 期限、正常 hook、崩潰不重啟、停止等待中的 Host，以及 shutdown 回收卡住的 hook。
- Windows 原生 DebugActiveProcess 附加並停在 breakpoint，超過 heartbeat／startup 期限仍存活；
  另一 Module 繼續回應；detach 後同一 Session 仍能執行命令。
  停在 breakpoint 時明確 stop，中央仍透過 OS 終止，debugger 收到實際 termination exit。
- `module.contract_supervision` 加驗 debug fatal 的無限自動期限，以及明確 stop 縮短期限；
  原有正常 fatal 監督、故障鏈、退出回收測試仍通過。
- `control.channel` 加驗停用 expiry 的靜默／恢復與 EOF、雙方政策不一致、舊 Hello、
  未知 Hello version／非法旗標拒絕；正常 heartbeat、握手、背壓等原有測試保留。
- `shell.integration` 加驗載入前等待期間 help 與 Ctrl+C 完整回收；完整 Socket／Tcl／SDK 回歸通過。

首輪原生測試曾提早關閉 debug event 的 process/thread handles，造成 detach 失敗；
已改由 debug session 管理，最終兩組測試通過。首輪 Debug 的外部 SDK fixture 沿用舊 object，
清理後重建成功；增量相依追蹤疑慮仍保留於 [SDK README](../sdk/README.md#本機外部-sdk-增量建置疑慮2026-10-06)。

紀錄：`build/debug-support-final-build.log`、`build/debug-support-release-final-build.log`、
`build/debug-support-debug-final-tests.log`、`build/debug-support-release-tests.log`。
Linux／macOS 尚未建置或執行 debugger 測試；Visual Studio GUI／PDB source breakpoint 未人工實測。

## Context／pump 實作驗證（2026-10-06）

Windows x64／MSVC：Debug、Release 完整建置與 SDK 公開標頭獨立編譯成功。
本輪兩組完整 CTest 各 **57/57** 通過（Debug 74.44 秒、Release 86.76 秒）；
包含外部 SDK 原始碼消費端、增量標頭相依、Plugin 交付、既有故障監督及 Shell 整合。
最後補上短 Manual 工作的 wait_started 競爭修正後，兩組重新完整建置，
Context、Module 程序／Manager／Runtime 故障、debug、兩項 Shell 與 SDK consumer
各 **8/8** 通過（Debug 14.21 秒、Release 14.33 秒）。這是測試耗時，不是效能量測。

新增 `module.context` 以真正 Plugin／ModuleHost 程序驗證：

- main 正常返回、收尾成功不重啟；直接 exit(0)、main 失敗、收尾失敗／卡住依故障政策處理。
- pump 固定發布上限、callback 中新增發布／Publisher／Subscriber 延至下一輪。
- 多 Publisher 合併、NewOnly 起點、get_last 不推進 callback 游標、空批次成功。
- 借出未 commit 已淘汰 slot 的覆蓋計數，不重複計數；callback 接管可留存的訊息。
- 遞迴 pump 拒絕、自行取消訂閱、主執行緒與自建 worker 的實際派送位置及停止。
- 忽略 callback 例外的 pump Result 仍退出／重啟，中央保存 SubscriptionCallback 故障來源。
- 已完成短工作的 wait_started 成功，wait_running 仍回 NotRunning。

故障與退出案例有 CTest 期限，Manager 持有及回收子程序。既有 Bus 保護讀者等待、
崩潰回收、父程序失聯、Plugin 版本拒絕及 SDK 消費端也在回歸範圍內。
Linux／macOS 未執行；未量測大量 Module 的輪詢、配置與 callback 負載成本。
本機紀錄為 build/context-*-build.log、build/context-*-tests.log，未納入版本控制。

## 共享鎖第二輪待處理事項

中央 admission、資源世代、Host 私有 lease、停止取消與確認退出回收已接線，契約與
測試案例見 [INSTANCE.md](INSTANCE.md)。後續 Param 接線已完成，契約見 [Param README](../param/README.md)。

- Linux／macOS 必須原生驗證本次 FileLock admission、資源清理與 Host 行為。
- epoch 前綴保留有容量上限；尚無離線維護／壓縮工具。到上限時明確回報啟動失敗，
  不自動刪除可能仍對應舊 mapping 的記錄。完整限制及下一步見 INSTANCE.md。

### 2026-10-08 第二輪驗證

Windows x64／MSVC Debug、Release 建置成功，各 61 套測試皆取得通過結果
（Release 的三套建置型測試補載工具鏈環境後重跑）。包含新增的 instance、locks、
lock_protocol、原有故障／退出測試及外部 SDK；詳細紀錄見 [INSTANCE.md](INSTANCE.md)。


## Context Log 與中央保存（2026-10-08）

Context 增加 context.log；Host 持有 adapter 與獨立 Log Session，Plugin 僅連入 API／fmt。
中央 Manager 擁有 collector／writer；Framework entry 自動設定保存路徑。
正常停止先排空 OutputHub，再停止 Log，最後釋放 Control Service。
Log 建立失敗可診斷，不當作 Module 啟停失敗；fatal report 仍獨立。
Context 布局更新為 Plugin entry version 5／bootstrap v10，需完整重新建置相連程式。
API、backpressure、fence 與容量見 [Log](../log/README.md)。

## Recorder 接線（2026-10-12）

Context 新增 list_topics(after)，仍借用 Host 的既有 Bus Client／控制 Session。
Bag Recorder 是 Manual Module：main 呼叫 pumpOnce 與有界週期探索，另有一個專用磁碟 worker。
內建錄製 Module 的收尾期限使用 config::bag_stop_timeout（30 秒）；CMake module helper 的
STOP_TIMEOUT_CONFIG 只調整該 ModuleType policy，不改其他 Module 預設或中央關閉政策。
見 [Bag README](../bag/README.md)。
