# VINC SDK 與元件目錄

對外整合入口是 `<vinc/vinc.hpp>`，CMake target 為 `vinc::sdk`。
它整合現有框架 API 與連結相依；不建立額外 runtime，不因 include 就啟動任何服務。
正式 API 仍由各元件維護，OSAL 可與只依賴標準函式庫的 common 一併獨立使用。
整合入口包含 common 的 `<vinc/time.hpp>`，可直接使用 `vinc::Deadline`；亦包含 `<vinc/contract.hpp>` 的契約終止入口。

整合入口亦包含 `<vinc/osal/path.hpp>` 的嚴格 UTF-8 路徑轉換；
契約與限制見 [OSAL](../osal/README.md#utf-8-與原生路徑)。

## 使用方式

一般 Module 作者可先看 [userExampleModule C++／Python 對照](../../../examples/userExampleModule/README.md)，
包含可執行的 Shell 操作與 API 閱讀索引。

以下是 C++ SDK。可選的系統 Python 綁定位於 Module System，建置開關為 `VINC_BUILD_PYTHON`；
不會讓一般 C++ 使用端或中央連入 Python。Context 的 Bus／Param／Log、訊息容器與 ABI 限制見
[Python SDK](../module_system/python/README.md)。

```cpp
#include <vinc/vinc.hpp>
```

```cmake
# 使用原始碼套件；請先初始化 library/ 下 Tcl 與 toml++ submodule。
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(VINC_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(VINC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/open-vinc/src open-vinc)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE vinc::sdk)
```

## 公開 CMake functions

`add_subdirectory(path/to/open-vinc/src open-vinc)` 後，即可在使用者自己的 CMakeLists 呼叫：

```cmake
add_message_dir("${CMAKE_CURRENT_SOURCE_DIR}/messages")
add_message_dir("${CMAKE_CURRENT_SOURCE_DIR}/camera/messages")

add_module(camera_module
    MODULE_TYPE_NAME Camera
    SOURCES camera.cpp camera_driver.cpp
)
target_include_directories(camera_module PRIVATE include)
```

- `add_module(target MODULE_TYPE_NAME name SOURCES ...)` 建立 MODULE 動態庫，自動連結
  `vinc::sdk`／`vinc::messages`，沿用 C++23、Plugin 可見性及 Windows CRT 契約。
  target 是一般 CMake target，可另外呼叫 target_link_libraries／target_include_directories。
  ModuleType 名稱須與 C++ `REGISTER_MODULE` 相同；同次組裝重複種類或 target 會拒絕。
- 不再提供 `MODULE_BASE_NAME`。Plugin 以無參數建構子建立；Module source 可使用
  `CameraModule() : BaseModule("cameraNode") {}` 提供預設提案，或覆寫 `on_name(args)`。
  中央仍驗證名稱、拒絕重名並保留重啟名稱一致性。詳見 Module System。
- Framework 內部亦可指定 `STOP_TIMEOUT_CONFIG`，值為既有 vinc::config 常數名稱；
  一般使用者不必指定。這不是新增的執行期 policy API。
- `add_message_dir(directory)` 登記該目錄直接包含的 `.msg`，不遞迴。相對路徑以呼叫端
  CMakeLists 所在目錄解讀。不存在的目錄拒絕；相同 canonical 目錄去重。
  內建 vinc_messages 目錄保留，可增加多個使用者目錄；所有訊息共用 vinc 命名空間。
- 配置結束時統一收集一般 CMake 呼叫，外部父專案在 add_subdirectory 返回後登記仍有效；
  不需手動 finalize。請在正常配置流程呼叫，不在使用者自己的 DEFER callback 中晚登記。
- `.msg` 在配置時登記；Build 先編譯 Ribosome、統一生成標頭與型別清單，再編譯所有相依
  使用端。不在 configure 期間偷做巢狀編譯。每次建置仍重新生成，沿用失敗封閉契約。
  新增／刪除 `.msg` 由 CMake 追蹤，重名（含大小寫）拒絕；跨目錄引用由 Ribosome 解析。
- 中央 catalog 延至外部父專案配置結束後收集，建置中央亦建置所有已登記 Plugin。
  關閉 VINC_BUILD_APPS 時仍可建置 Plugin／訊息，但不產生中央；不是即時載入 Plugin。

上述 functions 仍以原始碼套件提供；不要求使用者另外 include 私有 CMake 檔。
舊 `vinc_add_process_module()` 名稱不保留。生成標頭沿用 `<vinc_messages/Name.hpp>`；
Bag 與啟用時的 Python SDK 取得同一份合併型別清單，沒有新增 runtime schema registry。

### 本輪驗證（2026-10-12）

Windows x64、MSVC 19.51.36257、CMake 4.3.2、Ninja 1.13.2，啟用系統 Python 3.13：

- Debug／Release 完整建置成功；Debug 完整 CTest 75/75 通過。
- Release 完整 CTest 初次 74/75 通過；Socket Shell 測試將非同步 Log 通知的提示符
  誤認為 help 回覆結尾。測試改為先辨認命令回覆，不修改 Shell runtime。
  修正後 Release 連續 5 次、Debug 補跑 1 次均通過；其餘 74 項未因測試修正重跑。
- 兩種組態的 sdk.external_consumer 均通過：父專案後註冊 Module 與訊息、跨目錄相依、
  訊息修改／新增／刪除、無效 schema 失敗與復原、重複 ModuleType／大小寫訊息碰撞，
  以及中央實際啟動外部 Module 並讀到更新後的訊息常數。
- 生成器舊獨立來源副本腳本仍有過時假設，未列入本輪測試；見
  [Ribosome 待處理事項](../ribosome/README.md#待處理事項)。Linux／macOS 本輪未驗證。

本機紀錄位於 build/cmake-api-*-tests.log；Plugin entry 6／bootstrap v11 的相容性與
重建要求見 [Module System](../module_system/README.md#source-擁有實例命名與公開-cmake-接線2026-10-12)。

`vinc::sdk` 本身不提供 main、不生成使用者訊息；include 與連結不會啟動服務。
中央 `Vinc` 啟動時自動初始化 Tcl 並開啟 Socket Shell，兩者都是預設功能，
使用者不負責呼叫初始化或服務啟停函式；Module 子程序也不建立中央 Shell。
上例關閉 `VINC_BUILD_APPS` 是只建置 SDK 消費端；需要中央執行檔時保留該選項為 ON。
SDK 對 Shell 只提供錯誤分類與 `SocketShellOptions` 設定資料；
`Shell`、`SocketShell`、`ScriptService`、`TclRuntime` 和初始化函式均屬框架內部接線。
使用者透過中央 CLI 調整 IP、port 與 timeout，詳見 [Shell README](../shell/README.md)。
框架建置使用 `vinc_deploy_tcl(target)`，預設部署到 target 實際輸出目錄，亦可明確指定目的地；
部署 helper 的契約與平台限制見 [Tcl 整合](../../../library/tcl/README.md)。
登記的自訂訊息與專案內建訊息統一使用 `vinc::messages`（既有 `vinc_messages` target 仍可用），
並另外 include `<vinc_messages/name.hpp>`。SDK 不包含 Ribosome 開發介面。
目前交付方式為原始碼加 `add_subdirectory`；尚未提供安裝後的 `find_package(VINC)` 套件。

整合入口會帶入較多標頭與連結相依。需要較小相依集合時，直接選用元件的標頭與 target。

## 元件與 target

| 元件 | target | 責任 |
| --- | --- | --- |
| common | `vinc::common` | 統一 Result／Error 基礎、暫時 config |
| 錯誤查詢整合 | `vinc::errors` | 組合各元件 C++ 靜態錯誤表，提供唯一文字查詢入口 |
| osal | `vinc::osal` | 通用 OS 能力 |
| components/concurrency | `vinc::concurrency` | 框架並行工具 |
| control_channel | `vinc::control` | 控制通道、協定與 heartbeat |
| output_hub | `vinc::output_hub` | 程序輸出擷取、分送與呈現 |
| components/message_storage | `vinc::message_storage` | BaseMessage、MessageArray 與生成訊息支援 |
| message_bus | `vinc::message_bus` | Publisher／Subscriber、BusManager 與 RingBuffer |
| param | `vinc::param` | Context 參數 facade、型別與 codec；Host／中央內部 target 見 Param README |
| module_system | `vinc::module` | BaseModule、Plugin client 相依 |
| module_system | `vinc::module_manager` | 中央 ModuleManager |
| module_system | `vinc::module_entry` | 提供 Host main／wmain 的 object target |
| shell | `vinc::shell` | 框架內部的 Shell、Socket Shell、Tcl 執行；設定及錯誤型別公開 |
| vinc_messages | `vinc::messages` | .msg 定義與生成標頭使用需求 |
| sdk | `vinc::sdk` | 以上現有框架 API 的便利入口，不含生成訊息 |

## 目錄與相依原則

```text
component/
├── CMakeLists.txt
├── README.md
├── include/vinc/<component>/
│   ├── public_api.hpp
│   └── detail/                 # 內部接線／布局／native 宣告
├── src/                        # .cpp；可按 runtime、host 或平台分目錄
└── tests/                      # 測試專用 helper 亦留在此處
```

- 正式程式的標頭留在元件的 include，實作留在 src。測試 helper 不成為 SDK 的一部分。
- detail 可被編譯器看見，不代表外部 API 承諾；元件之間只依賴 README 指定的接線契約。
- CMake target 傳遞標頭／連結需求，不加入其他元件的 src include 路徑。
- 模板所需的內部定義可隨標頭提供；不為了目錄外觀改變模板或 runtime 行為。
- `apps/vinc` 組裝中央 main 與 Module catalog；`examples/module`、`examples/message_bus` 是應用範例。
- 根目錄的 [`vinc_messages`](../../../vinc_messages/README.md) 套件擁有 `messages/*.msg`；生成結果位於本次建置的 `generated/include/vinc_messages`。
  不同建置目錄各自生成，原始碼目錄不再保存生成的訊息標頭。
- ModuleManager 與 BaseModule hooks 已使用 common Result；整數狀態錯誤表已移除。
  Manager 使用明確 `create()`，Shell 以 reference 注入，沒有隱含 singleton。
- Context／pump 已接入 BaseModule；公開入口包含 `<vinc/module/context.hpp>`。模式與生命週期見 Module System README。
- Common RuntimeServices bridge 為框架內部接線，不加入 `vinc.hpp`；正式入口僅新增
  `common_error.hpp` 錯誤分類。Plugin／Host 綁定生命週期見 Common 與 Module System 文件。
  `add_module` 建立動態庫；正式 ModuleHost loader 與生命週期見 Module System。

## 元件接線契約

- Module Runtime 透過 `vinc/message_bus/detail/runtime.hpp` 建立、接收與關閉 Message Bus Client。
- ModuleManager 透過 `vinc/message_bus/detail/bus_manager.hpp` 提交控制要求、通知已確認退出與等待回收。
- Message Bus 透過 `vinc/message/detail/framework_access.hpp` 綁定固定共享區與操作訊息映像。
- RingBuffer／OSAL native/state 標頭供其所屬元件實作與測試使用，不作為其他元件的一般入口。
- 中央程式透過 `vinc/shell/detail/tcl_runtime.hpp` 與 `detail/shell.hpp`，依序完成
  Tcl 初始化、Manager／Shell 建立、命令保留與 catalog 登記、Socket 啟動及操作迴圈。
  這是中央框架的內部責任；一般 SDK 使用者不建立或管理這些服務。
  `detail/socket_shell.hpp` 僅供 Shell 實作與框架測試使用。生命週期見 Shell README。

## 驗證方式與限制

- `sdk_header_smoke`：每個整合入口列出的公開標頭各自作為第一個 include 編譯。
  `public_boundary.cpp` 另檢查便利入口不洩漏 Shell 控制類別及 Tcl 初始化宣告。
- `sdk.consumer`：只使用 vinc.hpp／SDK target，啟動實際 Module、呼叫命令並關閉。
- `sdk.external_consumer`：CTest 驅動 `tests/sdk_consumer` 的獨立配置、建置與執行，
  不手動加入 component include 路徑。驗證生成訊息、Module 啟停／命令，並實際啟動中央，
  確认 Tcl 腳本與 Socket Shell 自動啟動；包含自訂輸出目錄、搬移、缺少資源與重建補回。
- `tests/ribosome_generation.cmake`：隔離副本驗證生成、失敗清理與復原，避免讀到舊原始碼標頭。

2026-10-03 元件目錄整理驗證（Windows x64／MSVC）：

- Debug／Release 完整建置成功，最後一輪 CTest 各 **26／26** 通過。
- 公開標頭獨立編譯及 `sdk.consumer` 的實際 Module 啟停、命令呼叫通過。
- 獨立 CMake 消費端由乾淨目錄建置成功，主程式執行成功，Module entry target 建置成功。
- Ribosome 隔離副本的生成、失敗清理、復原、巢狀標頭與完整建置全部通過。
- 比對 82 個既有 `.cpp`，除 include 指令外沒有實作內容變更；正式 `.hpp` 已移出元件 src。
- 首輪 Debug 的跨程序鎖測試曾逾時，單獨重跑及最後兩組完整測試均通過。
  原因尚未確認，紀錄見 [Concurrency README](../components/concurrency/README.md)。

Linux／macOS 尚待原生驗證；
Tcl 整合目前仍要求單組態 generator，不能由生成路徑具組態區隔推論整體已支援多組態建置。

### 2026-10-04 測試可讀性收尾

`tests/consumer.cpp` 的命令等待 3 秒與關閉等待 5 秒已改為 `command_timeout`／
`shutdown_timeout` 測試常數，數值、檢查順序及退出碼維持原樣。
Windows x64／MSVC 19.51.36257 的 Debug／Release 完整建置成功，`sdk.consumer`
兩種組態均通過；同輪七個 OSAL 套件亦全數通過，共各 **8/8**。
此輪未重新建置獨立消費端專案，也未執行 Linux／macOS 驗證。

## Error domain 遷移

SDK 與 OSAL 會傳遞 `vinc::errors` 連結需求；直接使用 common 的獨立程式若需
`error_to_string()`，須同時連結 common 與查詢整合 target。common 不反向依賴查詢表。
正式入口包含新的 `<vinc/osal/error.hpp>`；OutputHub 主標頭會帶入自己的錯誤 enum。
錯誤表的共用建置契約 `<vinc/error_catalog.hpp>` 可單獨引用，不由便利 SDK 標頭帶入。
各元件的遷移範圍與獨立建置接線見 common README。

## SDK 重審修正（2026-10-06）

- Shell／SocketShell 控制標頭移到 detail；整合入口只保留設定與錯誤型別，補入
  `module/contract_report.hpp`。中央既有自動初始化與啟停順序維持，沒有新增服務開關。
- Tcl 部署預設使用 executable target 的實際目錄，保留明確目的地；建置期間的檔案鎖
  防止不同 target 同時複製到同一位置，不增加 runtime 執行緒或鎖。
- 恢復被 Message Storage 內容覆蓋的 Shell README，依現行錯誤、路徑及故障隔離契約核對。
- 獨立 SDK 消費端已納入 CTest，包含中央預設服務及 Tcl 部署回歸。

Windows x64／MSVC／Ninja 驗證：

- Debug／Release 完整建置與公開標頭獨立編譯成功，包含禁止暴露內部 Shell 啟動宣告的檢查。
- `sdk.external_consumer` 兩組均通過：Debug 75.05 秒、Release 88.41 秒。
  各自從獨立目錄配置與建置，實際測試生成訊息、Module 命令／關閉、中央 Tcl／Socket 自動啟動、
  父專案後設且含空白的輸出路徑、共用目的地、明確部署目錄、搬移及缺少資源後重建補回。
- 七個 `shell.*`、`sdk.consumer`、`module.shell`、`message_bus.shell`：
  Debug **10/10**（18.25 秒）、Release **10/10**（22.01 秒）通過；加上外部消費端，各 **11/11**。
- 本輪未執行其他元件的完整 CTest、Tcl 上游測試、Linux／macOS 或 PuTTY GUI 驗證。

## 待處理事項

- **已重現：Windows Tcl 建置根目錄含空白時，NMAKE 失敗。** 上游 `rules.vc` 的
  inference rules 使用無引號來源／輸出路徑；改用相對路徑後，仍在上游絕對路徑 mkdir
  步驟失敗。本輪保留既有建置方式，不修改上游；後續須另討論建置位置策略。
  執行檔輸出與部署目錄含空白是不同情境，已加入本輪回歸。
  證據及平台限制見 [Tcl 待處理事項](../../../library/tcl/README.md#待處理事項)。
- 原始碼套件以 add_subdirectory 整合；安裝後 find_package、單組態以外的建置及
  Linux／macOS 原生部署仍未完成驗證，不由本輪 Windows 結果推論支援。

## Plugin 連結集合（2026-10-06）

Module helper 設定 `VINC_MODULE_PLUGIN` target property。`vinc::sdk` 對此 consumer 提供相同
便利標頭，但排除中央 Manager、Shell、OutputHub、BusManager 與 Control Service 的實作。
一般 executable consumer 保留原本完整連結集合；Plugin 只能使用 Module 端 API，不能因
標頭可見就自行建立中央服務。public header 位置由所屬 target 提供，不引用其他元件 src。

正式 helper 使用 CMake MODULE library；Windows 產出 DLL，不提供獨立 main。
Plugin 與 Host 需相同工具鏈、標準庫、組態、布局及 CRT；不承諾預編譯 Plugin 跨版本 ABI。
`vinc::module_entry` 僅供 Host 組裝，不能再連入應用 Module。
既有 source-build 的 add_subdirectory 交付方式不變；部署時同時提供 ModuleHost 與 Plugin，
並指定其絕對路徑，詳見 Module System 的 Manager::create 與 ModuleType::library。


Plugin 遷移整合：Windows Debug／Release 完整建置與全套 CTest 各 55/55 通過，
包含以 Plugin 交付的 sdk.consumer／sdk.external_consumer 與公開標頭 smoke。
實際 SDK Plugin link closure 不含中央服務與 Control Session 實作；Linux／macOS 未驗證。


### 本機外部 SDK 增量建置疑慮（2026-10-06）

Module 除錯支援驗證中，外部 consumer 曾沿用舊 `start_module` 簽章的 main.cpp.obj，
造成 LNK2019。該 object 早於本輪標頭修改；fixture 的 CMake rules.ninja 中
`msvc_deps_prefix` 為亂碼，而主要建置目錄為「注意: 包含檔案:」。
**已確認：MSVC 本地化 showIncludes 的實際輸出與 CMake 生成的前綴編碼不符，
導致 Ninja 沒有記錄標頭相依。**
本輪清理該測試 fixture 的建置產物後，Debug／Release 外部 consumer 完整重建與測試均通過；
兩種組態全套各 56/56。這不等於修復增量相依追蹤。
同日以獨立最小專案重現：全新配置後只修改標頭，Ninja 回報 no work to do，
執行檔仍回傳舊值，`ninja -t deps` 顯示 0 個相依。直接配置、設定 VSLANG=1033，
以及模擬 fixture 的巢狀 cmake -P 配置均重現。本機 MSVC 只安裝 1028 語系資源，
VSLANG=1033 下實際輸出仍為繁體中文；不能假設設定該變數就能取得英文輸出。
隔離測試只修正生成 rules.ninja 的前綴，並重編 object 以重建相依記錄後，
Ninja 正確記錄標頭，後續只修改標頭也會重編。此為診斷對照，尚未修改正式建置設定。
既有外部 Debug consumer 在 clean 後重建的 main.cpp.obj 亦仍為 0 個標頭相依，
因此 clean 只消除當次舊產物，連全新配置也不能保證排除此問題。

當時規劃修正配置與編譯的輸出語系／編碼一致性，並增加只改公開標頭的 consumer 增量測試；
修復結果見下一節，未使用相容 overload 或手改生成檔。
原始失敗紀錄見 build/debug-support-debug-tests.log；最小重現與對照紀錄見
build/sdk-incremental-probe/results.json、correct-prefix-results.json（本機診斷產物，未納入版本控制）。

### MSVC／Ninja 增量建置修復（2026-10-06）

上述已確認問題已修正。Common 的 `cmake/VincMsvcDependencies.cmake` 在每次配置結束前
重新偵測 MSVC `/showIncludes` 前綴，以 UTF-8 解碼並用已知的 Unicode 標頭名稱驗證，
再於頂層目錄設定 Ninja 使用的前綴。這同時涵蓋主專案、Common／OSAL 獨立建置與
`add_subdirectory` 的外部 SDK 消費端；不依賴英文語系包、不修改生成的 rules.ninja。
偵測無法確認編碼時明確中止配置並提供 `CMakeFiles/VincShowIncludes/output.log`，
避免再次生成會默默漏編的設定；Windows MSVC／Ninja 請使用 UTF-8 開發者環境。
此接線只套用 Windows MSVC／Ninja，其他編譯器與 generator 不變。

**既有受影響目錄需先重新配置，再做一次 clean rebuild。** 前綴修正不會補回既有 object
已遺失的標頭相依；完成一次重建後即可正常增量建置。本機兩份外部 SDK fixture 已完成此步驟。

`sdk.external_consumer` 新增真正的增量回歸：先執行 SDK consumer 的測試程式確認值為 1，
只改建置目錄內的標頭，增量建置後必須得到 2。過程不修改儲存庫公開標頭；
只得到成功的建置退出碼或 no work to do 不算通過。

Windows x64／MSVC 19.51.36257、CMake 4.3.2、Ninja 1.13.2，繁體中文語系驗證：

- Debug／Release 外部 SDK 建置、增量回歸、Module 啟停與中央部署皆通過（各 1/1）。
  main.cpp.obj 由 0 個標頭相依恢復為 47 個，包含 module_manager.hpp。
- Debug／Release 主專案 SDK 公開標頭獨立編譯成功，sdk.consumer 各 1/1 通過。
- Common 的原因鏈、獨立容量配置及 catalog 正反例測試，Debug／Release 各 3/3 通過；
  加上上述兩項 SDK 測試，本輪相關 CTest 兩組各 5/5。
- 全新最小建置目錄的直接配置、VSLANG=1033、巢狀 cmake -P 三種情境均能在
  只改標頭後重編並取得新值；刻意留下錯誤前綴也會由配置收尾修正。
- 本輪未重跑完整框架 CTest，未驗證 Linux／macOS、其他 Windows 語系或工具鏈版本。

本機紀錄：build/msvc-deps-*-external.log、build/msvc-deps-*-sdk-build.log、
build/msvc-deps-*-common.log，
以及 build/sdk-incremental-probe/fixed/results.json。

## 中央名稱與 Host 私有鎖

中央建置可設定 `VINC_NAME`；同名占用會由 Manager::create 回報錯誤。
框架私有 `ContextLockAccess` 經 Host 取得 lease，未加入 SDK 總標頭或公開 Context 方法。
Concurrency Guard 的操作／析構留在 Host；Plugin 不因此擁有另一份框架鎖管理服務。
Param 接線後 Plugin entry version 5／bootstrap v10 需完整重新建置相連程式；
名稱、依賴及清理契約見 [Module System INSTANCE.md](../module_system/INSTANCE.md)。

## Param 接線（2026-10-08）

整合入口提供 `<vinc/param/params.hpp>`、`types.hpp` 與 `error.hpp`。Module 從繼承的
`context.param` 使用，不自行建立服務。Plugin 只連結 facade／codec；Host 擁有唯一
ModuleParams 控制接線，中央擁有 ParamManager 與 TOML。
資料位置、保存／權限／pump 契約見 [Param README](../param/README.md)。
既有 config／Socket Shell 設定未自動遷移至 Param。


## Log API（2026-10-08）

vinc/vinc.hpp 包含 Log；Module 透過繼承的 context.log 使用。SDK target 帶入 fmt，
不洩漏 spdlog headers／definitions；Plugin 不取得 Log Host／Manager 或 control service。
Context 布局更新為 Plugin entry version 5／bootstrap v10。見 [Log](../log/README.md)。

## 雙時間與 UTC（2026-10-13）

`<vinc/vinc.hpp>` 已包含 `<vinc/time.hpp>`，可使用 `vinc::unix_nanoseconds()` 與
`vinc::format_utc(uint64_t)`；API 契約見 [Common](../common/README.md#unsigned-unix-time-and-utc-formatting-2026-10-13)。
BaseMessage 的 Unix／單調發布時間變更需中央、Host、Plugin／SDK 使用端與 Python 擴充一起重建。
目前 Plugin entry 7／bootstrap v12；payload 映像与訊息 schema hash 不變。
