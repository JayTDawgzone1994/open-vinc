# macOS Param 乾淨 SDK 驗收（2026-11-03）

## 結果與受測版本

**Debug／Release 的全新 SDK producer 均建置成功；六次正式安裝驗收腳本全部通過。**
每種組態各執行 `installed_consumer`、`installed_application`、`installed_python` 三套，
不是六項 CTest，也不是重新跑過完整框架測試。

受測 commit：`9f0ec0bc9f1b3a7ef752c067adcd9de8427252a5`（`develop`，含 PR #24）。
工作分支：`codex/macos-param-sdk-acceptance-20261103`。
Plugin entry 13、bootstrap 16、Param wire v3；正式程式、測試 fixture 與建置設定均未改動。
本報告及後續文件整理不改變受測非文件內容。

| 項目 | Debug | Release |
| --- | --- | --- |
| 全新 SDK-only configure／build | 通過，build 39.99 秒 | 通過，build 41.17 秒 |
| `installed_consumer.cmake` | 通過，10.68 秒 | 通過，10.47 秒 |
| `installed_application.cmake`，`WITH_UI=ON` | 通過，80.76 秒 | 通過，83.33 秒 |
| `installed_python.cmake` | 通過，113.55 秒 | 通過，139.90 秒 |
| 三套安裝腳本 | **3/3** | **3/3** |

耗時只保留執行紀錄，沒有 benchmark。C++ consumer 腳本內部各執行四項 CTest，
包含 consumer 與 coordinator 的 standalone／shared／late；兩組各 4/4。
Python `flow-41`／`flow-42`、拒絕案例及 App 多次部署流程屬對應腳本內部案例，不另累加成獨立套件。

## 環境與隔離

| 項目 | 實測值 |
| --- | --- |
| OS／CPU | macOS 26.3.1(a)，build 25D771280a；Apple M1 Pro，arm64 |
| C++ | Apple Clang 21.0.0（clang-2100.1.1.101）；libc++ `_LIBCPP_VERSION=210106` |
| SDK／工具 | macOS SDK 26.5；CMake 4.4.4；Ninja 1.13.2.git.kitware.jobserver-pipe-1 |
| Python | CPython 3.12.14，64-bit、GIL-enabled、`Py_DEBUG=0`；由既有 runtime 建立 `中文 SDK venv` |
| 前端 | Node v24.22.0；既有 lockfile 對應的 node_modules |

來源快照位於 `/private/tmp/open-vinc-param-sdk-20261103/source`，全部追蹤檔案逐位元複製；
沒有 App Python 設定覆寫。固定 submodule 與 node_modules 由原 checkout 連結使用，版本記錄於
`environment.json`。兩組 producer、安裝目錄、consumer 與 Python venv 均建立於本輪 Git 外目錄。
不使用舊 producer／SDK／consumer 二進位，也不修改系統 SDK 登記或安裝位置。

兩組 producer 都設定 `VINC_INSTALL_SDK=ON`、`VINC_BUILD_APPS=OFF`、`BUILD_TESTING=OFF`，
未編譯 Python binding。各自 90 筆 C++ 編譯命令全部為 `-std=c++23`；
Debug 含 `-g` 且無 `NDEBUG`，Release 含 `NDEBUG`。下游驗收前的 producer 檢查確認
沒有 `_native*.so`，也沒有 `module_system/python/src/` 的編譯命令；
Python 是 SDK 安裝、搬移後才由下游啟用，產物位於獨立 consumer 子目錄。
SDK 製作可能使用建置工具直譯器，不把「未編譯 binding」擴張為系統完全沒有 Python。

建置平行度：producer 4、consumer 2；Debug／Release 與三套腳本依序執行。
環境設 `PYTHONMALLOC=debug`、`PYTHONFAULTHANDLER=1`、`PYTHONDONTWRITEBYTECODE=1`。
既有 Release 編譯警告保留於 build log，未在這輪修改。
沿用正式腳本的各命令期限（consumer 預設 90 秒、application 180 秒、Python 建置 240 秒、
應用執行 35 秒）；沒有調大這些期限。外層 driver 的整套逾時與實際命令保留於 `results.json`。

## 方法與功能證據

- **C++ consumer：** 安裝、搬移 SDK 後以 `find_package(vinc)` 連結預編譯庫，使用 installed
  Ribosome 生成訊息、執行 consumer／跨映像 coordinator、驗證增量標頭重建及授權檔案。
  正式負向案例涵蓋錯誤組態、安裝組態、版本、模擬編譯器 metadata、修改設定標頭及不相容重裝拒絕；
  不相容安裝不得覆寫原 package。模擬 MSVC metadata 只驗證 lookup 拒絕，並非原生 Windows 證據。
- **App：** 從搬移 SDK 建置，安裝並再次搬移 App，隱藏下游 build 與 SDK 後從不同 cwd 啟動。
  驗證自訂訊息 Bus、callback、Recorder、UI 解碼、兩個獨立自啟動 Host、完整程序退出。
  訊息 41→42、增加／刪除／無效 schema／修復、內嵌腳本修改、禁止腳本覆寫、資源補檔與使用者檔案保護均通過。
  build／SDK／App inventories 由正式 fixture 檢查；三輪 App 輸出均保留成功與收尾標記。
