# Open-VINC

A C++23 framework for modular applications with isolated processes, shared-memory messaging, and a central control Shell. A reusable foundation for robotics, drones, games, automation, and other software.

以 C++23 開發的模組化應用框架，提供獨立程序、共享記憶體訊息交換與中央控制 Shell，作為機器人、無人機、遊戲、自動化及其他軟體的共同基礎。

Open-VINC prioritizes cross-platform operation, speed, low overhead, maintainability, and explicit lifecycle and failure contracts. It is under active development. This README describes the current source tree; use the documentation at your selected Git tag for a release.

Open-VINC 重視跨平台、速度、輕量、可維護性，以及明確的生命週期與失敗契約。目前持續開發中。本文件描述目前原始碼狀態，使用發行版時請閱讀對應 Git tag 的文件。

English and Traditional Chinese are paired below. Commands and code blocks are shared by both languages.

以下採英文與繁體中文上下對照，指令與程式碼共用。

## Contents / 目錄

- [Capabilities / 功能現況](#capabilities)
- [Architecture / 架構](#architecture)
- [Build and run / 建置與執行](#build-and-run)
- [Develop a Module / 開發 Module](#develop-a-module)
- [Messages and communication / 訊息與通訊](#messages-and-communication)
- [Operation and diagnostics / 操作與診斷](#operation-and-diagnostics)
- [Validation and limits / 驗證與限制](#validation-and-limits)
- [Repository and documentation / 目錄與文件](#repository-and-documentation)
- [License and attribution / 授權與署名](#license-and-attribution)

<a id="capabilities"></a>

## Capabilities / 功能現況

| Area / 項目 | Current behavior / 目前行為 |
| --- | --- |
| Module System | One process per instance, bidirectional heartbeat, shutdown supervision, restart policy, and debugger attachment support. / 每個實例獨立程序、雙向 heartbeat、關閉監督、重啟政策與 debugger 附加支援。 |
| C++ and Python | C++ Plugin libraries loaded by ModuleHost; optional system-Python Modules using native bindings. / C++ Plugin 動態庫由 ModuleHost 載入；可選用系統 Python 與原生綁定執行 Module。 |
| Context | FrameworkLoop or Manual mode; pump dispatches subscription, Param, RPC, and Shell callbacks. / FrameworkLoop 與 Manual 模式；pump 派送訂閱、Param、RPC 與 Shell callback。 |
| Messages | Ribosome compiles `.msg` definitions into typed classes with relocatable storage, strings, and nested arrays. / Ribosome 將 `.msg` 產生為具型別訊息類別，支援可重定位儲存、字串與巢狀陣列。 |
| Message Bus | Shared-memory publish/subscribe, multiple Publishers per topic, local push, shared loans, callbacks, and latest-message reads. / 共享記憶體發布訂閱、同 topic 多發布者、本地 push、共享 loan、callback 與最新訊息讀取。 |
| RPC | Asynchronous typed request/response, bounded shared slots, queue-full reporting, and pump dispatch. / 非同步具型別 request／response、有界共享槽位、滿載回報與 pump 派送。 |
| Param | Typed registration, watched local caches, permissions, pump callbacks, protected A/B snapshots, and TOML persistence. / 具型別註冊、監聽快取、權限、pump callback、受保護的 A/B 快照與 TOML 保存。 |
| Shell and scripts | Console, Raw TCP Socket Shell, Tcl scripts, cancellation, and embedded startup scripts. / 互動終端、Raw TCP Socket Shell、Tcl 腳本、中斷與啟動腳本打包。 |
| Logging | Central capture of stdout/stderr and structured Error/Warn/Info/Debug records with time and Module identity. / 中央擷取 stdout／stderr，保存含時間與 Module 身分的 Error／Warn／Info／Debug 結構化紀錄。 |
| Bag Recorder | SQLite recording of selected or all discovered topics, bounded queues, and loss statistics. Payload recording requires compiled-in types. Player is not implemented. / 以 SQLite 錄製指定或探索到的全部 topic，具備有界佇列與損失統計；payload 限建置時已知型別，Player 尚未實作。 |
| Web UI | Optional-to-start Module, plugin workspace, manual-refresh Module Graph, and read-only Topic Monitor with Hz, collision/loss status, and latest-message previews. / 預設不啟動的官方 Module、plugin 工作區、手動刷新 Module Graph，以及含 Hz、碰撞／漏資料狀態與最新訊息預覽的唯讀 Topic Monitor。 |

<a id="architecture"></a>

## Architecture / 架構

```text
Central Vinc / 中央
  ModuleManager + Shell/Tcl + Param + RPC + BusManager + Log
       |
       | Socket: identity, lifecycle, heartbeat, commands, control metadata
       | Socket：身分、生命週期、heartbeat、命令與控制資訊
       |
       +---- ModuleHost process ---- C++ Plugin + Context
       +---- system Python process ---- Python Module + Context

  Module <====== shared memory / 共享記憶體 ======> Module
                 Message Bus / RPC payloads
```

A C++ Module is a Plugin library, not a standalone executable. Vinc starts a separate ModuleHost for each instance; the Host loads the Plugin and owns its process-level control services. Python Modules run in separate system-Python processes. Authors use the Context inherited from BaseModule instead of implementing sockets or heartbeat.

C++ Module 是 Plugin 動態庫，不是獨立執行檔。Vinc 為每個實例啟動獨立 ModuleHost，由 Host 載入 Plugin 並持有程序級控制服務；Python Module 則在獨立的系統 Python 程序執行。開發者使用 BaseModule 繼承的 Context，不必自行實作 Socket 或 heartbeat。

Sockets carry control information. Message Bus payloads use one shared RingBuffer per Publisher; RPC payloads use fixed shared slots. Subscription delivery copies a validated message into the receiving process before invoking user callbacks. A Publisher loan lets users fill shared storage directly, then commit it.

Socket 傳控制資訊。Message Bus 每個 Publisher 各自使用共享 RingBuffer；RPC payload 使用固定共享槽位。訂閱交付先將已驗證訊息複製到接收程序，再呼叫使用者 callback；Publisher loan 則可直接填共享空間，完成後 commit。

OSAL provides general-purpose Process, Socket, Pipe, SharedMemory, SharedWord, FileLock, and path conversion facilities. Pipe is a separate byte-stream tool; Module control uses Socket. OSAL can be used independently with the standard-library-only common component.

OSAL 提供通用 Process、Socket、Pipe、SharedMemory、SharedWord、FileLock 與路徑轉換等能力。Pipe 是獨立的位元組串流工具，Module 控制使用 Socket。OSAL 可搭配只依賴標準函式庫的 common 獨立使用。

See [Module System](src/vinc/module_system/README.md), [Message Bus](src/vinc/message_bus/README.md), and [OSAL](src/vinc/osal/README.md) for ownership and failure contracts.

所有權與失敗契約詳見上述元件文件。

<a id="build-and-run"></a>

## Build and run / 建置與執行

### Requirements / 工具需求

- **C++23 compiler and standard library**, **CMake 3.20+**, and **Ninja**. C++23 is required, with language extensions disabled; newer toolchains may need newer CMake. / **支援所用 C++23 功能的編譯器與標準庫**、**CMake 3.20 以上**及 **Ninja**；要求 C++23 且關閉語言擴充，較新工具鏈可能需要較新 CMake。
- **Windows:** an x64 MSVC developer environment with NMAKE for Tcl. Keep build-directory paths free of spaces due to a known Tcl integration limitation. / **Windows：**使用含 NMAKE 的 x64 MSVC 開發環境；目前 Tcl 整合有已知限制，建置目錄路徑須避免空白。
- **Linux/macOS:** a suitable C++ toolchain, make, and zlib development files for Tcl. Ubuntu 22.04 is the minimum Linux target; its default GCC 11 is not the C++23 baseline. / **Linux／macOS：**適用 C++ 工具鏈、make 與 Tcl 所需 zlib 開發檔。Linux 最低目標為 Ubuntu 22.04，不以預設 GCC 11 為 C++23 基準。
- **Default Web UI build:** Node.js and npm; local validation records use Node.js 24.19.0. These are build-time tools, not browser/runtime requirements. / **預設 Web UI 建置：**需要 Node.js 與 npm，本機驗證紀錄使用 Node.js 24.19.0；部署後的瀏覽器與執行環境不需要 Node.js。
- **Optional Python SDK:** compatible 64-bit, GIL-enabled CPython and Development.Module files. The deployment environment supplies Python. / **選用 Python SDK：**需要相容的 64 位元、含 GIL 的 CPython 與 Development.Module 開發檔，部署環境自行提供 Python。

See [Tcl integration](library/tcl/README.md), [Python SDK](src/vinc/module_system/python/README.md), and [third-party dependencies](library/README.md) for detailed requirements.

詳細需求與部署限制見上述文件。

### Prepare dependencies / 準備相依環境

Run `tools\setup.bat` on Windows, or `bash tools/setup.sh` on Ubuntu/macOS.
These scripts install/check tools, restore pinned submodules and install UI npm
dependencies. They **do not configure or build your project**. UI and optional
Python SDK prerequisites are included by default; use `--no-ui` or `--no-python`
to omit them, or `--check` to inspect without installing. Standalone temporary
compiler probes check toolchain capabilities. See [setup instructions](tools/README.md)
for platform requirements, environment selection and validation limits.

Windows 執行 `tools\setup.bat`，Ubuntu／macOS 執行 `bash tools/setup.sh`。
腳本安裝／檢查工具、還原固定 submodule 並安裝 UI npm 套件，**不配置或編譯使用者專案**。
預設包含 UI 與選用 Python SDK 的相依，可用 `--no-ui`／`--no-python` 略過；
`--check` 僅檢查、不安裝。工具鏈能力由獨立暫存小程式驗證。
平台需求、環境選擇與驗證限制見 [setup 說明](tools/README.md)。

### Configure, build, test / 配置、建置、測試

Run from the repository root in a configured compiler environment. Restore pinned submodules first; CMake does not install npm packages automatically.

在已配置編譯器的環境中，從儲存庫根目錄執行。先還原固定版本的 submodule；CMake 不會自動安裝 npm 套件。

```sh
git submodule update --init --recursive
npm ci --prefix src/vinc/ui/web
cmake -S src -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Use a separate directory with `-DCMAKE_BUILD_TYPE=Release` for Release. Tcl currently requires a single-configuration generator. These commands run tests on your machine; they do not imply that every platform has passed the same suite.

Release 請另用建置目錄並指定 `-DCMAKE_BUILD_TYPE=Release`。目前 Tcl 整合要求單一組態產生器。上述指令是在你的環境執行測試，不表示所有平台均已通過相同套件。

| CMake option / 選項 | Default / 預設 | Purpose / 用途 |
| --- | --- | --- |
| `VINC_BUILD_APPS` | `ON` | Build the central Vinc application. / 建置中央 Vinc 應用。 |
| `VINC_BUILD_EXAMPLES` | `ON` | Build and register bundled example Modules. / 建置並登記內建範例 Module。 |
| `VINC_BUILD_UI` | `ON` | Build the official Web UI when apps are enabled; does not auto-start it. / 啟用 apps 時建置官方 Web UI，但不自動啟動。 |
| `VINC_BUILD_PYTHON` | `OFF` | Build native bindings and bootstrap resources, without bundling Python. / 建置 Python 原生綁定與啟動資源，不附帶直譯器。 |
| `BUILD_TESTING` | `ON` | Configure CTest tests. / 配置 CTest 測試。 |

To build without the UI, add `-DVINC_BUILD_UI=OFF` and omit `npm ci`. SDK-only consumers with `VINC_BUILD_APPS=OFF` also do not require Node.js. Native Plugins and Hosts must use matching toolchains, standard libraries, build configurations, layout settings, and Windows CRT.

不需要 UI 時加入 `-DVINC_BUILD_UI=OFF`，可省略 `npm ci`。設定 `VINC_BUILD_APPS=OFF` 的純 SDK 使用端也不需要 Node.js。原生 Plugin 與 Host 必須使用相同工具鏈、標準庫、建置組態、布局設定及 Windows CRT。

### First run / 第一次執行

Start Vinc from the repository root:

從儲存庫根目錄啟動 Vinc：

```powershell
# Windows PowerShell
.\build\debug\Vinc.exe
```

```sh
# Linux / macOS
./build/debug/Vinc
```

Enter the following in the **Vinc Shell**, not the OS terminal. `exampleModule` requires examples enabled; `webUi` requires the UI build.

在 **Vinc Shell** 輸入下列命令，不是在作業系統終端。`exampleModule` 需啟用範例建置，`webUi` 需啟用 UI 建置。

```text
help
exampleModule
exampleNode status
webUi
ui status
```

Open `http://127.0.0.1:8080/`, then select **Topics → Topic Monitor** or **Module Graph → Module Graph**. `webUi` is built by default but starts only when requested. In the local Vinc Shell, `exit` or Ctrl+C requests complete shutdown and waits for managed processes.

開啟 `http://127.0.0.1:8080/`，選擇 **Topics → Topic Monitor** 或 **Module Graph → Module Graph**。`webUi` 預設建置，需明確啟動才會執行。在本機 Vinc Shell 使用 `exit` 或 Ctrl+C，要求完整關閉並等待受管理程序結束。

<a id="develop-a-module"></a>

## Develop a Module / 開發 Module

Start with the runnable [C++/Python API examples](examples/userExampleModule/README.md): lifecycle and command hooks, publish/subscribe, message containers, Param, Log, and both execution modes. Typed RPC has a separate [example](examples/rpc/README.md).

建議從可執行的 [C++／Python API 對照範例](examples/userExampleModule/README.md) 開始，涵蓋生命週期與命令 hook、發布訂閱、訊息容器、Param、Log 與兩種執行模式；具型別 RPC 另有[範例](examples/rpc/README.md)。

### C++

Use `<vinc/vinc.hpp>` and inherit `BaseModule`, explicitly choosing `RunMode::FrameworkLoop` or `RunMode::Manual`. FrameworkLoop runs the pump for you; Manual implements its own `main()` and decides when to pump. Heartbeat runs separately from user callbacks.

使用 `<vinc/vinc.hpp>` 並繼承 `BaseModule`，明確選擇 `RunMode::FrameworkLoop` 或 `RunMode::Manual`。FrameworkLoop 由框架執行 pump；Manual 實作自己的 `main()` 並決定何時 pump。Heartbeat 與使用者 callback 分開執行。

After adding Open-VINC to a parent CMake project, register message directories and Plugins with the public functions:

在父 CMake 專案加入 Open-VINC 後，使用公開函式登記訊息目錄與 Plugin：

```cmake
add_subdirectory(path/to/open-vinc/src open-vinc-build)

add_message_dir("${CMAKE_CURRENT_SOURCE_DIR}/messages")
add_module(camera_module
    MODULE_TYPE_NAME camera
    SOURCES camera.cpp camera_driver.cpp
)
```

`add_module()` creates a Plugin and links `vinc::sdk`/`vinc::messages`. `MODULE_TYPE_NAME` must match the C++ `REGISTER_MODULE` declaration. The Module source chooses its instance name; complete names must be unique, without automatic numeric suffixes. Other targets explicitly link `vinc::sdk` and, when consuming messages, `vinc::messages`.

`add_module()` 建立 Plugin 並連結 `vinc::sdk`／`vinc::messages`。`MODULE_TYPE_NAME` 須與 C++ `REGISTER_MODULE` 宣告一致。實例名稱由 Module 原始碼決定，完整名稱不得重複，不會自動加數字尾碼。其他 target 若使用 SDK 或生成訊息，分別明確連結 `vinc::sdk` 與 `vinc::messages`。

The umbrella header does not include generated messages. Include `<vinc_messages/Name.hpp>` or `<vinc_messages/all_messages.hpp>` separately. See the [SDK guide](src/vinc/sdk/README.md) for dependency, source-consumer, and CMake function contracts.

整合頭檔不包含生成訊息，需另外 include `<vinc_messages/Name.hpp>` 或 `<vinc_messages/all_messages.hpp>`。相依、原始碼整合與 CMake 函式契約詳見 [SDK 指南](src/vinc/sdk/README.md)。

### Python

Build with `-DVINC_BUILD_PYTHON=ON` and select the build interpreter with `-DPython_EXECUTABLE=/absolute/path/to/python3`. Vinc does not embed Python or install packages. At runtime, it selects Python from PATH unless overridden with `Vinc --python PATH`.

建置時加入 `-DVINC_BUILD_PYTHON=ON`，並以 `-DPython_EXECUTABLE=/absolute/path/to/python3` 指定建置用直譯器。Vinc 不嵌入 Python、不安裝套件；執行時預設從 PATH 選取 Python，可用 `Vinc --python PATH` 覆寫。

Place Modules beside Vinc as `data/module/<ModuleType>/<ModuleType>.py`. Vinc scans once at startup; use the ModuleType name in the same Shell as C++ Modules. Python ABI must match the native extension. On Windows, MSVC Runtime dependencies are checked before loading it. See the [Python guide](src/vinc/module_system/python/README.md).

將 Module 放在 Vinc 旁的 `data/module/<ModuleType>/<ModuleType>.py`。中央啟動時掃描一次，在同一個 Shell 以 ModuleType 名稱啟動，與 C++ Module 操作一致。Python ABI 必須符合原生擴充；Windows 會在載入前檢查所需 MSVC Runtime。詳見 [Python 指南](src/vinc/module_system/python/README.md)。

<a id="messages-and-communication"></a>

## Messages and communication / 訊息與通訊

Ribosome generates typed classes from [`.msg` definitions](vinc_messages/README.md). `add_message_dir()` collects additional directories at build configuration; generation finishes before consumers compile. New types require rebuilding their consumers, including Recorder and Python bindings when used. Runtime loading of unknown message definitions is not implemented.

Ribosome 從 [`.msg` 定義](vinc_messages/README.md) 產生具型別訊息類別。`add_message_dir()` 在建置配置時收集額外目錄，生成步驟先於使用端編譯。新增型別需重建使用端，包含有使用到的 Recorder 與 Python 綁定；尚未實作執行期載入未知訊息定義。

Create local messages with `vinc::make_message<MessageType>()` and publish through Context. Local push copies into bounded shared storage; loan/commit fills it directly. Messages support MessageArray fields and nested arrays. Recoverable capacity failures return Result; invalid contract-based `[]` access is a programmer error.

使用 `vinc::make_message<MessageType>()` 建立本地訊息，透過 Context 發布。本地 push 會複製到有界共享空間，loan／commit 則直接填共享空間。訊息支援 MessageArray 與巢狀陣列。可恢復的容量失敗使用 Result，違反 `[]` 存取契約則屬於 programmer error。

Each Publisher chooses its slot count and per-message byte capacity. Slow Subscribers can miss overwritten messages; loss is reported. A Writer waiting for a protected Reader copy may wait indefinitely, recording collisions and waiting time. Heartbeat timeout alone does not authorize reclaiming memory still held by a live process.

每個 Publisher 設定槽位數與單筆 byte 容量。慢速 Subscriber 可能漏掉已覆寫訊息，框架會回報損失。Writer 等待受保護的 Reader 複製時允許無限等待，並記錄碰撞與等待時間；僅有 heartbeat timeout，不足以回收仍由存活程序持有的共享資源。

The framework stamps publications with publisher identity, Unix nanoseconds for calendar tracing, and a separate same-host monotonic timestamp for Bus ordering. `vinc::format_utc()` and Python `vinc.format_utc()` format the calendar timestamp. Topic names may use `/` but not `.`; Param names use `module.param` and cannot contain `/`.

框架替發布資料填入發布者身分、供日期追查的 Unix 奈秒時間，以及供 Bus 排序的同主機單調時間。可用 `vinc::format_utc()` 或 Python `vinc.format_utc()` 顯示 UTC。Topic 名稱可用 `/`，不能用 `.`；Param 使用 `module.param`，不能含 `/`。

See [Ribosome](src/vinc/ribosome/README.md), [Storage](src/vinc/components/message_storage/README.md), [Message Bus](src/vinc/message_bus/README.md), [Bus design](src/vinc/message_bus/DESIGN.md), and [RPC](src/vinc/rpc/README.md) for schema, ordering, delivery, and ownership contracts. Generator failure/recovery and isolated regression instructions are in the Ribosome guide.

Schema、排序、交付、所有權，以及生成器失敗／復原與獨立回歸測試方式，詳見上述元件文件。

<a id="operation-and-diagnostics"></a>

## Operation and diagnostics / 操作與診斷

### Shell and startup / Shell 與啟動

Vinc initializes Tcl and starts Socket Shell automatically at `127.0.0.1:5500` by default. PuTTY can connect in Raw mode. There is no authentication or encryption. Use `--shell-ip`/`--shell-port` for configuration; see [Socket Shell](src/vinc/shell/SOCKET.md) before exposing it beyond localhost.

Vinc 自動初始化 Tcl 並啟動 Socket Shell，預設為 `127.0.0.1:5500`，可用 PuTTY Raw 模式連入，目前沒有登入驗證或加密。以 `--shell-ip`／`--shell-port` 調整位址與埠；對 localhost 以外開放前，請閱讀 [Socket Shell 契約](src/vinc/shell/SOCKET.md)。

Use `Vinc -S startup.tcl` for startup scripts, `srun file.tcl` within Shell, and Ctrl+D to cancel the foreground script. `vinc_configure_app(NAME MyApp STARTUP_SCRIPT startup.tcl)` embeds an application's default startup script; those applications reject `-S`. See [Shell](src/vinc/shell/README.md) and [application packaging](apps/vinc/README.md).

使用 `Vinc -S startup.tcl` 執行啟動腳本，Shell 內用 `srun file.tcl` 執行其他腳本，Ctrl+D 中斷前景腳本。`vinc_configure_app(NAME MyApp STARTUP_SCRIPT startup.tcl)` 可內嵌應用預設啟動腳本，此類應用不接受 `-S`。詳見 [Shell](src/vinc/shell/README.md) 與[應用打包](apps/vinc/README.md)。

### Param and Log / 參數與紀錄

Modules register and watch typed parameters through `context.param`; pump updates local caches and callbacks. Shell provides `param list/get/set/save/status`. Saved values and registration defaults live in `vinc_data/<VINC_NAME>/params.toml` beside Vinc. Live changes require explicit save. Existing `vinc::config` settings have not all moved to Param.

Module 透過 `context.param` 註冊與監聽具型別參數，由 pump 更新本地快取與 callback。Shell 提供 `param list/get/set/save/status`。保存值與註冊預設值位於 Vinc 旁的 `vinc_data/<VINC_NAME>/params.toml`，即時調整須明確 save 才保存。既有 `vinc::config` 設定並未全部搬到 Param。

Use `context.log.error/warn/info/debug` for structured records. Existing `printf`/stdout/stderr are centrally captured without structured severity. Logs default to `vinc_data/<VINC_NAME>/logs/<date>/`, 16 MiB per file and 128 MiB total. `log status` shows diagnostics; `log show on` enables structured display in the current Shell. Bounded queues can lose records under overload; accepted writes do not guarantee persistence.

使用 `context.log.error/warn/info/debug` 寫結構化紀錄；既有 `printf`／stdout／stderr 也由中央擷取，但沒有結構化等級。預設路徑為 `vinc_data/<VINC_NAME>/logs/<日期>/`，單檔 16 MiB、總量 128 MiB。`log status` 查看狀態，`log show on` 開啟目前 Shell 的結構化顯示。佇列有上限，過載可能遺失紀錄；接受寫入不代表保證持久保存。

See [Param](src/vinc/param/README.md), [Log](src/vinc/log/README.md), and [OutputHub](src/vinc/output_hub/README.md) for permissions, persistence, and output contracts.

權限、保存與輸出契約詳見上述文件。

### Topic Monitor and recording / Topic 監看與錄製

The [Topic Monitor](src/vinc/ui/README.md) shows publication Hz, Publisher/Subscriber counts, collisions, lost messages, and bounded previews of compiled-in types. Unknown types still show schema and statistics. Green means no recent observed issue; yellow means collision/loss or recovery; red means prolonged Writer waiting or a preview error; gray means unavailable/stale data. These are not general application-health guarantees. Previewing creates a real Subscriber and can affect contention.

[Topic Monitor](src/vinc/ui/README.md) 顯示發布 Hz、Publisher／Subscriber 數、碰撞、漏資料與建置時已知型別的有界預覽；未知型別仍可看 schema 與統計。綠燈表示近期未觀察到問題，黃燈表示碰撞／漏資料或恢復觀察，紅燈表示 Writer 長時間等待或預覽錯誤，灰燈表示資料不可用／過期。燈號不代表所有應用功能健康；預覽會建立真正的 Subscriber，也可能增加競爭。

Use `bagRecorder record session01 --all`, then `recorder status` and `recorder finish` in Shell. The default main SQLite database limit is 16 GiB; transaction temporary files take additional space. Capacity exhaustion or disk failure stops recording and reports the cause without shutting down Vinc. Recording can drop data when it cannot keep up. Player is not implemented. See [Bag Recorder](src/vinc/bag/README.md).

在 Shell 使用 `bagRecorder record session01 --all` 開始錄製，以 `recorder status` 與 `recorder finish` 查看及結束。主 SQLite 資料庫預設上限為 16 GiB，交易暫存檔另占空間。達上限或磁碟失敗會停止錄製並回報原因，不關閉中央。跟不上時可能丟資料；Player 尚未實作。詳見 [Bag Recorder](src/vinc/bag/README.md)。

<a id="validation-and-limits"></a>

## Validation and limits / 驗證與限制

Windows, Linux, and macOS are target platforms with OS-specific backends. Verification varies by component and revision. Windows x64/MSVC has extensive Debug/Release records; do not infer identical Linux/macOS or compiler coverage. Component READMEs record environments, test scope, results, and remaining issues. Linux deployment also requires glibc/libstdc++ compatibility checks.

Windows、Linux、macOS 都是目標平台並有各自後端，驗證範圍依元件與版本而異。Windows x64／MSVC 有較完整的 Debug／Release 紀錄，不能推論 Linux／macOS 或各編譯器均有相同覆蓋。各元件 README 記錄環境、測試範圍、結果與待辦；Linux 部署還需核對 glibc／libstdc++ 相容性。

For example, the [Topic Monitor validation recorded on 2026-10-25](src/vinc/ui/README.md#topic-monitor-實際驗證) covers 12 related CTests in each Windows Debug/Release build, frontend type checking, and 5 Chromium tests. This is not full-project or cross-platform certification.

例如上述 2026-10-25 的 Topic Monitor 驗證包含 Windows Debug／Release 各 12 項相關 CTest、前端型別檢查與 5 項 Chromium 測試，不是全專案或跨平台認證。

<a id="windows-validation"></a>

**Validated Windows environment (2026-10-27) / Windows 已驗證環境（2026-10-27）**

| Item / 項目 | Validated version / 已驗證版本 |
| --- | --- |
| OS / 作業系統 | Windows, build **26200**, x64 |
| Compiler and standard library / 編譯器與標準函式庫 | MSVC **19.51.36257.0**, toolset / 工具集 **14.51.36231** |
| Windows SDK | **10.0.26100.0** |
| Build tools / 建置工具 | CMake **4.3.2**, Ninja **1.13.2** |
| Configuration / 組態 | Debug and Release; apps, examples, Python and Web UI enabled / 啟用 apps、examples、Python 與 Web UI |
| Source / 原始碼 | Commit **`ab236857480bdf85e81129361f517fc72fcd6935`** |

Full Debug and Release project builds succeeded. The selected OSAL, OutputHub and Log suites passed **23/23 in each configuration** (11.76 s / 8.72 s). Coverage includes Process, Pipe, submission-error wrapping, four Pipe contract cases, Socket, SharedMemory, FileLock, OutputHub integration/faults, and Log unit/process/transport. The deterministic `osal.pipe_attach_failure` test also passed separately in both configurations; it does not establish actual Windows IOCP failure injection. These were selected regression suites, not full-project CTest runs. Local logs: `build/windows-macos-followup-debug-ctest.log` and `build/windows-macos-followup-release-ctest.log`.

Debug／Release 全專案建置成功；選定的 OSAL、OutputHub 與 Log 測試兩種組態各 **23/23 通過**（11.76／8.72 秒）。範圍包含 Process、Pipe、提交錯誤包裝、四個 Pipe 契約案例、Socket、SharedMemory、FileLock、OutputHub 整合／故障，以及 Log 單元／程序／傳輸。確定性 `osal.pipe_attach_failure` 亦於兩種組態分別重跑通過，但不代表已注入 Windows IOCP 的真實失敗。這是選定套件的回歸，並非全專案 CTest。上述本機 log 保存完整結果。

<a id="linux-validation"></a>

**Validated Linux environment (2026-10-27) / Linux 已驗證環境（2026-10-27）**

| Item / 項目 | Validated version / 已驗證版本 |
| --- | --- |
| Distribution / 發行版 | Ubuntu **24.04.5 LTS**, WSL 2, x86_64 |
| Kernel / 核心 | **6.18.54.1-microsoft-standard-WSL2** |
| Compiler / 編譯器 | GCC / G++ **13.3.0** (`13.3.0-6ubuntu2~24.04.1`) |
| C library / C 函式庫 | glibc **2.39** |
| C++ standard library / C++ 標準函式庫 | libstdc++ headers / 標頭 **13.3.0**; runtime package / 執行時套件 **14.2.0-4ubuntu2~24.04.1** |
| Build tools / 建置工具 | CMake **3.28.3**, Ninja **1.11.1** |
| Configuration / 組態 | Standalone OSAL Debug, non-root user, build directory on the WSL Linux filesystem / 獨立 OSAL Debug、非 root 帳號，建置目錄位於 WSL Linux 檔案系統 |
| Source / 原始碼 | Commit **`ab236857480bdf85e81129361f517fc72fcd6935`** plus the descriptor test fix / 加上 descriptor 測試修正 |

The OSAL library and four selected test targets built successfully; **`osal.descriptor`, `osal.process`, `osal.shared_memory` and `osal.file_lock` passed 4/4**. Direct descriptor execution, an inherited-fd-3 launcher, and 20 consecutive descriptor CTest runs passed. `strace` verified real low-fd acquisition, `EMFILE` during relocation, cleanup of new names, and preservation of existing names after failed open. Five deliberately broken variants in isolated source copies were detected, and the restored copy passed. See the [descriptor findings](src/vinc/osal/README.md#待處理事項) for the original CTest fixture defect and negative controls. Logs and traces are under `build/wsl-ubuntu2404-descriptor-review/`.

OSAL library 與四個選定測試目標建置成功；**`osal.descriptor`、`osal.process`、`osal.shared_memory`、`osal.file_lock` 共 4/4 通過**。descriptor 直接執行、繼承 fd 3 的啟動方式，以及連續 20 次 CTest 均通過。`strace` 確認真正的低 fd 建立、搬移時 `EMFILE`、新建名稱回收與 open 失敗保留既有名稱；隔離副本中五個刻意破壞的版本均被攔下，還原後通過。原始 CTest fixture 問題與負向對照詳見 [descriptor 紀錄](src/vinc/osal/README.md#待處理事項)，本機 log 與 trace 保存於上述目錄。

This Linux report covers the stated OSAL paths. It does not establish a full-project Linux build, Release results, Ubuntu 22.04 deployment compatibility, or resolution of the separately deferred fd creation/spawn race. Earlier Ubuntu 22.04 / GCC 12.3 execution evidence was reported separately; it is not counted as part of this four-suite WSL run.

此 Linux 報告涵蓋上述 OSAL 路徑；全專案 Linux 建置、Release、Ubuntu 22.04 部署相容性與另列的 fd 建立／spawn 競爭問題，不在這四套 WSL 結果的範圍內。先前另有 Ubuntu 22.04／GCC 12.3 的執行驗證回報，不併入本輪測試數。

<a id="macos-validation"></a>

**Validated macOS environment (2026-10-27) / macOS 已驗證環境（2026-10-27）**

| Item / 項目 | Validated version / 已驗證版本 |
| --- | --- |
| macOS | **26.3.1(a)**, OS build `25D771280a` |
| Architecture / 架構 | Apple Silicon / `arm64` |
| Compiler / 編譯器 | Apple Clang **21.0.0** (`clang-2100.1.1.101`) |
| Standard library / 標準函式庫 | libc++ **21.1.6** (`_LIBCPP_VERSION=210106`) |
| macOS SDK | **26.5** |
| Build tools / 建置工具 | CMake **4.4.4**, Ninja **1.13.2** (`1.13.2.git.kitware.jobserver-pipe-1`) |
| Web UI build tools / Web UI 建置工具 | Node.js **24.22.0**, npm **11.19.1** |
| Configuration / 組態 | Debug; apps, examples and Web UI enabled; Python disabled / 啟用 apps、examples、Web UI；停用 Python |
| Source / 原始碼 | Base commit `13204c20a361` plus local macOS fixes and Pipe contract tests / 基礎 commit 加上本機 macOS 修正與 Pipe 契約測試 |

This records a tested configuration, not a minimum macOS deployment version or a released version of Open-VINC. Minimum macOS compatibility and Intel/x86_64 support have not been established by this run.

此表記錄實際通過驗證的組合；macOS 最低部署版本仍未確立，也不代表已發布的 Open-VINC 版本。本次未驗證 Intel／x86_64 Mac。

The initial clean default Debug build including the Web UI succeeded. All 82 CTests passed across the full run (79 passed) and the corrected Socket/Log test rerun (3 passed). Frontend type checking also passed. Manual Vinc execution verified example Module commands, Web UI HTML/JS/CSS HTTP 200 responses, enabled file logging, and complete shutdown. Logs are under `build/macos-debug-fixed-*.log`.

初次乾淨的預設 Debug 建置（含 Web UI）成功；完整 CTest 首輪通過 79 項，修正 Socket／Log 測試情境後重跑其餘 3 項通過，合計 82 項均通過。前端型別檢查亦通過。另實際啟動 Vinc，確認範例 Module 命令、Web UI HTML／JS／CSS 的 HTTP 200 回應、檔案 Log 啟用及完整關閉。紀錄位於 `build/macos-debug-fixed-*.log`。

After the Pipe submission-error wrapping and contract-test additions, the full Debug project rebuilt successfully and **24/24 OSAL and OutputHub CTests passed**, including 9 new tests. This follow-up did not rerun the full project suite; do not add the two runs' counts together. [Pipe validation details](src/vinc/osal/PIPE.md#斷管與提交失敗契約測試2026-10-27) record the scope and negative controls. Local logs: `build/macos-pipe-contract-final-build.log` and `build/macos-pipe-contract-final-ctest.log`.

補齊 Pipe 提交錯誤包裝與契約測試後，完整 Debug 專案再次建置成功，**OSAL 與 OutputHub 的 24/24 項 CTest 通過**，包含新增 9 項測試。此後續回歸沒有重跑全專案套件，兩輪測試數不可直接相加；範圍與反向驗證詳見 [Pipe 驗證紀錄](src/vinc/osal/PIPE.md#斷管與提交失敗契約測試2026-10-27)。本機紀錄為 `build/macos-pipe-contract-final-build.log` 與 `build/macos-pipe-contract-final-ctest.log`。

Release, optional Python bindings, browser automation, and other operating systems were not tested in these macOS runs. Use a fresh build directory when upgrading from the original `vinc/` binary-tree layout, which collided with `Vinc` on case-insensitive filesystems; intermediate framework artifacts now live under `framework/`.

上述 macOS 驗證未涵蓋 Release、選用 Python 綁定、瀏覽器自動化或其他 OS。原本 `vinc/` 中間產物目錄與 `Vinc` 在不區分大小寫的檔案系統衝突，現改放 `framework/`；從舊布局升級時請使用新的建置目錄。

- **ABI and deployment:** native Plugins require matching toolchain, standard library, configuration, layout, and Windows CRT. There is no installed `find_package(VINC)` SDK or complete relocatable Plugin/UI package yet. / **ABI 與部署：**原生 Plugin 須符合工具鏈、標準庫、組態、布局及 Windows CRT 契約；尚無安裝式 `find_package(VINC)` SDK 或完整可重定位的 Plugin／UI 部署包。
- **UI scope:** Read-only Topic Monitor and a manually refreshed Module Graph are available. Module control, Param editing, Log panels, 3D panels, and browser management RPC are not wired. Trusted plugins share page privileges, not a sandbox. / **UI 範圍：**已提供唯讀 Topic Monitor 與手動刷新的唯讀 Module Graph；Module 控制、Param 編輯、Log 面板、3D 面板與瀏覽器管理 RPC 尚未接線。受信任 plugin 共用頁面權限，不是隔離沙箱。
- **Future capabilities:** native Plugin hot discovery/loading, dynamic message schemas, Bag Player, and framework-to-framework multi-machine transport are not implemented. / **未來能力：**尚未實作原生 Plugin 熱探索／載入、動態訊息 schema、Bag Player 與框架對框架的多機傳輸。
- **Failure isolation:** process boundaries limit crash propagation, but do not promise security sandboxing or hard real-time behavior. Heartbeat detects only limited liveness; resource bounds and shared-memory recovery remain explicit contracts. / **故障隔離：**程序邊界可限制 crash 連帶影響，不等於安全沙箱或硬即時保證；heartbeat 只能有限度偵測存活，資源上限與共享記憶體回收仍依明定契約。

Framework C++ APIs use `vinc::Result<T>`/`vinc::Error` for expected recoverable failures, with `vinc::error_to_string()` for diagnostics. Contract violations terminate the offending process. This does not eliminate STL, allocator, third-party, or user-code exceptions. Python API failures use Python exceptions. See [Common](src/vinc/common/README.md) and component/language guides.

框架 C++ API 以 `vinc::Result<T>`／`vinc::Error` 回報預期可恢復失敗，用 `vinc::error_to_string()` 查診斷文字；違反契約會終止違規程序。這不會消除 STL、配置器、第三方或使用者程式的例外；Python API 失敗使用 Python exception。詳見 [Common](src/vinc/common/README.md) 與元件／語言指南。

<a id="repository-and-documentation"></a>

## Repository and documentation / 目錄與文件

```text
open-vinc/
├── apps/vinc/              # Central application / 中央應用
├── examples/               # Runnable examples / 可執行範例
├── library/                # Pinned third-party dependencies / 固定版本第三方相依
├── vinc_messages/          # Built-in .msg definitions / 內建訊息定義
└── src/vinc/
    ├── common/             # Result, Error, time, contracts, config
    ├── osal/               # General-purpose OS facilities / 通用 OS 能力
    ├── components/         # concurrency, message_storage
    ├── module_system/      # BaseModule, Context, Host, Manager, Python
    ├── control_channel/    # Control sessions and heartbeat / 控制通道
    ├── message_bus/        # Shared-memory publish/subscribe / 共享記憶體發布訂閱
    ├── rpc/                # Typed request/response / 具型別請求回覆
    ├── param/              # Typed parameters and persistence / 參數與保存
    ├── output_hub/         # Process-output capture / 程序輸出擷取
    ├── log/                # Structured logging and storage / 結構化紀錄
    ├── shell/              # Console, Socket Shell, Tcl
    ├── ribosome/           # Build-time message generator / 建置時訊息產生器
    ├── bag/                # SQLite Recorder
    ├── ui/                 # Web UI Module and plugins
    └── sdk/                # <vinc/vinc.hpp> and vinc::sdk
```

Each component owns headers, implementation, tests, CMake targets, and contracts. Headers live in `include/`, internal integration headers in `include/.../detail/`, and implementation files in `src/`. Header visibility does not make detail APIs supported user interfaces.

各元件自己維護標頭、實作、測試、CMake target 與契約。標頭放在 `include/`，內部接線標頭放在 `include/.../detail/`，實作放在 `src/`；編譯器看得到 detail，不代表它是受支援的使用者 API。

| Read next / 後續閱讀 | Guide / 指南 |
| --- | --- |
| First Module and API comparison / 第一個 Module 與 API 對照 | [C++ / Python examples](examples/userExampleModule/README.md) |
| SDK integration and CMake / SDK 整合與 CMake | [SDK](src/vinc/sdk/README.md) |
| Lifecycle, Context, debugging / 生命週期、Context 與除錯 | [Module System](src/vinc/module_system/README.md), [debugging](src/vinc/module_system/DEBUGGING.md) |
| Python environment and bindings / Python 環境與綁定 | [Python SDK](src/vinc/module_system/python/README.md) |
| Definitions and containers / 定義與容器 | [Messages](vinc_messages/README.md), [Ribosome](src/vinc/ribosome/README.md), [Storage](src/vinc/components/message_storage/README.md) |
| Publication and requests / 發布與請求 | [Message Bus](src/vinc/message_bus/README.md), [RPC](src/vinc/rpc/README.md) |
| Configuration and diagnostics / 設定與診斷 | [Param](src/vinc/param/README.md), [Log](src/vinc/log/README.md), [OutputHub](src/vinc/output_hub/README.md) |
| Control and startup / 控制與啟動 | [Shell](src/vinc/shell/README.md), [Socket Shell](src/vinc/shell/SOCKET.md), [application](apps/vinc/README.md) |
| Recording and visualization / 錄製與視覺化 | [Bag](src/vinc/bag/README.md), [Web UI](src/vinc/ui/README.md), [UI plugins](src/vinc/ui/PLUGIN.md) |
| OS and synchronization / OS 與同步工具 | [OSAL](src/vinc/osal/README.md), [Concurrency](src/vinc/components/concurrency/README.md), [Control Channel](src/vinc/control_channel/README.md) |
| Development conventions / 開發規範 | [AGENTS.md](AGENTS.md), [WORKING_STYLE.md](WORKING_STYLE.md), [NAMING.md](NAMING.md) |

Third-party sources and wrappers live in [library/](library/README.md). Upstream code is kept unmodified; retain required licenses and notices when distributing dependencies and generated UI assets.

第三方原始碼與封裝集中於 [library/](library/README.md)，維持上游原樣；散布相依套件與生成 UI 資源時，請保留各自要求的授權及聲明。

<a id="license-and-attribution"></a>

## License and attribution / 授權與署名

Unless otherwise noted, Open-VINC's original source code and documentation are licensed under the **Apache License, Version 2.0** (`Apache-2.0`). See [LICENSE](LICENSE) for the complete terms and [NOTICE](NOTICE) for author attribution and project contact information.

除另有標示外，Open-VINC 的原創原始碼與文件採用 **Apache License, Version 2.0**（`Apache-2.0`）。完整條款見 [LICENSE](LICENSE)，作者署名與專案聯絡資訊見 [NOTICE](NOTICE)。

Commercial use, modification, and redistribution are permitted subject to the license terms. The software is provided on an "AS IS" basis, without warranties, as specified in LICENSE.

依授權條款允許免費商用、修改及再散布；軟體依 LICENSE 所載以「現狀」提供，不附帶保證。

Third-party dependencies and vendored files retain their own licenses and notices. See [library/](library/README.md) and the license and notice files supplied with each dependency; the Open-VINC license does not replace them.

第三方相依套件與納入的第三方檔案維持各自的授權及聲明。請參閱 [library/](library/README.md) 與各套件附帶的授權、聲明檔案；Open-VINC 的授權不取代這些要求。

[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) collects complete dependency terms and incorporated-code attributions. Binary builds carry it together with LICENSE/NOTICE in `licenses/open-vinc/`; retain that directory when redistributing. Python and Web UI packages also carry their own copies. See [SDK license delivery](src/vinc/sdk/README.md#license-delivery).

[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) 集中保存完整相依條款及內含作品署名。
二進位建置會連同 LICENSE／NOTICE 放入 `licenses/open-vinc/`，再散布時一併保留；
Python 與 Web UI 套件亦各自攜帶。詳見 [SDK 授權交付](src/vinc/sdk/README.md#license-delivery)。
