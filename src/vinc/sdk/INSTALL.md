# Installed C++ SDK / 安裝式 C++ SDK

App dispatches both roles and launches itself; standalone Host export/copy wiring
is removed. Clean SDK-only and relocated C++ App validation passed on Windows/WSL;
the final Windows/WSL integration regressions also passed. The subsequent native
macOS baseline is recorded in [MACOS_VALIDATION.md](MACOS_VALIDATION.md).
The newer Param restart hints passed fresh native Mac Debug/Release installed-SDK
acceptance; see [MACOS_PARAM_VALIDATION.md](MACOS_PARAM_VALIDATION.md).
App 已自行分流／啟動自己，獨立 Host 匯出與複製接線已移除；乾淨 SDK-only 與搬移 C++ App
及最終整合回歸已通過 Windows／WSL 驗證，後續 Mac 原生基線見上述報告。
新版 Param／重啟標記已補齊 Mac Debug／Release 乾淨 SDK，方法及限制見上述新報告，
不套用舊基線直接認證。

This package reuses prebuilt framework libraries through
`find_package(vinc CONFIG REQUIRED)`. Source `add_subdirectory()` remains supported.
The package is opt-in (`VINC_INSTALL_SDK=ON`), with no download, dependency build,
or service startup during lookup. Lookup compiles/runs a small native ABI probe.

此套件讓外部 C++ 專案透過 `find_package(vinc CONFIG REQUIRED)` 使用預編譯核心。
原有 `add_subdirectory()` 保留；以 `VINC_INSTALL_SDK=ON` 明確啟用安裝規則。
查找套件不下載或重建相依、不啟動框架服務；會編譯並執行小型本機 ABI 探針。

## Scope / 本輪範圍

