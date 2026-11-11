<a id="vinc-中央程式"></a>

# Vinc central application / Vinc 中央程式

## Official System Modules / 官方 System Module

Both the source App and `vinc_add_app()` register UI (`ui`) and Bag Recorder
(`recorder`) with the [Module System policy](../../src/vinc/module_system/README.md#system-modules).
Startup waits for Ready or failure before continuing to Tcl. A failed attempt is
reported but remains supervised for retry; central and other services continue.
Shell exposes `ui enable|disable|status` and `recorder record|status|finish|info`.
They are hidden from ordinary Module listings, with process/recovery details in
`help --verbose`. Only whole-framework shutdown stops these protected processes.

來源版 App 與 `vinc_add_app()` 都以 System 政策登記 UI／Recorder；啟動等待就緒或失敗後
才繼續 Tcl。失敗會回報，但仍由中央監督重試，不關閉其他服務。
Shell 以上述內建命令操作，一般 Module 清單隱藏它們，`help --verbose` 保留程序與恢復診斷。
只有整個框架 shutdown 才停止這些受保護程序。

## Startup status / 初始化狀態

Central records each completed initialization step, then Shell renders one status
row per step after OSAL Terminal is ready. ANSI terminals use green/red check/cross
markers; redirected output uses plain `[OK]` / `[FAIL]`. The final summary counts
failed steps without implying that a failed official Module stopped the framework.
Initialization order, deadlines, cancellation, and official-Module cleanup policies
are unchanged. Readiness describes startup completion, not ongoing health.

中央收集各初始化步驟的結果，待 Shell 的 OSAL Terminal 準備好後統一顯示每項一行。
ANSI 終端使用綠／紅勾叉，重導向則用純文字 `[OK]`／`[FAIL]`。最後列出失敗步驟數；
官方 Module 啟動失敗不代表中央停止。初始化順序、期限、取消與收尾政策不變；
ready 表示完成啟動，不保證之後一直健康。

Complete startup errors (including a distinct Manager failure, when present) are
submitted to the existing bounded framework Log before entering Shell. Log's queue,
disk-failure and durability limits still apply; files live beside the executable
under `vinc_data/<instance>/logs/`. If Log could not be configured, details remain
on screen. A fatal failure before Shell starts prints a plain aborted-startup report
with full details, never a ready banner. Failure descriptions do not infer a busy
port from a generic Module hook failure. Runtime output/Log formatting is unchanged.

完整錯誤鏈及有差異的 Manager 故障送入既有框架 Log，沿用有界佇列、磁碟故障與保存限制；
檔案位於執行檔旁 `vinc_data/<instance>/logs/`。Log 無法配置時，完整細節仍留在畫面。
Shell 啟動前必須中止的錯誤會顯示純文字中止摘要與完整原因，不印 ready。
不從一般 hook 失敗推論 port 被占用；本輪不更動 runtime Log 的呈現格式。

Validation (2026-11-01): Windows x64 / MSVC 19.51 / Release passed
`shell.startup_report`, `shell.integration`, `shell.console_input`, and `app.entry`.
The production-entry fixture checks a synthetic official-Module startup failure,
one console failure row, the complete cause chain in a new log file, continued
script execution, and a fatal listener-configuration failure without a ready banner.
This is not native socket fault injection. Linux/macOS and a fresh installed-SDK
consumer were not rerun for this presentation change.

驗證（2026-11-01）：Windows x64／MSVC 19.51／Release 通過上述四項測試。
正式入口測試以替身 Module 製造啟動失敗，確認畫面只有一筆失敗、完整原因鏈寫入新 Log、
腳本繼續執行，以及 listener 設定失敗時不誤印 ready；這不代表原生 socket 故障注入。
本輪未重跑 Linux／macOS 與全新 installed-SDK 使用端。

Follow-up: the full Windows Debug build exposed a missing `startup_report.cpp`
source in `shell_fault_tests`, which compiles its own Shell implementation.
After adding that source, the full Debug build and `shell.faults` /
`shell.startup_report` passed. The earlier selected-target Release build had not
covered this fixture.

後續完整 Windows Debug 建置發現 `shell_fault_tests` 自行編譯 Shell 時漏列
`startup_report.cpp`。補齊後，完整 Debug 建置及上述兩項測試通過；
前次選定 target 的 Release 建置未涵蓋此故障測試。


The confirmed next-step design is tracked in the
[Multi-App composition plan](MULTI_APP.md): shared Modules/messages, per-App Kconfig
selection and deployment, and automatic official UI/Bag startup. Stage 2 adds the
shared source/installed `vinc_add_app()` constructor, per-App catalogs and optional
shared Python bindings. Stage 3 adds independent deployment/install and automatic official startup.
All application paths now use the common constructor documented in
the [SDK composition guide](../../src/vinc/sdk/INSTALL.md#c-application-composition--c-應用組裝).

已確認的下一輪設計見 [多 App 組裝開工文件](MULTI_APP.md)：共用 Module／Message、
各 App 獨立 Kconfig 選擇與部署，以及官方 UI／Bag 自動啟動。第二階段接上來源／安裝式
共用的 `vinc_add_app()`、各自 catalog 與選用的共用 Python 綁定；第三階段已接上
獨立部署／安裝及官方 Module 自動啟動。所有 App 已統一使用共用建構器，見上方 SDK 組裝指南。

The framework supplies the sole supported production App entry; user-defined or
replacement central/Host mains are unsupported. Entry ownership and
executable consolidation are documented in the
[Single executable application plan](SINGLE_EXECUTABLE.md). App dispatches both roles and launches itself. Production standalone Host targets
and SDK export/copy references are removed. Clean SDK-only and relocated C++ App
validation and final integration regressions passed on Windows/WSL; the plan
tracks the remaining native macOS validation.
正式 App 的唯一入口由框架提供，不支援使用者自訂或替換中央／Host main。
入口所有權與合併步驟見上述契約；App 已分流角色並啟動自己，正式獨立 Host target 與
SDK 匯出／複製引用已移除。乾淨 SDK-only 與搬移 C++ App 已通過 Windows／WSL 驗證；
最終整合回歸亦通過；剩餘 macOS 原生驗證由計畫追蹤。

The `app.entry` fixtures compile this production main in plain and embedded-script
forms. Additional `app.*` tests reuse Module lifecycle, Context, fatal, debug,
Plugin-loader, Bus, Param, Log and RPC suites with the App selected as native Host.
The test-only selection in `tests/support/manager.hpp` is not a production
environment option. Injection Hosts remain explicit test fixtures.

`app.entry` 以正式 main 編譯一般／內嵌腳本版本；其他 `app.*` 測試以 App 作為原生
Host，重用 Module 生命週期、Context、fatal、debug、Plugin loader、Bus、Param、
Log 與 RPC 測試。`tests/support/manager.hpp` 的選擇方式只供測試，不是正式環境變數
介面；故障注入仍保留明確指定的測試 Host。

The bundled `Vinc` target uses `vinc_add_app()` with identity `open-vinc` and
[its own Kconfig file](vinc.conf). Output is `<build>/output/Vinc/<configuration>/Vinc[.exe]`.
The bundled App's compilation, generated settings and test work directory is
`<build>/intermediate/vinc/`. The source directory remains `apps/vinc/`.
UI/Recorder are mandatory; registered example Modules are explicitly selected in that file.
`VINC_BUILD_APPS=OFF` omits this demonstration App and its example registration;
external consumers register their own Modules and Apps. All production catalogs use
relative deployed Plugin paths. Build settings and editors follow [configuration](../../tools/configure/README.md).

內建 `Vinc` target 與外部 App 共用建構器；身份為 `open-vinc`，產物採上方獨立目錄。
內建 App 的編譯、生成設定與測試工作資料放在 `<build>/intermediate/vinc/`；
原始碼目錄仍為 `apps/vinc/`。重新配置不會自動刪除舊 build 的 `apps/vinc/`，
清理前須確認其中沒有需要保留的執行期資料。外部 SDK 使用者的 CMake 工作目錄仍由其專案管理。
設定檔明確選入範例，UI／Recorder 固定提供。`VINC_BUILD_APPS=OFF` 只略過內建示範 App
及其範例登記，不會禁止外部呼叫 `vinc_add_app()`。

### Intermediate directory validation (2026-11-06) / 建置工作目錄驗收

The bundled App now uses `<build>/intermediate/vinc/`. The two existing Windows
Debug/Release `apps/vinc/` trees (including their old runtime data) were explicitly
authorized for removal, inspected for running images and directory links, and
deleted. Empty `apps/` parents were removed too. Both configurations rebuilt
successfully without recreating `apps/`; the ready-to-run Apps remain in `output/`.
This manual cleanup is not an automatic CMake migration policy.

本輪經使用者明確授權，檢查程序與目錄連結後，清除 Windows Debug／Release 舊的
`apps/vinc/`（含該目錄內的舊執行期資料）及空 `apps/` 父目錄。兩種組態重新建置成功，
沒有重新產生 `apps/`；可執行 App 仍在 `output/`。這是本機清理，不是 CMake 自動刪除政策。

Windows Debug (MSVC 19.51.36257 / CMake 4.3.2) and WSL Ubuntu 24.04 Release
(GCC 13.3.0 / CMake 3.28.3) each passed **6/6**: `app.embedding`, `app.entry`,
`app.system_services`, `module.shell`, `message_bus.shell`, and `bag.shell`.
CTest and install scripts point at the new intermediate directory. The first WSL
configure attempt found a missing example CMake file in the validation copy;
after synchronizing the previously completed examples, rebuild and tests passed.
Full-project CTest, Windows Release runtime tests, Python and native macOS were
not rerun for this directory-only change. The installed SDK composer is unchanged;
the preceding SDK acceptance remains recorded in the SDK installation guide.
Native macOS follow-up is listed in the root TODO.

Windows Debug also passed a fresh `VincApp_Vinc` install and execution from that
package: official UI/Recorder initialized, `recorder status` reported idle, and
all managed processes exited normally. See `build/intermediate-install.log` and
`build/intermediate-package-run.log`.

Windows Debug／WSL Release 上述六項各 **6/6 通過**；Windows Release 僅重建。
另以 Windows Debug 全新安裝 App，通過 UI／Recorder 初始化、狀態查詢及完整關閉。
WSL 首次配置因驗收副本缺少範例 CMake 檔失敗，同步後通過。
本輪未重跑完整 CTest、Python 或原生 macOS；SDK 共用建構器未變動，沿用前輪紀錄。
本機紀錄：`build/intermediate-windows-*.log`，以及 WSL 驗收目錄的 `intermediate-*.log`。

<a id="打包應用程式的啟動腳本"></a>

## Embedded application startup script / 打包應用程式的啟動腳本

Source and installed consumers use the same constructor; see [SDK composition](../../src/vinc/sdk/INSTALL.md).
來源與安裝式使用端共用建構器，target 決定檔名、`INSTANCE_NAME` 決定排他身份：

```cmake
set(VINC_BUILD_APPS OFF CACHE BOOL "Build the bundled App")
add_subdirectory(open-vinc/src open-vinc-build)
# Register shared Modules/messages, then select them in my-app.config.
vinc_add_app(MyApp
    INSTANCE_NAME my-app
    VINC_BUILD_CONFIG my-app.config
    STARTUP_SCRIPT startup.tcl
)
```

Multiple Apps can be declared. Module and config paths follow the SDK composition contract.
可宣告多個 App；同機不同身份仍需自行避免服務 port 衝突。

- STARTUP_SCRIPT resolves relative to the caller source directory; missing/directory input fails configure. Build accepts strict UTF-8/BOM, rejects NUL, normalizes CRLF/CR to LF, and generates C++ bytes. It neither guesses encoding nor evaluates Tcl syntax; runtime reports syntax/command failures.
- Empty scripts are legal. No CMake expansion/extra Tcl substitution. Editing the source normally rebuilds the EXE; generated headers stay build-local. No new runtime thread/temp script/external interpreter dependency.
- Embedded apps reject -S before service creation and offer no --no-startup. Unconfigured central retains -S file.tcl [args...]; srun is unchanged.
- Execute once after central initialization/catalog/Python discovery/Socket Shell and official UI/Bag readiness or failure. Success/failure leaves central and started Modules running; exit requests normal shutdown, cancellation keeps existing behavior.
- argv0/info script is embedded:startup.tcl, argc0/empty argv. It is a diagnostic identity, not a path; source/line diagnostics remain.
- All Tcl environments expose ::vinc::app_dir for separately deployed resources:

- 正式入口由框架提供；不支援使用者另寫 main。

- `STARTUP_SCRIPT` 相對於呼叫函式的來源目錄解析；不存在或是目錄時 configure 失敗。
  建置時驗證 UTF-8（接受 BOM、拒絕 NUL），正規化 CRLF／CR 為 LF，再生成 C++ byte array。
  不猜編碼、不執行 Tcl 來驗證語法；Tcl 語法及命令錯誤仍於啟動時報告。
- 空腳本合法。來源內容不經 CMake 變數展開或 Tcl 二次替換；修改腳本後正常 build 即更新 EXE。
  生成標頭在建置目錄，無新 runtime thread、暫存腳本檔或外部腳本直譯器相依。
- 有內嵌腳本的應用不接受 `-S`，會在建立服務前回報參數錯誤；不提供 `--no-startup`。
  未指定內嵌腳本的 App 保留既有 `-S file.tcl [args...]`。Shell 的 `srun` 不受影響。
- 腳本在中央初始化、catalog／Python discovery、Socket Shell 及官方 UI／Bag 啟動後執行一次。
  官方 Module 分別等待 Ready 或有界失敗；失敗回報後仍執行腳本，Bag 預設待命不錄製。
  正常完成或失敗都保留框架及已啟動 Module；`exit` 才要求正常關閉，取消沿用原有流程。
- 內嵌入口的 `argv0`／`info script` 為 `embedded:startup.tcl`，`argc=0`、`argv` 為空。
  此名稱是診斷身分，不是真實路徑；錯誤保留來源及行號。
- 所有 Tcl 執行環境都提供 `::vinc::app_dir`（EXE 所在目錄），可讀取另外部署的資源：

```tcl
source [file join $::vinc::app_dir scripts common.tcl]
```

Cwd is unchanged, as are ordinary -S/srun relative paths. Embedding does not bundle other scripts, Plugins, Python, or Tcl library assets. All App catalogs use relative deployed Plugin paths. No terminal/Socket disable option was added.

不自動改變 cwd；`-S`／`srun` 及一般相對路徑的既有語意維持。入口內嵌不會把其他 Tcl
腳本、Module Plugin、Python 或 Tcl library 資源一併嵌入。所有 App 的 catalog 都使用相對部署路徑，支援完整套件搬移。本輪不增加停用 Terminal／
Socket Shell 的設定。

Complete parent example: [embedded_app](../../examples/embedded_app/CMakeLists.txt). Configure with cmake -S examples/embedded_app -B build/my-app, then build target MyApp. Output is build/my-app/MyApp/<configuration>/.

完整父專案範例在 [examples/embedded_app](../../examples/embedded_app/CMakeLists.txt)：
`cmake -S examples/embedded_app -B build/my-app`，再 `cmake --build build/my-app --target MyApp`。
沿用框架工具鏈需求；輸出在 `build/my-app/open-vinc`（依生成器組態而定）。

Tcl and Socket Shell initialize automatically with central startup. Users configure CLI rather than call service-init/start/stop APIs. Central uses Shell detail wiring; SDK exposes settings/errors only. The shared deployment manifest follows actual output paths, including later parent customization.

Tcl 腳本與 Socket Shell 都是中央啟動時自動準備的預設功能，使用者只設定 CLI，
不呼叫初始化或服務啟停 API。中央使用 Shell 的 detail 接線標頭；SDK 只提供設定與錯誤型別。
建置時共用部署清單跟隨執行檔實際輸出位置部署資源，包含父專案後設的輸出目錄。

<a id="入口與生成來源"></a>

## Entry points and generated sources / 入口與生成來源

After platform argument conversion, only argv[1] is inspected for the reserved
`--vinc-module` family. Host launches call the existing Module Runtime before any
central initialization; unsupported/malformed bootstrap versions are rejected there.
Central queries `osal::executable_path()` and explicitly supplies that path to Manager.
Startup scripts run only in central. Internal-looking arguments after `-S` or inside
Module hook arguments remain user data. Both source and installed App entry targets
link `vinc::module_runtime`; this does not start Runtime services in central.

平台參數轉換後，僅檢查 argv[1] 的保留 `--vinc-module` 系列標記。Host 在中央初始化前
直接進既有 Module Runtime；不支援或損壞的 bootstrap 由 Runtime 拒絕。中央查詢
`osal::executable_path()` 並明確交給 Manager；啟動腳本只在中央執行。`-S` 後或 Module
hook 中類似內部標記的參數仍是使用者資料。原始碼及安裝式 App target 均連結既有
`vinc::module_runtime`，不因此在中央啟動 Runtime 服務。

Startup scans executable-adjacent modules/python/<ModuleType>/<ModuleType>.py without execution. Missing directories mean no Python Modules; individual issues diagnose/continue. --python PATH before -S overrides the Kconfig-selected default. CONFIG_VINC_APP_PYTHON=y in App Kconfig builds/deploys native package/bootstrap, not an interpreter; see [Python](../../src/vinc/module_system/python/README.md).

- 中央啟動時亦掃描執行檔旁 `modules/python/<ModuleType>/<ModuleType>.py`，不執行腳本。
  缺少目錄視為沒有 Python Module；個別登記問題回報後繼續啟動。
  `--python PATH` 指定系統 Python／venv，預設依 PATH 選取；放在 `-S` 之前。
  App Kconfig 的 `CONFIG_VINC_APP_PYTHON=y` 建置並部署 `python/vinc` 原生擴充與啟動腳本，不部署直譯器。
  詳見 [Python Module](../../src/vinc/module_system/python/README.md)。

Binary builds also deploy `licenses/open-vinc/` beside the executable. Keep it with the application; see [SDK license delivery](../../src/vinc/sdk/README.md#license-delivery).

二進位建置亦在執行檔旁部署 `licenses/open-vinc/`；打包應用程式時一併攜帶，見
[SDK 授權交付](../../src/vinc/sdk/README.md#license-delivery)。

- parse_arguments handles only startup/help/argument-error flow; args after -S belong to Tcl, and --help exits before services.
- run_services initializes Tcl, creates Manager, configures Log/Param persistence, creates Shell/reserves names, registers catalog, scans Python, starts Socket Shell, then runs Shell/script. Common Results are checked; persistence errors are diagnosed while live services remain. Shell is destroyed before Manager. Service failure returns to application only after shutdown; existing STL/third-party boundaries remain there.
- Windows wmain converts UTF-16/UTF-8 and allocates strings before borrowed argv pointers.
- example_module expands naming/command/lifecycle hooks; --name consumes only its first pair with unchanged returns/exceptions.
- Catalog templates and VincModule.cmake register metadata, then file(GENERATE) resolves configured Plugin paths, retaining order/Result failures. Never edit generated catalog sources.

- `parse_arguments()` 只解析 CLI，回傳啟動、顯示說明或參數錯誤的內部流程狀態。
  `-S` 路徑後的所有參數仍交給 Tcl；`--help` 在建立服務前退出。
- `run_services()` 依序完成 Tcl 初始化、Manager 建立、Log／Param 保存設定、Shell 保留命令名稱、catalog 註冊、Python 掃描、
  Socket Shell 啟動及 Shell 執行的順序，逐步檢查共用 Result；保存服務錯誤會提示，live 服務仍可繼續。
  Shell 透過 `create()` 建立並先於 Manager 銷毀；`run()` 也回傳 Result，
  服務故障須等既有完整關閉流程結束後才回到 `application()`。
  既有 STL／第三方例外邊界仍由 `application()` 保留。
- Windows `wmain()` 保留 UTF-16 到 UTF-8 的轉換邊界；字串配置完成後才建立借用指標陣列。
- `examples/module/example_module.cpp` 已展開命名 hook、命令迴圈及其他 hook；
  `--name` 仍只消耗第一組名稱參數，既有回傳與例外行為未改變。
- Catalog 由本目錄 CMake 的外層模板及 `src/vinc/module_system/cmake/VincModule.cmake`
  的註冊片段生成。模板先代入 metadata，再由 `file(GENERATE)` 解析組態對應的 Plugin 庫路徑；
  保留註冊順序，註冊錯誤以 Result 傳回；生成結果不應手動修改。

CLI numeric limits/exit codes are named in the entry point; connection limit uses config::shell_socket_max_connections. Existing values remain; see [Shell errors](../../src/vinc/shell/README.md#共用錯誤介面).

CLI 逾時上限、最小有效值及退出碼在入口內具名；連線數上限引用現有
`vinc::config::shell_socket_max_connections`。原有 CLI 數值未變動；Shell 的統一錯誤介面
及破壞性 API 變更見 [Shell README](../../src/vinc/shell/README.md#共用錯誤介面)。

<a id="驗證"></a>

## Recorded validation / 驗證

<a id="內嵌啟動腳本2026-10-14"></a>

### Embedded startup validation (2026-10-14) / 內嵌啟動腳本（2026-10-14）

Windows x64/MSVC 19.51/CMake 4.3.2/Ninja: full Debug/Release builds and **12/12** related CTests each (93.30 s/128.51 s).

Windows x64／MSVC 19.51、CMake 4.3.2、Ninja：Debug／Release 完整建置成功，
兩種組態的相關 CTest 各 **12/12** 通過（93.30 秒／128.51 秒）。

- app.embedding: empty/BOM/CRLF/CR/Chinese/emoji, invalid/overlong/surrogate/truncated UTF-8, scripts beyond normal string-literal size.
- shell.tcl: isolated embedded environments, argv/script/app_dir/line numbers, return/cancel/exit; script failure leaves Modules/Shell operable.
- sdk.external_consumer: parent API, Unicode generation/execution, -S rejection, script-only incremental rebuild, invalid encoding failure/recovery, relocated EXE/Tcl/sibling scripts sourced from another cwd. This does not test relocating Module Plugins.
- Existing Socket/Console/Terminal/Integration/Faults/Unit and Module/Bus/Bag script suites passed; external scripts remain usable.

- `app.embedding`：空腳本、UTF-8 BOM、CRLF／CR、中文及 emoji、非法／過長編碼、
  surrogate、截斷編碼及超過一般字串常值大小的腳本生成。
- `shell.tcl`：內嵌文字的獨立環境、`argv0`／`info script`、`app_dir`、原始行號、
  `return`、取消與 `exit`；腳本錯誤後 Module 仍存活且可繼續操作 Shell。
- `sdk.external_consumer`：父專案呼叫 CMake API、實際生成及執行 Unicode 內容、拒絕 `-S`、
  僅修改腳本的增量重建、非法編碼阻止 build 及修復後重建；搬移 EXE／Tcl 資源／相鄰腳本後，
  從不同 cwd 啟動並成功 `source`。此搬移案例不測試重新定位 Module Plugin。
- 其餘 Shell Socket／Console／Terminal／Integration／Faults／Unit、中央 Module／Bus／Bag
  腳本測試通過，原有外部腳本流程保持可用。

Logs: build/startup-{debug, release}-final-tests.log. No native Linux/macOS or other unrelated full-suite rerun in that round.

紀錄在 `build/startup-{debug,release}-final-tests.log`。Linux／macOS 本輪未實機驗證；
未重跑其餘不相關元件的完整 CTest。

Recorded 2026-10-06 external SDK Debug/Release verified automatic Tcl/Socket startup, custom output paths/relocation, Module/Bus operation and Shell regressions. See [SDK evidence](../../src/vinc/sdk/README.md#sdk-重審修正2026-10-06).

2026-10-06 SDK 邊界與部署修正：Windows Debug／Release 的外部消費端均驗證
中央自動啟動 Tcl／Socket Shell、自訂輸出位置與搬移部署；既有中央模組／Bus 操作及
Shell 回歸也通過，範圍與結果見 [SDK 驗證](../../src/vinc/sdk/README.md#sdk-重審修正2026-10-06)。

Recorded 2026-10-04 readability: full Windows Debug/Release builds, three integration tests each (Console, Module Shell, Bus Shell). Catalog then contained its original two types with correct configured launch paths; this is historical, not the current catalog count.

2026-10-04 可讀性整理：Windows x64／MSVC 的 Debug、Release 完整建置通過，
兩種組態各有 `shell.console`、`module.shell`、`message_bus.shell` 三個整合測試通過。
當時生成 catalog 含原有兩個 ModuleType 並使用正確組態路徑；這是歷史紀錄，不是目前 catalog 數量。

Each configuration compared 34 CLI inputs/exit codes/stdout/stderr against the pre-cleanup executable. Unicode/spaced script paths and post-S Unicode/spaces/help forwarding passed. No other suites or Linux/macOS run in that round.

兩種組態各比對 34 組 CLI 輸入與整理前執行檔，退出碼、stdout、stderr 一致；
另驗證中文／空白腳本路徑及 `-S` 後包含空白、中文、`--help` 等參數的轉交。
本輪未重跑其餘 CTest；Linux／macOS 未驗證。

<a id="utf-8-路徑2026-10-05"></a>

## UTF-8 paths (2026-10-05) / UTF-8 路徑（2026-10-05）

-S uses [OSAL path_from_utf8](../../src/vinc/osal/README.md#utf-8-與原生路徑), rejecting invalid encoding before service creation instead of creating a char8_t view. General Windows argv conversion remains in wmain; arguments are not all treated as paths. Unicode/emoji deployment/Tcl/script paths passed Windows Debug/Release; Linux/macOS were not tested then.

`-S` 路徑改用 [OSAL path_from_utf8](../../src/vinc/osal/README.md#utf-8-與原生路徑)，
非法編碼會顯示具體錯誤並以參數錯誤結束啟動，不再建立 char8_t view。
Windows 的一般 argv UTF-16／UTF-8 轉換仍留在 wmain，不把一般參數當成路徑處理。
Windows Debug／Release 均已驗證中文／emoji 的部署目錄、Tcl 資源與腳本檔名。
此段保留當時 Windows 路徑案例紀錄；目前各平台的建置與執行範圍見[平台驗證報告](../../README.md#validation-and-limits)。

<a id="待處理事項"></a>

## App composition unification / App 組裝統一

Implemented 2026-11-02; validation scope is recorded below.
The bundled App, source consumers and installed SDK consumers share `vinc_add_app()`.
Removed the legacy catalog/deployment pipeline and `vinc_configure_app()`.
`VINC_BUILD_UI`, `VINC_BUILD_EXAMPLES`, `VINC_NAME`, `VINC_CONFIG_FILE` are no longer
configuration inputs; CMake may warn about unused command-line values. UI capability
is always present in the official full SDK. `INSTANCE_NAME` remains per-App.

2026-11-02 統一實作；驗收範圍如下。舊入口不設相容別名，舊 CMake 變數
不讀取、不拒絕、不自動清除。內建設定檔選入既有範例；自訂 App 的 Module 預設仍不選入。
CLI、System Module 監督、Python ABI、腳本及 Param／port 行為維持既有契約。
舊平鋪 build 產物不會自動刪除；請執行新目錄內的 App，不要誤啟動舊檔。

### Validation (2026-11-02) / 本輪驗證

- Windows x64, MSVC 19.44.35229/toolset 14.44.35207, Release, CMake 4.3.2,
  CPython 3.13.12: bundled App build and **28/28** related CTests passed.
- WSL Ubuntu 24.04 x86_64, GCC 13.3/libstdc++, Release, CPython 3.12.3,
  Node 24.19.0: source App with Python enabled and **32/32** related CTests passed.
- Configuration tool: **17/17** on Windows and WSL. Windows tools: **118** tests,
  **22 skipped**, no failures.
- Windows installed Python SDK: relocation, custom Bus/RPC/Param, schema incremental
  changes, ABI/runtime rejection, resource repair and disabling Python passed.
- Windows installed multi-App suite: explicit/late selection, unselected-target
  exclusion, shared Plugin/binding reuse, individual install, relocation, identity
  exclusion/concurrency, UI/Bag failure isolation and managed-file cleanup passed.
- Windows source SDK consumer: complete incremental/embedding suite passed
  (`sdk.external_consumer`, 472 seconds), including custom schemas, late Module
  registration, relocation, embedded-script updates/rejection, and selection cleanup.

Windows／WSL 如上列環境實測；預設內建範例、單一程序入口與 System Module 政策維持。
舊 CMake UI／Examples／名稱／設定檔參數以刻意相反或無效值驗證，不會覆蓋 App 設定。
WSL 的 Python 選擇只改測試副本；儲存庫預設仍為關閉。
本輪未重跑 Debug、macOS 原生或 WSL installed SDK 全套；不可由上述結果推稱已通過。
本機原始紀錄：`build/app-composition-review/`（不納入 Git）。

### Native macOS follow-up / macOS 原生補驗（2026-11-02）

At `627c409`, Debug/Release builds, source/installed multi-App composition and clean
SDK downstream acceptance passed on macOS arm64. Original CTest passed **125/126**
in each configuration: the example fixture omitted deployed Plugin/UI resources.
The external complete-deployment comparison passed; the fixture remains unfixed.
See the [macOS report](../../src/vinc/sdk/MACOS_VALIDATION.md) for methods and limits.

macOS 原生補驗已有結果，取代上方該輪未測的狀態；原始全套各有一項 fixture 失敗，
不宣稱 126/126。完整證據與未套用修正的對照見上方報告，歷史 Windows／WSL 結果保留。

## Resolved follow-up / 已解決後續事項

<a id="外部-module-註冊晚於中央-catalog-收集"></a>

### External registration after catalog collection: resolved / 外部 Module 登記晚於 catalog 收集：已解決

Fixed 2026-10-12: collect at outermost normal configure completion, reread Module registrations, generate catalog, and add Plugin build dependencies. Parent add_module after add_subdirectory is included. Default instance names now come from source, not catalog. See [SDK](../../src/vinc/sdk/README.md) for validation.

2026-10-12 已調整接線：中央 catalog 收集延至最外層 CMake 目錄的正常配置結束，
此時重新讀取 Module 登記資訊，產生 catalog 並加入 Plugin 建置相依。
外部父專案在 add_subdirectory(open-vinc/src ...) 返回後呼叫 add_module 亦會納入。
實例預設名稱不再存放於 catalog，由 Module source 決定。
實際驗證結果見 [SDK README](../../src/vinc/sdk/README.md)。
