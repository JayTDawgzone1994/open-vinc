# Open-VINC 名稱與含義

本文件保存已確認的專案詞彙，供後續討論、設計與開發沿用。名稱以簡短、清楚為方向；
術語確認不代表對應功能已實作，也不代表既有程式碼已完成改名。
本文件維護名稱、含義與必要區別；API、建置接線及生命週期的完整契約由元件文件維護。
工作流程、文件更新授權與衝突處理依 [AGENTS.md](AGENTS.md)。

## 已確認的核心名稱

| 名稱 | 含義 |
| --- | --- |
| `ModuleType` | 可啟動的模組種類，描述要建立哪一種 Module。 |
| `Module` | 由 ModuleType 啟動而建立的模組實例。討論時直接稱 Module，不必再加長為 ModuleInstance。 |
| `BaseModule` | 使用者繼承的 C++ 模組基底類別，提供生命週期與命令 hook；保留此名稱及 `base_module.hpp`。 |
| `Context` | Module 使用框架服務的入口，取代先前討論中的 PH／process header 名稱。 |

一個 ModuleType 可以建立多個 Module，例如相同相機種類啟動成 `cameraFront` 與
`cameraRear`。ModuleType 在建立 Module 後仍存在，可以繼續啟動其他 Module；
種類與實例是兩個不同概念。

「啟動後稱 Module」是在區分種類與實例，不表示必須等到 Running 狀態才可稱 Module，
也不代表停止後會變回 ModuleType。

## Module System 命名

**Module System** 是管理模組執行與生命週期的元件，目錄為 `src/vinc/module_system/`，
共同維護 `BaseModule`、子程序 Runtime 與中央 `ModuleManager`。
統一錯誤分類使用 `ErrorDomain::ModuleSystem`；Module 仍指啟動後的模組實例。

標頭使用 `<vinc/module/...>`。Runtime、Manager 與子程序入口的 target 及錯誤契約見
[Module System README](src/vinc/module_system/README.md)。

## 身分識別

| 名稱 | 含義 |
| --- | --- |
| `PID` | 作業系統分配的程序 ID。 |
| `MID` | Module ID，VINC 中央框架分配給 Module 的身分 ID。 |
| `VID` | 預留給未來 VINC 對 VINC 的識別；具體語意、配置與生命週期尚待設計，尚未實作。 |

PID 與 MID 屬於不同的識別空間，不能互相替代。MID 沿用目前框架 ID 的生命週期：
每次啟動或重新啟動都取得新 ID，在同一次中央框架程序生命週期內不重複；
不承諾跨中央框架重啟或跨機器唯一。Module 名稱另行維護，同時登記的完整名稱不得重複。

MID 型別保持 uint64_t，有效範圍由 `mid.hpp` 定義為 1～2^63−2；0 無效，
2^63−1 保留給中央的共享鎖 OwnerId，不發給 Module。耗盡回報錯誤，不重用或回繞。
共用數值邊界置於 common 的 `owner_id.hpp`，Concurrency 不依賴 Module System。

Module 的識別使用 `get_mid()`、`ModuleSnapshot.mid` 與 `mid.hpp`。

## Message Bus 命名

跨程序訊息交換元件統一稱為 **Message Bus**，目錄使用 `message_bus`，命名空間為
`vinc::message_bus`。使用入口與接線見 [Message Bus README](src/vinc/message_bus/README.md)。

Publisher、Subscriber、Client 保留既有職責名稱。環形緩衝區命名為 `RingBuffer`，
共享控制標頭命名為 `RingBufferHeader`，兩者由 Message Bus 內部維護。
RingBuffer 包含 Publisher 的讀寫保護協定，不屬於通用容器層；契約見
[Message Bus 設計](src/vinc/message_bus/DESIGN.md)。

中央管理元件命名為 `BusManager`，
負責 topic、Publisher／Subscriber 註冊、共享資源預算、拓樸通知與退出回收；訊息 payload 不經過它。
它是框架接線介面，位於 `vinc::message_bus::detail`。

## Topic 與 Param 名稱分隔符

