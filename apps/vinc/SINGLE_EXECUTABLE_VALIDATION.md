# Single executable validation / 單一執行檔驗收

These are dated validation records, not current implementation steps.
歷史結果與原始失敗保留當時範圍；文中的「待驗」不代表目前仍未完成。
Current contract / 現行契約：[contract](SINGLE_EXECUTABLE.md)。
Later native acceptance: [Mac source](../../src/vinc/module_system/MACOS_SOURCE_FUNCTIONAL_VALIDATION.md), [clean SDK](../../src/vinc/sdk/MACOS_SDK_DEPLOYMENT_VALIDATION.md).

## Step 1 results (2026-10-30) / 第一步結果

The production App entry now uses `detail::is_module_launch()` before central
argument parsing. It recognizes the reserved marker family, while the unchanged
Runtime checks the exact bootstrap version and fields. App resolves its OS path
before creating Manager, whose factories now require explicit arguments and reject
empty/relative paths before central admission. Existing test/benchmark callers
explicitly select the temporary standalone Host. Source and installed App targets
link the existing Runtime so the shared main remains buildable in both modes.

正式 App 在中央參數解析前以 `detail::is_module_launch()` 辨識保留標記系列，既有 Runtime
繼續驗證精確版本與欄位。App 查得 OS 路徑後才建立 Manager；factory 必須明確傳入參數，
空白／相對路徑在中央 admission 前遭拒。既有測試／benchmark 明確指定暫留的獨立 Host。
原始碼與安裝式 App target 同時補上既有 Runtime 連結，確保共用 main 兩種組裝皆可建置。

`app.entry` compiles the real App source in plain and embedded-script fixtures.
It checks missing/malformed/current/old/future bootstrap rejection, two distinct
Module PIDs, exact self-executable paths, one script execution and one Shell
listener, unchanged embedded `-S` rejection, and internal-looking tokens remaining
script/Module arguments. Both fixtures run from an unrelated cwd with spaces and
non-ASCII characters in their executable directory. `module.instance` additionally
checks explicit Host-path validation and identity release.

`app.entry` 以正式 App 原始碼建立一般／內嵌腳本 fixture，驗證缺漏／損壞／新舊版本拒絕、
兩個不同 Module PID、精確的自身執行檔路徑、腳本與 Shell 各一次、內嵌 `-S` 拒絕，以及
類似內部標記仍能作為腳本／Module 參數。執行檔目錄含空白與非 ASCII 字元，cwd 另設。
`module.instance` 另驗證必填 Host 路徑與失敗時不占住中央身分。

| Environment / 環境 | Step 1 validation / 第一步驗證 |
| --- | --- |
| Windows x64, MSVC 19.51.36257, CMake 4.3.2, Ninja 1.13.2 | Debug/Release builds succeeded. Release selected CTests 23/23 passed. Debug 22 existing tests passed, then corrected `app.entry` passed separately. Installed App, external SDK consumer, Param and RPC additionally passed 4/4. Debug／Release 建置成功；Release 23/23；Debug 原有 22 項通過後，新測試修正並單獨通過；另驗 installed App、外部 SDK consumer、Param、RPC，4/4 通過。 |
| WSL Ubuntu 24.04, GCC/libstdc++ 13.3, CMake 3.28.3, Ninja 1.11.1 | Release build succeeded; 22 existing tests passed, then corrected `app.entry` passed separately. Installed SDK application + Param/RPC process tests passed 3/3. Release 建置成功；原有 22 項通過後，新測試單獨通過；installed App／Param／RPC 3/3 通過。 |

The initial new fixture incorrectly treated Shell's retained ModuleType argument
as the instance name. Fixing that test assumption resolved its failure on both OSes;
no production naming/runtime change was needed. The selected existing tests cover
Module lifecycle/context/faults/bridge/debug plus Bus, Log, SDK and Shell flows.
They still use standalone Host fixtures where not explicitly exercising App entry.
The opt-in Bus benchmark target also compiled on WSL after its explicit Host-path
update; no performance measurements were run in this step.
This is not full matrix completion: standalone Host removal, new-entry-specific
authentication/fault coverage in the later integration pass, Python mixed operation,
and native macOS validation remain pending. Existing macOS evidence is preserved.

