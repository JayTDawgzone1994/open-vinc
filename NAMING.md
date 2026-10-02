# Open-VINC 名稱與含義

本文件保存已確認的專案詞彙，供後續討論、設計與開發沿用。名稱以簡短、清楚為方向；
術語確認不代表對應功能已實作，也不代表既有程式碼已完成改名。

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

## 身分識別

| 名稱 | 含義 |
| --- | --- |
| `PID` | 作業系統分配的程序 ID。 |
| `MID` | Module ID，VINC 中央框架分配給 Module 的身分 ID。 |
| `VID` | 預留給未來 VINC 對 VINC 的識別；具體語意、配置與生命週期尚待設計，尚未實作。 |

PID 與 MID 屬於不同的識別空間，不能互相替代。MID 沿用目前框架 ID 的生命週期：
每次啟動或重新啟動都取得新 ID，在同一次中央框架程序生命週期內不重複；
不承諾跨中央框架重啟或跨機器唯一。Module 名稱另行維護，同時登記的完整名稱不得重複。

Module 的識別原先曾稱為 VID，現已統一為 MID，包含 `get_mid()`、`ModuleSnapshot.mid`
與 `mid.hpp`；此次改名不改變 ID 的配置與生命週期。

## Context 的範圍

Context 是先前 PH 設計的正式概念名稱，預計提供：

- 建立 Publisher／Subscriber 等訊息交換入口。
- 註冊及存取參數。
- 由使用者主動呼叫 `Context::pump()`，檢查訂閱資料、參數更新與到期工作，執行待處理 callback。

`pump()` 處理目前可處理的一批事件，沒有工作就返回；應用 callback 在呼叫它的 thread 執行。
這不代表使用者 callback 本身有執行時間保證。框架的 heartbeat、連線接收與停止監督
維持獨立，不以使用者呼叫 Context 為必要條件。

目前 Context 尚未實作。`Context` 與事件處理入口 `pump()` 的名稱已確認；
`vinc::Context` 的命名空間、`context()` 取得方式與 `ctx` 區域變數名稱仍屬示意，
完整 API 簽章、生命週期與每次處理的事件數量等細節仍待設計。

## 已確認的鎖分層

| 名稱 | 所在層與含義 |
| --- | --- |
| `FileLock` | OSAL 的專用檔案排他鎖抽象，封裝原生資源、等待與釋放。 |
| `FileLockGuard` | 成功取得 FileLock 後的持鎖物件，負責釋放本次持有的鎖。 |
| `InterprocessMutex` | 共用同步層的跨程序 mutex；使用 FileLock，補上本地 thread 互斥、持鎖 thread 與非遞迴規則。 |
| `InterprocessMutexGuard` | 成功取得 InterprocessMutex 後的持鎖物件，負責釋放本次持有的鎖。 |

鎖物件用來取得鎖；Guard 表示一次成功的持鎖，其生命週期結束時自動釋放鎖（RAII），
也可透過明確的解鎖操作提早釋放。這是鎖與持鎖物件的配對命名，不是將鎖類別改名為 Guard。

FileLockGuard 不綁定取得 thread，可以經同步交付後在其他 thread 釋放；
InterprocessMutexGuard 必須由取得鎖的 thread 釋放。
兩者目前已分層實作，細節見 [OSAL FileLock](src/vinc/osal/FILE_LOCK.md) 與
[共用同步層](src/vinc/sync/README.md)。

程式中的持鎖物件已統一命名為 `FileLockGuard` 與 `InterprocessMutexGuard`。

## 與既有程式碼的關係

命名整理保留既有 API 風格與行為，不藉機縮短 getter、改變大小寫慣例或刪除功能。
以下對照已套用到程式、專案內呼叫端與文件，未提供舊名稱相容別名：

| 舊名稱 | 目前名稱 |
| --- | --- |
| `ModuleDefinition` | `ModuleType` |
| `ModuleId`／`module_id.hpp` | `MID`／`mid.hpp` |
| `get_instance_id()`／`instance_id_` | `get_mid()`／`mid_` |
| `get_node_name()`／`node_name_` | `get_module_name()`／`module_name_` |
| `node_base_name` | `module_base_name` |
| `node_interface()`／`node_autoComplete()` | `module_interface()`／`module_autoComplete()` |
| `ModuleSnapshot.module`／`ModuleSnapshot.id` | `ModuleSnapshot.module_type`／`ModuleSnapshot.mid` |
| `register_module()`／`unregister_module_factory()` | `register_module_type()`／`unregister_module_type()` |
| `get_registered_factory_names()`／`get_active_node_names()` | `get_registered_module_type_names()`／`get_active_module_names()` |
| `has_factory()`／`has_active_node()` | `has_module_type()`／`has_active_module()` |
| `call_module_interface()`（啟動） | `call_module_type_interface()` |
| `call_node_interface()`（命令） | `call_module_interface()` |
| `call_module_autoComplete()`（種類） | `call_module_type_autoComplete()` |
| `call_node_autoComplete()`（模組） | `call_module_autoComplete()` |
| `InterprocessLock` | `InterprocessMutexGuard` |
| CMake `MODULE_NAME`／`NODE_NAME` | `MODULE_TYPE_NAME`／`MODULE_BASE_NAME` |

舊 testModule 的 static 建立入口同步改為 `module_type_interface()`，
與實例的 virtual `module_interface()` 區分；保留其原有行為。

真正建立物件的函式仍可稱 factory，例如 `ModuleFactory`；訊息儲存結構中的 node
也保留原意。Shell 顯示使用 ModuleTypes／Modules 與 MID／PID。

`ModuleSnapshot.generation` 目前保留，Module 接線時與 MID 同值；通用控制通道、
OutputHub 的 generation，以及 Bus slot 的 generation 不一概改成 MID。
Publisher、Subscriber、請求、Socket session 與 OSAL 資源 ID 保留各自語意。
類別、函式、變數、檔案的全域拼寫規則尚未另行制定。

新增或更新本文件時，只將使用者已確認的名稱列為決策；提案應明確標示。
後續若使用者更新名稱，以最新決策為準，並在獲得文件更新授權時同步記錄。
