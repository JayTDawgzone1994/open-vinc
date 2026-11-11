# macOS 乾淨 SDK／搬移部署驗收（2026-11-11）

這是三階段 Mac 補驗的第三階段：以新 SDK-only producer 驗證下游 C++／Python／App、
多 App、Module 分類部署、輸出目錄、搬移及清理。來源功能由
[第二階段](../module_system/MACOS_SOURCE_FUNCTIONAL_VALIDATION.md)維護；Host 收尾與自然退出
由[第一階段](../module_system/MACOS_CONTEXT_SHUTDOWN_VALIDATION.md)維護。
本輪不做 benchmark，不把驗收耗時當成效能結果。

## 受測來源與環境

- 基底 `aab573100a231aa4b4e960fb9e9b49c47ad9d1d8`，加上第二階段的兩項共用修正：
  Python Shell fixture 的 canonical 暫存路徑、C++ parallel 教學 status 的 flush。
  本輪沒有另改 repo 內框架、測試 fixture 或正式 CMake 實作。
- Plugin entry **19**、bootstrap **v16**、Bus control **v4**。舊 entry-13 SDK 報告保留歷史範圍，
  不用來替這次的 parallel／wait／RPC／Param 遷移背書。
- macOS **26.3.1(a)**／**25D771280a**，Apple **M1 Pro arm64**；Apple Clang **21.0.0**
  (`clang-2100.1.1.101`)、libc++ **210106**、macOS SDK **26.5**。
- CMake **4.4.4**、Ninja **1.13.2.git.kitware.jobserver-pipe-1**、CPython **3.12.14**
  64-bit／一般 GIL／`Py_DEBUG=0`，Node **24.22.0**。
- 固定 submodule 的精確提交、工具版本、來源 hash、工作樹 patch 與實際命令均保留於 Git 外證據。

## 乾淨 producer 與隔離方法

Git 外工作根為 `/private/tmp/open-vinc-sdk-deployment-20261111/`。
`source/` 複製本輪追蹤來源與報告，固定 submodule／既有 node_modules 以連結使用；
沒有 App Python 設定覆寫。各組態使用獨立新 build，不沿用前兩階段 producer／SDK 二進位。
另外建立 `中文 SDK venv`，由安裝、搬移後的下游啟用 Python。

```sh
cmake -S "$SOURCE/src" -B "$SDK_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DVINC_INSTALL_SDK=ON -DVINC_BUILD_APPS=OFF -DBUILD_TESTING=OFF \
  -DVINC_BUILD_UI=ON -DVINC_BUILD_BUS_BENCHMARK=OFF \
  -DPython3_EXECUTABLE="$TOOL_PYTHON"
cmake --build "$SDK_BUILD" --parallel 4
```

Release 使用另一個 build 並指定 `Release`。各 producer **96** 筆 C++ 編譯均為 C++23，
Debug／Release 旗標正確；在任何下游測試前，確認沒有 Python binding 編譯命令、
沒有 `_native*.so`、沒有內建 App，也沒有舊 `apps/vinc` 工作目錄。
這不表示製作 SDK 完全不用 Python：設定／生成工具仍可使用直譯器；交付的是下游 binding 建置材料。

驗收使用維護中的 `installed_consumer/application/python/publisher_defaults/recorder.cmake`、
`multi_app.py`、`system_apps.py` 與 Python Module 宣告／掃描測試。
SDK／App 安裝在本輪私有目錄，不更動系統 SDK 登記、系統 PATH 或使用者正式部署。
各次部署／consumer／App 為新目錄，兩組主要驗收與需要固定埠的執行期測試均串行。

## 實際結果

| 腳本／步驟 | Debug | Release |
| --- | --- | --- |
| 全新 SDK-only build | 通過，46.01 s | 通過，46.48 s |
| C++ consumer | 通過，12.12 s | 通過，11.28 s |
| App／Log／UI | 通過，91.82 s | 通過，99.89 s |
| 下游 Python／parallel／wait | 通過，194.19 s | 通過，247.16 s |
| Publisher 專用 App | 通過，26.75 s | 通過，28.29 s |
| Recorder 專用 App | 通過，25.75 s | 通過，26.79 s |
| 來源多 App | 通過，198.74 s | 通過，217.91 s |
| installed SDK 多 App | 通過，146.54 s | 通過，151.58 s |
| 搬移 C++ System Apps | 通過，92.45 s | 通過，94.27 s |

