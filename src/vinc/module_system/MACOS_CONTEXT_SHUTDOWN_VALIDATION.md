# macOS Host 收尾與 Context：第一階段驗收（2026-11-10～11）

## 結果與來源

**macOS Debug／Release Host 收尾核心、相關回歸及各自註冊的完整 CTest 均通過。**
受測 commit：`aab573100a231aa4b4e960fb9e9b49c47ad9d1d8`（`develop`，PR #32）。
與發布修復 commit `820d4ba` 的 Git tree 相同；不是交付文件建立時尚未含修復的 `6151381`。
工作分支：`codex/macos-context-shutdown-acceptance-20261110`。
驗收於 11 月 10 日啟動，11 月 11 日完成。本輪只執行既有測試並更新文件，沒有修改正式程式、fixture、框架期限、重啟政策或錯誤分類。

| 驗證 | Debug | Release |
| --- | --- | --- |
| 全新建置（全部預設 targets） | 通過，93.08 秒 | 通過，110.38 秒 |
| 相關 CTest | 47/47，158.15 秒 | 47/47，149.15 秒 |
| 受控重跑（每輪四情境） | 10/10 輪，15.88 秒 | 10/10 輪，15.57 秒 |
| 普通 Host Context 重跑 | 5/5，50.97 秒 | 5/5，51.26 秒 |
| 自然短命 Module | 100/100 正常，exit72=0，47.18 秒 | 100/100 正常，exit72=0，47.57 秒 |
| 完整註冊 CTest | 165/165，1128.52 秒 | 165/165，1166.12 秒 |

不同輪次分開記錄，不把 focused、full 與重複執行加總成不同測試數。
受控重跑每輪四個情境，10 輪即 40 個情境執行；不是 40 項獨立 CTest。
自然探針 100 次與 Context 重跑也不代表實際應用的故障率估計。

## 環境與建置範圍

- macOS 26.3.1(a)，build 25D771280a；Apple M1 Pro arm64。
- Apple Clang 21.0.0（clang-2100.1.1.101）、libc++ `_LIBCPP_VERSION=210106`、macOS SDK 26.5。
- CMake 4.4.4、Ninja 1.13.2.git.kitware.jobserver-pipe-1；Node／npm 版本與絕對路徑見環境紀錄。
- CPython 3.12.14，64-bit、`Py_DEBUG=0`、GIL enabled，供設定工具及 installed SDK Python 使用。
- 目前 Plugin entry 19、bootstrap v16、Bus v4。App／Host／Plugin／測試全部重新編譯，沒有混用舊 SDK。

來源為 `/private/tmp/open-vinc-context-shutdown-20261110/source` 的逐位元快照；
沒有覆寫任何追蹤設定。固定 submodule 與 UI node_modules 連結到已初始化的原 checkout，
版本及來源 hash 留在 `environment.json`／`source-sha256.json`。
兩個全新、互不共用二進位的 Debug／Release build 以 Ninja 建置，平行度 4；測試依序 `-j1` 執行，
沒有讓兩組固定埠測試或建置互相競爭。沒有操作其他 App 或永久電源設定；測試期間的
`caffeinate -i -w <driver-pid>` 隨 driver 結束。

配置明確開 `BUILD_TESTING=ON`、`VINC_BUILD_APPS=ON`、`VINC_INSTALL_SDK=ON`，
關閉 `VINC_BUILD_BUS_BENCHMARK`，並提供 installed Python 的直譯器。
沿用 bundled C++ App 的原 Kconfig，來源版 Python binding 保持停用；`module.python*`／
`rpc.python` 未註冊，不以它們「沒有失敗」宣稱已有覆蓋。`sdk.installed_python`、
`sdk.multi_app` 在兩組均實際註冊並執行，下游 binding 由各自 fixture 編譯。
這不是 `VINC_BUILD_APPS=OFF`／`BUILD_TESTING=OFF` 的 SDK-only producer 矩陣，
第二階段來源 Python 與第三階段乾淨交付項目仍保留。

各組 320 筆 C++ 編譯命令皆為 `-std=c++23`；Debug 含 `-g` 且無 `NDEBUG`，
Release 含 `NDEBUG`。`VINC_SHUTDOWN_TESTING` 僅出現在 test Host 的 `shutdown_host.cpp`／
`shutdown_bus.cpp`，未加到正式 Runtime／Bus 編譯單元；`sdk_header_smoke` 亦隨預設 targets 建置。
既有編譯警告保留，不因此修改程式或壓低檢查。

## 收尾契約證據

| 受控情境（每組各 10 次） | 應有結果；兩組皆逐次通過 |
| --- | --- |
| `before-close` | 有效通知在關閉前接受，hooks／completion 正常，exit 0、零重啟、無主要／清理故障。 |
| `during-teardown` | watchdog 通過 finished 檢查後，由 gate 控制收尾期間的有效通知派送；Bus 尚未關閉，receiver 先結束再退休 Bus，exit 0。 |
| `malformed` | Host 對畸形封包仍記錄 ModuleSystem BusFailed／Receive 與 MessageBus Protocol／Receive、exit 72；中央可保留該故障或實際 ProcessExited，不接受任意錯誤或吞成成功。 |
| `hook-error` | 真實 HookFailed 保留 exit 70，不得因收尾修復轉成成功。 |

協調使用維護中測試的有界 gates，不用固定 sleep 猜測關鍵順序。每個情境保留
MID、hooks／pre_teardown 結果、first_close、Bus receive、watchdog exit 及中央主要／清理故障。
Gate timeout／exit 99 會令測試失敗，不視為原缺陷重現。原舊順序對照已在 Windows 完成；
本輪沒有另改 Mac 正式程式或建立舊順序 mutation，不能宣稱做過 Mac 舊版 A/B 對照。