新 fixture 初次把 Shell 保留的 ModuleType 參數誤當實例名稱，修正測試假設後兩平台通過；
不需改正式命名／Runtime。原有測試涵蓋 Module 生命週期、Context、故障、bridge、debug，
以及 Bus／Log／SDK／Shell 流程；未指定測試 App 入口的 fixture 仍使用獨立 Host。
另在 WSL 建置選用的 Bus benchmark target，確認明確 Host 路徑接線可編譯；本輪未跑效能量測。
這不代表整張驗收矩陣完成：舊 Host 移除、新入口的認證／故障專項整合覆蓋、Python 混用與
macOS 原生驗證仍待後續輪次；既有 macOS 證據保留。

Local logs / 本機紀錄：`build/single-entry-debug-build.log`,
`build/single-entry-debug-test.log`, `build/single-entry-debug-entry.log`,
`build/single-entry-release-build.log`, `build/single-entry-release-test.log`,
`build/single-entry-linux.log`, `build/single-entry-linux-entry.log`,
`build/single-entry-linux-sdk.log`, `build/single-entry-windows-sdk.log`,
`build/single-entry-linux-benchmark-build.log`. These ignored files are local evidence, not
published artifacts. / 上述忽略檔為本機證據，未作為發布產物。

## Step 2 results (2026-10-30) / 第二步結果

The production `vinc_module_host` executable, `vinc::module_entry` object target,
standalone main source and generated Host-path usage requirement are removed.
Manager no longer builds a child executable as a side effect of linking. App keeps
the Runtime link added in step 1; Plugin link ownership is unchanged.

正式 `vinc_module_host` 執行檔、`vinc::module_entry` object target、獨立 main 原始碼與
生成 Host 路徑的使用相依已移除。連結 Manager 不再間接建置子程序執行檔；App 沿用第一步
的 Runtime 連結，Plugin 的服務所有權與相依邊界不變。

Under `BUILD_TESTING`, Module System tests own `vinc_test_host` and the
`vinc_test_manager` interface target. That interface explicitly supplies the test
Host dependency and generated `vinc_test/host_path.hpp` to fixture consumers;
neither target/header is exported or installed. Production Manager exposes none
of these test paths. The opt-in Bus benchmark dispatches its own Host role and
obtains its executable path through OSAL, without depending on a test Host.

啟用 `BUILD_TESTING` 時，Module System 的 tests 建置 `vinc_test_host` 與
`vinc_test_manager` interface target；後者明確提供 fixture 的 Host 建置相依及生成的
`vinc_test/host_path.hpp`，兩個 target 與該標頭皆不匯出、不安裝。正式 Manager 不外洩
測試路徑。選用 Bus benchmark 經 OSAL 查得自身路徑並分流自己的 Host，不依賴測試 Host。

The approved SDK wiring adjustment removes the Host tool export, unused generated
filename, deployment manifest field, copy and dependency. Existing SDK fixtures
now exercise their own test entry or the real App. Deployment still rejects App
names colliding with resource directories and still restores missing licenses.
There is no filename-based cleanup of existing deployment/build directories.

依已同意的範圍調整，一併移除 SDK Host 工具匯出、未使用的生成檔名、部署 manifest 欄位、
複製與相依。既有 SDK fixture 改用測試自己的入口或真正 App；部署仍拒絕 App 名稱與資源
目錄衝突，也保留補回遺失授權檔的驗證。不依檔名清理既有部署／build 目錄。

| Environment / 環境 | Validation / 驗證 |
| --- | --- |
| Windows x64, MSVC 19.51.36257, CMake 4.3.2, Ninja 1.13.2 | Debug and Release builds passed; selected process/App/Shell/Bus/Param/RPC/Bag tests passed 37/37 in each configuration. Debug／Release 建置成功，選定回歸各 37/37 通過。 Installed SDK consumer/application and external source consumer passed 3/3. 安裝式 SDK 基本消費端／App 及外部 source consumer，3/3 通過。 |
| WSL Ubuntu 24.04, GCC/libstdc++ 13.3, CMake 3.28.3, Ninja 1.11.1 | Release build passed; selected regressions 36/36 and installed SDK consumer 1/1 passed. Release 建置成功，選定回歸 36/36、安裝式 SDK 基本消費端 1/1 通過。 |