以下為已確認的命名決策。Topic 分隔符已由 Client 與中央驗證，實作與測試紀錄見
[Message Bus README](src/vinc/message_bus/README.md#topic-分隔符驗證2026-10-08)；Param 見
[Param README](src/vinc/param/README.md)。

| 名稱種類 | `.` | `/` | 範例 |
| --- | --- | --- | --- |
| Topic | 禁止 | 允許 | `camera/frames`、`frames` |
| Param | 用於 Module 與參數的歸屬分隔 | 禁止 | `camera.exposure` |

Topic 的 `分類/名稱` 是使用建議，不強制含有 `/`，也不要求前綴等於發布者的 Module 名稱。
使用者可依用途命名，但須遵守上述分隔符限制；同一 Topic 仍可有多個 Module 發布。
Topic 的分段不建立子 Module 或子 Context，也不由 Context 自動加上 Module 前綴。

Param 的完整名稱使用 `module.param`；登記自己的參數時可使用參數簡稱，由 Context
依自己的 Module 身分補上歸屬。完整名稱恰有一個點，各段限 ASCII 字母、數字、`_`、`-`；
完整名稱含 Module 與點上限 256 bytes。Module 名稱同樣不含點或斜線，`vinc` 保留給中央。
字串參數內容與檔案路徑仍支援 UTF-8；型別、權限與快取行為由 Param README 維護。

收到訊息的實際發布者透過既有 `publisher_name()` 取得，保留此 API 名稱，不改為
`get_publisher()`。發布者 metadata 由框架填入，使用者不必自行填寫，也不從 Topic
名稱推測來源；`publisher_instance()` 是 Publisher ID，不是 Module MID。

## 訊息容器與存取名稱

| 名稱 | 含義 |
| --- | --- |
| `MessageArray` | 訊息欄位使用的陣列容器；也可在程序內獨立使用，但使用此容器不代表內容符合跨程序傳輸契約。 |
| `RingBuffer` | Message Bus 為 Publisher 維護的訊息佇列，保存多筆訊息；與單筆訊息內的陣列欄位分工不同。 |
| proxy | 借用原資料的輕量存取介面；複製 proxy 保持借用關係，不產生內容的獨立副本，唯讀權限也不會提升。 |

Proxy 的有效期依元素種類、節點及借用 lease 的契約決定；詳細失效條件與複製方式見
[Message Storage README](src/vinc/components/message_storage/README.md)。

## OutputHub 命名

中央輸出管理元件統一稱為 **OutputHub**，目錄與命名空間使用 `output_hub`，
完整類別名稱為 `vinc::output_hub::OutputHub`。
`TextRenderer`、`Subscription` 等保留各自的職責名稱，仍由此元件維護。
使用入口與輸出契約見 [OutputHub README](src/vinc/output_hub/README.md)。

## Shell 命名

中央操作介面統一稱為 **Shell**，元件目錄為 `src/vinc/shell/`，
框架內部類別完整名稱為 `vinc::Shell`，由中央管理生命週期，不作為使用者的服務啟停 API。
輔助型別仍在 `vinc::shell` 命名空間，`SocketShell`、`CommandRouter` 等保留職責名稱。
中央程式叫 `Vinc.exe`／`Vinc`；使用入口與建置接線見 [Shell README](src/vinc/shell/README.md)。

## Context 與 pump 的命名狀態

`Context` 是中央核准、由 Host 綁定到 BaseModule 的服務入口；使用者使用繼承的 `context`。
`pumpOnce()` 處理一輪固定範圍的事件，`pumpLoop()` 反覆派送至框架停止；`living()` 查詢本地繼續執行狀態。
繼承 BaseModule 時明確選擇 `RunMode::FrameworkLoop` 或 `RunMode::Manual`。
完整 API、執行緒與停止契約見 [Module System README](src/vinc/module_system/README.md#context-與執行模式)。

## 已確認的鎖分層

共用工具集中於 `src/vinc/components/`，並行控制工具位於 `components/concurrency/`，
命名空間為 `vinc::concurrency`。此分類目前提供跨程序互斥鎖，生命週期由使用它的元件管理。

| 名稱 | 所在層與含義 |
| --- | --- |
| `FileLock` | OSAL 的專用檔案排他鎖抽象，封裝原生資源、等待與釋放。 |
| `FileLockGuard` | 成功取得 FileLock 後的持鎖物件，負責釋放本次持有的鎖。 |
| `InterprocessMutex` | 共用同步層的跨程序 mutex；使用共享記憶體原子所有權與發布選擇位元，由中央確認程序退出後接管。 |
| `InterprocessMutexGuard` | 成功取得 InterprocessMutex 後的持鎖物件，負責釋放本次持有的鎖。 |

鎖物件用來取得鎖；Guard 表示一次成功的持鎖，其生命週期結束時自動釋放鎖（RAII），
也可透過明確的解鎖操作提早釋放。這是鎖與持鎖物件的配對命名，不是將鎖類別改名為 Guard。

FileLock 與 InterprocessMutex 現為不同實作，後者不再包裝前者；Guard 契約見 [OSAL FileLock](src/vinc/osal/FILE_LOCK.md) 與
[共用同步層](src/vinc/components/concurrency/README.md)。

## API 命名與適用邊界

API 名稱須區分 ModuleType 與 Module，例如 `register_module_type()` 登記種類，
`get_active_module_names()` 查詢實例。Getter 保留 `get_mid()`、`get_module_name()` 等既有風格；
完整 API 契約見 [Module System README](src/vinc/module_system/README.md)。
已完成更名的舊名稱與路徑不提供相容別名；命名整理不順便改變行為或刪除功能。

真正建立物件的函式仍可稱 factory，例如 `ModuleFactory`；訊息儲存結構中的 node
也保留原意。Shell 顯示使用 ModuleTypes／Modules 與 MID／PID。

`generation` 表示所屬元件的世代識別，不一概改成 MID；ModuleSnapshot 與 MID 的對應由
Module System README 維護。通用控制通道、OutputHub 與 Message Bus slot 各自保留 generation 語意。
Publisher、Subscriber、請求、Socket session 與 OSAL 資源 ID 保留各自語意。
類別、函式、變數、檔案的全域拼寫規則尚未另行制定。

本文件只將使用者已確認的名稱列為決策；提案應明確標示。更新流程依 AGENTS。

## Vinc 名稱

`VINC_NAME` 是建置時指定的本機中央名稱，用於同名排他啟動與共享資源紀錄歸屬。
它不等於 Module 名稱、MID 或預留的 VID。其範圍與生命週期見
[Module System INSTANCE.md](src/vinc/module_system/INSTANCE.md)。