自然短命 Module 沿用 `loss` workload，不注入封包，停用自動重啟以保留首代結果。
Debug／Release 各 100 筆都確認：

- `scenario=natural ... exit=0 restarts=0`，summary 為 `natural observations=100 exit72=0`。
- 各 100 筆 `hooks_result=ok`、`pre_teardown_result=ok`、`receiver_exited_before_bus_shutdown=1`。
- 各 100 次 first-close 都是 `FinalTeardown`，`Other` 為零；沒有 `central_primary`、`central_cleanup` 或 `gate_timeout`。

探針命令退出碼 0 本身不夠；上述各筆內容及計數另由 `natural-audit.json`／`natural-first-close-audit.json` 核對。
受控 malformed 的預期 exit 72 與自然 workload 的非預期 exit 72 分開判讀。
`module.context` 另以普通 production Host 重跑五次，包含有限 pump batches、拓樸變動／退訂、
正常完成及首代退出檢查；不是只依靠 instrumented Host。

## 回歸、命令與清理

Focused 選擇 `^(module\.|message_bus\.|rpc\.|param\.|control\.)`，確認並非空選擇，
包含 shutdown_evidence、context、context_wait、parallel／parallel_stop／topic_scheduler、
process／runtime_faults／manager_faults、Bus error／cleanup／notification_snapshots，及 RPC／Param／control。
完整套件依 `*-inventory.json` 的註冊內容執行；不同平台的未註冊測試不算略過或通過。
`--no-tests=error` 避免空集合誤報成功，所有失敗保留首次輸出，沒有修改期限或斷言來重跑變綠。

完整實際命令、退出碼、外層界限及耗時統一見 `results.json`／`run_validation.py`；
可依 [Mac 交付清單](MACOS_CONTEXT_SHUTDOWN_HANDOFF.md) 重跑，加入本輪的 matching compiler、
Ninja／CPython 絕對路徑。新增的 `VINC_TEST_SDK_PYTHON_EXECUTABLE` 是測試配置選項，
沒有改 framework App 的 Kconfig。完整套件使用下列配置；`<python>` 選擇本文的 CPython，
兩組 `<build>` 必須是互不共用的新目錄：

```sh
cmake -S <snapshot>/src -B <build> -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_BUILD_TYPE=<Debug-or-Release> -DCMAKE_MAKE_PROGRAM=<ninja> \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBUILD_TESTING=ON \
  -DVINC_BUILD_APPS=ON -DVINC_INSTALL_SDK=ON -DVINC_BUILD_BUS_BENCHMARK=OFF \
  -DPython3_EXECUTABLE=<python> -DVINC_TEST_SDK_PYTHON_EXECUTABLE=<python>
cmake --build <build> --parallel 4
```

CTest／重跑及自然探針命令沿用上方交付清單，保留每組獨立輸出；不要把 source Python Kconfig
改為啟用後仍沿用本表的測試總數。工具 bin 目錄與 Node／npm 必須在啟動 CTest 的 PATH 中。

每種組態順序為 configure → 全部 targets build → focused → controlled 10 輪 → Context 5 輪 →
natural 100 次 → full。CTest 重複輪次使用 verbose 保存所有通過案例的 trace；每一步另複製
`Testing/Temporary/LastTest.log`，並輸出獨立 JUnit XML，不讓後續輪次覆蓋先前證據。
自然探針從實際 CTest JSON command 取得 Host／Plugin 路徑，不猜測副檔名。

測試前檢查 31410／31411 listener，兩埠無占用。測試自有 Manager／程序採原有有界 shutdown；
全部執行後再以本輪 build 路徑檢查沒有殘留 native App／Host／測試程序，listener 結果另存
`ports-final.txt`。未停止其他使用者程序，也未刪除不屬於本輪的共享資源。
非文件來源與受測 hash 一致；本輪文件更新不改受測實作。

## 證據與剩餘範圍

原始紀錄位於 Git 外 `/private/tmp/open-vinc-context-shutdown-20261110`：

- `environment.json`、`tool-versions.json`、`stdlib.txt`、`source-sha256.json`。
- `results.json`、兩組 inventory／compile audit、focused／full／repeat logs、JUnit 與各輪 LastTest.log。
- `*-natural-100.log`、`natural-audit.json`、`natural-first-close-audit.json`、`final-audit.json`、`process-audit.json`、埠檢查。

移交包：`/Users/jay/Downloads/open-vinc-context-shutdown-20261110/evidence.tar.gz`，
同目錄 `SHA256SUMS` 及包內 manifest 可核對。Git 僅保存報告及文件索引，不加入 raw logs 或 build。

本輪關閉 Host 收尾／Context 的 Mac 第一階段缺口，不關閉整個 parallel-pump 或新版所有功能的 Mac 驗收。
來源版 Python／GIL／引用生命週期及跨語言範例、SDK-only producer／分類部署／新輸出位置的專項交付
仍依後續兩階段與根目錄 TODO 追蹤；完整套件中有覆蓋的交集可在來源不變時引用，不必無理由重跑。
沒有 benchmark／pump cost probe、Intel Mac、舊 macOS／其他 Python minor 或真實應用故障率保證。

## 後續階段狀態（2026-11-11）

上方未涵蓋的來源 Python／GIL 由[第二階段](MACOS_SOURCE_FUNCTIONAL_VALIDATION.md)補齊；
新 SDK-only、搬移／分類部署及輸出位置由[第三階段](../sdk/MACOS_SDK_DEPLOYMENT_VALIDATION.md)補齊。
本報告的 165 項全套與自然退出仍僅代表原始受測來源／設定，不與後續腳本合併計數。