上表是手動呼叫既有驗收腳本，不是重新登記或重跑完整專案 CTest。
C++ consumer 腳本內部有四項 CTest，各組 4/4；不與外層腳本相加。
Python profile／多次 App 啟動、schema 與負向案例也是所屬腳本內部案例。
Python Module 宣告另有 **13/13** 單元案例，不依組態重複累加。

上表主要驗收腳本首次執行均通過；負向案例的預期拒絕不是整套失敗。
額外掃描 probe 首次選到仍帶內嵌腳本的 Third，`-S` 被契約正確拒絕。這是暫存 driver 的前置順序錯誤，
不是 SDK 功能故障；原始 driver 的錯誤輸出 `extra-driver.log`、擷取檔 `Debug-source-python-discovery-initial-captured.log`、
`extra-results-initial.json` 均保留。完成四份非內嵌 Third 建置後，才開始掃描補驗與剩餘步驟，
不修改原掃描斷言、`-S` 規則或期限。最終額外 38 個命令步驟的證據由 Debug 原有 9 個
成功且不受此 fixture 影響的步驟，加上後續 29 個成功步驟組成；不是聲稱 38 個全部重跑或首次全過。
第二階段的兩項原始失敗仍由其報告保留，不能把整個三階段寫成首次全過。

## SDK 與執行期契約證據

- **C++ consumer：** 移動 SDK，以 `find_package(vinc)` 建置外部程序與跨 image coordinator。
  使用 installed Ribosome、自訂訊息、增量標頭重建；核對授權，拒絕錯誤組態／安裝組態、
  版本、編譯器 metadata、修改設定標頭及不相容重裝。模擬 MSVC metadata 不是 Windows 原生證據。
- **單一 App：** 從移動 SDK 建置、安裝並再搬移 App，隱藏原 SDK／consumer 後由無關 cwd 執行。
  驗證 Bus、callback、Recorder、自訂 UI 解碼、真實 HTTP 31411、Shell CLI port 0、
  兩個獨立 self-host Module PID 及完整退出。SDK／App inventory 各只允許一個程序執行檔。
  包含 embedded/plain startup、訊息增刪／非法 schema／恢復、資源補檔與使用者檔案保護。
- **Log Param：** installed application 另跑三次 Log 生命週期，驗證保存停用→停用時保存啟用→
  重啟後啟用，停用期間檔案清單／hash／mtime 保留，reopen 不繞過啟動政策。
- **Python：** SDK 製作時不綁定 Python ABI；搬移後才編譯 C++ Plugin 與 `_native`。
  中文／空白 Custom 路徑、外部 FindPython venv、Bus／同步與可重複 RPC Client、Param current／
  快取與 restart hints、三個不同 PID，以及 ABI／缺失 override 的故障隔離均由正式腳本驗證。
- **parallel／wait：** 搬移的 C++ 與 Python 各啟用兩個 Topic workers，受控交會要求跨 Topic 真正重疊，
  保留獨立訊息直到停止。flow-41／flow-42 均要求 `INSTALLED_CPP/PYTHON_PARALLEL_OK`、
  `INSTALLED_CPP/PYTHON_WAIT_OK`、RPC／Param 成功及完整程序回收；不以輸出存在代替腳本內的順序斷言。
- **Publisher／Recorder：** 專用搬移 App 驗證自訂訊息、原有／新預設容量、最後合法值修正、
  Publisher 不被事後設定更改，以及 Recorder wait 接線、64 筆錄製、finish／程序退出。
  512 筆逐筆內容／順序與受控 loop 分支由第二階段來源測試補齊，不冒稱本腳本錄了 512 筆。

## 分類部署與多 App

來源與 installed SDK 的 `multi_app.py` 各在兩組態執行，直接核對跨子目錄宣告的
First／Second／Third 實際輸出為 `<top-build>/output/<App>/<configuration>/`。
只有選入 Module 進入各自 catalog／部署；C++-only App 不建 Python，另外兩個 App
共用相容 binding 與 Module target，但各有獨立副本、腳本／身分與官方 UI 接線。

實際啟動、安裝／搬移、同時運行、同名排他、官方 UI BindFailed／Bag 缺庫／載入停滯、
所有權與越界路徑拒絕、資源修復、取消選用、保留使用者修改與手動 Module、兄弟 App 隔離
均由完整腳本驗證。`system_apps.py` 另驗兩個搬移 C++ App 的真實 HTTP 衝突、限流與恢復、
不同 MID／graph 身分、Recorder 持續錄製，以及停止一個 App 不影響另一個。