Native MSVC consumers must meet the [Common toolchain policy](../common/README.md#build-toolchain-policy)
(19.44 or newer). This is only the minimum: a prebuilt SDK still requires the exact
compiler version, build configuration and ABI recorded when it was built.

原生 MSVC 使用者須符合 [Common 工具鏈規則](../common/README.md#build-toolchain-policy)
（最低 19.44）。最低版本不代表 SDK 可混用版本：預編譯 SDK 仍要求原本的精確編譯器版本、
建置組態與 ABI。

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

<a id="sdk-install-command"></a>

### One-command SDK installation / 一鍵安裝 SDK

After preparing dependencies and pinned submodules, run from the checkout:
先準備系統／UI 相依與固定版本 submodule，再執行：

```powershell
py -3 build.py --install-vinc-sdk
# Equivalent Windows launcher: build.bat --install-vinc-sdk
```

```sh
./build.py --install-vinc-sdk
# Equivalent launcher: sh build.sh --install-vinc-sdk
```

This mode defaults to Release; ordinary builds still default to Debug. It builds
this checkout's complete SDK (including UI/Bag resources and Python binding build
material), not example Apps or prebuilt Python bindings. Dependencies are checked,
not installed; missing UI npm packages still require `npm ci`. CMake owns all
install rules, generated messages, assets and license delivery.

此模式預設 Release，一般建置仍預設 Debug。建置完整 SDK，含 UI／Bag 資源及 Python
綁定建置材料，不建置範例 App、不預先綁定 Python ABI。只檢查相依，不自動安裝工具；
缺少 UI npm 套件時仍需先執行 `npm ci`。安裝清單、訊息、資源及授權均由 CMake 維護。

| Platform / 平台 | Default dedicated directory / 預設專用目錄 |
| --- | --- |
| Windows | `%LOCALAPPDATA%\OpenVINC\sdk` |
| Linux | `$XDG_DATA_HOME/open-vinc/sdk`, otherwise `~/.local/share/open-vinc/sdk` |
| macOS | `~/Library/Application Support/OpenVINC/sdk` |

`--sdk-root PATH` selects the **exact dedicated installation directory**, not a
parent for versioned installations. One location holds one SDK. `--config Debug`
selects Debug instead; `--no-register` leaves CMake registration unchanged.
`--jobs`, `--build-dir` and non-conflicting `-D` settings remain available. External
`--source`, partial `--target`, and overrides of the required SDK feature flags
are rejected. SDK builds use a separate `build/<platform>-<compiler>-sdk-<config>`
tree. No cache is automatically cleaned or reused across incompatible toolchains.
New wrapper records also reject mixing ordinary-build and SDK-install purposes
in one tree; legacy record migration follows the
[build toolchain contract](../../../tools/README.md#toolchains-and-safety--工具鏈與保護邊界).

`--sdk-root PATH` 是**實際專用安裝目錄**，不是多版本的上層目錄；同一位置只放一份 SDK。
可選 `--config Debug`、`--no-register`、`--jobs`、`--build-dir` 與不衝突的 `-D`。
不接受外部來源、部分 target 或覆蓋 SDK 必備功能開關。SDK 與一般 App 分開建置目錄，
不自動清除 cache、不跨不相容工具鏈沿用。
新紀錄也會拒絕一般建置與 SDK 安裝共用同一 build 目錄；舊紀錄升級依上述工具鏈契約處理。

#### Update and failure contract / 更新與失敗契約

The installer uses plain text, with no terminal UI dependency. Six numbered stages
identify destination/tool checks, CMake configure, build, staged verification,
replacement/final verification, and CMake registration. `--no-register` marks the
last stage as skipped. CMake output remains streamed; the final summary states the
configuration, destination, installation/recovery state, registration result and
this invocation's bounded command-log path. Failures before opening that log say
no log was created, rather than pointing to an earlier invocation's file.

安裝器維持純文字，不增加終端 UI 相依。依序顯示六個編號階段：目的地／工具檢查、
CMake 配置、建置、暫存驗證、替換／最終驗證、CMake 登記；`--no-register` 明確標為
略過登記。CMake 輸出仍即時顯示，最後摘要列出組態、位置、安裝／還原狀態、登記結果及
本次有容量上限的命令 log 路徑。開啟 log 前就失敗時會顯示本輪尚未建立 log，
不把前次 log 當成本次結果。只有全部要求步驟成功才顯示 completed；登記失敗仍為失敗，
但會明確說明新版 SDK 已安裝並提供 `vinc_DIR`。

```text
[1/6] Check destination and build tools
[2/6] Configure SDK
[3/6] Build SDK
[4/6] Stage and verify SDK
[5/6] Replace and verify installation
[6/6] Register SDK with CMake

SDK installation completed
Location: ...
Configuration: Release
SDK status: New SDK installed and verified.
CMake registration: Updated for this user; existing project caches unchanged.
Log: .../.vinc-build.log (bounded command output)
Explicit selection: -Dvinc_DIR=.../lib/cmake/vinc
```

- An existing installer-owned SDK requires `[y/N]` confirmation before toolchain
  initialization or building. No, empty input, EOF or cancellation does not change
  the SDK/registry or create build files. The prompt identifies the current build
  profile, source checkout and requested configuration; CMake reports the newly
  selected compiler/profile during configuration.
- The `.vinc-sdk-install.json` inventory covers files, content hashes and empty
  directories. Unowned directories, modified/missing/extra contents, links and
  Windows reparse points are rejected. It is an accidental-data-loss guard, not
  authentication against a malicious same-user process. Source/build trees and
  user/filesystem roots cannot be SDK destinations.
- Build/install into a sibling staging directory first. Compile, link and run a
  small external SDK probe there; then rename the old SDK aside, move the new SDK
  into place and repeat the probe at its final location. Ordinary failures and
  interrupts during replacement attempt to restore the previous installation.
  Delete the old SDK only after successful final verification. It does not retain
  a history of successful SDK installs.
- A sibling `.NAME.install-lock` protects this installer's operations against the
  same destination. The build lock remains held through installation. These locks
  do not stop external compilers or direct `cmake --install`; stop consumers before
  updating. Two directory renames are not an atomic filesystem transaction.
- A hard kill/power loss can leave `.NAME.sdk-backup` or `.NAME.stage-*`. A retained
  backup blocks the next update for manual inspection/recovery. Do not delete the
  only valid SDK; restore the backup to the destination if needed. Failed backup
  cleanup/rollback is reported with its location, rather than deleting more data.
- After verification, update only this installer's CMake user-registry entry.
  Registration failure returns nonzero but preserves the installed SDK and prints
  its explicit `vinc_DIR`. No machine PATH, system registry or other installer
  entries are modified. Separate destinations may coexist; the last successful
  registration becomes this installer's default, including a Debug installation.

- 已有本工具安裝的 SDK 時先詢問 `[y/N]`；否、空輸入、EOF 或取消不變更 SDK／登記，
  不開始建置。提示舊 profile、本次來源及組態；選定工具鏈會在配置時顯示。
- 所有權清單包含檔案雜湊與空目錄；未知目錄、增刪／修改內容、連結或 junction 均停止。
  這是防誤刪措施，不是防禦同權限惡意程序的安全沙箱。
- 先暫存安裝並驗證，再備份舊目錄、替換及驗證最終位置；一般失敗／中斷嘗試還原。
  新版成功才清掉舊版，不累積版本歷史。
- 同目的地安裝互斥，但無法阻止直接 CMake 安裝或其他專案讀取；更新前須停止使用端
  建置。兩次目錄 rename 不構成完全原子的交易。
- 強制終止／斷電可能留下暫存或備份；備份存在時拒絕繼續更新，先人工確認及還原。
  還原或清理受 OS 拒絕時明確回報保留位置。
- 安裝驗證成功後才更新本工具自己的使用者 CMake 登記；登記失敗仍保留已安裝 SDK，
  回傳失敗並提供 `vinc_DIR`。不改系統 PATH，不移除其他工具的登記。

#### CMake discovery / CMake 查找

New compatible projects can use `find_package(vinc CONFIG REQUIRED)` without a
prefix argument. Windows uses HKCU's `Software\Kitware\CMake\Packages\vinc`;
Linux/macOS use `~/.cmake/packages/vinc`. The owned entry is
`open-vinc-build-installer`, pointing to `lib/cmake/vinc`, not the build tree.
Other registry entries have unspecified search order and trigger a warning.
An explicit `vinc_DIR`, an existing cache, or disabled package-registry lookup
still controls the consumer. Existing project caches are not rewritten. Debug,
compiler and ABI mismatches remain errors; no automatic SDK selection or fallback.
See [CMake's registry contract](https://cmake.org/cmake/help/v3.20/manual/cmake-packages.7.html#package-registry).

相容的新專案可直接 `find_package(vinc CONFIG REQUIRED)`。登記指向安裝目錄的
`lib/cmake/vinc`，不指向建置目錄。其他登記可能影響搜尋順序，會提示但不擅自刪除；
使用者明確指定的 `vinc_DIR`、既有 cache 或停用 registry 的設定仍有效。
不替既有專案換 SDK，不自動挑選另一個 Debug／工具鏈版本，不放寬 ABI 檢查。

#### Installer validation (2026-10-31) / 安裝器驗證

```sh
python3 -B -m unittest discover -s tools/tests -p test_sdk_install.py -v
# Windows: replace python3 with py -3.
```

| Environment / 環境 | Evidence / 驗證 |
| --- | --- |
| Windows 11 x64 build 26200; MSVC 19.51.36257; CMake 4.3.2; Ninja 1.13.2; Python 3.13.12 | 18 installer cases: 16 passed, POSIX-only registry and unprivileged symlink cases skipped; Windows junction rejection passed. Clean full Release SDK install, decline/update, staged/final probe and external App with generated messages/Module DLLs passed. A separate space/Chinese-prefix install passed. / 18 項中 16 通過，2 項跳過；junction 防護、完整 Release 安裝、拒絕／接受更新、搬移前後探針、自訂訊息／DLL App，以及空白中文目錄通過。 |
| WSL Ubuntu 24.04 x86_64; GCC 13.3.0; CMake 3.28.3; Ninja 1.11.1; Python 3.12.3 | 18 installer cases: 17 passed, Windows-only junction case skipped. Clean full Release SDK install, decline/update, staged/final probe and external App with generated messages/Module shared libraries passed. / 18 項中 17 通過，跳過 Windows junction；完整 Release 安裝、拒絕／接受更新、探針與外部訊息／Module App 建置通過。 |

Both platforms additionally passed all 15 ordinary build-wrapper cases, including
the real CMake fixture. Installer cases include injected build/install/registration
failures, validation failure rollback, interruption immediately after rename,
concurrent-installer rejection, unowned/modified-content rejection, and real CMake
registry lookup using an isolated test package. The actual `vinc` user default was
not changed. Windows also rejected replacement while a consumer was still using
the SDK; the original SDK remained intact, and the sequential retry passed.

兩平台另有一般 build wrapper 15 項全部通過，包含真實 CMake fixture。
安裝測試包含注入建置／安裝／登記失敗、驗證失敗及 rename 後中斷還原、並行拒絕、
未知／修改檔案拒絕，以及隔離測試套件的真實 CMake registry 查找；未改正式 Vinc 預設。
Windows 實際遇過使用端仍開啟 SDK 時拒絕替換，舊版完整保留；使用端結束後依序重試通過。

Local logs are under `build/script-validation/{windows,wsl}-sdk-*.log` and
`{windows,wsl}-build-regression.log` (not tracked). Existing framework compiler
warnings were not changed. Full runtime suites, native macOS installer execution,
power-loss recovery and Ubuntu 22.04 deployment were not validated in this round;
prior platform/runtime evidence is unchanged. The first installer validates a
small external executable on each install, not the complete framework test suite.

本機 log 留在上述 build 路徑，不提交 Git。既有編譯器警告未調整；本輪沒有重跑完整
Runtime suites，也未驗證 macOS 原生安裝器、斷電還原或 Ubuntu 22.04 部署，
不影響既有平台驗證紀錄。每次安裝自動執行的是小型外部探針，不是完整框架測試。

Output follow-up (2026-10-31): 22 installer cases ran on each platform (Windows:
20 passed/2 platform or permission skips; WSL: 21 passed/1 Windows-only skip).
All 15 build-wrapper cases passed on both platforms, including real CMake builds.
Real incremental Release SDK updates verified all six phase headings and final
summaries on Windows/WSL, using isolated destinations and `--no-register`.
Injected failures checked accurate stage/log reporting, rollback success versus
blocked rollback, installed-but-registration-failed, and cleanup failure after
verification. No native macOS follow-up or complete Runtime rerun was performed.
Local logs: `build/script-validation/{windows,wsl}-sdk-output-*.log` and
`{windows,wsl}-build-output-regression.log`.

輸出整理追加驗證：兩平台各執行 22 項安裝器測試，Windows 20 通過／2 跳過，WSL
21 通過／1 跳過；一般 build wrapper 含真實 CMake 的 15 項均通過。隔離目錄的
Release 增量更新實測六階段與摘要；注入失敗驗證還原成功／受阻、登記失敗、
驗證後清理失敗，以及尚未建立 log 的提示。不變更正式 SDK 登記，不宣稱 macOS
已驗證，也未重跑完整 Runtime suites。

### Direct CMake installation / 直接使用 CMake 安裝

Use a prepared native toolchain and initialized pinned submodules. Windows requires
the matching MSVC developer environment. Ninja single-config Debug/Release are the
initial package profiles; use a different prefix for each configuration/toolchain.

先準備工具鏈及固定版本 submodule；Windows 使用匹配的 MSVC developer 環境。
第一版採 Ninja 單組態 Debug／Release，各工具鏈與組態使用不同安裝目錄。

```sh
npm ci --prefix src/vinc/ui/web
cmake -S src -B build/sdk-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DVINC_INSTALL_SDK=ON -DVINC_BUILD_APPS=OFF -DBUILD_TESTING=OFF
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
Direct CMake installation does not register the package or provide the wrapper's
ownership/update transaction. No package manager, archive publication or system PATH change is provided.

`VincSDK` 是安裝規則的 component 名稱，不是 `find_package(... COMPONENTS ...)` 的選項；
尚未提供套件子集選擇。直接 CMake 安裝不會登記或建立 wrapper 的更新所有權；
不提供套件管理器、發布壓縮檔，也不修改系統 PATH。

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
  Generated catalogs register them as protected System Modules, with fixed `ui`
  and `recorder` instances. Use their Shell builtins, not direct type launches or
  `debug stop`; see [System lifecycle](../module_system/README.md#system-modules).
- Python-enabled Apps share one binding target compiled against all registered
  messages. C++-only Apps do not depend on it. `Python3_EXECUTABLE` selects the
  configuration interpreter; App Kconfig selects the binding interpreter and runtime
  default. `Python_EXECUTABLE` may belong to other CMake consumers; it does not
  override VINC's Kconfig selection. See the [selection contract](../../../tools/configure/README.md#module-python-selection--module-python-選擇).
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
  central continues while System supervision schedules recovery. `ui disable`
  closes HTTP only and survives process restart; `recorder finish` ends recording.
  Ordinary lifecycle stop is forbidden; framework shutdown still stops both.

- 設定檔必填、腳本選填，相對路徑以呼叫端 CMakeLists 為準；內嵌腳本仍拒絕 `-S`。
- 各 App 有獨立執行檔、catalog、身分、腳本與生成標頭，預設輸出目錄也分開。
- 一般配置期間可晚登記／跨目錄登記，保留 target 後續設定；不支援在使用者 DEFER 中登記。
- 僅選入的自訂 Module 進入 catalog／相依；所有 App 共用訊息全集及相容的工具鏈／ABI。
- 共用 Module target 與 Python 綁定；UI 僅按 App 重編薄接線，底下函式庫共用。
  `webUi`／`bagRecorder` 保留給官方 Module。
- GUI／menu 各自保存指定檔案。原始碼使用新入口時先關閉舊 `VINC_BUILD_APPS`／範例，
  再自行宣告 App；所有 App 共用 `vinc_add_app()`。
- 個別安裝使用 `--component VincApp_<App>`，各 App 放在 `<prefix>/<App>/`。
  所有權標記拒絕其他 App 覆蓋；清單僅刪除未被修改的舊管理檔案，保留使用者資料。
- 更新前先停止 App；同一目的地的複製會序列化，但不是整包原子替換。
  I/O 失敗可能留下部分更新，須修復／重試後再啟動，不支援執行中熱更新。
- 中央在 Tcl 前啟動 UI／Bag 並等待就緒或有界失敗；Bag 待命不錄製。
  官方 Module 失敗會回報後繼續，由 System 監督排程恢復。`ui disable` 只關閉 HTTP，
  程序重啟仍保留此意圖；`recorder finish` 只結束錄製。一般 stop 被拒絕，整體 shutdown 仍停止兩者。


Version requests use exact matching. The current package version follows the
0.1.4 baseline; this does not publish a release/tag or promise compatibility with
every other binary carrying that version. Always use one matching SDK build.
Do not edit its installed headers/configuration. Rebuild the SDK when changing
layout settings; editing `config.hpp` is detected on the next package lookup.

指定版本時採完全匹配；目前套件版本沿用 0.1.4 基準，沒有因此發布新 tag／Release，
也不承諾所有同版號的任意產物都相容。請整套使用同一次匹配的 SDK 建置。
不要修改安裝目錄中的標頭／設定；調整布局設定須重建 SDK，修改 `config.hpp` 會在下次查找時遭拒。

## Matching-build checks / 相容性檢查

SDK application configuration and Python bindings use the same minimum-version
policy as source builds; see [Common toolchain policy](../common/README.md#build-toolchain-policy).
The SDK installs that policy with its CMake helpers. Merely finding and linking
the C++ SDK does not discover or require Python.

SDK 應用設定與 Python 綁定沿用來源建置的最低版本規則，見上述 Common 契約；
SDK 會隨 CMake helper 交付規則。單純查找及連結 C++ SDK 不會搜尋或要求 Python。

For Python Modules, enable `CONFIG_VINC_APP_PYTHON=y` in the App config file and select
System/Custom Python in that same file. SDKs include the binding build material even
when the producer builds no Python binding; consumer builds require a compatible
CPython development environment and compile only the adapter, not framework
libraries. Enabled Apps deploy their own `python/vinc` package. User Module
scripts/dependencies remain the application's responsibility under `data/module`.
See [Python SDK](../module_system/python/README.md#installed-sdk-applications--安裝式-sdk-應用)
for ABI, pinned pybind11, resource ownership and runtime-selection contracts.

Python Module 可在各 App 設定檔啟用，並以同份 Kconfig 選擇 System／Custom Python。即使製作 SDK
時關閉 Python，套件仍附帶建置材料；使用端需相容的 CPython 開發環境，只編譯接線，
不重建核心庫。啟用的 App 各自部署 `python/vinc`；使用者腳本及額外相依仍自行部署
於 `data/module`。ABI、pybind11、產物所有權及執行期選擇契約由上述 Python 文件維護。

On MSVC, SDK installation builds and ships a separate `/MT` (`/MTd` in Debug)
archive closure for the binding, including its Common error catalog and fmt.
This happens even when the producer builds no Python binding and requires no Python development
files in the producer. Installed Apps compile their adapter and generated message
headers with that closure; they do not recompile the framework. Central/C++ Plugins
remain `/MD` (`/MDd`). The imported `*_python_runtime` targets are internal recipe
dependencies, not a second public C++ SDK. CRT compatibility checks reject mixing
the two closures; missing installed archives reject package lookup. Do not override
the App's global CRT to select Python's private CRT.

MSVC 的 SDK 安裝會另外建置及交付 `/MT`（Debug 為 `/MTd`）庫，含 Common 錯誤表及 fmt。
即使製作者未建置 Python binding 也會交付，不要求製作者提供 Python 開發檔。
下游只編譯 Python 接線與生成訊息；中央及 C++ Plugin 維持 `/MD`／`/MDd`。
`*_python_runtime` 是內部建置相依，不是第二套公開 C++ SDK；混用 CRT 或套件缺庫會失敗。
不要修改 App 全域 CRT 來選擇 Python 的私有 CRT。

The binding recipe, runtime policy helper, generated metadata and deployment
manifest travel with the SDK/App. Linux/macOS retain normal system runtimes.
This adds producer build/archive storage cost, not extra runtime DLLs in each App.
Static CRT updates require rebuilding/redeploying the binding; exact SDK compiler,
configuration and Python ABI requirements are unchanged.

接線 recipe、Runtime 政策 helper、metadata 與部署清單隨 SDK／App 交付。
Linux／macOS 維持原本系統 Runtime。增加的是製作 SDK 的編譯與 archive 容量，
不會為 App 增加額外 Runtime DLL；靜態 CRT 更新需重建與部署，原 ABI 配對要求不變。

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

The bundled source App now uses this same constructor. Current migration and
validation evidence is maintained in the [App composition report](../../../apps/vinc/README.md#app-composition-unification--app-組裝統一).
內建 App 已統一組裝，最新遷移與驗證結果見上述主紀錄；下方保留各階段的歷史證據。

## Validation / 驗證

新版 Plugin entry 13／Param wire v3 的 Mac 乾淨 SDK producer、搬移 consumer／App／Python
已通過 Debug／Release 各三套；見 [Mac SDK 報告](MACOS_PARAM_VALIDATION.md)。
以下退出修復／FD／舊 SDK 數字保留其原始受測版本，不代表本輪重跑完整框架。

Latest Mac exit-repair results are in the [step 3 report](../osal/MACOS_EXIT_VALIDATION.md).
Original Python-enabled full CTest is Debug **151/153**, Release **152/153**;
the SDK subset passed **14/14** each. The report separately records clean
Python-disabled producers, downstream handoff, and retained integration failures.

最新退出修復的 macOS 驗證由上方報告維護；SDK 子集通過不等於原始全套全綠，
乾淨 producer／下游矩陣亦分開記錄，不沿用舊版本結果代替。

Earlier native Mac FD-repair acceptance at `a19c0d4`: the default C++ producer's
Debug/Release full CTest passed **132/132** each, including installed SDK/Python/App
checks; additional clean SDK consumer and relocated App acceptance also passed.
That revision's independent exit stress reproduced an observation gap; see the [Mac FD report](../osal/MACOS_FD_VALIDATION.md). Subsequent repair and acceptance are recorded above, without replacing that baseline failure evidence.
This configuration does not register the older Python-enabled example fixture below.

該輪 FD 修復補驗與退出調查見上述歷史報告；當時的一般整合通過不代表退出問題已修復，
也不代表下方已知的範例部署 fixture 已解決。

Earlier macOS follow-up at `627c409`: Debug/Release source and clean installed-SDK
acceptance passed within the [recorded scope](MACOS_VALIDATION.md). Original full
CTest is **125/126** per configuration; the example fixture's deployment omission
was unresolved at that time. External corrected-deployment evidence is
reported separately, not counted as an original-suite pass. The subsequent
[maintained fixture repair](../../../examples/userExampleModule/README.md#macos-deployment-fixture-gap)
passed focused native Mac Debug/Release checks; historical full-suite counts remain unchanged.

先前 `627c409` 的 macOS 補驗見相應報告；兩組乾淨 SDK 下游矩陣通過，但該輪原始全套各保留一項
範例 fixture 失敗。下方歷史結果保留其原始版本與環境範圍。

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

The moved consumer also rebuilds the maintained OSAL coordination fixture against
installed targets only: one executable and two independently linked libraries.
It checks shared ownership/exclusion, deadlines, cancellation, invalid/late binding
and unloading while the owner remains alive. Test sources are copied into the
consumer workspace; they are not installed as SDK APIs or linked from producer outputs.
Current FD/spawn repair acceptance and platform limitations are recorded in the
[stage 4 evidence](../osal/FD_SPAWN_PLAN.md#stage-4-acceptance).

`sdk.installed_consumer` 會安裝、搬移 SDK、獨立建置外部專案，執行安裝版 Ribosome、
存取生成訊息、初始化 Tcl，並以明確 Host 路徑建立／關閉 Manager 服務及比對授權檔。
負向測試涵蓋版本／組態不符、Windows CRT／Linux libstdc++ ABI 不符、config 標頭遭修改、
錯誤安裝組態，以及禁止覆蓋不同 profile；修改使用者標頭後也須實際重編並改變程式輸出，
包含本地化 MSVC 的增量相依追蹤。
測試直接呼叫 Tcl 內部初始化僅為驗證庫與資源是否完整，不是使用者啟動框架的建議 API。

搬移後的使用端另以安裝式 target 重建既有 OSAL 協調測試：一個執行檔與兩個獨立連結庫，
驗證共用互斥、期限／取消、錯誤／過晚綁定與 owner 存活期間的卸載。
測試來源複製到使用端工作目錄，不作為 SDK API 交付，也不連結製作者的產物。

Platform results are recorded after running the tests; a successful package
configure alone is not execution evidence. macOS requires native validation.

平台結果須依實際測試記錄；套件配置成功不代表執行已通過。macOS 仍須原生驗證。

For manual `installed_consumer.cmake` runs on slow mounted filesystems,
`-DSDK_TEST_COMMAND_TIMEOUT=600` raises its per-command limit in seconds; the
default stays 90. It also reports the process result, including timeout, on failure.
This does not change production waits or CTest's separate outer timeout. Prefer a
native filesystem for large SDK resource copies; record any increased test limit
with the result.

在較慢的掛載檔案系統手動執行 `installed_consumer.cmake` 時，可明確指定
`-DSDK_TEST_COMMAND_TIMEOUT=600`（秒）；預設仍為 90，失敗會顯示包含逾時在內的執行結果。
不改正式程式等待或 CTest 外層期限；大型 SDK 資源建議使用原生檔案系統，調高測試期限
須隨驗證結果記錄。

<a id="static-python-sdk-validation"></a>

### Static Python SDK validation (2026-11-02) / Python 靜態庫交付驗證

Fresh Windows SDK producers use MSVC 19.44.35229, CMake 4.3.2, Ninja 1.13.2 and
Node 24.19.0, with UI enabled and Apps/examples/tests/Python binding disabled.
Downstream Python Apps select unmodified official CPython 3.13.12 x64 with its
original VCRUNTIME 14.42.34438.0. An isolated WinRT 3.2.1 MSVCP 14.29.30157.0 is
deliberately loaded before Vinc in external-import and Module bootstrap tests;
no system/interpreter DLL is replaced. `PYTHONMALLOC=debug` and faulthandler are enabled.

Windows SDK 由上述工具鏈全新建置，製作者不啟用 Python 綁定，使用端才選擇 Python。
測試刻意先載入舊 MSVCP，再使用 Vinc 自訂訊息、Context 服務；不替換系統或 Python DLL。

| Environment / 環境 | Result / 結果 |
| --- | --- |
| Windows Release installed Python | Full `installed_python.cmake` fixture passed (500.17 s). / 完整安裝式 Python 驗收通過。 |
| Windows Debug installed Python | Full `installed_python.cmake` fixture passed with `/MTd`, including old-DLL preloading, relocation, rejection, repair and cleanup. / Debug 完整安裝式 Python 驗收通過。 |
| Windows Release native C++ | `installed_consumer.cmake` passed: relocation, generated messages, execution, ABI/version/header rejection and incremental header rebuild. / 原生 C++ 安裝回歸通過。 |
| WSL2 Ubuntu 24.04, GCC/libstdc++ 13.3.0, Release | `installed_consumer.cmake` passed with `SDK_TEST_COMMAND_TIMEOUT=600`; the original 90-second install attempt stopped during Tcl resource copying on `/mnt/d`. / 原生 SDK 安裝、搬移與拒絕測試通過；記錄提高的測試期限，不改 Runtime 等待。 |
| WSL2 Ubuntu 24.04, GCC/libstdc++ 13.3.0, CPython 3.12.3 | Independently installed Python recipe built a shared extension and passed custom `SdkPacket` creation/serialization/deserialization. No Node/UI App fixture was used. / 安裝式 Python 接線與自訂訊息通過；未跑 Linux UI App 全流程。 |

The Python fixture moves the SDK, builds downstream with Git discovery disabled,
installs/moves the App, hides the SDK/consumer build paths, and starts from an
unrelated working directory. A native Module and Python Module exchange custom
messages and RPC/Param updates, then shut down. Schema addition/removal/failure/
recovery, Python ABI refusal, corrupted Runtime metadata isolation, missing
deployment repair and Python-disable cleanup are checked. Installed static/native
CRT mixing and a missing static archive must fail configuration; no `/MD` fallback
is permitted. User Module sources and unowned files survive managed cleanup.

測試搬移 SDK 與 App、暫藏 SDK／使用端 build 路徑，從無關工作目錄執行；驗證自訂訊息、
RPC／Param、退出回收、schema 增刪與失敗復原、ABI／metadata 拒絕、補檔與停用清理。
混用 CRT 或缺少靜態庫時須配置失敗；使用者檔案與 Module 原始碼須保留。

Artifact inspection verified 20 Python static archives per SDK configuration,
with no producer source/build path in the exported targets. Their combined size
is 21.83 MiB in Release and 52.83 MiB in Debug. These archives are SDK build inputs,
not additional App deployment files. The custom-message fixture's Python extension
is 2.09 MiB in Release and 11.72 MiB in Debug; these are current artifact sizes,
not a matched `/MD` size comparison. Its imports contain only Python and reviewed
Windows system DLLs, while the central App retains dynamic MSVC CRT imports.

產物檢查確認每個 SDK 組態含 20 個 Python 靜態庫，匯出 target 不含製作者路徑。
上述靜態庫只供 SDK 建置使用，不隨每個 App 額外部署；Python 擴充不依賴動態 MSVC CRT，
中央仍維持原本的動態 CRT。大小是本輪產物實測，並非與 `/MD` 的同條件差額比較。

Evidence is in ignored `build/runtime-policy/stage3-*.log`,
`build/runtime-policy/stage3-audit.json` and each
`sdk-{release,debug}/sdk-python-test/*.log`. Native macOS, full Linux UI/Python App
integration and repeat Application Verifier qualification are not covered here.
The earlier [static-CRT experiment](../module_system/python/MT_VALIDATION.md) and
[source-binding validation](../module_system/python/README.md#runtime-policy-rollout)
remain separate evidence, not substitutes for this installed-package test.

本輪未驗證 macOS 實機、完整 Linux UI／Python App，也未重跑 Application Verifier。
先前靜態 CRT 實驗及來源版結果另存，不把它們當成本輪 installed SDK 驗證。

<a id="macos-clean-sdk-acceptance"></a>

### Clean macOS SDK acceptance (2026-11-02) / macOS 乾淨 SDK 交付驗收

Source: `50a88325245a02ce93d33f985e763284ed3f56bc` plus the uncommitted
`codex/notification-snapshot-mutex` repair. No production implementation changed
in this acceptance round. Environment: macOS 26.3.1(a), build 25D771280a, arm64;
Apple Clang 21.0.0 (`clang-2100.1.1.101`), Apple libc++ 21.1.6, SDK 26.5,
CMake 4.4.4, Ninja 1.13.2, GIL-enabled CPython 3.12.14, Release.

A fresh producer used `VINC_INSTALL_SDK=ON`, `VINC_BUILD_UI=ON`,
`VINC_BUILD_PYTHON=OFF`, `VINC_BUILD_APPS=OFF`, `VINC_BUILD_EXAMPLES=OFF`
and `BUILD_TESTING=OFF`. Configuration and the complete build passed. Its 92
compile commands contain no Python adapter sources or `_python_runtime`
variants. The downstream Apps build their own Python extension using a venv
whose path contains Chinese characters and a space.

使用全新 SDK-only producer，關閉 Python binding、App、examples 與測試，只在下游
才啟用 Python；不是沿用先前 Python-enabled producer 的產物。上述完整建置通過，
使用端以中文／空白路徑的 CPython venv 驗證。

| Check / 檢查 | Result / 結果 |
| --- | --- |
| `installed_consumer.cmake` | Passed: moved SDK, generated messages, native execution, ABI/version/header refusal and incremental rebuild. / 原生 C++ SDK 交付通過。 |
| `installed_python.cmake` | Passed (157.13 s): Custom Kconfig default without `--python`, moved SDK/App, hidden SDK/consumer build, unrelated cwd, custom Bus/RPC/Param, distinct central/native/Python PIDs, schema add/edit/remove/failure/recovery, ABI rejection, missing override, resource repair and Python-disable cleanup. / Python-OFF 製作者、下游啟用 Python 的完整流程通過。 |
| `installed_application.cmake` | Passed (90.00 s): single process executable inventory, self-host Modules, moved deployment, custom Bus/Bag/UI, embedded/plain startup, schema regeneration and rejection checks. / 單一執行檔交付通過；SDK 清單 1233 檔、App 清單 873 檔，各只有一個程序執行檔。 |
| Installed `multi_app.py` | Passed (142.16 s): three Apps, System/Custom sharing one venv, real Host/Python execution, concurrent isolation, relocation, official Module failure handling, bounded stalled-loader test, repair and selection cleanup. / 安裝式多 App 完整流程通過。 |
| Source `multi_app.py --source` | Passed (203.75 s): the same complete multi-App fixture through source integration, using the same Chinese/space-path venv. / 來源版多 App 同一套完整流程通過。 |
| Deployment interpreter probes | **6/6 passed:** source and installed Third Apps each use a different deployment venv from PATH, reject an empty Python PATH, and accept explicit `--python` despite that empty PATH. / 兩種來源各三項通過，證明 System 未記住建置時直譯器。 |

The extra probes install Third into a fresh directory, hide its consumer build,
and, for the installed case, also hide the SDK and producer build. They run from
an unrelated cwd and verify actual `sys.executable`, native `SELF_HOST_OK`, Python
message access and complete Module shutdown. Hidden directories are restored in
`finally`. The missing-Python case must report failure while central still completes
normal shutdown; the installed-Python fixture separately verifies a native peer
remains commandable after missing-Python and wrong-ABI failures.

補充探測移開 consumer build；安裝式案例另移開 SDK 與 producer build，直接核對
新部署 venv 的 `sys.executable`。正向案例驗證 Python 訊息存取與原生 self-host，
缺 Python 案例驗證錯誤與正常收尾。原生 peer 在 Python 啟動失敗後仍可接受命令的
證據由上表 `installed_python.cmake` 提供。

**Runtime audit:** downstream `_native.cpython-312-darwin.so` imports only
`/usr/lib/libc++.1.dylib` and `/usr/lib/libSystem.B.dylib`, with no `LC_RPATH`.
The 24 installed CMake package files contain no checkout/build path reference.
The extension's deployment minimum is 26.0. The
[confirmed macOS runtime policy](../common/README.md#macos-system-runtime-decision)
is unchanged.

**Evidence and limits:** ignored `build/macos-sdk-acceptance-20261102/` contains
`run_acceptance.py`, exact commands/timings in `acceptance-results.json`, fixture
logs, `probe_runtime_selection.py`, `runtime-selection-results.json`, runtime
audits and `validation.json` with 694 source/configuration hashes.
The initial sandboxed native consumer/source multi-App attempts failed with
`OpenFileLock: Operation not permitted`; native IPC-enabled reruns are recorded
separately, without weakening framework permissions. Existing fixture timeouts
were retained. ABI refusal modifies test metadata; missing-development and
version-floor selection checks use the documented test doubles. This round
does not by itself establish Debug SDK delivery; that coverage was added by the
[Debug follow-up](#macos-debug-sdk-acceptance). Intel Mac, older macOS, other CPython minors,
arbitrary third-party extension coexistence, interactive browser/editor behavior
and benchmark results remain outside these records.

原始沙箱失敗紀錄保留，不算產品通過證據；允許原生 IPC 後的結果另存，未修改正式
權限或放寬測試期限。這是上述 arm64／Release／CPython 3.12 的功能驗收，其他平台、
組態、版本及效能不由本輪結果推論。

<a id="macos-debug-sdk-acceptance"></a>

### Native macOS Debug SDK acceptance (2026-11-02) / macOS Debug SDK 驗收

Source: `599e7af11147a0c2e678799b7b31507e1c24d663`, branch
`codex/macos-debug-acceptance`. Same macOS 26.3.1(a) arm64, Apple Clang 21.0.0 /
libc++ 21.1.6, SDK 26.5, CMake 4.4.4, Ninja 1.13.2 and GIL-enabled CPython
3.12.14 environment as the Release record above; this round uses **Debug**
framework libraries, Plugins, Apps and Python bindings, not a Py_DEBUG interpreter.
No production code, test implementation or fixture deadline changed.

The Python-enabled full Debug build and all 126 suites have passing evidence
across the initial run and its one rerun; the sleep-overlapped initial failure
and exact counts are owned by the
[Python Debug record](../module_system/python/README.md#macos-full-debug-validation).
The separate clean producer below disables Python, Apps, examples and tests,
enables SDK/UI, and builds with `CMAKE_BUILD_TYPE=Debug`. Benchmarks are disabled.

| Clean producer / downstream check | Result |
| --- | --- |
| Producer configure / complete build | Passed, 2.33 s / 43.76 s. |
| `installed_consumer.cmake` | Passed, 9.45 s: native SDK relocation, execution, ABI/version/header rejection and rebuild. |
| `installed_python.cmake` | Passed, 112.95 s: downstream-only binding, Custom venv default, custom Bus/RPC/Param, schema changes, relocation, wrong ABI/missing Python isolation, repair and cleanup. |
| `installed_application.cmake` | Passed, 81.50 s: one executable per App, real self-host Modules, Bus/Bag/UI, moved package and generation/rejection checks. |
| Installed `multi_app.py` | Passed, 129.61 s: complete multi-App fixture with a Chinese/space-path venv. |
| Source `multi_app.py --source` | Passed, 175.59 s: same fixture through source integration. |
| Deployment Python probes | **6/6 passed**, 19.29 s total: source/installed Apps each use a different deployment PATH venv, reject missing Python and accept explicit overrides. |

Debug 完整建置／CTest 與乾淨 SDK-only producer 分開驗證。上表所有交付流程通過，
包含中文／空白 venv、搬移與暫藏 SDK／build 後執行，以及 Python／原生 Module 混合
通訊、故障隔離、補檔及停用清理。沿用既有 fixture 的完整斷言，沒有為通過而修改期限。

**Audit:** the clean producer has 92 compile commands, no Python adapter build
and no MSVC Python CRT variants. Its C++ commands contain `-g` without `NDEBUG`.
Both the full-build and clean-SDK downstream Debug extensions import only system
libc++/libSystem, with no `LC_RPATH`. All 24 installed CMake package files are free
of checkout/build path references. Source/configuration hashes remained unchanged;
temporarily hidden directories were restored and no task-path child processes
remained. Temporary sleep-prevention assertions were released after testing.

Evidence is under ignored `build/macos-debug-acceptance-20261102/`:
`run_sdk_debug.py` records the exact producer/fixture commands and existing
deadlines; `acceptance-results.json` records all eight successful stages;
`probe_runtime_selection.py`, `runtime-selection-results.json`, fixture logs,
`runtime-dependencies.log` and `validation.json` preserve deployment, dependency,
source and environment evidence. Reproduce the existing installed fixtures with
`CONFIG=Debug` and source/installed `multi_app.py` with `--config Debug`; on this
Mac, tests ran under temporary `caffeinate -i -s` protection after the first full
CTest run encountered idle sleep. No persistent power setting was changed.

This closes the full macOS Debug Python/App/SDK functional-coverage gap for this
arm64/CPython 3.12 environment. It does not establish Intel Mac, older macOS,
other Python minors, arbitrary third-party extension coexistence, interactive
GUI/curses/browser acceptance or performance. No benchmarks were run.
本輪已補齊此環境的完整 Debug 功能驗收；不把結果推論到其他平台、版本或效能。

<a id="runtime-final-acceptance"></a>

### Runtime final acceptance (2026-11-02) / Runtime 整合驗收

**Native macOS follow-up:** the notification snapshot repair subsequently enabled
the Python-enabled Release producer and installed SDK/App tests on macOS arm64.
Exact results and the Python services test-path correction are maintained in the
[Mac validation record](../module_system/python/README.md#macos-snapshot-repair-validation).
The separate Python-OFF producer has now passed the
[clean macOS SDK acceptance](#macos-clean-sdk-acceptance) above.
The subsequent [Debug SDK acceptance](#macos-debug-sdk-acceptance) also passed;
the [full Debug CTest record](../module_system/python/README.md#macos-full-debug-validation)
preserves the sleep-related initial failure and unchanged rerun.
The Windows/WSL matrix below remains its original evidence, not a rerun of the
mutex repair on those platforms.

This acceptance uses the formal root `build.py` entry for fresh Release builds
with Python, UI, Bag, Apps, examples and tests enabled. Windows uses MSVC
19.44.35229, CMake 4.3.2, Ninja 1.13.2 and unmodified official CPython 3.13.12 x64.
WSL2 uses Ubuntu 24.04.5 x86_64, GCC/libstdc++ 13.3.0, CMake 3.28.3, Ninja 1.11.1
and CPython 3.12.3. Both frontend builds use Node 24.19.0.

本輪由正式根目錄 `build.py` 全新建置 Release，啟用 Python、UI、Bag、Apps、範例與測試。
Windows 保留官方 Python 原附的 Runtime DLL；Linux 使用一般非 root 帳號與原生檔案系統，
沿用系統 Runtime，沒有套用 Windows `/MT`。Linux 來源快照核對了本輪 691 個非 Markdown
第一方檔案，與 Windows 驗收來源一致。

| Environment / 環境 | Result / 結果 |
| --- | --- |
| Windows and WSL2 / Windows 與 WSL2 | Fresh formal-entry builds passed. / 正式入口全新完整建置成功。 |
| Windows Release regression | **120/120** CTests passed, run as two disjoint groups (96 + 24) to serialize UI port use with Linux validation. Includes source Python, OSAL, Bus, Module, RPC, Shell, external SDK consumer, App lifecycle, fault isolation, UI and Bag. / 來源建置的全部 120 項回歸通過，分組只為避免 UI port 干擾。 |
| Windows build/setup tooling | `python -B -m unittest discover -s tools/tests -v`: 97 passed, 22 skipped (platform-specific or opt-in integration cases), no failures. / 建置與 setup 工具測試 97 項通過、22 項跳過，無失敗。 |
| Windows installed multi-App | `multi_app.py` passed: per-App Python selection, shared binding, relocation, concurrent distinct Apps, same-name rejection, startup faults, resource repair and managed cleanup. / 多 App 組裝、Python 選取、並行隔離及部署回歸通過。 |
| Windows installed System Modules | `system_apps.py` passed: real UI bind failure, restart rate limiting, recovery after releasing the port, isolated graphs/recording and orderly shutdown. / UI 真實綁定失敗、限流恢復、錄製與關閉隔離通過。 |
| WSL2 Release integration | **13/13** selected CTests passed (674.85 s): installed Python, multi-App, System Modules, Python lifecycle/messages/services/Shell/RPC and UI graph/topics/server/Module/Shell. / Linux 本輪整合測試全數通過，不代表重跑所有元件測試。 |

The Linux installed-Python fixture also passed custom-message schema changes,
invalid-schema recovery, ABI refusal, relocation, deployment repair and
Python-disable cleanup. It kept the existing per-command test bounds on the
native filesystem. Windows-only CRT metadata/import checks are covered by the
Windows stages, not counted as Linux coverage.

Linux 安裝式 Python 另通過自訂訊息 schema 變更、錯誤復原、ABI 拒絕、搬移、部署修復
與停用清理；原生檔案系統上的重驗沿用既有各指令期限。Windows 專用 CRT metadata／
匯入檢查由 Windows 階段提供證據，不列為 Linux 已驗項目。

The Windows installed tests use the stage-three SDK producer with Python disabled;
downstream Apps enable it. Tests exercise HTTP and real subprocesses, not an
interactive browser or debugger. Elapsed test times include fixture builds and
other concurrent validation work; they are not comparative build-cost benchmarks.
The earlier WinRT/OCR and Application Verifier experiments remain separate evidence.

Windows 安裝式測試沿用第三階段「製作者關閉 Python、使用端才啟用」的 SDK。
本輪驗證 HTTP 與真實子程序，沒有操作互動瀏覽器或 debugger；測試耗時包含建置 fixture
與同期驗證負載，不用來比較 `/MD`／`/MT` 效能。先前 WinRT／OCR 與 Application Verifier
實驗另存，不把先前結果描述為本輪重跑。

An initial Linux build succeeded under `/tmp`, but WSL service startup later
removed that temporary directory. Acceptance was rebuilt under a persistent
non-root home directory. This was lost test state, not an observed framework
failure. No production waits or existing test timeouts were relaxed for that retry.

首次 Linux 建置雖成功，但 WSL 服務啟動清除了 `/tmp` 測試目錄，因此改用持久的使用者
家目錄重新建置與驗收；這是測試狀態遺失，不是已證實的框架故障，也未因此放寬正式等待
或既有測試期限。

Evidence is under ignored `build/runtime-policy/final-*.log` and
`final-windows-*.json`. The subsequently completed
[native Mac acceptance](../osal/MACOS_EXIT_VALIDATION.md#current-acceptance-status)
retains each round's scope; the [root TODO](../../../TODO.md) tracks remaining
cross-platform checks. Passing this matrix does not promise arbitrary CPython minor ABI,
third-party native-library or cross-toolchain compatibility.

證據位於上述 Git 忽略的檔案。後續 macOS 實機驗證已完成，見上述驗收總結；
根目錄 TODO 保留跨平台補驗，既有 Mac 結果維持各輪原適用範圍。不因本輪通過就承諾任意 Python minor ABI、第三方原生庫
或不同工具鏈可以混用。

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
an interactive browser session. At that stage, native macOS validation of this packaging
path and `osal::executable_path()` was pending; subsequent evidence is in
[MACOS_VALIDATION.md](MACOS_VALIDATION.md). At this historical stage, low-level Manager source defaults and installed
Python were still limited; the current composition section and Python README describe
their subsequent updates. SDK artifacts were not published as a Release in this stage.

本機證據為 Git 忽略的 `build/sdk-stage2-*.log`。UI 本輪驗證 HTTP，未操作互動瀏覽器。
該歷史輪次的套件及 `osal::executable_path()` 尚待 Mac 原生驗證；後續證據見上述 Mac 報告。
該歷史輪次仍有底層 Manager source 預設與安裝式 Python 的限制；後續更新見目前組裝段落
及 Python README。本輪次沒有發布 SDK Release 產物。
