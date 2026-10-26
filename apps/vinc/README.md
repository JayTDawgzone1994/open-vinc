<a id="vinc-中央程式"></a>

# Vinc central application / Vinc 中央程式

This directory composes CLI, Module catalog, and central services; Shell logic belongs to src/vinc/shell. Target shell outputs Vinc.exe/Vinc in the build directory, optionally renamed below. Modules register during ordinary configure; the catalog is generated at outermost configure completion, inside the build tree.

本目錄負責 CLI、Module catalog 與中央服務的組裝。
Shell 邏輯仍由 `src/vinc/shell` 元件維護。
CMake target 為 `shell`，預設輸出為建置目錄中的 `Vinc.exe`／`Vinc`，可透過下方應用設定更名。
Module 在正常配置流程登記，最外層配置結束時統一生成 catalog；生成檔在本次建置目錄內。

VINC_BUILD_APPS=OFF omits central; VINC_BUILD_EXAMPLES=OFF omits examples/catalog entries. CLI, listener defaults, and shutdown follow [Shell](../../src/vinc/shell/README.md).

`VINC_BUILD_APPS=OFF` 可略過中央程式；`VINC_BUILD_EXAMPLES=OFF` 可略過內建範例及其 catalog 項目。
一般中央的 CLI、socket 預設值與關閉行為維持原有契約。操作方式見 [Shell README](../../src/vinc/shell/README.md)。

<a id="打包應用程式的啟動腳本"></a>

## Embedded application startup script / 打包應用程式的啟動腳本

Call once in the parent after add_subdirectory(open-vinc/src ...):

父專案在 `add_subdirectory(open-vinc/src ...)` 完成後呼叫一次：

```cmake
set(VINC_NAME "my-app" CACHE STRING "Central instance identity")
add_subdirectory(open-vinc/src open-vinc-build)

# Register application Modules and message directories here as needed.
vinc_configure_app(
    NAME MyApp
    STARTUP_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/startup.tcl"
)
```

- Requires APPS; configures existing shell, one central application per build.
- NAME is an extensionless ASCII letter/digit/_/- filename; Windows appends .exe. It does not change VINC_NAME, which must be set before adding the framework for [exclusive identity/data ownership](../../src/vinc/module_system/INSTANCE.md). Distinct same-host apps also need distinct ports.
- STARTUP_SCRIPT resolves relative to the caller source directory; missing/directory input fails configure. Build accepts strict UTF-8/BOM, rejects NUL, normalizes CRLF/CR to LF, and generates C++ bytes. It neither guesses encoding nor evaluates Tcl syntax; runtime reports syntax/command failures.
- Empty scripts are legal. No CMake expansion/extra Tcl substitution. Editing the source normally rebuilds the EXE; generated headers stay build-local. No new runtime thread/temp script/external interpreter dependency.
- Embedded apps reject -S before service creation and offer no --no-startup. Unconfigured central retains -S file.tcl [args...]; srun is unchanged.
- Execute once after central initialization/catalog/Python discovery/Socket Shell. Success/failure leaves central and started Modules running; exit requests normal shutdown, cancellation keeps existing behavior.
- argv0/info script is embedded:startup.tcl, argc0/empty argv. It is a diagnostic identity, not a path; source/line diagnostics remain.
- All Tcl environments expose ::vinc::app_dir for separately deployed resources:

- 必須啟用 `VINC_BUILD_APPS`；本函式設定既有 `shell` target，一份建置只配置一個中央應用。
- `NAME` 是不含副檔名的輸出檔名，限 ASCII 字母、數字、`_`、`-`；Windows 自動加 `.exe`。
  它不更改 `VINC_NAME`。中央身分須在加入框架前設定，影響排他及資料歸屬，見
  [Instance 契約](../../src/vinc/module_system/INSTANCE.md)。同機不同應用的 Socket port 仍需分開。
- `STARTUP_SCRIPT` 相對於呼叫函式的來源目錄解析；不存在或是目錄時 configure 失敗。
  建置時驗證 UTF-8（接受 BOM、拒絕 NUL），正規化 CRLF／CR 為 LF，再生成 C++ byte array。
  不猜編碼、不執行 Tcl 來驗證語法；Tcl 語法及命令錯誤仍於啟動時報告。
- 空腳本合法。來源內容不經 CMake 變數展開或 Tcl 二次替換；修改腳本後正常 build 即更新 EXE。
  生成標頭在建置目錄，無新 runtime thread、暫存腳本檔或外部腳本直譯器相依。
- 有內嵌腳本的應用不接受 `-S`，會在建立服務前回報參數錯誤；不提供 `--no-startup`。
  未呼叫此函式的中央保留既有 `-S file.tcl [args...]`。Shell 的 `srun` 不受影響。
