# Multi-App validation / 多 App 驗收

These are dated validation records, not current implementation steps.
歷史結果與原始失敗保留當時範圍；文中的「待驗」不代表目前仍未完成。
Current contract / 現行契約：[contract](MULTI_APP.md)。
Later native acceptance: [Mac SDK](../../src/vinc/sdk/MACOS_SDK_DEPLOYMENT_VALIDATION.md), [two Recorder Apps](../../src/vinc/sdk/MACOS_RECORDER_CONTROL_VALIDATION.md).

## Stage 1 result / 第一階段結果（2026-10-31）

Module registration now preserves target customization while excluding unselected
targets from the default build. Catalog rendering accepts an explicit target list.
Internal configuration helpers resolve all late registrations into per-App
selection properties/dependencies and editor targets. Dynamic choices use exact,
case-sensitive `CONFIG_VINC_MODULE_<ModuleType>` names and default off; official
Modules are not switches in this schema. See the
[configuration implementation and validation](../../tools/configure/README.md#stage-1-module-selection).

Module 登記保留 target 後續設定能力，未選 target 不列入預設建置；catalog 改由明確的
target 清單生成。內部設定 helper 在登記結束後解析各 App 的選擇、相依與編輯入口。
選項採大小寫有別的 `CONFIG_VINC_MODULE_<ModuleType>` 並預設關閉；新 schema 不提供官方
Module 開關。實作邊界與驗證紀錄見上方連結。

At stage 1, public `vinc_add_app(VINC_BUILD_CONFIG ...)` wiring was still pending.
The stage 2 result below supersedes that implementation status. Real tiny-Plugin
fixtures validated stage 1; alone they did not prove multiple production Apps,
deployment or automatic UI/Bag startup.

第一階段時公開接線仍未完成；此實作狀態已由下方第二階段紀錄更新。第一階段小型 Plugin
fixture 本身不代表多個正式 App、部署或 UI／Bag 自動啟動已完成。

## Stage 2 result / 第二階段結果（2026-10-31）

`VincApplication.cmake` now provides the same public constructor for source and
installed SDK consumers. Each App owns its config, generated catalog/identity/UI
root, embedded script and output directory. Compatible custom Plugins, the Bag
adapter and enabled Python bindings share targets; UI has a thin adapter per App.
Late Module/message registration remains supported. Framework-owned main and
self-Host dispatch are unchanged. `config.hpp`, Param, ports and startup policy
were not changed.

來源／安裝式 SDK 共用建構器，各 App 有獨立設定、catalog、身分、UI 根目錄、腳本及
執行檔輸出。相容的自訂 Module、Bag 接線與 Python 綁定共用 target，UI 按 App 建立薄接線。
仍支援晚登記 Module／Message；框架唯一 main 與啟動自己為 Host 的流程不變。
未修改 `config.hpp`、Param、port 或官方 Module 啟動政策。

CMake forbids adding subdirectories during deferred execution. A read-only feature
preflight therefore prepares Python before that phase; the final registered Module
manifest still validates every selection. No upstream pybind11 modification or
configure-time nested build was introduced. Ribosome's existing every-build
regeneration policy remains: separate build invocations may recompile shared
consumers. The reuse check verifies one shared compile in one combined App build,
not unchanged timestamps across separate builds.

CMake 不允許 deferred 階段加入子目錄，因此先唯讀解析 Python 功能選項，再於所有登記
完成後驗證 Module 清單。未修改上游 pybind11，也未在 configure 執行巢狀 build。
Ribosome 每次 build 重新生成的既有政策保留：分開呼叫 build 可能重編共用消費端。
測試驗證同一次多 target 建置只編一次共用 Module，不宣稱不同 build 間時間戳不變。

Validation uses base `4315cfa` plus the existing working tree and these changes:

| Environment / 環境 | Result / 結果 |
| --- | --- |
| Windows x64, MSVC 19.51.36257, CMake 4.3.2, Ninja 1.13.2, CPython 3.13.12 | Installed SDK fixture passed in Debug and Release; source fixture passed in Release. Three real Apps per run. / 安裝式 Debug／Release、來源式 Release 均通過，每輪三個真實 App。 |
| WSL Ubuntu 24.04.5, GCC/libstdc++ 13.3, CMake 3.28.3, Ninja 1.11.1, CPython 3.12.3 | Source Release fixture passed with three real Apps. / 原始碼 Release 三個 App 通過。 |
| Windows / WSL configuration parser | 15/15 each, including preflight and final unknown-Module rejection. / 各 15/15，含預讀及最終未知 Module 拒絕。 |
| Windows Debug / Release regression | `sdk.configuration`, `sdk.app_selection`, `module.shell`, `app.entry`: 4/4 each. Existing runtime binaries were reused because this stage changes build composition, not their implementation. / 各 4/4；本輪未改 Runtime 實作，沿用既有回歸執行檔。 |

The real fixture checks exclusion of an unselected invalid source, late message
generation, distinct catalogs/scripts/identities, shared Plugin/Python targets,
App-local UI adapters, reserved ModuleType rejection and recovery. C++ Modules
verify their actual executable path; Python Modules use the generated custom type.
All three scripts complete and shutdown reports every Module process exited.
Tests stage Tcl/Python explicitly; UI assets/HTTP and automatic UI/Bag startup
are not part of this stage's runtime evidence.

實際案例涵蓋未選的錯誤原始碼不建置、晚登記訊息、各自 catalog／腳本／身分、共用產物、
UI 薄接線、保留名稱拒絕及恢復。C++ Module 核對真正執行檔路徑，Python 使用生成的
自訂訊息；三個 App 腳本及正常收尾均通過。測試自行準備 Tcl／Python，沒有把 UI
資源／HTTP 或 UI／Bag 自動啟動列成本輪成果。

Reproduce with `src/vinc/sdk/tests/multi_app.py --help`, then choose `--source`
or `--sdk-build` with the native matching toolchain. Each invocation preserves a
fresh `run-*` directory under `--work`. Installed producers with UI, Ninja and
`VINC_TEST_SDK_PYTHON_EXECUTABLE` register the same driver as `sdk.multi_app`.
Local successful artifacts are under `build/multi-app-stage2-installed-{Debug,Release}`,
`build/multi-app-stage2-source-Release`, and WSL `/var/tmp/vinc-multi-app-stage2-source/run-tg1a0yv_`.
Native macOS, Linux installed SDK, Windows source Debug, complete framework
regression, deployment/relocation and concurrent central execution were not
revalidated for this stage. Earlier platform evidence remains historical evidence.

可用上述 driver 選 `--source`／`--sdk-build` 重跑；每次保留新的 `run-*` 目錄供診斷。
具 UI、Ninja 與 Python 測試環境的 SDK producer 會登記同一個 `sdk.multi_app`。
本輪未重驗 macOS、Linux 安裝式 SDK、Windows 來源 Debug、全框架、搬移部署及中央並行；
不將舊平台紀錄改寫成本輪證據。

## Stage 3 implementation / 第三階段實作

The App composer now renders Plugin paths relative to the executable and stages
Tcl, selected Plugins, UI assets, licenses and optional Python resources before
linking. `VincApp_<target>` installs one App to `<prefix>/<target>`. Separate ownership
markers and digest manifests reject cross-App output collisions, repair missing
resources, remove only unchanged obsolete managed files, and preserve user data.
Runtime logs/params and user Python Modules are not packaging inputs.

Central starts `webUi` and then `bagRecorder` before Tcl using existing Manager
readiness/deadline APIs. Startup failure reports the type/MID/reason and requests
bounded cleanup; it does not prevent the other official Module or the script.
Recorder starts idle. Explicit stop remains effective. No Param/config.hpp/port
policy changes, additional threads or independent Host executable were introduced.

各 App catalog 改為相對執行檔路徑，建置自動準備 Tcl、所選 Plugin、UI、授權及選用的
Python 資源；`VincApp_<target>` 可獨立安裝至 `<prefix>/<target>`。所有權標記與摘要清單
拒絕跨 App 覆蓋、補回遺失資源，僅移除未被修改的舊管理檔案；不打包執行中的 log／param
或使用者 Python Module。

中央在 Tcl 前依序啟動 UI／Bag，沿用 Manager 的就緒／期限契約。失敗回報型別、MID、
原因並請求有界清理，不阻止另一官方 Module 或腳本。Bag 待命不錄製，明確停止不被推翻。
本輪不改 Param、config.hpp 或 port 政策，不新增執行緒或獨立 Host 執行檔。

Stage 4 still owns the complete acceptance matrix, concurrent central cases and
final cross-document/example review. Prior platform evidence above remains unchanged.
Stop an App before updating its package: copies are serialized, but not transactional
whole-directory replacement. Repair/retry a failed deployment before running it.

第四階段仍負責完整驗收矩陣、中央並行情境及文件／範例最後一致性檢查；不改寫上述歷史平台證據。
更新前停止 App；複製有序列化但不是整包原子替換，失敗後須修復／重試再執行。

### Stage 3 validation / 第三階段驗證

Environment versions match the stage 2 table above. The working tree remains based
on `4315cfa`; no publication is implied by these local results.

環境版本同第二階段表格；驗證基於 `4315cfa` 加目前工作樹，不代表已提交或發布。

- Windows installed Debug and Release: three-App composition/execution, independent install and
  relocation, UI bind/Bag load failure, ownership/path rejection, license repair,
  Python/Module removal and sibling/user-file preservation passed. The expanded
  Release run also passed explicit UI stop and stalled official loader recovery.
- WSL Ubuntu 24.04 source Release: the same three-App deployment flow passed.
  Separate incremental fixture checks also passed explicit UI stop and stalled UI
  loader recovery: central reports failure, starts Bag, runs Tcl and confirms all
  child processes exited. Linux installed-SDK packaging was not repeated this stage.
- Windows Release existing installed suites: `sdk.installed_application` (228.09 s)
  and `sdk.installed_python` (314.11 s) passed. They hide/move SDK/build inputs,
  exercise typed Bus/Bag/UI HTTP and Python Bus/RPC/Param, schema/script edits,
  ABI failure isolation, resource repair and user-data preservation.
- Rebuilt production entry fixtures plus `sdk.package_inventory`,
  `sdk.license_inventory`, `sdk.owned_files`, `sdk.configuration`,
  `sdk.app_selection`, `sdk.license_deployment`: **7/7** passed. Rebuilt legacy central
  also passed `module.shell` and `bag.shell` (**2/2**).
- Added validation of `.vinc-owned-files.tmp` before cleanup/write. The malicious
  symlink test passed on WSL without modifying its target. Windows could not create
  a symlink under this account and explicitly skipped that subcase; ordinary
  ownership/traversal cases passed there.

- Windows 安裝式 Debug／Release 三個 App：獨立安裝／搬移、官方 Module 故障隔離、所有權／路徑拒絕、
  授權補回、停用 Python／移除 Module 及其他 App／使用者檔案保護均通過。
  擴充後的 Release 另通過明確停止 UI 與官方載入停滯回收。
- WSL 原始碼 Release 同一流程通過，另補驗明確停止 UI 與載入停滯期限回收；Bag／Tcl 繼續，
  所有子程序退出。這輪未重跑 Linux 安裝式 SDK 包裝。
- Windows Release 舊安裝式 C++／Python 套件兩組通過，含搬移後的實際通訊、UI HTTP、
  訊息／腳本增量重建、ABI 隔離與檔案修復。入口及部署／設定回歸 7/7，舊中央 Shell 回歸 2/2。
- 暫存清單的符號連結拒絕在 WSL 通過；Windows 因無建立符號連結權限明確跳過該子案例，
  不把它列成 Windows 通過證據。

Artifacts: `build/multi-app-stage3-installed-Debug/run-vekv4hx9`,
`build/multi-app-stage3-installed-Release/run-u9pt5b2h`,
`build/sdk-package-release/sdk-application-test`, `build/sdk-package-release/sdk-python-test`,
`build/stage3-entry-deployment-checks.log`, and WSL
`/var/tmp/vinc-multi-app-stage3-source/run-gubkxbt1` (`explicit-stop.log`, `stall.log`).
Reproduce the main driver with `src/vinc/sdk/tests/multi_app.py --help`;
its loader injection is test-only and never part of a deployed App catalog.
Native macOS, complete framework regression, concurrent central admission/port
scenarios and the final documentation sweep remain stage 4 coverage; existing
macOS evidence is retained, not reclassified as unverified.

主要 driver 可透過上述 `--help` 重跑；停滯 Plugin 僅為測試注入，不進正式 App catalog。
本輪未重跑 macOS／全框架／中央並行與 port 矩陣；第四階段做最後驗收與文件整理。
保留既有 macOS 驗證，不改寫為整個平台未經驗證。

<a id="stage-4-acceptance"></a>

## Stage 4 acceptance / 第四階段驗收（2026-10-31）

**Status: local acceptance mostly passed; the phase remains open for the remaining
coverage below.** This round changed tests and documentation, not runtime policy,
Param, `config.hpp` or the public App API. Base revision is `4315cfa` plus the
existing worktree; no commit, push or release was performed.

**狀態：本機主要驗收通過，以下待驗範圍尚未關閉。** 本輪修改測試與文件，不改 Runtime
政策、Param、config.hpp 或公開 App API。基於 `4315cfa` 加現有工作樹，未提交／推送／發布。

Environment: Windows 11 Pro build 26200, AMD Ryzen 7 5700X3D (8 cores/16 threads),
MSVC 19.51.36257, CMake 4.3.2, Ninja 1.13.2, CPython 3.13.12; WSL Ubuntu 24.04.5
x86_64 on the same machine, GCC/libstdc++ 13.3, glibc 2.39, CMake 3.28.3,
Ninja 1.11.1 and CPython 3.12.3. Environment versions were checked locally.

### New checks and results / 新增檢查與結果

`multi_app_runtime.py` keeps the two real relocated Apps alive through fixture-only
startup control, connects to their Socket Shells, and uses existing commands to
move First's UI to an OS-selected port before starting Second. It then verifies:

- The same App identity is rejected before a second Socket Shell is created.
- Different Apps have distinct Shell/UI ports; their actual HTTP graphs show only
  their own selected ModuleTypes. Both can have an instance named `probe`.
- First is C++-only; Second also runs the system-Python Module. Each script completes.
- Stopping First confirms all its children exited without breaking Second's graph
  or Recorder command. First can then restart, proving admission was released.
- Cleanup is bounded and owns only each test process tree. No user process is killed.

新增 helper 讓搬移後的兩個真實 App 保持執行，透過 Socket Shell 操作既有 UI 啟停／port，
以 HTTP graph 核對實際 Module 清單。涵蓋同名拒絕、異名並行、同名 Module 各自獨立、
C++／Python 選配、腳本完成、獨立關閉、子程序回收及重新取得 admission。
測試只管理自己建立的程序樹，不終止其他服務。

- **Linux installed Release:** the complete multi-App driver passed, including the
  new coexistence cases, stalled loader, resource repair, selection cleanup and
  user/sibling-file preservation. Artifacts:
  `/var/tmp/vinc-multi-app-stage4-installed/run-t6l8txwq`.
- **Windows installed Debug:** coexistence, graph isolation, same-name exclusion,
  official failure and stalled loader cases passed. The later post-cleanup startup
  failed its healthy-UI assertion because an unrelated Astro preview started using
  `127.0.0.1:8080`. The App correctly reported UI BindFailed and exited cleanly.
  This run is **partial, not a passing full suite**. Artifacts:
  `build/multi-app-stage3-installed-Debug/run-3kyb1tfd/coexist-*.log`.
- **Windows installed Debug retry:** after the user authorized pausing the website
  preview, the complete suite passed, including the previously blocked post-cleanup
  startup. Artifacts: `build/multi-app-stage3-installed-Debug/run-o30u13lr`;
  log: `build/stage4-windows-multi-app-Debug.log`.
- **Windows installed Release retry:** the same complete suite passed in a fresh
  fixture, including coexistence, bounded loader failure and deployment cleanup.
  Artifacts: `build/multi-app-stage3-installed-Release/run-fktqhfg9`;
  log: `build/stage4-windows-multi-app-Release.log`.
  After both suites, the website preview was restored on `127.0.0.1:8080` and
  independently checked to return HTTP 200. The port-related blocker is resolved.
- **Windows Release:** rebuilt all existing targets; **103/103** selected CTest
  checks passed in 290.53 s, including App-host lifecycle/fault, SDK settings,
  OSAL, Bus, Param, RPC, Shell and Bag. Log: `build/stage4-windows-runtime.log`.
- **Linux Release:** rebuilt producer targets; **107/107** selected CTest checks
  passed in 159.67 s. Log: `/var/tmp/open-vinc-sdk.eE3JjSQa/stage4-runtime.log`.
- **Linux installed SDK supplemental suites:** Python and C++ application suites
  both passed. They cover relocated bundles, custom Bus/RPC/Param messages,
  Bag/UI integration, schema incremental changes, ABI rejection, resource repair
  and cleanup. Evidence is in producer build directories `sdk-python-test` and
  `sdk-application-test` under `/var/tmp/open-vinc-sdk.eE3JjSQa/build`.
  The final coexistence helper also passed against existing relocated artifacts;
  logs: `/var/tmp/vinc-multi-app-stage4-installed/run-bfhr8ks2/final-runtime-recheck`.
- The regression command excludes `sdk.multi_app`, `sdk.installed_python`,
  `sdk.installed_application`, and `sdk.external_consumer`. Multi-App is exercised
  separately, as are the two supplemental Linux SDK suites above; unchanged Windows C++/Python service and package evidence from
  stage 3 remains applicable. This is not a claim that every packaging suite ran again.

- Linux 安裝式 Release 的完整多 App driver 通過。
- Windows 安裝式 Debug 的並行／隔離等新增案例通過；後段受到外部網站占用 port 影響，
  不把整組列為通過，也不把正常的 BindFailed 當成框架回收漏洞。
- 使用者授權暫停網站後，Windows Debug 已從全新測試目錄完整重跑通過，包含先前受阻的
  清理後啟動驗證；前一筆部分結果保留作為問題與修復紀錄。
- Windows Release 也從全新測試目錄完整通過同一套驗收。兩組結束後已恢復網站預覽，
  確認 127.0.0.1:8080 回應 HTTP 200；本次 port 環境阻礙已解除。
- Windows Release 重建後 103/103、Linux Release 107/107 回歸通過。長時間 SDK 整合組未全部重跑；第三階段仍適用
  的證據沿用並明確分開，不宣稱全套包裝測試重新執行。
- Linux 另行完整通過安裝式 Python 與 C++ application 兩組，涵蓋搬移、自訂訊息服務、
  Bag／UI、schema 增量、ABI 拒絕及資源修復／清理；最後版本的並行 helper 也重跑通過。

### Findings / 發現與處理

1. **Fixed test assumption:** HTTP checks leave POSIX TCP TIME_WAIT connections.
   The later occupied-port fixture originally failed before invoking the App.
   `ss` showed no listener and both Apps had confirmed child cleanup. POSIX-only
   `SO_REUSEADDR` now permits rebinding those closed connections; the complete Linux
   rerun passed. Windows retains its original bind behavior.
2. **Documentation corrected:** root quick start now queries the auto-started UI and
   commands the existing Recorder. English/Chinese Host inventory and relocation
   statements agree; new source/installed composition is distinguished from the
   legacy shell. The SDK guide includes per-App configuration/install commands and
   the deployment tree. Existing historical validation is retained.
3. **Remaining coverage:** Windows Debug/Release full retries passed after the
   authorized preview pause and restoration. Native macOS verification of these new multi-App changes remains outstanding;
   prior macOS framework validation is not removed or relabelled as unverified.
   Interactive GUI/menu editors and simultaneous independent build invocations are
   not covered. Existing Tcl build-root-space and configurator Unicode-output
   limitations remain in their owning READMEs, outside this round's fixes.

1. 已修正測試對 TIME_WAIT 的假設；確認無 listener／殘留 App 後，在 POSIX fixture 使用
   SO_REUSEADDR，Linux 完整重跑通過。Windows 維持原本 bind 行為。
2. 已更新根目錄教學與中英不一致的 Host／搬移描述；補各 App 設定／安裝與產物結構。
3. 經授權暫停並恢復網站後，Windows Debug／Release 完整重驗已通過。macOS 這次新增能力仍待實機驗證，
   保留過去已完成的驗證；GUI／menu 互動操作未重驗。既有 Tcl 空白建置路徑及 Unicode
   診斷限制仍由所屬 README 追蹤，不納入本輪修復。
