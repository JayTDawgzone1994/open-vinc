# macOS exit repair: native and application acceptance

發布前已整合 develop `f16da98`（Param entry 13／wire v3）；整合後檢查另見
[發布前驗證](#publication-validation)。下方原生壓力、完整 CTest 與乾淨 SDK 數字
屬於合併前的受測版本，不自動涵蓋新版 Param ABI。

<a id="current-acceptance-status"></a>

## Current acceptance status / 目前驗收狀態（2026-11-03）

**macOS 退出修復及後續兩項測試修復已有原生證據；Windows／Linux 修復後驗證待補。**
這是不同輪次結果的索引，不是一次全綠的完整 CTest 報告。最後的變更只修訂範例測試腳本，
因此定點重驗該項；沒有在最後變更後重跑完整 154 項，也沒有做 benchmark。

| 輪次／受測內容 | 實際結果 | 目前狀態與詳細紀錄 |
| --- | --- | --- |
| 退出進度修復：原生退出／FD、來源 App／Python、乾淨 SDK | 原生 6,624/6,624；八組乾淨 SDK 下游通過；完整 Debug 151/153、Release 152/153 | 原始兩項測試失敗保留；方法與環境見本文後續章節 |
| Plugin 分類測試修復，新增中央保存順序及失敗清理契約 | 完整 Debug／Release 各 153/154；新路徑 10/10、同時執行 4/4、三個反向測試均抓到預期錯誤 | Plugin 缺口已修復；唯一剩餘範例部署失敗另列下一輪；見 [Plugin 報告](../module_system/MACOS_PLUGIN_CLASSIFICATION.md) |
| 範例暫存部署補齊 Plugin／UI／授權檔案 | `examples.user_module` Debug 1/1（9.06 秒）、Release 1/1（8.02 秒） | 原 C++／Python 斷言與新增 UI／Bag Ready 檢查通過；見 [範例紀錄](../../../examples/userExampleModule/README.md#macos-deployment-fixture-gap) |

最終範例補驗重用前輪 binaries；34 個 App／Module／Python 檔案的前後 hash 一致。
相較前輪受測非文件輸入，只有 `examples/userExampleModule/tests/smoke.py` 改變。
兩組輸出均確認全部 Module 退出，結束後的本案程序篩選沒有殘留。
這些結果關閉本輪 Mac 發現的兩項測試缺口，不承諾任意排程、其他 CPU／工具鏈或三平台均已驗證。

剩餘行動統一列於 [TODO](../../../TODO.md#osal-macos-退出事件後的回收進度)。
已完成的資源建立／spawn 競爭驗收仍見 [原生 FD 報告](MACOS_FD_VALIDATION.md)，
與後來的退出觀察缺口分開追蹤。Repository 保存本文件、元件報告及可重跑的測試；
原始 logs、JUnit、外部診斷副本和壓縮包留在 Git 外。

### Evidence archive index / 原始證據包索引

各檔案位於驗證主機 `/Users/jay/Downloads/<目錄>/evidence.tar.gz`。
SHA-256 用於移交時核對內容；只有 repository 的讀者仍可從上述報告查看方法與結果，
若需獨立檢查原始輸出，須一併取得對應證據包。這些既有包未因本次文件整理而覆寫。

| 目錄 | SHA-256 |
| --- | --- |
| `open-vinc-exit-step3-20261103` | `99168ba09c340ff33fc85eb871ba69a9593bb9cc1a7375074d462ccb61af875b` |
| `open-vinc-plugin-repair-20261103` | `20a1fd1443d7c83f115a12ae0fcde4810994fe426470ea201572db007f697e8c` |
| `open-vinc-example-deployment-20261103` | `7fb4d3849fa8e71ea2e442a2cc33dbd0570ab4dcc8e8445ae73a95abd6395c16` |

## Original step 3 scope / 原始第三步範圍（2026-11-03）

本文件維護退出進度修復的第三步 macOS 驗收。修復、生命週期設計與受控契約由
[退出進度紀錄](PROCESS_EXIT_PROGRESS.md)維護；修復前原生失敗由
[FD 驗收報告](MACOS_FD_VALIDATION.md)維護。兩份歷史結果不覆寫為本輪成功。

**第三步測試與報告完成，原始完整 CTest 並非全綠：Debug 151/153、Release 152/153。**
原生退出／FD 6,624 案、兩組 OSAL／App 子集與四項 Python Module 測試、來源 multi-App 與八組乾淨 SDK
下游驗收通過。當時保留範例部署 fixture 與一次未定位的 Debug Plugin 分類失敗；
後續兩項測試修復及各自證據見下文，不改寫本報告的原始全套結果。
原始第三步只執行測試、保存證據與更新文件，沒有再修改正式程式或維護中的測試；
後續 Plugin／範例測試修復是上方另列的授權輪次。
沒有 benchmark，沒有 commit／push；Windows／Linux 的修復後原生回歸仍屬第四步。

## Tested content and environment / 受測內容與環境

基準 HEAD 是 `a19c0d456efa8f4ad95f4570e8b43f1b6daf1e00`，加上本工作樹第二步尚未提交的
退出修復及契約測試。**不能把本輪結果歸給未包含修復的 a19c0d4 本身。**
結果包保存 `working-tree.patch`、新增測試來源與 `source-sha256.json`，可辨識實際受測內容。

| 項目 | 本輪環境 |
| --- | --- |
| 系統／CPU | macOS 26.3.1(a)，25D771280a；Apple M1 Pro，arm64 |
| C++ | Apple Clang 21.0.0（clang-2100.1.1.101）；libc++；macOS SDK 26.5；C++23 |
| 建置 | CMake 4.4.4；Ninja 1.13.2.git.kitware.jobserver-pipe-1；獨立 Debug／Release 目錄 |
| Python | CPython 3.12.14；外部 supervisor 使用 Apple Python 3.9.6 |
| 前端工具 | Node 24.22.0；重用既有依賴安裝，精確 submodule pins 見 `environment.json` |

來源快照放在 Git 外，包含 772 個一般檔案及第二步新增來源；逐檔記錄輸入 SHA-256。
固定版本依賴及 `node_modules` 以連結重用。只在快照的 bundled App 設定中啟用
`CONFIG_VINC_APP_PYTHON=y`／System Python，讓完整 CTest 實際包含 Python Module；
沒有改 repository 的預設設定。不同組態的 App／UI／同名中央測試依序執行。
主驗收設 `PYTHONMALLOC=debug`、`PYTHONFAULTHANDLER=1`、不寫 Python bytecode。

兩組完整來源各 300 個 C++ 編譯命令、乾淨 producer 各 90 個、外部 native wrapper 各 32 個，
全部為 `-std=c++23`；Debug 均有 `-g` 且無 `NDEBUG`，Release 均有 `NDEBUG`。
完整來源確實編譯 Python adapter。`otool -L` 確認兩組 App 與 Python extension 均使用
系統 `/usr/lib/libc++.1.dylib`，保留既有 macOS runtime 決定。
無追蹤副本的 150 個 Common／OSAL 檔案與來源快照相同；快照與 repository 輸入的唯一
差異是上述 App 設定。`final-audit.json` 保存核對結果。
建置 logs 仍保留 aggregate initializer 與 `posix_spawn_file_actions_addchdir_np` 棄用警告；
本輪未整理 warning 或藉驗收擴大修改正式程式。

原生退出對照、乾淨 producer 建置與部分整合測試曾在同一主機重疊執行。
次數及時間只用於功能證據與逾時診斷，不是效能比較、自然故障率或延遲保證。

## Native exit and FD comparison / 原生退出與 FD 對照

外部 harness 沿用前份 FD 報告的修正後判讀工具。它編譯本輪 Common／OSAL 的
**未插入追蹤點副本**，不替換 `kevent64` 或 `waitpid`，不新增外部 reaper。
共用 observer 的唯一通知／空 poll 受控時序則由第二步已維護的契約測試負責。

| 每組態的外部案例 | Debug | Release |
| --- | --- | --- |
| 正常 spawn／退出、三個標準串流為 Null | 3,000/3,000 | 3,000/3,000 |
| 缺少標準串流 × Inherit／Null／Capture | 144/144 | 144/144 |
| 相同矩陣，既存串流設 CLOEXEC | 144/144 | 144/144 |
| 1／63／64 個 Pipe binding、缺少串流及 capture | 24/24 | 24/24 |
| 合計 | **3,312/3,312** | **3,312/3,312** |

每案都有外部程序上限與原始輸出；結果為 **6,624/6,624**，沒有 wait error 或 supervisor
逾時。正常退出組與兩組串流矩陣均取得 exit code 0；檢查子程序 FD 身分、輸入／輸出 bytes、
capture 與額外 FD。這 6,576 案也確認父程序 FD 3–511 的清理前後集合一致，
沒有 immediate mismatch 或一秒清理寬限後仍不一致；binding 組使用自己的內容／映射／EOF 斷言。
不把有限 FD 掃描描述成任意範圍的完整洩漏證明。

退出等待沿用五秒期限。工具仍保留失敗後 `try_wait()` 的診斷能力，但本輪沒有走入該補救；
失敗後才取得結果不會被分類成通過。工具的慢案例程序快照也只記錄本案所屬 parent／child。

修復前相同的無追蹤正常退出組曾出現 Debug 4/3,000、Release 1/3,000 逾時；本輪各為零。
這是同類工作負載的修復後對照，不能單靠有限抽樣證明所有排程均正確。
真正確定性的進度與生命週期驗證由 `osal.process_exit_progress.*` 及 Mac backend 契約提供。

## Source App / Python acceptance / 來源 App 與 Python 驗收

完整來源建置啟用 Python，沒有排除已知失敗項目：

| 原始完整 CTest／其中子集 | Debug | Release |
| --- | --- | --- |
| 完整 153 項 | **151/153**，959.12 秒 | **152/153**，908.27 秒 |
| OSAL（包含新增 15 項退出契約） | 50/50 | 50/50 |
| App | 12/12 | 12/12 |
| SDK | 14/14 | 14/14 |
| Module（包含四項 Python） | 21/22 | 22/22 |
| UI／Bag | 5/5、3/3 | 5/5、3/3 |

兩組皆沒有 skipped。Debug 失敗為 `module.plugin` 與 `examples.user_module`；
Release 只有 `examples.user_module` 失敗。表中子集包含在 153 項內，不重複加總。
Kconfig 設定解析的內部 unittest 各 17/17、Python 選擇／ABI probe 各 24/24，亦已包含在
相應 SDK CTest。涵蓋 System／venv／Custom、實際 probe timeout、無效輸出、cache／重新探測；
Python Shell／installed Python 另覆蓋 `--python` 與 ABI 拒絕的隔離行為。
`module.python_services` 的同號／異號 Publisher 身分與四種故障情境兩組均通過。
完整逐案輸出保存在各 build 的 `Testing/Temporary/LastTest.log`；JUnit 的成功輸出可能截短。

工具測試另為 118 項：95 通過、23 項 Windows 專用案例依平台略過；不能記成 118 項都執行成功。

另外執行 source `multi_app.py`：Debug／Release 皆通過。涵蓋 First 的預設排除、Second／Third
共用 Module／Python、各 App catalog／script／identity、真實 self-Host、同名排他、不同 App
並行、獨立安裝／搬移、UI graph／port 衝突、Bag 載入失敗、停滯官方 loader 的有界處理、
資源修復與選擇清理。這兩次來源模式測試不包含在 153 項 CTest 內。

### Plugin failure classification / Plugin 故障分類

Debug 全套中的 `module.plugin` 在 2.19 秒後以 `failure category retained` 斷言失敗，
原始輸出另有 Log shutdown diagnostic。這證明快照中的故障分類未符合該次案例預期，
但原測試沒有印出 fixture 路徑、預期代碼與實際代碼，**尚不能確定是哪個案例或根因**。
同輪以正式 App 作 Host 的 `app.plugin` 通過，不能用它抵銷 `module.plugin` 的失敗。
相同的 Log shutdown 字樣也出現在通過的 `module.python_services` 輸出中，
不能用該行代替缺少的 Module 故障分類資訊。

Git 外診斷副本只在該斷言失敗時增加 path、expected、actual 與 exit 輸出，
保留原測試條件、600ms startup policy、八秒等待期限及原 fixture。
另從原 HEAD 建置 OSAL，僅替換診斷父程序連結的 OSAL archive，其他 Manager／Host／Plugin
仍使用本輪建置。這個對照只能隔離父端 OSAL 差異，**不是未修復完整版本的驗收**。
重跑結果另存，不覆蓋原始 CTest／JUnit；失敗原因未確認前不將其宣告為環境問題。

| Debug 定點對照 | 實際結果 |
| --- | --- |
| 原封不動的 `module_plugin_tests` binary | 10/10 通過 |
| 只增加失敗診斷的測試副本、修復後父端 OSAL | 先 5/5、再 20/20 通過 |
| 同一診斷副本、父端換回原 HEAD 的 OSAL | 20/20 通過 |

因此本輪確認的是一次真實斷言失敗及 55 次後續通過，**未定位根因，也未證明與退出修復無關**。
建議下一輪先讓維護中的斷言保留上述分類資訊，再對照啟動期限、故障回報／確認及退出收尾
的先後次序。沒有證據前不放寬 600ms policy、不刪斷言、不改 Module 的故障優先順序。

**後續調查更新（2026-11-03）：** 上述為第三步結束時的證據。後續已在新路徑原 fixture
重現 StartupTimeout 搶先於 Plugin 錯誤，並以有限慢載入及外部測試預算拆分驗證機制。
完整方法／結果由 [Module 調查報告](../module_system/MACOS_PLUGIN_CLASSIFICATION.md)
維護。後續維護中測試已拆分預算、補上中央保存順序與失敗清理契約；修復後 macOS
Debug／Release 完整 CTest 各 153/154，只有既有範例部署失敗。原始那次缺少分類仍不能
唯一回溯；本報告的原始全套結果不改寫。

### Example deployment / 範例部署

`examples.user_module` 沿用先前已確認的暫存部署缺漏：只複製 executable、`lib`、Python
套件與使用者 Python Module，未帶入 `modules` 及 `vinc_data/open-vinc/ui`。
它在完整來源測試保持原樣執行並記錄失敗；外部補齊部署比較另行計數。
本輪不修改 repository fixture，也不藉補齊外部副本宣稱原始全套全綠。
Debug／Release 外部完整部署皆通過，原範例全部斷言與正常收尾都保留。
兩組原 build 的 executable／libraries 雜湊前後一致，部署副本的 executable 也與原 binary 相同。

**後續修復（2026-11-03）：** 維護中的部署腳本已補齊 Plugin／UI／授權檔案，
macOS Debug／Release `examples.user_module` 定點各 1/1 通過，詳見
[範例修復紀錄](../../../examples/userExampleModule/README.md#macos-deployment-fixture-gap)。
這是前輪完整結果之後的定點補驗，不宣稱修復後重新執行完整套件。

## Clean SDK handoff / 乾淨 SDK 交付

另以 `VINC_BUILD_APPS=OFF`、`BUILD_TESTING=OFF`、`VINC_INSTALL_SDK=ON` 製作 Debug／Release
producer，沒有啟用 Python 的 App。兩組最初各 93 個編譯命令（其中 90 個 C++），
交付前確認沒有 Python adapter 編譯或 `_native*.so`。初次乾淨建置保存在
`clean-sdk-prepare-results.json`；後續 driver 的增量建置時間不當成乾淨建置時間。

| 獨立下游矩陣 | Debug | Release | 主要斷言 |
| --- | --- | --- | --- |
| `installed_consumer.cmake` | 通過，內部 4/4 | 通過，內部 4/4 | SDK 搬移、使用端重建／相容性拒絕、standalone／shared／late coordinator |
| `installed_python.cmake` | 通過 | 通過 | 搬移 SDK 後才啟用 Python、中文／空白路徑 venv、自訂 Bus／RPC／Param、混合 native/Python Module、schema 增刪／失敗恢復、ABI 與缺直譯器隔離、部署修復／停用清理 |
| `installed_application.cmake`，WITH_UI=ON | 通過 | 通過 | 一個 App executable 的 central／Host、搬移後暫藏 SDK／consumer build、Bus／Bag／UI、自訂訊息、內嵌腳本與拒絕案例 |
| installed `multi_app.py` | 通過 | 通過 | 共用與隔離、獨立部署、不同 App 並行、UI／官方 Module 故障、選擇清理與使用者檔案保護 |

這八組是獨立 producer 的額外驗收，不用來源全套裡相似名稱的 SDK 測試取代。
各自 commands、結果及巢狀 logs 都保留；不將內部 consumer 四項重複算入原始 153 項。

## Reproduction and delivery / 重現與交付

外部工作目錄：`/private/tmp/open-vinc-exit-step3-20261103`。
`run_validation.py` 執行 Python-enabled source Debug／Release 完整建置、CTest 與 source multi-App；
`run_native.py` 執行上列無追蹤原生矩陣；`run_clean_sdk.py` 建立 App／Python 關閉的 producer，
再執行安裝／搬移後的 native、Python、App 與 multi-App 下游驗收。
runner 中的實際工具路徑與全部參數保存在對應 `*-results.json`／`results.json`。

持久交付位置：`/Users/jay/Downloads/open-vinc-exit-step3-20261103/evidence.tar.gz`。
包含受測來源、外部工具、完整／巢狀測試 logs、逐案 native 結果、原始失敗、診斷對照、
來源／runtime 核對與最終文件；另附逐檔 `MANIFEST.json`、壓縮檔 `SHA256SUMS` 及逐檔驗證結果。
不含 build binaries、上游依賴樹、`node_modules` 或 Git metadata。
結果包的 `EVIDENCE_README.md` 說明來源設定差異、命令重現與診斷副本的限制。

最後核對確認正式程式／測試與受測快照一致（本輪只更新文件），`git diff --check` 通過。
所有 runner 已結束；最後唯讀程序清單以本輪工作目錄路徑篩選，沒有匹配程序。
各測試自身的退出／回收斷言也一併保留。
原始 logs／結果包放在 Git 外，repository 只新增／更新報告索引；本輪未 commit／push。

## Limits and remaining work / 限制與剩餘工作

- 第四步仍須在 Windows／Linux 原生建置及執行同一份退出修復與受控契約，並補整合回歸。
  過去平台通過紀錄不能替代本輪私有 native 介面改動的驗證。
- `module.plugin` 分類測試已修復並完成 macOS 驗證，中央順序及失敗清理另有受控契約；
  Windows／Linux 重驗待補，見 [Module 紀錄](../module_system/README.md#macos-plugin-classification-follow-up)。
  範例部署 fixture 也已修正並通過 Mac 定點補驗；本報告的原始全套失敗均保留。
- 沒有驗證 Intel Mac、最低支援 macOS、其他 Python minor 或工具鏈組合。
- 沒有強制取得真實 PID 重用；舊事件／新紀錄分離由受控契約及第一步 native token 證據支援。
- 沒有 benchmark、硬即時延遲保證、GUI 人工巡覽或完整故障注入矩陣。

<a id="publication-validation"></a>

## Publication validation / 整合 develop 後的發布前驗證（2026-11-03）

本輪將 develop `f16da98` 合入主題分支，受測實作 commit 為 `3c0aa44`。
保留 Param entry 13／wire v3、最新 C++／Python 範例的多次啟動／保存／重啟提示流程，
並合併授權檔案、UI 入口與官方 Module Ready 檢查；沒有覆蓋上游 Param 實作。
先前的 9.06／8.02 秒範例紀錄與受測 script hash 僅適用合併前版本。

在原本的 Git 外 Debug／Release 建置目錄同步受測來源，兩組都重新建置所有預設 targets，
快照仍只額外啟用 bundled App 的 Python/System 設定。執行選擇如下，兩組依序執行：

```sh
cmake --build <build> --parallel 4
ctest --test-dir <build> --output-on-failure --output-junit <result.xml> --parallel 1 \
  -R '^(osal\.|module\.|param\.|shell\.|app\.|examples\.user_module$)'
```

| 整合後來源驗證 | Debug | Release |
| --- | --- | --- |
| 全部預設 targets 建置 | 通過 | 通過 |
| 選定 CTest（無略過） | **96/96**，262.42 秒 | **96/96**，251.02 秒 |
| OSAL／Module／Param／Shell／App／範例 | 50／23／3／7／12／1，全部通過 | 50／23／3／7／12／1，全部通過 |

上述結果涵蓋新版 Plugin、Python、Param 權限／current reads／重啟標記，以及新版範例
多次中央啟動與明確保存。最終非文件來源與受測快照 hash 一致；之後僅補寫驗證文件。
環境同本文所列 macOS arm64／Apple Clang 21／CPython 3.12.14，既有編譯警告仍保留。

這是整合後來源回歸，沒有重跑全部 CTest、6,624 案原生批次或乾淨 installed SDK。
該輪尚未執行的新版 Param 乾淨 SDK consumer／application／python 矩陣，已由
[後續獨立 Debug／Release 驗收](../sdk/MACOS_PARAM_VALIDATION.md)補齊；此處不增記為當輪執行。
Windows／Linux 仍須驗證本 PR 的退出修復與測試修訂，沒有 benchmark。
原始輸出、命令、JUnit 與來源 hash 保存於 Git 外
`/private/tmp/open-vinc-publication-20261103`；封存包位於
`/Users/jay/Downloads/open-vinc-publication-20261103/evidence.tar.gz`，
同目錄 `SHA256SUMS` 可驗證完整性，原始輸出不納入 Git。
