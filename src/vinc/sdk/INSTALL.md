# Installed C++ SDK / 安裝式 C++ SDK

App dispatches both roles and launches itself; standalone Host export/copy wiring
is removed. Clean SDK-only and relocated C++ App validation passed on Windows/WSL;
the final Windows/WSL integration regressions also passed. Native macOS remains
tracked in the
[single executable application plan](../../../apps/vinc/SINGLE_EXECUTABLE.md).
App 已自行分流／啟動自己，獨立 Host 匯出與複製接線已移除；乾淨 SDK-only 與搬移 C++ App
及最終整合回歸已通過 Windows／WSL 驗證，剩餘 macOS 原生範圍見上述計畫。

This package reuses prebuilt framework libraries through
`find_package(vinc CONFIG REQUIRED)`. Source `add_subdirectory()` remains supported.
The package is opt-in (`VINC_INSTALL_SDK=ON`), with no download, dependency build,
or service startup during lookup. Lookup compiles/runs a small native ABI probe.

此套件讓外部 C++ 專案透過 `find_package(vinc CONFIG REQUIRED)` 使用預編譯核心。
原有 `add_subdirectory()` 保留；以 `VINC_INSTALL_SDK=ON` 明確啟用安裝規則。
查找套件不下載或重建相依、不啟動框架服務；會編譯並執行小型本機 ABI 探針。

## Scope / 本輪範圍

- Component headers and static libraries, `vinc::sdk`, the compiled error catalog,
  and their necessary link dependencies. Headers under `detail/` retain their
  existing internal contracts; installation does not make them public API.
- Pinned fmt/spdlog/SQLite/Tcl archives. fmt headers accompany the public Log API;
  private central dependencies do not become application service APIs.
- Installed tool `vinc::ribosome`; the message template,
  built-in `.msg` definitions and Tcl runtime scripts.
- `vinc_deploy_licenses(target)` and complete Open-VINC/third-party notices.
- Installed `add_module()`, `add_message_dir()` and `vinc_add_app()` compose a
  C++ application with consumer-owned messages/catalog/identity. Official Bag/UI
  adapters and optional Python bindings compile against that message list.

- 安裝元件標頭、靜態庫、`vinc::sdk`、已編譯的錯誤查詢表與必要連結相依。
  `detail/` 仍是既有內部接線，不因安裝而變成公開 API。
- 附帶固定版本 fmt／spdlog／SQLite／Tcl 靜態庫；Log 公開介面所需的 fmt 標頭一併提供。
  中央私有相依不因此變成使用者需要自行操作的服務。
- 提供 `vinc::ribosome` 工具 target、訊息模板、內建 `.msg` 與 Tcl scripts。
- 提供 `vinc_deploy_licenses(target)`，完整交付 Open-VINC 與第三方授權／署名。
- 安裝式 `add_module()`、`add_message_dir()` 與 `vinc_add_app()` 組裝 C++ 應用自己的訊息、
  catalog 與中央身分。官方 Bag／UI 接線及選用的 Python 綁定會依該訊息清單重新編譯。

## Build and install / 建置與安裝

Use a prepared native toolchain and initialized pinned submodules. Windows requires
the matching MSVC developer environment. Ninja single-config Debug/Release are the
initial package profiles; use a different prefix for each configuration/toolchain.

先準備工具鏈及固定版本 submodule；Windows 使用匹配的 MSVC developer 環境。
第一版採 Ninja 單組態 Debug／Release，各工具鏈與組態使用不同安裝目錄。

```sh
npm ci --prefix src/vinc/ui/web
cmake -S src -B build/sdk-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DVINC_INSTALL_SDK=ON -DVINC_BUILD_APPS=OFF -DVINC_BUILD_EXAMPLES=OFF -DVINC_BUILD_UI=ON -DVINC_BUILD_PYTHON=OFF -DBUILD_TESTING=OFF
cmake --build build/sdk-release
cmake --install build/sdk-release --component VincSDK --prefix /absolute/path/to/vinc-sdk-release
```

A utility-only package may omit UI, but the new App constructor requires UI/Bag
capabilities and rejects `NO_UI`/`NO_BAG`. The commands above assume Node/npm and
the C++ toolchain are prepared.