- 腳本在既有中央初始化、catalog／Python discovery 和 Socket Shell 啟動後執行一次。
  正常完成或失敗都保留框架及已啟動 Module；`exit` 才要求正常關閉，取消沿用原有流程。
- 內嵌入口的 `argv0`／`info script` 為 `embedded:startup.tcl`，`argc=0`、`argv` 為空。
  此名稱是診斷身分，不是真實路徑；錯誤保留來源及行號。
- 所有 Tcl 執行環境都提供 `::vinc::app_dir`（EXE 所在目錄），可讀取另外部署的資源：

```tcl
source [file join $::vinc::app_dir scripts common.tcl]
```

Cwd is unchanged, as are ordinary -S/srun relative paths. Embedding does not bundle other scripts, Plugins, Python, or Tcl library assets. Catalogs still contain build-time Plugin paths; embedding does not make complete deployment relocatable. No terminal/Socket disable option was added.

不自動改變 cwd；`-S`／`srun` 及一般相對路徑的既有語意維持。入口內嵌不會把其他 Tcl
腳本、Module Plugin、Python 或 Tcl library 資源一併嵌入。Module catalog 目前仍含建置時
Plugin 路徑，不能由本功能推論整套 Plugin 部署已任意可搬移。本輪不增加停用 Terminal／
Socket Shell 的設定。

Complete parent example: [embedded_app](../../examples/embedded_app/CMakeLists.txt). Configure with cmake -S examples/embedded_app -B build/my-app, then build target shell. Existing toolchain constraints apply; output is build/my-app/open-vinc, subject to generator configuration.

完整父專案範例在 [examples/embedded_app](../../examples/embedded_app/CMakeLists.txt)：
`cmake -S examples/embedded_app -B build/my-app`，再 `cmake --build build/my-app --target shell`。
沿用框架工具鏈需求；輸出在 `build/my-app/open-vinc`（依生成器組態而定）。

Tcl and Socket Shell initialize automatically with central startup. Users configure CLI rather than call service-init/start/stop APIs. Central uses Shell detail wiring; SDK exposes settings/errors only. vinc_deploy_tcl(shell) follows actual output paths, including later parent customization.

Tcl 腳本與 Socket Shell 都是中央啟動時自動準備的預設功能，使用者只設定 CLI，
不呼叫初始化或服務啟停 API。中央使用 Shell 的 detail 接線標頭；SDK 只提供設定與錯誤型別。
建置時 `vinc_deploy_tcl(shell)` 跟隨執行檔實際輸出位置部署資源，包含父專案後設的輸出目錄。

<a id="入口與生成來源"></a>

## Entry points and generated sources / 入口與生成來源

Startup scans executable-adjacent data/module/<ModuleType>/<ModuleType>.py without execution. Missing directories mean no Python Modules; individual issues diagnose/continue. --python PATH before -S selects system Python/venv, default PATH. VINC_BUILD_PYTHON builds/deploys native package/bootstrap, not an interpreter; see [Python](../../src/vinc/module_system/python/README.md).

- 中央啟動時亦掃描執行檔旁 `data/module/<ModuleType>/<ModuleType>.py`，不執行腳本。
  缺少目錄視為沒有 Python Module；個別登記問題回報後繼續啟動。
  `--python PATH` 指定系統 Python／venv，預設依 PATH 選取；放在 `-S` 之前。
  `VINC_BUILD_PYTHON=ON` 建置並部署 `python/vinc` 原生擴充與啟動腳本，不部署直譯器。
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
Linux／macOS 尚未驗證。

<a id="待處理事項"></a>

## Resolved follow-up / 已解決後續事項

<a id="外部-module-註冊晚於中央-catalog-收集"></a>

### External registration after catalog collection: resolved / 外部 Module 登記晚於 catalog 收集：已解決

Fixed 2026-10-12: collect at outermost normal configure completion, reread Module registrations, generate catalog, and add Plugin build dependencies. Parent add_module after add_subdirectory is included. Default instance names now come from source, not catalog. See [SDK](../../src/vinc/sdk/README.md) for validation.

2026-10-12 已調整接線：中央 catalog 收集延至最外層 CMake 目錄的正常配置結束，
此時重新讀取 Module 登記資訊，產生 catalog 並加入 Plugin 建置相依。
外部父專案在 add_subdirectory(open-vinc/src ...) 返回後呼叫 add_module 亦會納入。
實例預設名稱不再存放於 catalog，由 Module source 決定。
實際驗證結果見 [SDK README](../../src/vinc/sdk/README.md)。