- **Python／Param：** 從 installed SDK 材料編譯原生 Plugin 與 Python binding，使用下游自訂訊息。
  C++／Python 皆驗證省略 options 的 `Limited + None`，首次 pump 前 `read_current()` 不填入快取。
  Python 另驗證 Entry 副本獨立、自己可寫 Limited、外部 Limited 寫入遭拒、Public 寫入保留 metadata。
  C++ Module hint 與 Python App hint 經 Host 傳遞，callback 保留標記。
  `flow-41.log` 與 `flow-42.log` 都包含 `NATIVE_PARAM_CURRENT_OK`、`PYTHON_PARAM_CURRENT_OK`、
  `NATIVE_PARAM_RESTART_OK`、`PYTHON_PARAM_RESTART_OK`，以及三個不同 central／native／Python PID。
- **Shell 與錯誤隔離：** 兩種語言的 get/list/set 顯示 Module／App 重啟標記與提醒，包含先 `param save`
  的提示；相同值 set 仍提示 metadata，不宣稱追蹤「尚待重啟」狀態。缺失 `--python` 路徑與錯誤 ABI
  各自拒絕 Python，原生 Module 仍回應 `NATIVE_ALIVE`，中央正常退出並回收全部子程序。
  自訂訊息 Bus／RPC、訊息新增／刪除／錯誤復原及獨立 FindPython Interpreter 共存也通過。

正式 fixture 最後停用 Python 並清掉受管理部署檔案；`otool -L` 檢查兩組下游 build 保留的
`_native` extension 仍使用系統 `libc++.1.dylib`／`libSystem.B.dylib`，
維持[已確認的 macOS Runtime 政策](../common/README.md#macos-system-runtime-decision)。
結束後檢查本輪路徑與記錄的 App／Host PID，沒有殘留測試程序；非文件來源 hash 與受測快照一致。

## 重跑入口

先準備本文記錄的工具鏈、固定 submodule 和 UI 相依；`SRC` 為完整 repository 來源快照，
`CMAKE`／`NINJA` 為所選工具的絕對路徑，`PYTHON` 為要由下游啟用的 CPython。
每種 `CONFIG` 必須選用全新的 `SDK_BUILD`，`APPLICATION_WORK` 也必須不存在。

```sh
"$CMAKE" -S "$SRC/src" -B "$SDK_BUILD" -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  "-DCMAKE_MAKE_PROGRAM=$NINJA" "-DCMAKE_BUILD_TYPE=$CONFIG" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DVINC_INSTALL_SDK=ON \
  -DVINC_BUILD_APPS=OFF -DBUILD_TESTING=OFF -DVINC_BUILD_BUS_BENCHMARK=OFF
"$CMAKE" --build "$SDK_BUILD" --parallel 4

"$CMAKE" "-DSDK_BUILD=$SDK_BUILD" -DCOMPILER=/usr/bin/c++ "-DCONFIG=$CONFIG" \
  "-DMAKE_PROGRAM=$NINJA" -DGENERATOR=Ninja \
  "-DFIXTURE=$SRC/src/vinc/sdk/tests/installed_consumer" \
  "-DWORK=$SDK_BUILD/sdk-install-test" -P "$SRC/src/vinc/sdk/tests/installed_consumer.cmake"
"$CMAKE" "-DSDK_BUILD=$SDK_BUILD" -DCOMPILER=/usr/bin/c++ "-DCONFIG=$CONFIG" \
  "-DMAKE_PROGRAM=$NINJA" -DGENERATOR=Ninja -DWITH_UI=ON \
  "-DFIXTURE=$SRC/src/vinc/sdk/tests/installed_application" \
  "-DAPPLICATION_WORK=$APPLICATION_WORK" -P "$SRC/src/vinc/sdk/tests/installed_application.cmake"
"$CMAKE" "-DSDK_BUILD=$SDK_BUILD" -DCOMPILER=/usr/bin/c++ "-DCONFIG=$CONFIG" \
  "-DMAKE_PROGRAM=$NINJA" -DGENERATOR=Ninja "-DPYTHON=$PYTHON" \
  "-DFIXTURE=$SRC/src/vinc/sdk/tests/installed_python" -P "$SRC/src/vinc/sdk/tests/installed_python.cmake"
```

## 證據與限制

Git 內保存本報告、元件索引與正式 fixture；原始紀錄放在
`/private/tmp/open-vinc-param-sdk-20261103`。完整命令／耗時／狀態：`results.json`；
環境／來源：`environment.json`、`tool-versions.json`、`stdlib.txt`、`source-sha256.json`；
producer 與功能檢查：`*-producer-audit.json`、`audit.json`、`process-audit.json`。
六份頂層 `*-{consumer,application,python}.log` 保存腳本結果，Python 詳細流程位於
`<Config>/producer/sdk-python-test/`，App 輸出位於 `<Config>/application/* app/PackageApp/test-output.txt`。

封存包：`/Users/jay/Downloads/open-vinc-param-sdk-20261103/evidence.tar.gz`；
同目錄 `SHA256SUMS` 與包內 manifest 用於核對完整性，原始輸出與建置產物不提交 Git。

本輪關閉新版 Param 的 **macOS 乾淨 SDK Debug／Release** 缺口；來源回歸沿用
[發布前各 96/96](../osal/MACOS_EXIT_VALIDATION.md#publication-validation) 的獨立證據，
沒有重跑完整框架 CTest、來源八次生命週期範例、多 App、原生 6,624 案或 benchmark。
不將舊版完整套件的歷史失敗改記為全綠，也不宣稱此次重跑 Windows／Linux。
Intel Mac、較舊 macOS、其他 CPython minor／Py_DEBUG／free-threaded、跨編譯器／SDK ABI、
互動 GUI／終端目視與效能／壓力上限均不在本次驗收範圍。