獨立工具用套件可不含 UI，但新 App 建構器要求 UI／Bag 能力，不接受 `NO_UI`／`NO_BAG`。
以上指令假設 Node／npm 與 C++ 工具鏈已準備完成。

The component name `VincSDK` selects only these install rules. It is not a
`find_package(... COMPONENTS ...)` selector: package components are not exposed yet.
No installer, package manager, archive publication or system PATH change is provided.

`VincSDK` 是安裝規則的 component 名稱，不是 `find_package(... COMPONENTS ...)` 的選項；
尚未提供套件子集選擇。這輪不建立安裝器、套件管理器、發布壓縮檔，也不修改系統 PATH。

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_tool LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(vinc CONFIG REQUIRED)
add_executable(my_tool main.cpp)
target_link_libraries(my_tool PRIVATE vinc::sdk)
vinc_deploy_licenses(my_tool)
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/absolute/path/to/vinc-sdk-release
cmake --build build
```

## C++ application composition / C++ 應用組裝

`vinc_add_app()` supplies the production App entry. Users do not provide or replace
main, construct the central Manager, or configure Host launch wiring themselves.
See the [sole-entry contract](../../../apps/vinc/SINGLE_EXECUTABLE.md).
The plain `my_tool` linking example above is for independent utilities, not an
alternative central/Module bootstrap. App self-launch is implemented; SDK packages
and deployment recipes no longer provide a separate Host artifact.

`vinc_add_app()` 提供正式 App 入口；使用者不自行提供或替換 main、不自行建立中央 Manager，
也不管理 Host 啟動接線，見上述唯一入口契約。上方 `my_tool` 單純連結範例屬獨立工具用途，
不是中央／Module 的替代啟動方式。App 已啟動自己，套件／部署規則不再提供獨立 Host 產物。

The constructor supports multiple Apps in source and installed builds. Each App
names its own Kconfig file; Python defaults off, custom Modules default unselected,
and official UI/Bag are mandatory. Build and install stage each App's selected
Plugins, Tcl, UI assets, licenses and optional Python package. Catalog paths are
relative to the App executable, supporting relocation.

來源與安裝式建置共用多 App 介面；各 App 有自己的設定檔，Python 預設關閉、自訂 Module
預設未選，UI／Bag 固定包含。建置／安裝會部署所選 Plugin、Tcl、UI、授權及選用 Python
套件；catalog 相對於 App 執行檔解析，支援整包搬移。

```cmake
cmake_minimum_required(VERSION 3.20)
project(camera_application LANGUAGES CXX)
find_package(vinc CONFIG REQUIRED)
# Source alternative: disable the legacy shell/examples, then
# add_subdirectory(path/to/open-vinc/src open-vinc-build).

add_message_dir(messages)
add_module(camera_module MODULE_TYPE_NAME Camera SOURCES modules/camera.cpp)

vinc_add_app(CameraApp
    INSTANCE_NAME camera-station
    VINC_BUILD_CONFIG apps/camera/app.config
    STARTUP_SCRIPT apps/camera/startup.tcl
)
vinc_add_app(MonitorApp
    INSTANCE_NAME monitor-station
    VINC_BUILD_CONFIG apps/monitor/app.config
    STARTUP_SCRIPT apps/monitor/startup.tcl
)
```

For example, `apps/camera/app.config` may contain:

例如相機 App 的設定檔可寫：

```ini
CONFIG_VINC_MODULE_Camera=y
# CONFIG_VINC_APP_PYTHON is not set
```

`apps/monitor/app.config` may contain only the Python-off comment when it needs
no custom Modules. Both Apps still include official UI/Bag. Startup scripts operate
on the ready `ui`/`recorder` instances; do not repeat `webUi` or `bagRecorder` just
to create their default instances. For example:

Monitor 不需自訂 Module 時，其設定檔可以只放 Python 關閉註解；UI／Bag 仍包含。
腳本直接使用已就緒的 `ui`／`recorder`，不要再次建立同名實例。例如：

```tcl
ui status
recorder status
# Camera                       ;# Start a selected custom Module when needed.
# recorder record session01 --all
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -Dvinc_DIR=/absolute/path/to/vinc-sdk-release/lib/cmake/vinc
cmake --build build --target CameraApp MonitorApp --parallel 2
cmake --install build --component VincApp_CameraApp --prefix package
cmake --install build --component VincApp_MonitorApp --prefix package
```

This is one parallel build graph; it does not promise safe concurrent Ninja/CMake
invocations writing the same binary directory. Each installed App owns one directory:

以上為同一次建置中的平行 target，不承諾多個 Ninja／CMake 程序同時寫入同一 build 目錄。
安裝後各 App 各有一個目錄：

```text
package/
├── CameraApp/
│   ├── CameraApp[.exe]
│   ├── modules/               # Selected custom Plugins and official UI/Bag
│   ├── lib/tcl9.1/
│   ├── licenses/
│   └── vinc_data/camera-station/ui/
└── MonitorApp/
    ├── MonitorApp[.exe]
    ├── modules/
    ├── lib/tcl9.1/
    ├── licenses/
    └── vinc_data/monitor-station/ui/
