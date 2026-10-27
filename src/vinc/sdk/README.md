<a id="vinc-sdk-與元件目錄"></a>

# VINC SDK and component layout / VINC SDK 與元件目錄

Public convenience entry is `<vinc/vinc.hpp>`/vinc::sdk, aggregating existing APIs/dependencies without an extra runtime or implicit startup. Components retain API ownership; OSAL/Common can be used independently. The umbrella includes time.hpp/Deadline and contract.hpp fatal entries.

對外整合入口是 `<vinc/vinc.hpp>`，CMake target 為 `vinc::sdk`。
它整合現有框架 API 與連結相依；不建立額外 runtime，不因 include 就啟動任何服務。
正式 API 仍由各元件維護，OSAL 可與只依賴標準函式庫的 common 一併獨立使用。
整合入口包含 common 的 `<vinc/time.hpp>`，可直接使用 `vinc::Deadline`；亦包含 `<vinc/contract.hpp>` 的契約終止入口。

Strict UTF-8 path conversion is included through path.hpp; see [OSAL](../osal/README.md#utf-8-與原生路徑).

整合入口亦包含 `<vinc/osal/path.hpp>` 的嚴格 UTF-8 路徑轉換；
契約與限制見 [OSAL](../osal/README.md#utf-8-與原生路徑)。

<a id="使用方式"></a>

## Usage / 使用方式

Module authors can begin with runnable [C++/Python examples](../../../examples/userExampleModule/README.md) and their Shell/API index.

一般 Module 作者可先看 [userExampleModule C++／Python 對照](../../../examples/userExampleModule/README.md)，
包含可執行的 Shell 操作與 API 閱讀索引。

Below is the C++ SDK. Optional system-Python bindings live in Module System behind VINC_BUILD_PYTHON, without linking Python into central/general C++ consumers. See [Python](../module_system/python/README.md) for Context services, containers, and ABI.

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
vinc_deploy_licenses(my_app)
```

<a id="license-delivery"></a>

## License delivery / 授權交付

`vinc_deploy_licenses(target)` is available after the source-package `add_subdirectory`.
It copies the repository's `LICENSE`, `NOTICE`, and [THIRD_PARTY_NOTICES.txt](../../../THIRD_PARTY_NOTICES.txt)
to `licenses/open-vinc/` beside the target's actual output. Keep that directory with the binary when packaging or relocating it.
The parent application's own `LICENSE` is untouched. Calls are idempotent per target; rebuilding an up-to-date target restores missing notices without relinking.
This is build-time copying, with no runtime library or Node/Python requirement.

原始碼套件 `add_subdirectory` 後可使用 `vinc_deploy_licenses(target)`。
它將根目錄的 `LICENSE`、`NOTICE` 及 [THIRD_PARTY_NOTICES.txt](../../../THIRD_PARTY_NOTICES.txt)
複製到 target 實際輸出位置旁的 `licenses/open-vinc/`；打包或搬移二進位檔時須一起攜帶。
不覆蓋應用程式自己的 `LICENSE`。同一 target 重複呼叫不會重複登記；
重新建置已是最新的 target 也會補回遺失的聲明，不必重新連結。
這是建置期複製，不增加 runtime library，也不要求 Node 或 Python。

Central, ModuleHost, and Plugins created by `add_module` receive this deployment automatically.
Custom executables/libraries that directly link SDK/component targets call the helper explicitly, as above.
For a source-only package, retain the three root files and the upstream license/notice files; the helper does not create an installed CMake package.
The shared inventory includes optional components and incorporated third-party code, so it is a superset rather than a claim that every binary links every listed dependency.
Dependency-pin updates must refresh the inventory; see [library maintenance](../../../library/README.md#license-inventory).

中央、ModuleHost 及透過 `add_module` 建立的 Plugin 會自動部署。
自行直接連結 SDK／元件的 executable 或 library，需如上例明確呼叫 helper。
僅交付原始碼時，保留這三個根目錄檔案與上游授權／聲明；helper 不提供安裝式 CMake 套件。
共用清單包含可選元件及套件內納入的第三方程式碼，是完整盤點範圍，不表示每個 binary 都連入全部套件。
更新相依版本時須同步更新清單，見 [library 維護](../../../library/README.md#license-inventory)。

### License delivery validation (2026-10-26) / 授權交付驗證（2026-10-26）

Windows x64, MSVC 19.51.36257, CMake 4.3.2, Ninja 1.13.2, CPython 3.13.12, Node 24.19.0:
complete Debug/Release builds passed; the selected SDK/Python/UI CTest suites passed **13/13 in each configuration**.
Checks cover upstream license/attribution and pin matching, parallel shared-directory copying,
restoring deleted notices without relinking, preserving application-owned files, missing-input failure,
and real external SDK deployment/relocation. UI HTTP tests retrieve the license page and incorporated-code notices.
All **54** sampled deployed license files match their root inputs by SHA-256, including standalone Python/UI copies.
Frontend type checking passed. The small deployment fixture also passed with Ninja Multi-Config;
this is not a full-framework multi-config validation.

Windows x64、MSVC 19.51.36257、CMake 4.3.2、Ninja 1.13.2、CPython 3.13.12、Node 24.19.0：
Debug／Release 完整建置成功；選定的 SDK／Python／UI CTest **兩種組態各 13/13** 通過。
涵蓋上游條款／署名及版本比對、共用目錄並行複製、不重連結補回遺失聲明、保留應用自有檔案、
輸入缺漏失敗，以及實際外部 SDK 部署／搬移；UI HTTP 測試驗證授權頁面與內含作品署名。
包含獨立 Python／UI 副本的 **54** 份部署授權檔，SHA-256 均與根目錄來源一致。
前端型別檢查通過；小型部署 fixture 亦通過 Ninja Multi-Config，但不代表整個框架已驗證該 generator。

This round did not rerun the full framework test suite or Linux/macOS builds.
The old [isolated Ribosome fixture](../ribosome/README.md#待處理事項) now copies the legal inputs,
but its previously recorded stale assumptions remain unresolved and it was not run.
Local logs and artifact-hash results are under `build/license-delivery-20261026/`.

本輪未重跑完整框架測試，也未建置 Linux／macOS。
舊 [Ribosome 獨立 fixture](../ribosome/README.md#待處理事項) 僅補上授權來源檔複製，
先前記錄的過時假設仍未處理，未執行該腳本。本機紀錄與雜湊結果位於 `build/license-delivery-20261026/`。

<a id="公開-cmake-functions"></a>

## Public CMake functions / 公開 CMake functions

After add_subdirectory(path/to/open-vinc/src open-vinc), call from user CMakeLists:

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

- add_module(target MODULE_TYPE_NAME name SOURCES...) creates a MODULE library with SDK/messages/C++23/visibility/CRT contracts. It is an ordinary target supporting further target-scoped options. Type matches REGISTER_MODULE; duplicates are rejected.
- No MODULE_BASE_NAME. The Plugin uses a no-argument constructor; source supplies a default BaseModule name or on_name(args). Central validates names, rejects duplicates, and preserves restart naming.
- Internal STOP_TIMEOUT_CONFIG refers to an existing config constant, not a new runtime policy API.
- add_message_dir collects direct .msg children, not recursive. Resolve relative to caller source; reject missing directories and deduplicate canonical ones. Built-ins remain, all types share namespace vinc.
- Normal CMake registrations are collected at configure completion, including parent calls after add_subdirectory. No manual finalize or late registration in user DEFER callbacks.
- Configure registers only. Build compiles Ribosome, generates all headers/type lists, then consumers. No nested configure-time build; every build regenerates fail-closed. Added/deleted/case-colliding types are tracked/rejected and cross-directory references resolve together.
- Catalog collection waits for outer configure completion; central depends on all registered Plugins. APPS=OFF still builds messages/Plugins without central, not hot loading.

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

Functions are source-package APIs without manually including private CMake files. The old vinc_add_process_module alias is removed. Headers remain vinc_messages/Name.hpp; Recorder/enabled Python share the merged compile-time type list without runtime registry.

上述 functions 仍以原始碼套件提供；不要求使用者另外 include 私有 CMake 檔。
舊 `vinc_add_process_module()` 名稱不保留。生成標頭沿用 `<vinc_messages/Name.hpp>`；
Bag 與啟用時的 Python SDK 取得同一份合併型別清單，沒有新增 runtime schema registry。

<a id="本輪驗證2026-10-12"></a>

### Recorded CMake API validation (2026-10-12) / 本輪驗證（2026-10-12）

Windows x64/MSVC 19.51.36257/CMake 4.3.2/Ninja 1.13.2/system Python 3.13:

Windows x64、MSVC 19.51.36257、CMake 4.3.2、Ninja 1.13.2，啟用系統 Python 3.13：

- Full Debug/Release builds; Debug full **75/75**.
- Release initially **74/75**: a Socket test mistook asynchronous Log prompts for help completion. The test now identifies the response, with unchanged runtime. Five Release reruns/one Debug rerun passed; other 74 were not repeated after that test-only edit.
- Both external consumers cover late Module/message registration, cross-directory dependencies, edits/add/delete, invalid schema/recovery, duplicate types/case collisions, real central launch and updated constants.
- The isolated generator script has stale assumptions and was not run; see [Ribosome TODO](../ribosome/README.md#待處理事項). No Linux/macOS verification in that round.

- Debug／Release 完整建置成功；Debug 完整 CTest 75/75 通過。
- Release 完整 CTest 初次 74/75 通過；Socket Shell 測試將非同步 Log 通知的提示符
  誤認為 help 回覆結尾。測試改為先辨認命令回覆，不修改 Shell runtime。
  修正後 Release 連續 5 次、Debug 補跑 1 次均通過；其餘 74 項未因測試修正重跑。
- 兩種組態的 sdk.external_consumer 均通過：父專案後註冊 Module 與訊息、跨目錄相依、
  訊息修改／新增／刪除、無效 schema 失敗與復原、重複 ModuleType／大小寫訊息碰撞，
  以及中央實際啟動外部 Module 並讀到更新後的訊息常數。
- 生成器舊獨立來源副本腳本仍有過時假設，未列入本輪測試；見
  [Ribosome 待處理事項](../ribosome/README.md#待處理事項)。Linux／macOS 本輪未驗證。

Logs: build/cmake-api-*-tests.log. That change originally used entry 6/bootstrap v11; current ABI is entry 8/v13. All connected consumers rebuild together; see [Module source/CMake contracts](../module_system/README.md#source-擁有實例命名與公開-cmake-接線2026-10-12).

本機紀錄位於 build/cmake-api-*-tests.log；當時 entry 6／bootstrap v11 已被目前 entry 8／v13 取代；相容性與
重建要求見 [Module System](../module_system/README.md#source-擁有實例命名與公開-cmake-接線2026-10-12)。

SDK supplies neither main nor message generation by itself; include/link starts no services. Central-owned main with embedded startup is available through vinc_configure_app, documented in [application setup](../../../apps/vinc/README.md#打包應用程式的啟動腳本). Central initializes Tcl/Socket automatically, unlike child Modules. APPS=OFF above is SDK-only; retain ON for central. Shell exposes only errors/SocketShellOptions, not internal Shell/SocketShell/ScriptService/TclRuntime/init APIs. CLI selects listener settings; see [Shell](../shell/README.md). Build-time vinc_deploy_tcl follows the actual target directory or an explicit destination; see [Tcl](../../../library/tcl/README.md). User/built-in messages share vinc::messages and explicit generated includes; Ribosome development API is not SDK. Delivery remains source add_subdirectory, not installed find_package(VINC).

`vinc::sdk` 本身不提供 main、不生成使用者訊息；include 與連結不會啟動服務。
需要直接開啟即可執行固定部署腳本的應用，可使用 `vinc_configure_app(NAME ... STARTUP_SCRIPT ...)`，
由中央提供 main 並嵌入腳本；建置順序、名稱與部署契約見
[中央應用設定](../../../apps/vinc/README.md#打包應用程式的啟動腳本)。
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

The aggregate brings many headers/dependencies; select individual component headers/targets for smaller dependency sets.

整合入口會帶入較多標頭與連結相依。需要較小相依集合時，直接選用元件的標頭與 target。

<a id="元件與-target"></a>

## Components and targets / 元件與 target

| Component | Target | Responsibility |
| --- | --- | --- |
| common | vinc::common | Result/Error foundations/config |
| error catalog | vinc::errors | One static text-query implementation |
| osal | vinc::osal | General OS capabilities |
| concurrency | vinc::concurrency | Shared synchronization tools |
| control_channel | vinc::control | Sessions/protocol/heartbeat |
| output_hub | vinc::output_hub | Capture/fanout/rendering |
| message_storage | vinc::message_storage | BaseMessage/MessageArray/generated storage |
| message_bus | vinc::message_bus | Client, BusManager, RingBuffer |
| param | vinc::param | Context facade/types/codecs; internal targets in Param |
| log | vinc::log | Context logging/fmt, no central file owner |
| rpc | vinc::rpc | Typed request/response facade; internal targets in RPC |
| module_system | vinc::module | BaseModule/Plugin client dependencies |
| module_system | vinc::module_manager | Central Manager |
| module_system | vinc::module_entry | Host main/wmain object target |
| shell | vinc::shell | Internal services; public settings/errors only |
| vinc_messages | vinc::messages | Definitions/generation usage requirements |
| sdk | vinc::sdk | Aggregate, excluding generated messages |

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
| log | `vinc::log` | Context Log facade／fmt；中央保存私有相依不外洩 |
| rpc | `vinc::rpc` | Context typed request／response facade；Host／Manager 私有接線另列於 RPC |
| module_system | `vinc::module` | BaseModule、Plugin client 相依 |
| module_system | `vinc::module_manager` | 中央 ModuleManager |
| module_system | `vinc::module_entry` | 提供 Host main／wmain 的 object target |
| shell | `vinc::shell` | 框架內部的 Shell、Socket Shell、Tcl 執行；設定及錯誤型別公開 |
| vinc_messages | `vinc::messages` | .msg 定義與生成標頭使用需求 |
| sdk | `vinc::sdk` | 以上現有框架 API 的便利入口，不含生成訊息 |

<a id="目錄與相依原則"></a>

## Layout and dependency principles / 目錄與相依原則

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

- Headers stay in component include, implementations in src, test helpers outside SDK.
- Visible detail is not a public promise; only designated contracts are integration dependencies.
- Targets propagate usage requirements; never include another component src directly.
- Template-required detail may be header-visible without altering runtime/template semantics for aesthetics.
- apps/vinc composes main/catalog; process/Bus examples stay examples.
- Root [messages](../../../vinc_messages/README.md) owns .msg; each build writes its own generated headers, never source.
- Manager/hooks use Common Result, no old integer table. Manager creation is explicit; Shell borrows it, with no implicit singleton.
- Context/pump is implemented through BaseModule and context.hpp; lifecycle belongs to Module documentation.
- RuntimeServices is internal, not in vinc.hpp; common_error only adds public classification. Host/Plugin lifetime belongs to their contracts; add_module builds a Plugin, not an executable.

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

<a id="元件接線契約"></a>

## Designated integration seams / 元件接線契約

- Runtime creates/receives/closes Bus clients through detail/runtime.hpp.
- Manager submits requests/confirms exit/waits for reclamation through detail/bus_manager.hpp.
- Bus binds fixed storage through message/detail/framework_access.hpp.
- RingBuffer and native/state headers belong to owning implementations/tests, not general cross-component APIs.
- Central uses Shell detail initialization/creation, command reservation/catalog, Socket startup, and run loop. General SDK users do not own those services. SocketShell detail is for Shell/tests only.

- Module Runtime 透過 `vinc/message_bus/detail/runtime.hpp` 建立、接收與關閉 Message Bus Client。
- ModuleManager 透過 `vinc/message_bus/detail/bus_manager.hpp` 提交控制要求、通知已確認退出與等待回收。
- Message Bus 透過 `vinc/message/detail/framework_access.hpp` 綁定固定共享區與操作訊息映像。
- RingBuffer／OSAL native/state 標頭供其所屬元件實作與測試使用，不作為其他元件的一般入口。
- 中央程式透過 `vinc/shell/detail/tcl_runtime.hpp` 與 `detail/shell.hpp`，依序完成
  Tcl 初始化、Manager／Shell 建立、命令保留與 catalog 登記、Socket 啟動及操作迴圈。
  這是中央框架的內部責任；一般 SDK 使用者不建立或管理這些服務。
  `detail/socket_shell.hpp` 僅供 Shell 實作與框架測試使用。生命週期見 Shell README。

<a id="驗證方式與限制"></a>

## Validation and limits / 驗證方式與限制

- sdk_header_smoke compiles each public header first; public_boundary rejects leaked Shell/init declarations.
- sdk.consumer uses only vinc.hpp/SDK to launch/call/stop a real Module.
- sdk.external_consumer independently configures/builds/runs without manual component includes; checks messages/Modules, central auto Tcl/Socket, output paths/relocation/missing-resource rebuilds.
- The isolated Ribosome generation script has historical coverage but currently needs the fixes listed in [Ribosome TODO](../ribosome/README.md#待處理事項); it is not assumed valid just because older runs passed.

- `sdk_header_smoke`：每個整合入口列出的公開標頭各自作為第一個 include 編譯。
  `public_boundary.cpp` 另檢查便利入口不洩漏 Shell 控制類別及 Tcl 初始化宣告。
- `sdk.consumer`：只使用 vinc.hpp／SDK target，啟動實際 Module、呼叫命令並關閉。
- `sdk.external_consumer`：CTest 驅動 `tests/sdk_consumer` 的獨立配置、建置與執行，
  不手動加入 component include 路徑。驗證生成訊息、Module 啟停／命令，並實際啟動中央，
  確认 Tcl 腳本與 Socket Shell 自動啟動；包含自訂輸出目錄、搬移、缺少資源與重建補回。
- `tests/ribosome_generation.cmake`：曾用隔離副本驗證生成與復原，目前仍有 [Ribosome 待辦](../ribosome/README.md#待處理事項) 列出的過時假設，不能以歷史成功代表現在可直接執行。

Historical layout validation 2026-10-03, Windows x64/MSVC:

2026-10-03 元件目錄整理驗證（Windows x64／MSVC）：

- Full Debug/Release builds/final **26/26** each.
- Independent public-header and real SDK Module lifecycle/commands passed.
- Clean external CMake consumer/Module-entry target built and ran.
- Isolated generation cleanup/recovery/nested/full build passed then.
- 82 existing .cpp files had no implementation differences beyond includes; formal headers left src.
- Initial Debug mutex test timed out; isolated/final full runs passed with cause unconfirmed, retained in [Concurrency](../components/concurrency/README.md).

- Debug／Release 完整建置成功，最後一輪 CTest 各 **26／26** 通過。
- 公開標頭獨立編譯及 `sdk.consumer` 的實際 Module 啟停、命令呼叫通過。
- 獨立 CMake 消費端由乾淨目錄建置成功，主程式執行成功，Module entry target 建置成功。
- Ribosome 隔離副本的生成、失敗清理、復原、巢狀標頭與完整建置全部通過。
- 比對 82 個既有 `.cpp`，除 include 指令外沒有實作內容變更；正式 `.hpp` 已移出元件 src。
- 首輪 Debug 的跨程序鎖測試曾逾時，單獨重跑及最後兩組完整測試均通過。
  原因尚未確認，紀錄見 [Concurrency README](../components/concurrency/README.md)。

Current native platform results and their SDK coverage are recorded in the [platform reports](../../../README.md#validation-and-limits). Tcl requires single-config generators; configuration-separated paths do not establish overall multi-config support.

目前原生平台結果及 SDK 覆蓋範圍見[平台報告](../../../README.md#validation-and-limits)；
Tcl 整合目前仍要求單組態 generator，不能由生成路徑具組態區隔推論整體已支援多組態建置。

<a id="2026-10-04-測試可讀性收尾"></a>

### Test readability closeout (2026-10-04) / 2026-10-04 測試可讀性收尾

Consumer 3 s command/5 s shutdown values became named test constants, retaining order/exit codes. Windows MSVC 19.51.36257 full Debug/Release builds, SDK consumer and seven OSAL suites passed **8/8** each. No external-consumer rebuild/Linux/macOS run that round.

`tests/consumer.cpp` 的命令等待 3 秒與關閉等待 5 秒已改為 `command_timeout`／
`shutdown_timeout` 測試常數，數值、檢查順序及退出碼維持原樣。
Windows x64／MSVC 19.51.36257 的 Debug／Release 完整建置成功，`sdk.consumer`
兩種組態均通過；同輪七個 OSAL 套件亦全數通過，共各 **8/8**。
此輪未重新建置獨立消費端專案，也未執行 Linux／macOS 驗證。

<a id="error-domain-遷移"></a>

## Error-domain migration / Error domain 遷移

SDK/OSAL propagate vinc::errors; standalone Common text users link Common and errors explicitly, without reverse dependency. Umbrella includes OSAL errors; OutputHub includes its own enums. error_catalog.hpp can be included separately but is not part of the umbrella. Scope/standalone composition belongs to Common.

SDK 與 OSAL 會傳遞 `vinc::errors` 連結需求；直接使用 common 的獨立程式若需
`error_to_string()`，須同時連結 common 與查詢整合 target。common 不反向依賴查詢表。
正式入口包含新的 `<vinc/osal/error.hpp>`；OutputHub 主標頭會帶入自己的錯誤 enum。
錯誤表的共用建置契約 `<vinc/error_catalog.hpp>` 可單獨引用，不由便利 SDK 標頭帶入。
各元件的遷移範圍與獨立建置接線見 common README。

<a id="sdk-重審修正2026-10-06"></a>

## SDK review fixes (2026-10-06) / SDK 重審修正（2026-10-06）

- Shell control headers moved to detail; only settings/errors plus module contract-report types remain public. Automatic startup/order unchanged, no service-enable switches.
- Tcl deploy follows actual executable output or explicit destination; a build-only lock serializes shared destinations, without runtime locks/threads.
- Restored a Shell README previously overwritten by Storage material and reconciled error/path/isolation contracts.
- External SDK tests now cover default central services/deployment.

- Shell／SocketShell 控制標頭移到 detail；整合入口只保留設定與錯誤型別，補入
  `module/contract_report.hpp`。中央既有自動初始化與啟停順序維持，沒有新增服務開關。
- Tcl 部署預設使用 executable target 的實際目錄，保留明確目的地；建置期間的檔案鎖
  防止不同 target 同時複製到同一位置，不增加 runtime 執行緒或鎖。
- 恢復被 Message Storage 內容覆蓋的 Shell README，依現行錯誤、路徑及故障隔離契約核對。
- 獨立 SDK 消費端已納入 CTest，包含中央預設服務及 Tcl 部署回歸。

Windows x64/MSVC/Ninja evidence:

Windows x64／MSVC／Ninja 驗證：

- Full Debug/Release builds/public headers/public-boundary checks passed.
- External consumers passed (75.05 s/88.41 s), including source messages/commands/shutdown/automatic services, late spaced output paths/shared destinations/explicit deployment/relocation/missing-resource recovery.
- Seven Shell suites plus SDK/Module Shell/Bus Shell passed **10/10** (18.25 s/22.01 s), **11/11** with external consumer.
- No other full suites, upstream Tcl, Linux/macOS, or PuTTY GUI testing in that round.

- Debug／Release 完整建置與公開標頭獨立編譯成功，包含禁止暴露內部 Shell 啟動宣告的檢查。
- `sdk.external_consumer` 兩組均通過：Debug 75.05 秒、Release 88.41 秒。
  各自從獨立目錄配置與建置，實際測試生成訊息、Module 命令／關閉、中央 Tcl／Socket 自動啟動、
  父專案後設且含空白的輸出路徑、共用目的地、明確部署目錄、搬移及缺少資源後重建補回。
- 七個 `shell.*`、`sdk.consumer`、`module.shell`、`message_bus.shell`：
  Debug **10/10**（18.25 秒）、Release **10/10**（22.01 秒）通過；加上外部消費端，各 **11/11**。
- 本輪未執行其他元件的完整 CTest、Tcl 上游測試、Linux／macOS 或 PuTTY GUI 驗證。

<a id="待處理事項"></a>

## Outstanding issues / 待處理事項

- Reproduced Windows Tcl build-root spaces break unquoted upstream rules/mkdir even with relative-path attempts. No upstream patches; a build-location strategy needs separate design. Spaced deployment/output paths are distinct and tested. See [Tcl issues](../../../library/tcl/README.md#待處理事項).
- Source add_subdirectory remains delivery. Installed find_package, non-single-config builds, and native Linux/macOS deployment remain incomplete/unverified; Windows cannot establish them.

- **已重現：Windows Tcl 建置根目錄含空白時，NMAKE 失敗。** 上游 `rules.vc` 的
  inference rules 使用無引號來源／輸出路徑；改用相對路徑後，仍在上游絕對路徑 mkdir
  步驟失敗。本輪保留既有建置方式，不修改上游；後續須另討論建置位置策略。
  執行檔輸出與部署目錄含空白是不同情境，已加入本輪回歸。
  證據及平台限制見 [Tcl 待處理事項](../../../library/tcl/README.md#待處理事項)。
- 原始碼套件以 add_subdirectory 整合；安裝後 find_package、單組態以外的建置及
  Linux／macOS 原生部署仍未完成驗證，不由本輪 Windows 結果推論支援。

<a id="plugin-連結集合2026-10-06"></a>

## Plugin link closure (2026-10-06) / Plugin 連結集合（2026-10-06）

The helper sets VINC_MODULE_PLUGIN. SDK retains convenience headers but excludes central Manager/Shell/OutputHub/BusManager/Control Service implementations for Plugins. Ordinary executables retain the full closure. Visibility does not permit Plugin-owned central services. Owning targets supply headers without cross-component src includes.

Module helper 設定 `VINC_MODULE_PLUGIN` target property。`vinc::sdk` 對此 consumer 提供相同
便利標頭，但排除中央 Manager、Shell、OutputHub、BusManager 與 Control Service 的實作。
一般 executable consumer 保留原本完整連結集合；Plugin 只能使用 Module 端 API，不能因
標頭可見就自行建立中央服務。public header 位置由所屬 target 提供，不引用其他元件 src。

MODULE libraries produce Windows DLLs without standalone main. Match Host toolchain/stdlib/config/layout/CRT, without prebuilt cross-version ABI promises. module_entry is Host-only. Deploy Host/Plugin with specified absolute paths under current source integration; see Manager::create/ModuleType::library.

正式 helper 使用 CMake MODULE library；Windows 產出 DLL，不提供獨立 main。
Plugin 與 Host 需相同工具鏈、標準庫、組態、布局及 CRT；不承諾預編譯 Plugin 跨版本 ABI。
`vinc::module_entry` 僅供 Host 組裝，不能再連入應用 Module。
既有 source-build 的 add_subdirectory 交付方式不變；部署時同時提供 ModuleHost 與 Plugin，
並指定其絕對路徑，詳見 Module System 的 Manager::create 與 ModuleType::library。

Historical Plugin migration: Windows Debug/Release full builds/suites **55/55**, including Plugin-delivered SDK/external consumers/headers. Link closures excluded central/Session implementations. No Linux/macOS verification.

Plugin 遷移整合：Windows Debug／Release 完整建置與全套 CTest 各 55/55 通過，
包含以 Plugin 交付的 sdk.consumer／sdk.external_consumer 與公開標頭 smoke。
實際 SDK Plugin link closure 不含中央服務與 Control Session 實作；Linux／macOS 未驗證。

<a id="本機外部-sdk-增量建置疑慮2026-10-06"></a>

### External SDK incremental-build investigation (2026-10-06) / 本機外部 SDK 增量建置疑慮（2026-10-06）

A consumer retained an old start_module main.cpp.obj, causing LNK2019. Its rules.ninja showIncludes prefix was garbled while main build used localized Chinese. Encoding mismatch prevented Ninja header dependencies. Clean removed that stale object and both full suites passed 56/56, but was not a tracking fix. Minimal fresh/direct/`VSLANG=1033`/nested-cmake reproductions still reported no work and 0 dependencies after header edits; only 1028 language resources existed, so VSLANG did not produce English. A diagnostic-only corrected generated prefix plus one rebuild restored dependencies/incremental behavior. Even freshly cleaned fixtures still had 0 deps before correction: clean alone was insufficient.

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

The planned encoding-consistency fix/header-only regression was completed below without compatibility overloads or hand-edited generated files. Original logs: build/debug-support-debug-tests.log; local probe results/contrasts under build/sdk-incremental-probe.

當時規劃修正配置與編譯的輸出語系／編碼一致性，並增加只改公開標頭的 consumer 增量測試；
修復結果見下一節，未使用相容 overload 或手改生成檔。
原始失敗紀錄見 build/debug-support-debug-tests.log；最小重現與對照紀錄見
build/sdk-incremental-probe/results.json、correct-prefix-results.json（本機診斷產物，未納入版本控制）。

<a id="msvcninja-增量建置修復2026-10-06"></a>

### MSVC/Ninja incremental-build fix (2026-10-06) / MSVC／Ninja 增量建置修復（2026-10-06）

Common VincMsvcDependencies.cmake re-probes localized showIncludes as UTF-8 at configure completion, validates a Unicode header, then sets the top-level Ninja prefix. Covers main/Common/OSAL/external source consumers without English language packs or editing rules.ninja. Unverified encoding fails configure with CMakeFiles/VincShowIncludes/output.log, rather than silently omitting dependencies. Use UTF-8 Windows MSVC/Ninja environments; other compilers/generators unchanged.

上述已確認問題已修正。Common 的 `cmake/VincMsvcDependencies.cmake` 在每次配置結束前
重新偵測 MSVC `/showIncludes` 前綴，以 UTF-8 解碼並用已知的 Unicode 標頭名稱驗證，
再於頂層目錄設定 Ninja 使用的前綴。這同時涵蓋主專案、Common／OSAL 獨立建置與
`add_subdirectory` 的外部 SDK 消費端；不依賴英文語系包、不修改生成的 rules.ninja。
偵測無法確認編碼時明確中止配置並提供 `CMakeFiles/VincShowIncludes/output.log`，
避免再次生成會默默漏編的設定；Windows MSVC／Ninja 請使用 UTF-8 開發者環境。
此接線只套用 Windows MSVC／Ninja，其他編譯器與 generator 不變。

**Affected existing builds need reconfigure then one clean rebuild.** Prefix correction cannot reconstruct missing object dependencies. After that, incremental builds work; both local external fixtures were rebuilt.

**既有受影響目錄需先重新配置，再做一次 clean rebuild。** 前綴修正不會補回既有 object
已遺失的標頭相依；完成一次重建後即可正常增量建置。本機兩份外部 SDK fixture 已完成此步驟。

The real external-consumer regression runs a value 1 program, edits only a build-local header, rebuilds incrementally, and requires value 2. Repository public headers stay untouched; successful exit/no-work alone does not pass.

`sdk.external_consumer` 新增真正的增量回歸：先執行 SDK consumer 的測試程式確認值為 1，
只改建置目錄內的標頭，增量建置後必須得到 2。過程不修改儲存庫公開標頭；
只得到成功的建置退出碼或 no work to do 不算通過。

Recorded Windows x64/MSVC 19.51.36257/CMake 4.3.2/Ninja 1.13.2, Traditional Chinese locale:

Windows x64／MSVC 19.51.36257、CMake 4.3.2、Ninja 1.13.2，繁體中文語系驗證：

- Both external SDK/incremental/Module/central deployment suites passed **1/1**; main.cpp.obj dependencies increased 0→47 including Manager header.
- Both main SDK header builds/consumer **1/1** passed.
- Common causes/capacity/catalog positive-negative suites **3/3**; with SDK, related **5/5** each.
- Fresh direct/`VSLANG=1033`/nested configurations rebuild header-only edits correctly; configure corrects an intentionally wrong prefix.
- No full-framework rerun, Linux/macOS, other Windows languages/toolchains in that round.

- Debug／Release 外部 SDK 建置、增量回歸、Module 啟停與中央部署皆通過（各 1/1）。
  main.cpp.obj 由 0 個標頭相依恢復為 47 個，包含 module_manager.hpp。
- Debug／Release 主專案 SDK 公開標頭獨立編譯成功，sdk.consumer 各 1/1 通過。
- Common 的原因鏈、獨立容量配置及 catalog 正反例測試，Debug／Release 各 3/3 通過；
  加上上述兩項 SDK 測試，本輪相關 CTest 兩組各 5/5。
- 全新最小建置目錄的直接配置、VSLANG=1033、巢狀 cmake -P 三種情境均能在
  只改標頭後重編並取得新值；刻意留下錯誤前綴也會由配置收尾修正。
- 本輪未重跑完整框架 CTest，未驗證 Linux／macOS、其他 Windows 語系或工具鏈版本。

Local logs: build/msvc-deps-*-external.log, *-sdk-build.log, *-common.log, and build/sdk-incremental-probe/fixed/results.json.

本機紀錄：build/msvc-deps-*-external.log、build/msvc-deps-*-sdk-build.log、
build/msvc-deps-*-common.log，
以及 build/sdk-incremental-probe/fixed/results.json。

<a id="中央名稱與-host-私有鎖"></a>

## Central identity and private Host locks / 中央名稱與 Host 私有鎖

VINC_NAME selects central identity; conflicting names fail Manager::create. Private ContextLockAccess gets Host leases and is absent from umbrella/public methods. Host owns Guards/lifetime, not a second Plugin lock service. Current entry 8/v13 requires matched rebuilds; old Param/Log-stage entry 5/v10 is historical. See [Instance](../module_system/INSTANCE.md).

中央建置可設定 `VINC_NAME`；同名占用會由 Manager::create 回報錯誤。
框架私有 `ContextLockAccess` 經 Host 取得 lease，未加入 SDK 總標頭或公開 Context 方法。
Concurrency Guard 的操作／析構留在 Host；Plugin 不因此擁有另一份框架鎖管理服務。
目前 Plugin entry 8／bootstrap v13 需完整重新建置；先前 Param／Log 階段的 entry 5／v10 為歷史值。
名稱、依賴及清理契約見 [Module System INSTANCE.md](../module_system/INSTANCE.md)。

<a id="param-接線2026-10-08"></a>

## Param integration (2026-10-08) / Param 接線（2026-10-08）

Umbrella includes Param facade/types/errors, used through inherited context.param. Plugins link only facade/codecs; Host owns one ModuleParams bridge, central ParamManager/TOML. [Param](../param/README.md) owns persistence/permissions/pump contracts. Existing config/listener settings are not automatically migrated.

整合入口提供 `<vinc/param/params.hpp>`、`types.hpp` 與 `error.hpp`。Module 從繼承的
`context.param` 使用，不自行建立服務。Plugin 只連結 facade／codec；Host 擁有唯一
ModuleParams 控制接線，中央擁有 ParamManager 與 TOML。
資料位置、保存／權限／pump 契約見 [Param README](../param/README.md)。
既有 config／Socket Shell 設定未自動遷移至 Param。

<a id="log-api2026-10-08"></a>

## Log API（2026-10-08） / Log 介面（2026-10-08）

Umbrella includes Log/context.log and fmt, not spdlog headers/definitions. Plugins gain no Log Host/Manager/Control ownership. The original Log stage used entry 5/v10; current matched ABI is entry 8/v13. See [Log](../log/README.md).

vinc/vinc.hpp 包含 Log；Module 透過繼承的 context.log 使用。SDK target 帶入 fmt，
不洩漏 spdlog headers／definitions；Plugin 不取得 Log Host／Manager 或 control service。
Log 初次接線時為 entry 5／v10，目前為 entry 8／bootstrap v13，須匹配重建。見 [Log](../log/README.md)。

<a id="雙時間與-utc2026-10-13"></a>

## Dual time and UTC (2026-10-13) / 雙時間與 UTC（2026-10-13）

Umbrella time.hpp exposes unix_nanoseconds/format_utc; see [Common](../common/README.md#unsigned-unix-time-and-utc-formatting-2026-10-13). BaseMessage dual-time changes require rebuilding central/Host/Plugin/SDK/Python together. Current entry 8/bootstrap 13; payload/hash unchanged.

`<vinc/vinc.hpp>` 已包含 `<vinc/time.hpp>`，可使用 `vinc::unix_nanoseconds()` 與
`vinc::format_utc(uint64_t)`；API 契約見 [Common](../common/README.md#unsigned-unix-time-and-utc-formatting-2026-10-13)。
BaseMessage 的 Unix／單調發布時間變更需中央、Host、Plugin／SDK 使用端與 Python 擴充一起重建。
目前 Plugin entry 8／bootstrap v13；payload 映像与訊息 schema hash 不變。

<a id="rpc-sdk2026-10-14"></a>

## RPC SDK（2026-10-14） / RPC 介面（2026-10-14）

SDK/umbrella provide RPC types/errors/Context templates. Users register/call via inherited Context, not self-created central services. rpc is Plugin facade/image transport; rpc_host/rpc_manager are internal. Entry 8/v13 requires rebuilding all connected C++. See [RPC](../rpc/README.md) and [example](../../../examples/rpc/README.md).

vinc::sdk 與 vinc/vinc.hpp 提供 RPC 的型別、錯誤及 Context 模板 API。
一般使用端以繼承的 Context register_rpc／call_rpc 使用，不能自行建立中央服務。
vinc::rpc 是 Plugin facade／映像讀寫 target；vinc::rpc_host 與 vinc::rpc_manager 僅供框架接線。
ABI 更新為 Plugin entry 8／bootstrap v13，需重建所有相連 C++ 元件。
完整契約見 [RPC](../rpc/README.md)，範例見 [examples/rpc](../../../examples/rpc/README.md)。
