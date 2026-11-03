# macOS App／Python／SDK 驗證（2026-11-02）

測試版本：`627c409423fd9bacb2de504a079da9f86120c0ad`（當輪取得的 `develop`）。
測試分支：`codex/macos-develop-validation-20261102`。
本文件整理既有實測證據；加入文件不代表重新執行測試或套用了外部修正。

## 結果與狀態

**兩種組態完整建置成功；原始 CTest 各 125/126 通過，並非全綠。**
唯一失敗為 `examples.user_module` 的暫存部署漏複製資源。
同一份執行檔在外部副本補齊資源後，Debug／Release 的範例皆通過；
此對照不能計成原始 CTest 的 126/126；該測試版本尚未套用 repository 修正。
後續正式 fixture 與新版 Param／重啟標記（Plugin entry 13、Param wire v3）已有 Mac
[來源回歸](../osal/MACOS_EXIT_VALIDATION.md#publication-validation)及
[Debug／Release 乾淨 SDK 驗收](MACOS_PARAM_VALIDATION.md)。
本報告保留舊版本原始結果；下表不改記為新版標記或後續修復的驗收證據。

| 驗證 | Debug | Release |
| --- | --- | --- |
| 完整來源建置，內建 App 啟用 Python | 通過 | 通過 |
| 原始完整 CTest | 125/126，890.62 秒 | 125/126，898.43 秒 |
| 來源 `multi_app.py` 完整案例 | 通過 | 通過 |
| Python 關閉的乾淨 SDK producer | 通過 | 通過 |
| 乾淨 SDK 的原生／Python／Application／多 App 下游驗收 | 四組通過 | 四組通過 |
| 外部範例副本補齊部署資源 | 通過，同一份執行檔 | 通過，同一份執行檔 |

耗時只記錄執行情況，不是 benchmark。其他工具檢查：設定解析 **17/17**、
Python 選擇／ABI 探測 **24/24**；工具測試共 **118** 項，**95 通過、23 平台略過**，無失敗。
設定測試亦包含在 CTest 內，不將重複執行累加成不同案例數。

## 原生環境與方法

| 項目 | 實測環境 |
| --- | --- |
| OS／CPU | macOS 26.3.1(a)，build 25D771280a；Apple M1 Pro，arm64 |
| C++ | Apple Clang 21.0.0（clang-2100.1.1.101）；libc++ `_LIBCPP_VERSION=210106` |
| SDK／建置工具 | macOS SDK 26.5；CMake 4.4.4；Ninja 1.13.2.git.kitware.jobserver-pipe-1 |
| Python | CPython 3.12.14，64-bit、GIL-enabled、`Py_DEBUG=0` |
| 前端建置 | Node v24.22.0；既有 lockfile 相依 |

測試使用外部原始碼快照與全新建置目錄。唯一來源差異是快照的
`apps/vinc/vinc.conf` 啟用 `CONFIG_VINC_APP_PYTHON=y` 與
`CONFIG_VINC_PYTHON_SYSTEM=y`；repository 的預設設定未更動。
固定版本的上游相依及 `node_modules` 連結至原有目錄，版本記錄於 `environment.json`。

完整 producer 啟用 `VINC_BUILD_APPS`、`VINC_INSTALL_SDK`、`BUILD_TESTING`，
停用 `VINC_BUILD_BUS_BENCHMARK`。使用 Ninja、建置平行度 4、CTest 平行度 1；
主矩陣設 `PYTHONMALLOC=debug`、`PYTHONFAULTHANDLER=1`。
每種組態的 291 個 C++ 編譯命令皆使用 `-std=c++23`；Debug 全含 `-g` 且無
`NDEBUG`，Release 全含 `NDEBUG`。

測試期間使用暫時 `caffeinate -i`；未更動永久電源設定。結束後確認無本輪殘留程序，
原始追蹤檔雜湊一致。這是加入本報告前的工作樹驗證，不表示文件加入後仍無差異。

## 本輪待驗項目對照

| 項目 | 方法與通過範圍 |
| --- | --- |
| App 組裝統一 | 內建 App、來源及 installed SDK 共用 `vinc_add_app()`；獨立輸出／catalog／身份、共用 Plugin／binding、晚登記訊息、未選 target 排除、選擇清理、內嵌腳本及增量重建。 |
| 部署與 System Module | 安裝／搬移、同名排他、不同 App 並行及 HTTP graph 隔離；真實 port 衝突、限流及時間窗後恢復、官方載入故障／停滯、錄製與關閉隔離、使用者及 sibling App 檔案保護。 |
| Publisher 身分 | `module.python_services` 六情境：同號、異號、錯誤預期身分、Param／Bus／VincError callback 故障；檢查 peer 存活、收尾與參考釋放。兩組實際同號為 Publisher=2、Python MID=2；正常異號為 Publisher=1、MID=4。 |
| 建置 ABI 探測 | 真正 CMake helper 的 24 項測試：實際 10 秒逾時、無效／缺欄位／型別錯誤 JSON、非零退出、每 App／configure 重新探測、FindPython 共存／衝突、venv、cache 保護、patch 更新及 binding 重用探測結果。 |
| System Python 搜尋 | `osal.path`、`sdk.python_selection`、`module.python_shell`：不可執行檔、目錄、失效 symlink、空 PATH 項目、明確相對目錄、中文 venv、明確選擇及選定後失敗不回退。 |

上述功能在兩種組態均有通過證據；不因此隱去範例 smoke fixture 的原始失敗。
六份來源／installed binding 的 `_build.py` 與實際 Python minor、位元數及平台一致。
`otool -L` 顯示 extension 使用系統 `libc++.1.dylib`／`libSystem.B.dylib`，
維持 [macOS Runtime 決定](../common/README.md#macos-system-runtime-decision)。

## 乾淨 SDK 交付

另建 Debug／Release producer，設定 `VINC_BUILD_APPS=OFF`、`BUILD_TESTING=OFF`、
`VINC_INSTALL_SDK=ON`，沒有任何啟用 Python 的 App。兩組各 92 個編譯命令，
在執行下游測試前確認沒有 Python adapter 編譯或 `_native.so`。
先完成全新 producer 建置，再重用該 producer 執行下表；後續增量 build 時間不代表乾淨建置時間。

| 下游案例 | Debug | Release | 覆蓋 |
| --- | --- | --- | --- |
| `installed_consumer.cmake` | 通過，8.46 秒 | 通過，8.33 秒 | 原生 SDK 搬移、執行、相容性拒絕及重建。 |
| `installed_python.cmake` | 通過，110.23 秒 | 通過，137.56 秒 | 下游才啟用 binding、中文／空白路徑 venv、另一使用端先 FindPython、自訂 Bus／RPC／Param、schema 增刪改、搬移、ABI／缺直譯器隔離、修復與停用清理。 |
| `installed_application.cmake` | 通過，80.72 秒 | 通過，83.58 秒 | 每 App 單一執行檔、self-Host、搬移與暫藏建置輸入、Bus／Bag／UI、自訂訊息與拒絕案例。 |
| installed `multi_app.py` | 通過，128.39 秒 | 通過，135.05 秒 | 多 App 的共享、隔離、獨立部署、故障處理與選擇清理。 |

## 範例測試失敗與外部對照

問題及後續工作由 [範例 README](../../../examples/userExampleModule/README.md#macos-deployment-fixture-gap)
維護。`tests/smoke.py` 原本只複製執行檔、根目錄 DLL、`lib`、`python/vinc` 與 Python 範例，
缺少相對 catalog 所需的 `modules/`，也沒有官方 UI 資源。
保留的 Debug `result.txt` 顯示
`Shell/StartModule → ModuleSystem/LoadPlugin → OSAL/OpenLibrary`；兩組原始 CTest 都失敗。

外部對照只補複製 `modules/` 與 `vinc_data/open-vinc/ui/`，沿用原範例斷言，
沒有修改 Runtime、catalog 或 Plugin。兩組均印出 `C++ / Python user examples passed`
並確認所有 Module 程序退出。原執行檔與各自對照副本 SHA-256 完全一致：

| 組態 | 執行檔 SHA-256 |
| --- | --- |
| Debug | `40b50682a8007d3885f4cee984cce692a5cecb4d3075a5bf709b712ce1a118e4` |
| Release | `17e636956f2edb07c9b0ff51fc6004ead00d28af5f0e9fc45e1a43058da25567` |

外部 `proposed-example-fixture.patch` 已通過 `git apply --check`，但**未套用**；
本次只在 macOS 驗證對照，不宣稱 Windows／Linux 已驗證該修正。

上述是當時紀錄。2026-11-03 維護中的 fixture 已修復並通過 macOS Debug／Release
定點補驗，詳見 [範例 README](../../../examples/userExampleModule/README.md#macos-deployment-fixture-gap)；
本報告的原始失敗與外部比較均保留。

## 重跑與證據索引

使用同一 commit 的外部快照及上述設定，分別令 `config=Debug`／`Release`。
`python`、`cmake`、`ninja` 應選為環境表對應工具，PATH 的第一個 `python3` 為選定直譯器。
下列變數分別代表外部來源與建置目錄：

```sh
cmake -S "$source/src" -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE="$config" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DVINC_BUILD_APPS=ON -DVINC_INSTALL_SDK=ON -DBUILD_TESTING=ON \
  -DVINC_BUILD_BUS_BENCHMARK=OFF -DPython3_EXECUTABLE="$python" \
  -DVINC_TEST_SDK_PYTHON_EXECUTABLE="$python"
cmake --build "$build" --parallel 4
PYTHONMALLOC=debug PYTHONFAULTHANDLER=1 ctest --test-dir "$build" \
  --parallel 1 --output-on-failure --output-junit "$build/results.xml"
```

完整實際命令、乾淨 SDK 參數與外部對照 helper 都保存在紀錄包；
重跑原始版本應保留範例失敗，不應先套用 patch 再稱為原始結果。

紀錄包未加入 Git，也不是公開下載網址。驗證主機本機位置：
`/Users/jay/Downloads/open-vinc-macos-validation-627c409-20261102/evidence.tar.gz`。
大小 **6,046,896 bytes**；匯出時逐一核對 **1,684** 個 payload 檔案。
SHA-256：`5322a07b67797154ebbc4804ef063366b42ffcc60dea7c405046a59adfd01390`。
其他機器需要另外取得此包，不能只靠 repository 讀取原始 logs。

| 包內檔案 | 用途 |
| --- | --- |
| `environment.json`、`source-sha256.json` | 環境、submodule 版本與原始檔雜湊。 |
| `results.json`、`clean-sdk-prepare-results.json`、`clean-sdk-results.json` | 實際命令、各步退出碼與耗時。driver 完成不等於所有測試成功。 |
| `Debug-ctest.xml`／`.log`、`Release-ctest.xml`／`.log` | 各一項失敗的原始完整 CTest。 |
| `Debug-publisher-identity.log`、`Release-publisher-identity.log` | 六情境身分與故障輸出。 |
| `Debug-source-multi-app.log`、`Release-source-multi-app.log`、`*-clean-*.log` | 來源及乾淨 SDK 的各階段結果。 |
| `Debug-example-baseline.log`、`Debug/example-baseline/result.txt` | 保留的原始漏檔重現。 |
| `*-example-complete-deployment.log`、`probe_example_deployment.py`、`proposed-example-fixture.patch` | 外部對照結果、手法及未套用建議。 |
| `audit.json`、`cleanup.json`、`SHA256SUMS.json` | 編譯旗標、metadata、Runtime 相依、程序收尾及包內完整性。 |

包內另有來源快照與驅動腳本；不含全部編譯產物、installed SDK payload、
`node_modules` 或上游 symlink 目標。完整建置資料當時保留於
`/private/tmp/open-vinc-macos-develop-20261102`；暫存位置不保證永久保存。
本報告未將先前 FD 競爭專用結果包移入 Git。

## 驗證限制

- 沒有 benchmark、Intel Mac、舊 macOS、其他 CPython minor、Py_DEBUG／free-threaded、
  任意第三方原生 extension 組合或互動 GUI／menu／browser 操作證據。
- 不替代 Windows／Linux 原生驗證，不據此推論最低 macOS／CMake 版本。
- 缺開發檔、patch 更新等部分負向案例使用既有 fixture 故障注入，
  並非卸除系統套件或實際切換所有 Python 版本。
- FD 建立／spawn 競爭、FileLock unlock 狀態與 Socket Shell 延後項目未修改；
  一般 OSAL 回歸通過不代表競爭已解決。
