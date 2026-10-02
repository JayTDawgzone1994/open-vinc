# 獨立程序 Module

每個模組實例在獨立 OS 程序執行。中央以 OSAL Process 管理生命週期，
中央與子程序使用同一個 `vinc::control` 元件透過 OSAL 本機串流 Socket 溝通。
模組作者繼承 `BaseModule`，無須處理 Socket、封包或 heartbeat。

stdout／stderr 由中央 [OutputHub](../output/README.md) 自啟動起擷取，保留名稱與
啟動 generation。本地 Shell 會顯示訂閱之後的輸出；沒有接收者仍持續讀取，不保留歷史。
`wait_shutdown()` 包含已退出程序的有期限輸出收尾，不保證所有目的地已顯示資料。

## 程式閱讀順序

1. [`BaseModule`](include/module/base_module.hpp)：應用 hook、MID／名稱與 Bus 的可用時機。
2. [`ModuleManager`](include/module/module_manager.hpp)：中央登記、啟停、命令與查詢介面。
3. [`module_protocol.hpp`](src/module_protocol.hpp)：中央與子程序共用的操作碼、啟動參數位置與回覆布局。
4. [`module_runtime.cpp`](src/module_runtime.cpp)：`run_module_process` 串起啟動流程；
   `parse_bootstrap`／`connect_to_central` 驗證並建立連線，`register_module_name` 完成名稱註冊，
   `dispatch_commands`／`dispatch_request` 執行使用者 hook，`watch_central` 獨立接收控制訊息與監督停止期限。
5. [`module_manager.cpp`](src/module_manager.cpp)：`launch` 依序重設 generation、準備 listener、
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
#include <module/base_module.hpp>
class Camera : public vinc::BaseModule {
public:
    using BaseModule::BaseModule;
    int32_t on_start(const std::vector<std::string>& args) override { return 0; }
    int32_t module_interface(std::vector<std::string> args) override { return 0; }
    void on_stop() override {}
};
REGISTER_MODULE(Camera, "cameraModule")
```

```cmake
vinc_add_process_module(camera
    MODULE_TYPE_NAME cameraModule MODULE_BASE_NAME cameraNode SOURCES camera.cpp)