來源／installed 的 System-mode Third 套件另重用完整 Shell discovery 測試：複製完整套件後
由無關 cwd 啟動，檢查手動 Module、大小寫撞名、忽略並保留舊 `data/module`、非遞迴掃描、
PATH 無效候選／空段／顯式 cwd、中文 venv、ABI／匯入失敗及 Context 撤銷／正常回收。
此項使用第二階段已修正 canonical 路徑的共用 fixture，不增加平台特例。
多 App 原始 Third 有內嵌啟動腳本，不能使用 `-S`；因此在主要多 App 驗收完成後，
僅將暫存副本的 Third 改以不帶 `STARTUP_SCRIPT` 的既有 `vinc_add_app()` 宣告並重建，
保留 System 模式與原相依，再執行未改動的掃描腳本。原宣告、準備腳本及每次命令／結果均留存。

## 公開標頭、輸出目錄及舊資料

| 項目 | Debug | Release |
| --- | --- | --- |
| 搬移 SDK 的獨立標頭 smoke（含 boundary） | 58 個編譯單元通過 | 58 個編譯單元通過 |
| 內建 App 新輸出／工作目錄、重配舊資料保留、實際啟停 | 通過 | 通過 |
| 來源／installed 的自訂輸出與舊資料保留 | 各通過 | 各通過 |
| 來源／installed Third 的完整 Python discovery | 各通過 | 各通過 |

標頭 probe 使用從現有 `sdk/tests/CMakeLists.txt` 讀出的完整 `public_headers` 清單，
每個標頭獨立作為第一個 include，並複製原 `public_boundary.cpp`。
外部專案只連結搬移 SDK 的 `vinc::sdk`，不把 producer 的 include 路徑加回去。
此本輪暫存 driver 不成為新的 repo 維護規則。
初次外部 header probe 未顯式關閉 CMake 語言擴充，編譯旗標為 `gnu++23`；
稽核後在暫存專案補上 C++23 required／extensions OFF，兩組各 58 個單元以 `-std=c++23`
重新編譯並核對實際命令，初次旗標另存 JSON。不把初次寬鬆旗標當作嚴格 C++23 證據。

內建來源 App 使用本輪另一份全新 build，實際 target 在 `output/Vinc/<configuration>/Vinc`，
工作資料在 `intermediate/vinc`；首次 build 不建立舊 `apps/vinc`。
之後在舊目錄放入受控參數／Log／錄製／手動 Module 標記，重配、重建並正常啟停 App，
確認舊檔 hash 不變、新套件未搬入這些檔案。

另外在兩種整合模式切換 First 到含中文／空白的自訂輸出目錄，核對實際 target、重新建置及
正常執行。原輸出目錄的資料仍保留，未被自動移入新位置；probe 最後恢復原 CMake 宣告。
以上暫存 probe 只位於 Git 外，不修改正式 App recipe。

## 稽核、交付與限制

最終核對 **781** 個非 Markdown 輸入：repo 與實際受測快照均無非預期差異。
兩組新下游 `_native` 均為 arm64 並動態依賴系統 libc++／libSystem，維持
[既定 Mac Runtime 政策](../common/README.md#macos-system-runtime-decision)。
本輪路徑無殘留 App／Host／Python Module 程序；31410／31411 無 listener。

原始證據包：`/Users/jay/Downloads/open-vinc-sdk-deployment-20261111/evidence.tar.gz`，同目錄 `SHA256SUMS`。
包內保存 runner、實際命令／結果／log、來源／環境／編譯／流程與清理稽核、inventory、
SDK 內部流程紀錄及本報告；二進位與完整 build 留在上述 Git 外工作根，未加入 repo。

本輪只驗 Apple Silicon／CPython 3.12.14／同一原生工具鏈及各自組態；沒有證明 Intel Mac、
其他 macOS／Python minor、跨編譯器 ABI 或 Ninja Multi-Config 相容性。
Windows 特有 CRT／舊 MSVCP／WinRT 及 Linux glibc 部署不在本輪，不能記成 Mac 的 skip/pass。
未重跑全專案 CTest、benchmark、成本矩陣、CPU／吞吐／延遲或真實磁碟斷電驗證。
固定 HTTP 埠的跨測試隔離提案仍是獨立 TODO；本輪串行與埠稽核不表示已修復該測試設計缺口。