WSL benchmark smoke: 1 KiB push, 8 slots, 16 warmup + 10 measured samples. All
26 sent/received sequence entries matched, payload checks passed, and no loss was
recorded. This verifies self-host functionality, not comparative performance.
The local reporting script initially tried a nonexistent `summary.json` after
the successful run; a separate CSV check verified the actual outputs without rerunning.

WSL benchmark smoke 使用 1 KiB push、8 slots、16 筆暖機＋10 筆樣本；26 筆送收序號一致、
資料檢查皆通過，沒有遺漏。本次只驗證自我啟動功能，不作效能比較。本機彙整腳本起初在
成功執行後誤讀不存在的 `summary.json`，已另以實際 CSV 核對，未重跑量測。

The clean SDK-only build/install and artifact inventory matrix follows in step 3
below. Step 4 retains new-entry-specific fault/authentication coverage, optional
Python/native mixed operation and native macOS verification. Historical macOS
evidence is unchanged; these Windows/WSL results are not new macOS evidence.
The source `ui.module` and optional Python test fixtures were not enabled in this
round; installed-App regression did exercise its official UI integration.

完整乾淨 SDK-only 建置／安裝與產物清單見下方第三步；第四步保留新入口的故障／認證專項、
可選 Python／原生混用及 macOS 原生驗證。既有 macOS 證據保留，本輪 Windows／WSL 結果
不代表新的 macOS 證據。source `ui.module` 及可選 Python fixtures 本輪未啟用；
安裝式 App 回歸則有驗證其官方 UI 接線。

Local evidence / 本機證據：`build/single-entry-step2-debug-build.log`,
`build/single-entry-step2-debug-tests.log`, `build/single-entry-step2-release-build.log`,
`build/single-entry-step2-release-tests.log`, `build/single-entry-step2-windows-sdk.log`,
`build/single-entry-step2-linux.log`, `build/single-entry-step2-linux-extra.log`,
`build/single-entry-step2-benchmark-verify.log` (ignored, not published / 忽略檔，未發布).

## Step 3 results (2026-10-30) / 第三步結果

Fresh SDK producers were configured with `VINC_INSTALL_SDK=ON` and App, examples,
tests and Python builds disabled. Their Ninja target inventories contain no
production/test Host or App entry target. Each SDK was installed, moved and used
by the external `installed_application` fixture; no producer App was available.

全新 SDK producer 開啟 `VINC_INSTALL_SDK`，關閉 App、範例、測試及 Python 建置。
Ninja target 清單沒有正式／測試 Host 或 App 入口。每套 SDK 安裝、搬移後由外部
`installed_application` fixture 使用，producer 不提供可供偷用的 App。

| Environment / 環境 | Profile / 組態 | Result / 結果 |
| --- | --- | --- |
| Windows x64, MSVC 19.51.36257, CMake 4.3.2, Ninja 1.13.2 | Debug, UI off / UI 關閉 | Clean producer build, complete installed-App regression and detached execution passed. 乾淨建置、完整安裝式 App 回歸、獨立執行皆通過。 |
| Same Windows toolchain / 同上 | Release, UI on / UI 開啟 | Same checks passed, including custom-message UI preview. 同項檢查通過，包含自訂訊息 UI 預覽。 |
| WSL Ubuntu 24.04, GCC/libstdc++ 13.3, CMake 3.28.3, Ninja 1.11.1 | Release, UI on / UI 開啟 | Same checks passed; isolated framework source was also hidden for detached execution. 同項檢查通過；獨立執行時也移開隔離的框架原始碼。 |

The fixture now checks two distinct Module PIDs and compares each Module's
OS-reported image path with the central App path supplied by Tcl. Actual
build/install trees receive sorted SHA-256 inventories. They contain one App
process executable plus native Plugins; the SDK's only native executable is
Ribosome. No standalone Host, test Host or generated Host-path header was found.
The inventory guard's accepted/rejected fixtures passed on Windows (CTest 1/1)
and WSL (direct script), including a deliberately injected old Host and an
unrelated native executable. These are native-only package checks.

fixture 新增兩個不同 Module PID 及其 OS 執行檔路徑與 Tcl 交付中央路徑的比對。
實際 build／install 目錄輸出排序的 SHA-256 清單：App 只有一個程序執行檔及原生
Plugin，SDK 唯一原生執行檔為 Ribosome；沒有獨立 Host、測試 Host 或生成 Host 路徑
標頭。清單檢查器的正常／拒絕案例在 Windows（CTest 1/1）及 WSL（直接腳本）皆通過，
包含刻意放入舊 Host 及額外原生執行檔；此盤點範圍為純原生套件。