```

框架提供 `main`／Windows `wmain`。`MODULE_TYPE_NAME` 必須與 `REGISTER_MODULE` 一致，
每個執行檔只能登記一種模組；同一執行檔可以啟動多個程序實例。
CMake 產生中央的模組目錄，Shell 不再把模組實作 whole-archive 連進自身。
生成目錄內是本次建置的絕對路徑；搬移部署時應以 `register_module_type(ModuleType)` 提供部署位置。

`exampleModule` 是不依賴舊 Bus 的新範例，`exampleNode` 是其預設實例名。
舊 `vinc_test` 原始碼與靜態庫保留，但不加入程序模組目錄；其舊 AxonBus 問題留待後續處理。

## 應用介面與所有權

- 工廠以預設名稱建構模組，框架以 `unique_ptr<BaseModule>` 持有它；此時名稱尚未登記。
- 建構後框架設定 MID，呼叫 `on_name(args)` 取得名稱提案；中央核准後才呼叫 `on_start(args)`，回傳 0 才送 Ready。
- `on_name` 可修改參數列表以消化命名選項，剩餘參數傳給 `on_start`。MID 可透過 `get_mid()` 讀取。
- 建構、`on_name`、`on_start`、`module_interface`、`module_autoComplete`、`on_stop` 與解構均在子程序主執行緒序列執行。命名期間已有 heartbeat 與啟動期限保護。
- 命名失敗不執行 `on_start`／`on_stop`，正常例外展開仍會解構物件；不可在命名階段啟動需要 `on_stop` 清理的工作。
- Socket I/O 與 heartbeat 在通訊執行緒執行；另一個框架監督執行緒接收控制事件並監看停止期限。
- 正常停止會呼叫 `on_stop` 並解構物件。使用者自行建立的執行緒必須在這段流程中結束。
- 中央失聯時執行 `std::_Exit(72)`，不保證 callback、解構或緩衝日誌完成。
  正常停止超過清理期限時也會強制退出（73）；中央同時可透過 OSAL 強制終止。
- 初始化例外使程序失敗；命令 callback 例外轉為 -111 回覆。

BaseModule 不再把 `this` 登記到中央的 singleton。舊 static 建立入口（保留於 testModule 的 `module_type_interface`）
由框架工廠取代；初始化搬至建構子或 `on_start`。尚未啟動的 ModuleType 靜態 autocomplete
目前回傳空清單，執行中 Module的 virtual autocomplete 已透過 Socket 接通。

## 中央 API 與命名

`ModuleManager` 可獨立建立，也保留 `get()` 相容入口。Shell 主程式目前明確持有並傳入 manager。
`register_module_type` 登記 ModuleType、絕對執行檔路徑、模組基礎名稱與政策。
`start_module` 回傳中央配置的非零 `MID`（`uint64_t`），啟動工作由監督執行緒進行；
回傳成功不代表程序建立成功或應用已就緒，使用 `wait_started(mid)`，再以 `inspect(mid)->name` 取得核准名稱。
`ModuleSnapshot` 以 `module_type` 表示種類名稱、`mid` 表示框架身分、`pid` 表示 OS 程序 ID。
`inspect`、等待、命令、停止與移除 API 都接受 MID 或名稱；尚未取得名稱時必須使用 MID。

名稱由 Module 的 `on_name(std::vector<std::string>&)` 提案，預設回傳建構時的名稱。
中央原子地登記名稱；撞名直接回報 `module already exists`，不再產生 `_1`／`_2`。
名稱不得為空、超過 128 bytes、含控制字元，或與 ModuleType、micro-program、保留控制命令衝突。
同一 manager 內最終名稱唯一；多個實例應由模組作者自行解析參數並回傳不同名稱。
`exampleModule` 與 `busExample` 範例提供 `--name`，這是範例自己的選項，並非所有模組都必須支援的框架旗標：

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

`inspect` 保留狀態、最後的 PID、generation、重啟次數、退出碼／signal、錯誤文字與原生 OS 錯誤。
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
意外的正常退出也算程序遺失；明確 stop 或中央正常關閉不觸發重啟。
停止逾時不放棄管理；若 OS 拒絕終止，保留程序紀錄及原生錯誤並繼續監看，不假造已退出。

中央析構要求所有子程序停止，等待各政策停止期限加觀察寬限，必要時要求 OS 強制終止。
OS 終止失敗或程序完全無法排程時，無法提供絕對的退出時限。

完整關閉使用 `begin_shutdown()` 停止接受新工作，再呼叫 `wait_shutdown(deadline)`。
Timeout 時仍持續監督，可以再次等待；成功代表受管理程序已確認退出且 Bus 回收佇列已排空。
`snapshots()` 可顯示等待中的模組與 OS 錯誤。互動 Shell 的 Ctrl+C／exit 使用此流程，
等待到完成才退出，不把析構的時間限制當成程序已退出的證明。

## 命令與後續擴充

既有 `module_interface(args)` 與 `module_autoComplete(args)` 的呼叫已由中央代理經 Socket 送到子程序。
中央每個模組同時只接受一個等待回覆的請求；忙碌時回報 Backpressure。
逾時表示結果未知，不表示指令沒執行或已取消，不自動重送。
重啟／斷線會結束舊會話的等待。逾時後保留未完成請求的 Busy 狀態，直到收到其遲到回覆或程序退出；
遲到回覆不交付已結束的呼叫，但會釋放 Busy。回覆帶 request ID 並核對 domain／operation。
舊 `call_*_interface` 保留整數相容入口；新 Shell 使用 `command`／`complete` 的 Result API 保留錯誤。
互動 Shell 在工作執行緒等待啟動／命令／補全，鍵盤仍可用；既有腳本入口維持同步。
`wait_started` 依模組啟動期限加一秒等待 Ready；逾時不隱含取消背景啟動。

程序啟動標記升為 `--vinc-module-v2`，中央與模組須一同重新建置。
Lifecycle Domain=1：Ready Event=1（payload 為名稱字串）、Stop Event=2（空 payload），
RegisterName Request/Response=3（request ID=1）。Request 為提案名稱；Response 為 u32 狀態
（0 成功、1 拒絕）與核准名稱或錯誤字串。核准前不得送 Ready 或 Bus 請求。
Command Domain=2：Execute Request=1、Complete Request=2，payload 為參數序列。
Response 先放 i32 狀態的 u32 位元編碼，Complete 成功後再放字串序列。
未知 Request 回覆 -110；不認識的控制 Event 關閉連線。

[Shared Memory Bus](../bus/README.md) 已接上：在 `on_start()`／後續 hook 使用受保護的 `bus()`
取得 Client，可建立 Publisher／Subscriber；建構子與 `on_name` 尚不可呼叫。
Module watchdog 統一路由 Bus Domain=3 的控制訊息；中央另有 Broker 執行緒管理註冊與共享區。
大量資料不進 Socket，中央確認子程序退出後才清除其 reader 占用與發布歷史。
`ModuleManager(bus::Limits)` 可配置 Bus 容量；預設建構使用預設限制。
`busExample` 提供可操作的跨程序發布／接收範例。
每筆資料的 callback 通知及框架間多機通訊尚未實作。

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
驗證包含 Module 程序生命週期、Control Channel、Bus 與 Shell／Socket Shell／Tcl 整合。
本輪未更動測試情境與既有等待值；Linux／macOS 仍未執行。

## 待處理事項

以下項目尚未修正；本輪只整理可讀性，並保留現有公開介面、協定、鎖範圍與錯誤政策。

- **已確認：錯誤介面與子程序診斷尚未統一。**
  `module_runtime.cpp` 的應用生命週期例外集中回傳 70，watchdog 例外以 75 結束，
  命令 hook 例外回覆 -111；例外內容未在這些邊界保留下來。
  `module_manager.cpp` 的服務初始化、端點目錄建立與 MID 耗盡也仍有 `throw`。
  因此目前不能只靠統一的 `vinc::Error` 查得所有 Module 失敗原因。
  待錯誤介面遷移輪次，再討論穩定錯誤碼、原生錯誤及診斷資訊如何保留；
  本次為既有子程序退出碼加名稱，不代表它們已加入共同 Error 表。
- **待修正／驗證：測試 helper 的 UTF-8 路徑轉換。**
  `tests/module_helper.cpp` 的 `on_start` 將 `std::string` 的 char 儲存區轉為
  `const char8_t*`，再用它建構 `std::u8string`。相同位元寬度不足以保證別名存取合法，
  會影響啟動／停止 marker 測試的可攜性；與 concurrency 測試記錄的疑慮相同。
  建議後續改為真正的 char8_t 儲存區與明確複製，再驗證非 ASCII 路徑。
  本輪保留原轉換並標記 TODO；Windows 測試通過不能證明這種型別存取在所有平台合法。
  Manager 與 Windows main 的反向轉換是經 char 讀取 char8_t 物件位元組，屬於不同的別名方向。
