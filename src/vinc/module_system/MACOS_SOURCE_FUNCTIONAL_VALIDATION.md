# macOS 來源功能驗收（2026-11-11）

本報告是三階段 Mac 補驗的第二階段：來源 Python／GIL、parallel pump、RPC、Recorder
及 Log／網路／Publisher 三項 Param 遷移。第一階段 Host 收尾、受控與自然退出證據由
[Host 報告](MACOS_CONTEXT_SHUTDOWN_VALIDATION.md)維護；本輪不做 benchmark。

## 版本與環境

- 基底 `aab573100a231aa4b4e960fb9e9b49c47ad9d1d8`，加上下述兩項共用測試／範例修正。
  第一階段未提交文件一併保留；不把工作樹修正冒稱已包含在基底提交內。
- Plugin entry **19**、Module bootstrap **v16**、Bus control **v4**。
- macOS **26.3.1(a)**／build **25D771280a**，Apple **M1 Pro arm64**；未驗 Intel Mac。
- Apple Clang **21.0.0** (`clang-2100.1.1.101`)、libc++ **210106**、SDK **26.5**。
- CMake **4.4.4**、Ninja **1.13.2.git.kitware.jobserver-pipe-1**、CPython **3.12.14**
  64-bit，`Py_DEBUG=0`，一般 GIL build；Node **24.22.0**。依賴完整提交清單留在環境 JSON。
- Debug／Release 各有全新來源 build。每組稽核 **331** 筆 C++ 編譯均為 C++23、組態旗標正確，
  包含 **4** 筆 Python binding 編譯。各登記 165 項 CTest，本輪只選 **47** 項，不是全套驗收。

## 重建與執行方法

所有原始輸出位於 Git 外的 `/private/tmp/open-vinc-source-functional-20261111/`。
`source/` 是實際測試快照；`source-sha256.json` 保存修正前來源，`followup-source.patch`
及 `followup-source-hashes.json` 記錄兩項修正。函式庫使用已初始化的固定 submodule 提交。
最終稽核 781 個非 Markdown 輸入：repo 僅兩項預期修正，測試快照另有上述 App 設定覆寫；
repo 與實際測試快照的唯一非 Markdown 差異為 `apps/vinc/vinc.conf`。

快照的 `apps/vinc/vinc.conf` 額外啟用 Python/System、`userExamplePython`、`userManualPython`、
`parallelExample`、`parallelPython`；repo 的 App 設定未修改。
此設定建出來源 binding 與實際教學 Module，不是拿先前安裝的擴充套件代替。

```sh
cmake -S "$SOURCE/src" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DBUILD_TESTING=ON -DVINC_BUILD_APPS=ON \
  -DVINC_INSTALL_SDK=OFF -DVINC_BUILD_BUS_BENCHMARK=OFF \
  -DPython3_EXECUTABLE="$PYTHON"
cmake --build "$BUILD" --parallel 4
ctest --test-dir "$BUILD" -C Debug -j1 --no-tests=error \
  --output-on-failure --verbose --output-junit "$XML" -R "$EXACT_GROUP_REGEX"
```

Release 使用獨立 build 及 `Release` 組態。完整絕對工具路徑、精確正規式及每個命令見
`results.json`／`final-results.json`、`run_validation.py`／`run_final.py`。
環境設定 `PYTHONFAULTHANDLER=1`、`PYTHONMALLOC=debug`、`PYTHONDONTWRITEBYTECODE=1`。
所有程序測試串行，runner 有整組期限；沒有延長原測試觀察時間或弱化斷言。

## 首輪結果及根因

| 組態 | 來源 Python | Native | App／服務整合 | 首輪合計 |
| --- | --- | --- | --- | --- |
| Debug | **5/7**，71.49 s | **22/22**，67.96 s | **18/18**，101.96 s | **45/47** |
| Release | **5/7**，69.59 s | **22/22**，64.56 s | **18/18**，79.97 s | **45/47** |

兩組 configure／完整建置成功；首次建置為 Debug 99.75 s、Release 121.57 s。
相同的兩項失敗均保留原 log、JUnit 與 LastTest，不寫成首輪全部通過。

### Python Shell：同一暫存路徑的不同拼法

`python/tests/shell_test.py` 原先直接使用 `Path(temporary)`。
macOS 的 Python 暫存目錄為 `/var/folders/...`，中央掃描後的診斷包含 canonical
`/private/var/folders/...`；`samefile()` 證實兩者指向同一位置。
測試要求撞名拒絕訊息包含未正規化路徑，造成字串斷言失敗。

原始程序輸出顯示 `WEBUI` 撞名被拒、衝突入口未執行、合法 Python Module 正常啟停，
中央存活且完成清理。隔離副本只改成 `Path(temporary).resolve()`，Debug／Release
完整 Shell 腳本均通過；因此套回同一份跨平台 fixture，沒有新增平台條件。

### Parallel 教學：status 留在 C++ stdout 緩衝區

`module.python_parallel` 的八個生命週期 profile 已通過，最後實際教學互通案例卻無法在
原有 15 秒內看到 `parallelCpp received=4`；Python 的 `parallelPy received=4` 已出現。
外部診斷副本在失敗時保存已讀文字，正常停止後再排空剩餘輸出，且仍回報原失敗：
兩組都是停止前缺少 C++ status、停止後才出現 C++ `received=4`。
這證明 callback 已處理四筆，延遲的是觀察用文字，未證明 scheduler／GIL 故障。

