# Vinc 中央程式

本目錄負責 CLI、Module catalog 與中央服務的組裝。
Shell 邏輯仍由 `src/vinc/shell` 元件維護。
CMake target 為 `shell`，預設輸出為建置目錄中的 `Vinc.exe`／`Vinc`，可透過下方應用設定更名。
Module 在正常配置流程登記，最外層配置結束時統一生成 catalog；生成檔在本次建置目錄內。

`VINC_BUILD_APPS=OFF` 可略過中央程式；`VINC_BUILD_EXAMPLES=OFF` 可略過內建範例及其 catalog 項目。
一般中央的 CLI、socket 預設值與關閉行為維持原有契約。操作方式見 [Shell README](../../src/vinc/shell/README.md)。

## 打包應用程式的啟動腳本

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

不自動改變 cwd；`-S`／`srun` 及一般相對路徑的既有語意維持。入口內嵌不會把其他 Tcl
腳本、Module Plugin、Python 或 Tcl library 資源一併嵌入。Module catalog 目前仍含建置時
Plugin 路徑，不能由本功能推論整套 Plugin 部署已任意可搬移。本輪不增加停用 Terminal／
Socket Shell 的設定。

完整父專案範例在 [examples/embedded_app](../../examples/embedded_app/CMakeLists.txt)：
`cmake -S examples/embedded_app -B build/my-app`，再 `cmake --build build/my-app --target shell`。
沿用框架工具鏈需求；輸出在 `build/my-app/open-vinc`（依生成器組態而定）。

Tcl 腳本與 Socket Shell 都是中央啟動時自動準備的預設功能，使用者只設定 CLI，
不呼叫初始化或服務啟停 API。中央使用 Shell 的 detail 接線標頭；SDK 只提供設定與錯誤型別。
建置時 `vinc_deploy_tcl(shell)` 跟隨執行檔實際輸出位置部署資源，包含父專案後設的輸出目錄。

## 入口與生成來源

- 中央啟動時亦掃描執行檔旁 `data/module/<ModuleType>/<ModuleType>.py`，不執行腳本。
  缺少目錄視為沒有 Python Module；個別登記問題回報後繼續啟動。
  `--python PATH` 指定系統 Python／venv，預設依 PATH 選取；放在 `-S` 之前。
  `VINC_BUILD_PYTHON=ON` 建置並部署 `python/vinc` 原生擴充與啟動腳本，不部署直譯器。
  詳見 [Python Module](../../src/vinc/module_system/python/README.md)。

- `parse_arguments()` 只解析 CLI，回傳啟動、顯示說明或參數錯誤的內部流程狀態。
  `-S` 路徑後的所有參數仍交給 Tcl；`--help` 在建立服務前退出。
- `run_services()` 依序完成 Tcl 初始化、Manager 建立、Shell 保留命令名稱、catalog 註冊、Python 掃描、
  Socket Shell 啟動及 Shell 執行的順序，逐步檢查共用 Result。
  Shell 透過 `create()` 建立並先於 Manager 銷毀；`run()` 也回傳 Result，
  服務故障須等既有完整關閉流程結束後才回到 `application()`。
  既有 STL／第三方例外邊界仍由 `application()` 保留。
- Windows `wmain()` 保留 UTF-16 到 UTF-8 的轉換邊界；字串配置完成後才建立借用指標陣列。
- `examples/module/example_module.cpp` 已展開命名 hook、命令迴圈及其他 hook；
  `--name` 仍只消耗第一組名稱參數，既有回傳與例外行為未改變。
- Catalog 由本目錄 CMake 的外層模板及 `src/vinc/module_system/cmake/VincModule.cmake`
  的註冊片段生成。模板先代入 metadata，再由 `file(GENERATE)` 解析組態對應的 Plugin 庫路徑；
  保留註冊順序，註冊錯誤以 Result 傳回；生成結果不應手動修改。