All three profiles used paths with Chinese characters and spaces, a different
cwd, two custom Plugins, push/loan publication, subscription callbacks, Bag
recording and orderly shutdown. UI-enabled profiles additionally checked HTTP
preview. Existing schema edit/delete/rejection, feature disable/restore,
user-file preservation and license restoration checks passed. An additional
fresh deployment ran after temporarily moving the SDK, producer and consumer
build trees away; all were restored afterward. WSL also hid its isolated source.

三組皆以中文／空白路徑、不同 cwd、兩個自訂 Plugin 驗證 push／loan、訂閱 callback、
Bag 錄製與正常關閉；UI 組另驗 HTTP 預覽。既有 schema 修改／刪除／拒絕、功能關閉／
恢復、使用者檔案保留、授權檔修復檢查皆通過。額外以全新部署副本，暫時移開 SDK、
producer 與 consumer build 後執行；完成後全部還原，WSL 亦移開隔離來源。

Local harness corrections: the first Windows script misdecoded its UTF-8 path;
the first extra detached run reused a recording directory, correctly rejected by
Bag; Windows process exit-code collection also needed a retained process handle.
Corrected fresh runs passed. The path incident exposed a separate existing
[configuration stdout encoding limitation](../../tools/configure/README.md#redirected-unicode-diagnostics--unicode-重導診斷),
which was unresolved in this run; the linked section records its later repair. No production runtime change
was necessary in this step.

本機測試腳本修正：首次 Windows 執行誤讀 UTF-8 路徑；額外獨立執行首次重用錄製目錄，
Bag 正確拒絕；Windows 退出碼蒐集也補上保留 process handle。修正後的全新執行通過。
路徑事件另外暴露既有設定工具 stdout 編碼限制，當時未修復；後續修正已記錄於上述 README。
本步不需修改正式 runtime。

Step 4's new-entry fault/authentication and Python/native mixed-operation results
follow below. Native macOS validation remains open. These results do not replace
historical macOS evidence or establish macOS validation for this change.

第四步新入口的故障／認證與 Python／原生混用結果見下方，macOS 原生驗證仍未完成。
本次結果不取代既有 macOS 證據，也不代表此改動已通過 macOS。

Local evidence / 本機證據（ignored / 不上 Git）：
- `build/single-entry-step3-{debug,release,linux}.log` (fresh builds / 乾淨建置).
- `build/single-entry-step3-{debug,release}-retry.log` (Windows complete App tests / 完整 App 測試).
- `build/single-entry-step3-inventory-ctest.log` (inventory guard / 清單檢查器).
- `build/single-entry-step3/{Debug,Release}/producer-targets.txt`,
  `套件 retry/sdk-inventory.txt`, `套件 retry/*-inventory.txt`,
  `detached-output.txt` (target/file inventories and detached run / 清單及獨立執行).
- WSL `/var/tmp/open-vinc-sdk.eE3JjSQa/step3-producer-targets.txt`,
  `step3-套件 work/*inventory.txt`, `step3-detached-output.txt`.

## Step 4 integration checks / 第四步整合檢查

The production entry is unchanged in this step. `app.process`, `app.context`,
`app.contract`, `app.debug`, `app.plugin`, `app.message_bus`, `app.param`,
`app.log` and `app.rpc` reuse the established behavioral suites with
`app_entry_plain` selected as their native Host. That target compiles the actual
App main; it is not a replacement test main. `tests/support/manager.hpp` owns the
optional test selection and Unicode conversion; production reads no such setting.
Explicit injection Hosts in specialized tests keep their original responsibilities.

本步不修改正式入口。上述九個 `app.*` 案例將既有行為測試的原生 Host 指向
`app_entry_plain`，此 target 編譯真正的 App main，沒有另寫替代 main。
`tests/support/manager.hpp` 管理測試用選擇及 Unicode 轉換，正式程式不讀取該設定。
需要故障注入的特定測試仍使用原有明確指定的 Host。

The raw-parent lifecycle test now sends a wrong token over a fresh listener,
using valid bootstrap/report/notification resources. The target Plugin stalls in
its load-time initializer; a bounded handshake-failure exit proves it was not
entered. Both `module.process` and `app.process` run this case. Existing invalid
marker/field checks in `app.entry` still require rejection without central Shell
startup. Existing completion, heartbeat loss, explicit stop, restart, fault report,
loader-failure and debug tests exercise the real App's Host branch through the
new variants. Windows debug coverage uses native Debug APIs, not a manual GUI
or GDB/LLDB session.

原有 raw-parent 測試新增錯誤 token，使用全新 listener 與有效 bootstrap／回報／通知
資源；指定的 Plugin 在載入期 initializer 會長時間停滯，有界的握手失敗退出可確認
沒有先執行該 initializer。`module.process` 與 `app.process` 都涵蓋此例。
`app.entry` 保留錯誤標記／欄位不進中央 Shell 的檢查；其餘既有正常完成、heartbeat
失聯、明確停止、重啟、fatal 回報、loader 失敗及 debug 政策，經新變體走正式 App Host。
Windows 的 debugger 覆蓋是原生 Debug API，不是人工 GUI 或 GDB／LLDB 操作。

`sdk.installed_python` additionally checks the native peer's actual App path and
three distinct central/native/Python PIDs. It retains SDK/App relocation with
SDK/consumer builds hidden, custom messages across C++/Python, RPC, Param, Log,
message schema regeneration, wrong Python ABI isolation, resource repair and
feature-disable cleanup. The selected system interpreter still owns the Python
process; the App does not embed Python. Windows uses the existing isolated
CPython with compatible CRT, without changing the system Python installation.

安裝式 Python 測試另確認原生 peer 實際 App 路徑，以及中央／原生／Python 三個不同
PID。保留搬移 SDK／App 並移開 SDK／consumer build、跨語言自訂訊息、RPC、Param、
Log、schema 增量生成、錯誤 Python ABI 隔離、資源修復及關閉功能的清理。Python 程序
仍由所選系統直譯器執行，App 不內嵌 Python；Windows 使用原有相容 CRT 的隔離 CPython，
沒有修改系統 Python 安裝。

<a id="remaining-native-macos-handoff--macos-原生待驗證"></a>

### Native macOS handoff / macOS 原生驗收交接

**Completed on 2026-11-02 within the recorded Release scope:** the prior
[Mac regression](../../src/vinc/module_system/python/README.md#macos-snapshot-repair-validation)
and subsequent [clean SDK delivery](../../src/vinc/sdk/INSTALL.md#macos-clean-sdk-acceptance)
cover this handoff. The following instructions preserve the original Windows
workstation handoff; its pending/no-runner statements describe that historical round.
The later [Debug SDK follow-up](../../src/vinc/sdk/INSTALL.md#macos-debug-sdk-acceptance)
adds complete Debug coverage in the same native environment.
下方保留當時 Windows 工作站的交接內容；Mac 已依分輪證據完成 Release 與後續完整
Debug 驗收，不把當時的「無 runner／待驗證」當作目前狀態。

This Windows workstation provides WSL, not a native macOS runner. Preserve the
historical macOS reports; they do not validate this new entry path. On the same
source revision, use the normal prepared macOS toolchain with `BUILD_TESTING=ON`,
`VINC_BUILD_APPS=ON`, `VINC_INSTALL_SDK=ON` and
`VINC_TEST_SDK_PYTHON_EXECUTABLE` pointing to the intended compatible Python.
After building, run the full suite or at least the following integration groups,
and record OS/architecture/compiler/stdlib/configuration and actual results:

本工作站提供 Windows／WSL，沒有原生 macOS runner。保留歷史 Mac 報告，但不可當成本次
新入口證據。Mac 端應使用相同來源版本及正常準備好的工具鏈，開啟上述測試／App／SDK
選項，並指定相容 Python。建置後執行全套或至少以下整合組，記錄 OS、架構、編譯器、
標準庫、組態及真實結果：

```sh
ctest --test-dir <build> --output-on-failure -R '^(app\.|module\.|message_bus\.|param\.|rpc\.|log\.|shell\.)'
ctest --test-dir <build> --output-on-failure -R '^sdk\.(installed_consumer|installed_application|installed_python|package_inventory)$'
```

Do not run two configurations' central-App tests concurrently on the same OS;
some fixtures deliberately share a fixed central name. No Mac result is claimed
until that run completes. The separate configuration stdout encoding issue from
step 3 remains recorded in the owning README.

同一 OS 不要同時執行兩個組態的中央 App 測試；部分 fixture 刻意使用固定中央名稱。
實際完成前不宣稱 Mac 通過。第三步發現的設定工具 stdout 編碼問題仍獨立記錄於所屬 README。

### Step 4 results (2026-10-30) / 第四步結果

| Environment / 環境 | General regression / 一般回歸 | Installed Python / 安裝式 Python | Final affected checks / 最終受影響檢查 |
| --- | --- | --- | --- |
| Windows x64, MSVC 19.51.36257, CMake 4.3.2, Ninja 1.13.2, isolated CPython 3.13.12; Debug | 101/101 passed / 通過 | 1/1 passed / 通過 | 12/12 passed after the Windows environment helper update / Windows 環境變數 helper 更新後通過 |
| Same Windows toolchain / 同上; Release | 101/101 passed / 通過 | 1/1 passed / 通過 | 12/12 passed after the same helper update / 同項 helper 更新後通過 |
| WSL Ubuntu 24.04, GCC/libstdc++ 13.3, CMake 3.28.3, Ninja 1.11.1, CPython 3.12.3; Release | 105/105 passed / 通過 | 1/1 passed / 通過 | 2/2 passed after moving token rejection to the fresh listener / token 拒絕改用全新 listener 後通過 |
| Native macOS / 原生 macOS | Pending / 待驗證 | Pending / 待驗證 | No runner available here / 本機無 runner |

General regression excludes the four longer SDK consumer/application/external/
Python suites. The updated Python suite ran separately. Step 3's clean native
SDK/deployment evidence and step 2's external/basic consumer evidence remain
applicable: this step changes tests/documentation, not production runtime or SDK
composition. All enabled non-SDK suites and the remaining SDK unit checks ran;
source Python bindings were disabled in these producers, while installed-Python
consumers built and executed real bindings. Test durations are not performance
measurements.

一般回歸排除四組較長的 SDK consumer／application／external／Python；有改動的 Python
另行完整執行。第三步乾淨原生 SDK／部署及第二步外部／基本 consumer 證據仍適用：本步只改
測試與文件，未改正式 runtime 或 SDK 組裝。其餘已啟用非 SDK 測試及 SDK 單元檢查皆執行；
producer 關閉來源 Python 綁定，安裝式 Python consumer 則實際建置並執行綁定。
測試耗時不作效能量測。

The final Windows helper uses an owned UTF-16 environment copy instead of the
CRT-deprecated getter; its rebuilt test targets produced no new helper warning.
The 12-case reruns cover default Host selection plus all App cases. POSIX selection
is unchanged by that Windows-only fix. Captured token-failure diagnostics report
ModuleSystem/Connect with a ControlChannel/Protocol/Handshake cause, rather than
an observed handshake timeout. Existing specialized fault-injection Host tests
also passed in general regression; they do not claim to be App-entry tests.

Windows 最終 helper 改用自行持有的 UTF-16 環境變數副本，取代 CRT 已棄用的 getter；
重建後沒有新增 helper 警告。12 項重驗涵蓋預設 Host 選擇及全部 App 案例；POSIX 路徑
不受此 Windows 專用修正影響。token 失敗日誌為 ModuleSystem／Connect，原因鏈含
ControlChannel／Protocol／Handshake，實際觀察不是握手逾時。一般回歸亦保留並通過
專用故障注入 Host 測試，不將它們冒稱為 App 入口測試。

Local logs / 本機日誌（ignored / 不上 Git）：
- `build/single-entry-step4-windows.log`, `build/single-entry-step4-linux.log`.
- `build/single-entry-step4-{debug,release,linux}-tests.log`.
- `build/single-entry-step4-{debug,release,linux}-python.log`.
- `build/single-entry-step4-final-{debug,release}.log`.
- `build/single-entry-step4-auth-final-{debug,linux}.log`.
- Windows `build/sdk-package-{debug,release}/sdk-python-test/flow-*.log` and
  `wrong-abi.log`; WSL `/var/tmp/open-vinc-sdk.eE3JjSQa/build/sdk-python-test/`.