C++ 教學 `parallel_module.cpp` 的 `status` 只輸出 `\n`；stdout 接到管道時換行不保證 flush。
隔離控制只在該行加 `std::flush`，使用原測試執行檔、原 binding／Runtime、原 15 秒斷言，
Debug／Release 完整 parallel suite 均通過。正式套回相同一行修正；不改 Runtime／派送契約。
診斷用測試副本沒有加入 repo，也沒有把預期失敗的診斷命令算成通過。

## 最終補驗

| 組態 | Python | Native | App／服務整合 | 合計 |
| --- | --- | --- | --- | --- |
| Debug | **7/7**，65.94 s | **22/22**，54.07 s | **18/18**，92.33 s | **47/47** |
| Release | **7/7**，62.25 s | **22/22**，52.28 s | **18/18**，71.7 s | **47/47** |

兩項修正套回 repo 與測試快照後，兩組重新建置，再各跑全部選定的 47 項。
先前補驗 runner 曾試圖以 archive／binding SHA-256 不變來重用 40 項成功結果；
Debug 建置成功，但部分產物重建使該 guard 拒絕重用，runner 在開始測試前中止。
此為驗收編排的保守檢查，保留 `Debug-followup-build.log`／`followup-results.json`；
沒有視為 Runtime 測試失敗，也沒有跳過 guard 後直接宣稱成功，而是擴大重跑全部 47 項。

## 覆蓋與斷言

| 群組 | 實際覆蓋 |
| --- | --- |
| Python，7 項 | `examples.user_module`、`module.python/messages/services/shell/parallel`、`rpc.python`；來源重編 binding、真實 C++↔Python 訊息、RPC、Param、錯誤隔離、生命週期及中文部署路徑。 |
| Native，22 項 | `module.context/context_wait/runtime_faults/parallel/parallel_stop/topic_scheduler/plugin/publisher_defaults`、`message_bus.cleanup/defaults_process`、`param.values/process/protocol`、九項 native `rpc.*`。 |
| 整合，18 項 | 四項 `app.*`、四項 `bag.*`、四項 `log.*`、四項 `shell.*` 與兩項 `ui.*`。精確案例清單見每組 `selection.json`／JUnit。 |

- Parallel Python 是一項 CTest，內含 FrameworkLoop normal／idle、Manual normal／idle／return／fault／stall
  與非法 worker 數八個 profile，再跑真實 C++／Python 教學。包含 GIL 等待／drain、callback fault、
  停止期限、無關 native peer 存活、保留訊息與雙 Topic 四筆收送；不把內部 profile 膨脹成額外 CTest。
- 全部十項 RPC（九 native 加一 Python）檢查同步／非同步、固定容量、傳輸／handler 故障、
  target exit、同名新 MID、回收及 callback／GIL 生命週期；無效 Plugin／Context 回歸同組重跑。
- `bag.loop` 受控驗證忙碌不等、空閒等待、停止、探索期限與故障不餓死。
  `bag.module` 透過真實 Plugin／Bus／worker 錄製 512 筆，逐筆核對內容及順序，
  分開要求 Bus lost／queue dropped／oversized 為零；保留故障、容量及排空案例。
- Log：Limited／App、最後合法容量、desired/applied、save／重啟、停用檔案與計數、
  設定讀取失敗及 reopen；`app.entry` 也檢查三次檔案 Log 啟停流程。
- 網路：31410／31411、CLI／檔案／預設優先序、非法 IP、IPv6／port 0、連線持續、
  UI 同 MID disable／enable 套用新埠，以及占用埠時保留設定。
- Publisher：兩項預設、部分／完整覆寫、最後合法值修正、已建立 Publisher 不變、
  真實預算耗盡，以及 Python `None`／整數選項。

## 交付與限制

原始證據包：`/Users/jay/Downloads/open-vinc-source-functional-20261111/evidence.tar.gz`。
同目錄 `SHA256SUMS` 可核對壓縮包；包內保存基底／最終結果 JSON、JUnit、逐組 log、
環境／編譯／來源稽核、執行腳本、隔離診斷副本及最小修正 patch。
最終程序稽核無本輪測試子程序殘留，31410／31411 均無 listener。

報告與兩項可攜性小修可納入 repo；原始輸出、測試副本及壓縮包留在 Git 外。
第一階段的 165 項全套結果仍只對應其原始來源與設定，不冒稱已在這兩項修正後重跑全套。

本報告當時未含乾淨 SDK-only；後續[第三階段](../sdk/MACOS_SDK_DEPLOYMENT_VALIDATION.md)
已補新的 Debug／Release producers、公開標頭、consumer／application／python、搬移與隱藏原路徑、
分類部署及輸出目錄。各輪結果分開維護，不回溯擴大本報告的來源測試範圍。
本輪沒有執行 Windows 專用 Runtime／metadata 測試、其他 Python 版本、其他 Mac 架構、
benchmark、CPU／延遲量測，亦未宣稱 Windows／Linux 重跑了兩項小修。