CLI 逾時上限、最小有效值及退出碼在入口內具名；連線數上限引用現有
`vinc::config::shell_socket_max_connections`。原有 CLI 數值未變動；Shell 的統一錯誤介面
及破壞性 API 變更見 [Shell README](../../src/vinc/shell/README.md#共用錯誤介面)。

## 驗證

### 內嵌啟動腳本（2026-10-14）

Windows x64／MSVC 19.51、CMake 4.3.2、Ninja：Debug／Release 完整建置成功，
兩種組態的相關 CTest 各 **12/12** 通過（93.30 秒／128.51 秒）。

- `app.embedding`：空腳本、UTF-8 BOM、CRLF／CR、中文及 emoji、非法／過長編碼、
  surrogate、截斷編碼及超過一般字串常值大小的腳本生成。
- `shell.tcl`：內嵌文字的獨立環境、`argv0`／`info script`、`app_dir`、原始行號、
  `return`、取消與 `exit`；腳本錯誤後 Module 仍存活且可繼續操作 Shell。
- `sdk.external_consumer`：父專案呼叫 CMake API、實際生成及執行 Unicode 內容、拒絕 `-S`、
  僅修改腳本的增量重建、非法編碼阻止 build 及修復後重建；搬移 EXE／Tcl 資源／相鄰腳本後，
  從不同 cwd 啟動並成功 `source`。此搬移案例不測試重新定位 Module Plugin。
- 其餘 Shell Socket／Console／Terminal／Integration／Faults／Unit、中央 Module／Bus／Bag
  腳本測試通過，原有外部腳本流程保持可用。

紀錄在 `build/startup-{debug,release}-final-tests.log`。Linux／macOS 本輪未實機驗證；
未重跑其餘不相關元件的完整 CTest。

2026-10-06 SDK 邊界與部署修正：Windows Debug／Release 的外部消費端均驗證
中央自動啟動 Tcl／Socket Shell、自訂輸出位置與搬移部署；既有中央模組／Bus 操作及
Shell 回歸也通過，範圍與結果見 [SDK 驗證](../../src/vinc/sdk/README.md#sdk-重審修正2026-10-06)。

2026-10-04 可讀性整理：Windows x64／MSVC 的 Debug、Release 完整建置通過，
兩種組態各有 `shell.console`、`module.shell`、`message_bus.shell` 三個整合測試通過。
生成 catalog 已確認包含原有兩個 ModuleType，且使用各自組態的執行檔路徑。

兩種組態各比對 34 組 CLI 輸入與整理前執行檔，退出碼、stdout、stderr 一致；
另驗證中文／空白腳本路徑及 `-S` 後包含空白、中文、`--help` 等參數的轉交。
本輪未重跑其餘 CTest；Linux／macOS 未驗證。

## UTF-8 路徑（2026-10-05）

`-S` 路徑改用 [OSAL path_from_utf8](../../src/vinc/osal/README.md#utf-8-與原生路徑)，
非法編碼會顯示具體錯誤並以參數錯誤結束啟動，不再建立 char8_t view。
Windows 的一般 argv UTF-16／UTF-8 轉換仍留在 wmain，不把一般參數當成路徑處理。
Windows Debug／Release 均已驗證中文／emoji 的部署目錄、Tcl 資源與腳本檔名。
Linux／macOS 尚未驗證。

## 待處理事項

### 外部 Module 註冊晚於中央 catalog 收集

2026-10-12 已調整接線：中央 catalog 收集延至最外層 CMake 目錄的正常配置結束，
此時重新讀取 Module 登記資訊，產生 catalog 並加入 Plugin 建置相依。
外部父專案在 add_subdirectory(open-vinc/src ...) 返回後呼叫 add_module 亦會納入。
實例預設名稱不再存放於 catalog，由 Module source 決定。
實際驗證結果見 [SDK README](../../src/vinc/sdk/README.md)。
