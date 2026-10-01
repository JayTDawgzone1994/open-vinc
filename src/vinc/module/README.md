# 獨立程序 Module

每個模組實例在獨立 OS 程序執行。中央以 OSAL Process 管理生命週期，
中央與子程序使用同一個 `vinc::control` 元件透過 OSAL 本機串流 Socket 溝通。
模組作者繼承 `BaseModule`，無須處理 Socket、封包或 heartbeat。

## 建立模組

```cpp
#include <module/base_module.hpp>
class Camera : public vinc::BaseModule {
public:
    using BaseModule::BaseModule;
    int32_t on_start(const std::vector<std::string>& args) override { return 0; }
    int32_t node_interface(std::vector<std::string> args) override { return 0; }
    void on_stop() override {}
};
REGISTER_MODULE(Camera, "cameraModule")
```

```cmake
vinc_add_process_module(camera
    MODULE_NAME cameraModule NODE_NAME cameraNode SOURCES camera.cpp)
```

框架提供 `main`／Windows `wmain`。`MODULE_NAME` 必須與 `REGISTER_MODULE` 一致，
每個執行檔只能登記一種模組；同一執行檔可以啟動多個程序實例。
CMake 產生中央的模組目錄，Shell 不再把模組實作 whole-archive 連進自身。
生成目錄內是本次建置的絕對路徑；搬移部署時應以 `register_module(ModuleDefinition)` 提供部署位置。

`exampleModule` 是不依賴舊 Bus 的新範例，`exampleNode` 是其預設實例名。
舊 `vinc_test` 原始碼與靜態庫保留，但不加入程序模組目錄；其舊 AxonBus 問題留待後續處理。

## 應用介面與所有權

- 工廠以中央指定的名稱建構模組，框架以 `unique_ptr<BaseModule>` 持有它。
- 建構完成後呼叫 `on_start(args)`；回傳 0 才送 Ready。建構與初始化期間 heartbeat 已開始。
- 建構、`on_start`、`node_interface`、`node_autoComplete`、`on_stop` 與解構均在子程序主執行緒序列執行。
- Socket I/O 與 heartbeat 在通訊執行緒執行；另一個框架監督執行緒接收控制事件並監看停止期限。
- 正常停止會呼叫 `on_stop` 並解構物件。使用者自行建立的執行緒必須在這段流程中結束。
- 中央失聯時執行 `std::_Exit(72)`，不保證 callback、解構或緩衝日誌完成。
  正常停止超過清理期限時也會強制退出（73）；中央同時可透過 OSAL 強制終止。
- 初始化例外使程序失敗；命令 callback 例外轉為 -111 回覆。

BaseModule 不再把 `this` 登記到中央的 singleton。舊 static `module_interface` 建立物件的方式
由框架工廠取代；初始化搬至建構子或 `on_start`。尚未啟動的 factory 靜態 autocomplete
目前回傳空清單，執行中節點的 virtual autocomplete 已透過 Socket 接通。

## 中央 API 與命名

`ModuleManager` 可獨立建立，也保留 `get()` 相容入口。Shell 主程式目前明確持有並傳入 manager。
`register_module` 登記種類、絕對執行檔路徑、實例基礎名稱與政策。
`start_module` 原子地保留完整名稱並回傳名稱，啟動工作由監督執行緒進行；
回傳成功不代表程序建立成功或應用已就緒，使用 `wait_running` 或 `inspect` 確認。

名稱由中央配置 `name`、`name_1`、`name_2`…，避開既有實例、factory、micro-program 與保留的控制命令名稱。
同一 manager 內的最終完整名稱唯一；不同框架程序的命名空間相互獨立。
重啟保留名稱，但更換 generation 與連線，舊請求不會套用到新程序。
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

既有 `node_interface(args)` 與 `node_autoComplete(args)` 的呼叫已由中央代理經 Socket 送到子程序。
中央每個模組同時只接受一個等待回覆的請求；忙碌時回報 Backpressure。
逾時表示結果未知，不表示指令沒執行或已取消，不自動重送。
重啟／斷線會結束舊會話的等待。逾時後保留未完成請求的 Busy 狀態，直到收到其遲到回覆或程序退出；
遲到回覆不交付已結束的呼叫，但會釋放 Busy。回覆帶 request ID 並核對 domain／operation。
舊 `call_*_interface` 保留整數相容入口；新 Shell 使用 `command`／`complete` 的 Result API 保留錯誤。
互動 Shell 在工作執行緒等待啟動／命令／補全，鍵盤仍可用；既有腳本入口維持同步。
`wait_started` 依模組啟動期限加一秒等待 Ready；逾時不隱含取消背景啟動。

Lifecycle Domain=1：Ready Event=1（payload 為名稱字串）、Stop Event=2（空 payload）。
Command Domain=2：Execute Request=1、Complete Request=2，payload 為參數序列。
Response 先放 i32 狀態的 u32 位元編碼，Complete 成功後再放字串序列。
未知 Request 回覆 -110；不認識的控制 Event 關閉連線。

[Shared Memory Bus](../bus/README.md) 已接上：在 `on_start()`／後續 hook 使用受保護的 `bus()`
取得 Client，可建立 Publisher／Subscriber；建構子尚不可呼叫。
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
2026-10-01：Windows x64、MSVC 19.51.36257、SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2，
完整專案 Debug／Release 建置成功，兩種組態各七套 CTest 全通過（四套既有 OSAL 加三套新增測試）。
Linux／macOS 尚待原生建置與行為驗證；未進行效能量測。