```

Python-enabled Apps additionally deploy `python/vinc`; the system Python and user
`data/module` scripts remain external responsibilities. Distinct identities do not
avoid listener port conflicts. Use distinct Shell ports; stop/restart UI with its
existing `--port` option when necessary. Param-driven startup settings remain future work.

啟用 Python 的 App 另有 `python/vinc`，系統 Python 與使用者 `data/module` 自行提供。
不同身分不會自動解決 port 衝突；Shell 使用不同 port，UI 需要時用既有停止／重新啟動及
`--port` 操作。Param 啟動設定仍待後續設計。

- `VINC_BUILD_CONFIG` is required and relative to the calling CMakeLists directory.
  `STARTUP_SCRIPT` remains optional and follows the same path rule. It retains the
  embedded-script contract, including rejecting `-S` when embedded.
- Each App has its own executable, catalog, identity, script and generated headers.
  `INSTANCE_NAME` permits 1..48 lowercase ASCII letters, digits, `_`, `-`.
  Build outputs default to `<caller-build>/<target>/<configuration>`.
- Register Modules/messages during ordinary configuration, including after App
  declarations and in sibling directories. No manual finalize or registration
  inside user DEFER callbacks. Target customization remains available.
- Only selected custom Modules enter the catalog/dependencies. Explicitly building
  an unselected Module target is still allowed. All Apps share one message catalog,
  toolchain, configuration/layout ABI and, when enabled, CPython ABI.
- A compatible Module target is shared. Each App gets a thin UI adapter with its
  own `vinc_data/<INSTANCE_NAME>/ui` default; UI server libraries remain shared.
  The official `webUi`/`bagRecorder` ModuleType names are reserved.
- Python-enabled Apps share one binding target compiled against all registered
  messages. C++-only Apps do not depend on it. `Python3_EXECUTABLE` selects the
  configuration interpreter; `Python_EXECUTABLE` selects the binding ABI.
- GUI/menu targets are `<App>_vinc_guiconfig` and `<App>_vinc_menuconfig`.
  See [configuration](../../../tools/configure/README.md) for validation and editing.
- Install one App with `cmake --install build --component VincApp_CameraApp --prefix package`.
  Its package is `package/CameraApp/`; installing all uses one sibling directory per App.
- `.vinc-app-owner` rejects another App/identity claiming that directory. The owned-file
  manifest removes only unchanged obsolete managed files, preserving modified files
  and user data. Installation does not copy runtime logs/params or user Python Modules.
- Stop the App before updating its package. Copying is serialized per destination,
  but not a transactional whole-directory replacement: an I/O failure may leave a
  partially updated package. Repair/retry before starting it; hot update is unsupported.
- Central starts `webUi`/`ui` and `bagRecorder`/`recorder` before Tcl, waiting for Ready
  or bounded failure. Recorder stays idle until commanded. Failure is reported and
  central continues; explicit stop is not reversed by automatic startup.

- 設定檔必填、腳本選填，相對路徑以呼叫端 CMakeLists 為準；內嵌腳本仍拒絕 `-S`。
- 各 App 有獨立執行檔、catalog、身分、腳本與生成標頭，預設輸出目錄也分開。
- 一般配置期間可晚登記／跨目錄登記，保留 target 後續設定；不支援在使用者 DEFER 中登記。
- 僅選入的自訂 Module 進入 catalog／相依；所有 App 共用訊息全集及相容的工具鏈／ABI。
- 共用 Module target 與 Python 綁定；UI 僅按 App 重編薄接線，底下函式庫共用。
  `webUi`／`bagRecorder` 保留給官方 Module。
- GUI／menu 各自保存指定檔案。原始碼使用新入口時先關閉舊 `VINC_BUILD_APPS`／範例，
  再自行宣告 App；舊 `vinc_configure_app()` 暫時仍可設定內建 shell。
- 個別安裝使用 `--component VincApp_<App>`，各 App 放在 `<prefix>/<App>/`。
  所有權標記拒絕其他 App 覆蓋；清單僅刪除未被修改的舊管理檔案，保留使用者資料。
- 更新前先停止 App；同一目的地的複製會序列化，但不是整包原子替換。
  I/O 失敗可能留下部分更新，須修復／重試後再啟動，不支援執行中熱更新。
- 中央在 Tcl 前啟動 UI／Bag 並等待就緒或有界失敗；Bag 待命不錄製。
  官方 Module 失敗會回報後繼續，使用者明確停止後不自動重新啟動。


Version requests use exact matching. The current package version follows the
0.1.4 baseline; this does not publish a release/tag or promise compatibility with
every other binary carrying that version. Always use one matching SDK build.
Do not edit its installed headers/configuration. Rebuild the SDK when changing
layout settings; editing `config.hpp` is detected on the next package lookup.

指定版本時採完全匹配；目前套件版本沿用 0.1.4 基準，沒有因此發布新 tag／Release，
也不承諾所有同版號的任意產物都相容。請整套使用同一次匹配的 SDK 建置。
不要修改安裝目錄中的標頭／設定；調整布局設定須重建 SDK，修改 `config.hpp` 會在下次查找時遭拒。

## Matching-build checks / 相容性檢查

For Python Modules, enable `CONFIG_VINC_APP_PYTHON=y` in the App config file and select
`Python_EXECUTABLE` when necessary. SDKs include the binding build material even
when produced with `VINC_BUILD_PYTHON=OFF`; consumer builds require a compatible
CPython development environment and compile only the adapter, not framework
libraries. Enabled Apps deploy their own `python/vinc` package. User Module
scripts/dependencies remain the application's responsibility under `data/module`.
See [Python SDK](../module_system/python/README.md#installed-sdk-applications--安裝式-sdk-應用)
for ABI, pinned pybind11, resource ownership and runtime-selection contracts.

Python Module 可在各 App 設定檔啟用，必要時指定 `Python_EXECUTABLE`。即使製作 SDK
時關閉 Python，套件仍附帶建置材料；使用端需相容的 CPython 開發環境，只編譯接線，
不重建核心庫。啟用的 App 各自部署 `python/vinc`；使用者腳本及額外相依仍自行部署
於 `data/module`。ABI、pybind11、產物所有權及執行期選擇契約由上述 Python 文件維護。

Lookup rejects cross-compilation, multi-config generators, non-64-bit builds,
OS/compiler ID/compiler-version/configuration mismatches. A native probe compares
architecture, selected standard-library version/ABI, string size and Windows
CRT/iterator mode. Debug cannot silently fall back to Release. Installation into
an existing incompatible build-profile prefix fails before component files copy.

查找時拒絕交叉編譯、多組態 generator、非 64-bit，以及 OS／編譯器種類／版本／組態不符。
本機探針比對 CPU 架構、標準庫版本／ABI、string 大小及 Windows CRT／iterator 模式。
Debug 不會自動改用 Release；向不同建置 profile 的既有安裝目錄寫入，會在複製元件前拒絕。

These checks are conservative, not a complete ABI proof. They inspect CMake's
selected global flags; later target-local packing/ABI/CRT overrides, replacing
allocators, modified other headers, or mixing SDK artifacts remain unsupported.
System Threads/ZLIB and platform libraries remain system dependencies. Linux
glibc/libstdc++ deployment compatibility still needs validation on the deployment
machine; matching a compiler version alone does not establish it.

檢查偏保守，但不是完整 ABI 證明；探針依據 CMake 選定的全域編譯設定。
後續私自替單一 target 改 packing／ABI／CRT、替換 allocator、修改其他標頭或混合 SDK 產物仍不支援。
系統 Threads／ZLIB 及 OS libraries 仍由系統提供；Linux 部署的 glibc／libstdc++ 相容性仍須在
目標環境驗證，不能由編譯器版本相同推論。

## Tools and remaining composition / 工具與後續組裝

Installed Ribosome requires explicit `--input`, `--output`, and `--template`.
It never falls back to producer checkout/build paths. The template lives at
`${vinc_DATA_DIR}/ribosome/message_sample.txt`, built-ins at
`${vinc_DATA_DIR}/messages`. Generation output belongs in the consumer's build
directory, never inside the SDK. Registration helpers provide one merged
built-in/custom message list in the consumer build.

安裝版 Ribosome 必須明確帶入 `--input`、`--output`、`--template`，不回退到 SDK 製作者的舊路徑。
模板位於 `${vinc_DATA_DIR}/ribosome/message_sample.txt`，內建訊息位於 `${vinc_DATA_DIR}/messages`。
生成檔只放使用者的 build，不寫回 SDK；登記 helper 統一生成內建／自訂訊息清單。

`vinc_add_app()` supplies the application identity and its own OS-reported executable
path through the designated framework seam. Low-level `ModuleManager::create()`
requires explicit limits and an absolute Host path; no generated-path fallback
remains. Applications must use framework-owned composition, not a custom main.
The installed App links the prebuilt Module Runtime as well as central services;
no standalone Host tool is included in the package.
Python typed bindings use that same consumer message list. No dynamic message interpreter is added.

`vinc_add_app()` 經指定的框架接線傳入應用身分與 OS 查得的自身路徑。
底層 `ModuleManager::create()` 必須明確傳入 limits 與 Host 絕對路徑，不再有生成路徑預設。
應用必須由框架入口組裝，不支援自訂 main。安裝式 App 同時連結預編譯 Module Runtime 與
中央服務；套件不包含獨立 Host 執行檔。
Python 綁定使用同一組應用訊息清單，不新增動態訊息直譯器。

## Validation / 驗證

The native-only `sdk.installed_application` fixture inventories both the moved SDK
and the build/install App trees. It rejects retired Host binaries, test Hosts,
generated Host-path headers and unexpected native images, and writes sorted
SHA-256 manifests outside the inspected trees. The SDK may contain the Ribosome
build tool; an App contains its single process executable and native Plugin
libraries. `sdk.package_inventory` checks that this guard accepts valid trees and
rejects both a leftover Host and an unrelated executable.

`sdk.installed_application` 的純原生 fixture 會盤點搬移後 SDK，以及 build／install App
目錄；拒絕舊 Host、測試 Host、生成 Host 路徑標頭與非預期原生映像，在盤點目錄外輸出
排序後的 SHA-256 清單。SDK 可以含 Ribosome 建置工具；App 則只有一個程序執行檔及
原生 Plugin 庫。`sdk.package_inventory` 以正常樹、遺留 Host、額外執行檔驗證檢查器。

For a clean producer with `BUILD_TESTING=OFF`, run the application fixture directly
after building the SDK. `APPLICATION_WORK` must be a new absolute directory; it
keeps validation outputs outside the producer so that producer outputs can also
be temporarily hidden during a separate relocation check. Use the same compiler,
configuration and UI capability as the producer. The test uses the fixed central
identity `sdk-package-app`; do not run two copies concurrently on the same OS.

乾淨 SDK producer 使用 `BUILD_TESTING=OFF` 時，可在 SDK 建置後直接執行下列 fixture。
`APPLICATION_WORK` 必須是尚不存在的絕對路徑，將測試輸出放在 producer 外，方便另行
暫時移開 producer 做獨立部署檢查。編譯器、組態及 UI 能力須與 producer 一致。
此測試使用固定中央身分 `sdk-package-app`，同一 OS 上不要同時跑兩份。

```sh
cmake -DSDK_BUILD=/absolute/path/to/sdk-build -DAPPLICATION_WORK="/absolute/path/to/new package test" -DFIXTURE=/absolute/path/to/open-vinc/src/vinc/sdk/tests/installed_application -DGENERATOR=Ninja -DMAKE_PROGRAM=/absolute/path/to/ninja -DCOMPILER=/absolute/path/to/cxx -DCONFIG=Release -DWITH_UI=ON -P /absolute/path/to/open-vinc/src/vinc/sdk/tests/installed_application.cmake
```

This builds real custom-message Plugins, requires two distinct Module PIDs and
checks their OS-reported image path against the central App path. The existing
Bus/Bag/UI, orderly-shutdown, incremental-generation and feature-toggle checks
remain in the fixture. Python execution is tested separately.

fixture 建置真正自訂訊息 Plugin，要求兩個不同 Module PID，並將 Module 的 OS
執行檔路徑與中央 App 路徑比對；保留既有 Bus／Bag／UI、正常關閉、增量生成與功能切換
驗證。Python 執行由另一組測試負責。

`sdk.installed_consumer` installs to a temporary prefix, moves it, builds an
independent consumer, runs installed Ribosome, accesses generated message data,
initializes Tcl, creates/shuts down Manager services with an explicit Host path,
and verifies license copies. Negative cases cover wrong version/configuration,
Windows CRT/Linux libstdc++ ABI mismatch, changed config headers, wrong install
configuration and incompatible prefix overwrite. A consumer-header edit must
rebuild and change the executable's output, including localized MSVC builds.
The fixture deliberately calls internal Tcl initialization only to validate the
archive/resource closure; this is not a recommended user startup API.

`sdk.installed_consumer` 會安裝、搬移 SDK、獨立建置外部專案，執行安裝版 Ribosome、
存取生成訊息、初始化 Tcl，並以明確 Host 路徑建立／關閉 Manager 服務及比對授權檔。
負向測試涵蓋版本／組態不符、Windows CRT／Linux libstdc++ ABI 不符、config 標頭遭修改、
錯誤安裝組態，以及禁止覆蓋不同 profile；修改使用者標頭後也須實際重編並改變程式輸出，
包含本地化 MSVC 的增量相依追蹤。
測試直接呼叫 Tcl 內部初始化僅為驗證庫與資源是否完整，不是使用者啟動框架的建議 API。

Platform results are recorded after running the tests; a successful package
configure alone is not execution evidence. macOS requires native validation.

平台結果須依實際測試記錄；套件配置成功不代表執行已通過。macOS 仍須原生驗證。

### Stage-one results (2026-10-29) / 第一輪實測結果（2026-10-29）

| Environment / 環境 | Build / 建置 | Results / 結果 |
| --- | --- | --- |
| Windows 11 x64, MSVC 19.51.36257, CMake 4.3.2, Ninja 1.13.2 | Debug + Release | Installed-consumer test **1/1 each**, related regression suites **21/21 each** / 安裝測試各 1/1、相關回歸各 21/21 |
| Same Windows environment / 同上 | Release | Existing source `sdk.external_consumer` **1/1**, including custom messages/Modules/embedded startup / 原始碼外部專案 1/1，含自訂訊息、Module 與啟動腳本 |
| WSL Ubuntu 24.04.5 x86_64, GCC/libstdc++ 13.3, CMake 3.28.3, Ninja 1.11.1 | Release | Installed-consumer test **1/1**, related regression suites **21/21** / 安裝測試 1/1、相關回歸 21/21 |

The SDK build profiles disabled central apps, examples, UI and Python, and enabled
tests. This is not a rerun of the entire application/UI/Python test matrix.
Related suites cover Common, Ribosome, generated messages, Bus/Module process
integration, RuntimeServices, Shell and SDK/license delivery.

SDK 建置設定關閉中央 apps、範例、UI、Python，開啟測試；這不代表重跑整個應用／UI／Python
測試矩陣。相關回歸涵蓋 Common、Ribosome、生成訊息、Bus／Module 程序整合、RuntimeServices、
Shell、SDK 與授權交付。

An additional Linux check installed and moved the SDK, moved both producer source
and build directories out of their original locations, then configured/built/ran
a fresh external consumer successfully (**1/1**). Those temporary producer paths
were restored afterward. Windows tests relocate the SDK and remove its original
install prefix; they do not hide the full Windows source/build trees.

Linux 另將 SDK 安裝搬移，再把製作者的 source 與 build 目錄移離原位置；全新外部專案仍可
配置、建置、執行（**1/1**），驗證後已還原暫存來源。Windows 測試搬移 SDK 並移除原安裝位置，
沒有移開整個 Windows source／build。

Local evidence (ignored by Git): `build/sdk-package-{debug,release}-test.log`,
`build/sdk-package-{debug,release}-regression.log`,
`build/sdk-package-source-external.log`, `build/sdk-package-linux-final.log`,
and `build/sdk-package-linux-isolated.log`. Usable Windows SDKs were installed at
`build/sdk-stage/Debug` and `build/sdk-stage/Release`. They are local build outputs,
not published release assets. That first pass did not cover native macOS package
validation or complete application deployment; stage-two results are recorded below.

本機證據位於上述 Git 忽略的 logs；可用 Windows SDK 放在 `build/sdk-stage/Debug` 與
`build/sdk-stage/Release`，僅為本機產物，沒有發布 Release assets。
第一輪未涵蓋 macOS 原生套件驗證及完整應用部署；第二輪紀錄另列於下方。

### Stage-two validation (2026-10-29) / 第二輪驗證（2026-10-29）

The same native toolchains above built SDK profiles with UI enabled, central apps,
examples and Python disabled, and tests enabled. Node 24.19.0 built the packaged
frontend; installed consumers used neither Node nor Python. This is scoped package
and regression coverage, not the full project/platform test matrix.

沿用上表工具鏈，SDK 設定啟用 UI 與測試，關閉中央 apps、examples、Python。
Node 24.19.0 建置 SDK 的前端資源；安裝式使用端不使用 Node 或 Python。
以下是套件與相關回歸範圍，不代表重跑整個專案的所有平台測試。

| Environment / 環境 | Results / 結果 |
| --- | --- |
| Windows 11 x64 / MSVC 19.51, Debug + Release | Installed core **1/1 each**, application **1/1 each**, related regression **21/21 each**, OSAL path + UI **4/4 each** / 安裝核心、應用各 1/1，相關回歸各 21/21，路徑與 UI 各 4/4 |
| Windows Release source integration / 原始碼接線 | Existing `sdk.external_consumer` **1/1**, including embedded startup / 舊外部專案含內嵌腳本通過 |
| WSL Ubuntu 24.04.5 x86_64 / GCC 13.3, Release | Installed core **1/1**, application + path/UI **5/5**, related regression **21/21** / 安裝核心 1/1，應用加路徑／UI 5/5，回歸 21/21 |

The application fixture installs and moves a complete bundle, hides its SDK and
consumer build directories, and starts from an unrelated cwd. Two real ModuleHost
processes exchange a nested custom message via copied publication and loans, with
callback/get_last verification. Recorder writes that type; UI authenticates a local
session and decodes its preview through HTTP. Shutdown confirms all Module exits.
It also tests changed/deleted/invalid/recovered schemas, late cross-directory
registration, an edited embedded script, a plain `-S` app, no-official-Module mode,
duplicate types/second-app rejection, resource repair, and preservation/exclusion
of user files during rebuild/install. Deployment filename collisions are rejected
before any destination files are written.

應用 fixture 安裝並搬移整包，再移開 SDK 與使用者 build 目錄，從不相干的 cwd 啟動。
兩個真正的 ModuleHost 以複製及 loan 發布巢狀自訂訊息，驗證 callback／get_last。
Recorder 寫入該型別，UI 經本機 session 驗證後由 HTTP 回傳解碼預覽；關閉時確認所有 Module 退出。
另測試 schema 修改／刪除／無效／復原、延後跨目錄登記、內嵌腳本修改、一般 `-S` 應用、
排除官方 Module、拒絕重名／第二個應用、補回部署資源，以及重建／安裝保留或排除使用者檔案。
部署檔名衝突會在寫入目的地前拒絕。

Evidence is under ignored `build/sdk-stage2-*.log`. The tests exercise HTTP, not
an interactive browser session. Native macOS validation of this packaging path
and `osal::executable_path()` remains pending; earlier macOS framework evidence is
unchanged. At this historical stage, low-level Manager source defaults and installed
Python were still limited; the current composition section and Python README describe
their subsequent updates. SDK artifacts were not published as a Release in this stage.

本機證據為 Git 忽略的 `build/sdk-stage2-*.log`。UI 本輪驗證 HTTP，未操作互動瀏覽器。
本次套件及 `osal::executable_path()` 的 macOS 原生驗證待補，不撤銷先前框架的 macOS 證據。
該歷史輪次仍有底層 Manager source 預設與安裝式 Python 的限制；後續更新見目前組裝段落
及 Python README。本輪次沒有發布 SDK Release 產物。
