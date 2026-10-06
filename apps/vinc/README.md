# Vinc 中央程式

本目錄負責 CLI、Module catalog 與中央服務的組裝。
Shell 邏輯仍由 `src/vinc/shell` 元件維護。
CMake target 為 `shell`，輸出程式仍為建置目錄中的 `Vinc.exe`／`Vinc`。
Module 範例先登記，再生成本程式的 catalog；生成檔在本次建置目錄內。

`VINC_BUILD_APPS=OFF` 可略過中央程式；`VINC_BUILD_EXAMPLES=OFF` 可略過內建範例及其 catalog 項目。
既有 CLI、socket 預設值與關閉行為未改變。操作方式見 [Shell README](../../src/vinc/shell/README.md)。

Tcl 腳本與 Socket Shell 都是中央啟動時自動準備的預設功能，使用者只設定 CLI，
不呼叫初始化或服務啟停 API。中央使用 Shell 的 detail 接線標頭；SDK 只提供設定與錯誤型別。
建置時 `vinc_deploy_tcl(shell)` 跟隨執行檔實際輸出位置部署資源，包含父專案後設的輸出目錄。

## 入口與生成來源

- `parse_arguments()` 只解析 CLI，回傳啟動、顯示說明或參數錯誤的內部流程狀態。
  `-S` 路徑後的所有參數仍交給 Tcl；`--help` 在建立服務前退出。
- `run_services()` 保留 Tcl 初始化、Manager 建立、Shell 保留命令名稱、catalog 註冊、
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

### 外部 Module 註冊晚於中央 catalog 收集（2026-10-06 CMake 重審）

**已確認，尚未修復：** `CMakeLists.txt` 在進入中央目錄時立即讀取
`VINC_PROCESS_TARGETS`／`VINC_PROCESS_CATALOG`，並將當時的內容交給 `file(GENERATE)`。
外部父專案在 `add_subdirectory(open-vinc/src ...)` 返回後才呼叫
`vinc_add_process_module()`，新 Module 不會進入中央 catalog，也不會成為中央 target 的建置相依。
延後寫檔本身不會重新讀取已展開的變數。

已讀取上一輪 `sdk.external_consumer` 的實際生成檔：外部 Module 存在，但中央
`register_built_modules()` 是空函式。原測試由消費端手動登記 Module，未覆蓋中央自動登記。
**未核准提案：** 在組裝階段結束後統一收集 catalog 與 target 相依，並讓外部測試從真正的
中央 Shell 啟動該 Module。須先確認組裝時機，再修改；不要求一般使用者操作 runtime 初始化。
