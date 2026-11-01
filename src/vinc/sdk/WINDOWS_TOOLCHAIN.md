# Windows toolchain evaluation / Windows 工具鏈評估

Date: 2026-11-01. Historical experiments below preceded the approved native MSVC
minimum of 19.44. This minimum does not add ABI guarantees or relax Runtime loading.

日期：2026-11-01。以下歷史實驗先於已核准的原生 MSVC 19.44 最低門檻；此門檻**不代表
新增 ABI 保證或放寬 Runtime 防護**。

## Scope and environment / 範圍與環境

The evaluated source is the working tree based on `fd01072`, including the
uncommitted build-entry, terminal/Shell and System Module changes. This is not
verification of that Git commit alone or of a released tag. No production
compiler checks, Runtime checks, installed Python files or system toolchains
were changed for that initial evaluation. The subsequent minimum-version policy
is recorded separately below.

驗證對象是以 `fd01072` 為基礎、包含尚未提交的 build 入口、終端／Shell 與 System Module
修改的工作目錄；不能將結果套用到該 commit 本身或正式 tag。初次評估未修改正式編譯器檢查、
Runtime 防護、使用者 Python 安裝或系統工具鏈；後續最低版本規則另列於下方。

- Windows 11 x64; Windows SDK 10.0.26100.0; CMake 4.3.2; Ninja 1.13.2.
- MSVC 19.42.34444.0 / toolset directory 14.42.34433.
- MSVC 19.44.35229.0 / toolset directory 14.44.35207.
- CPython 3.13.12 x64, GIL enabled; pybind11 3.1.0.
- Release, dynamic CRT; C++23 remains required. CMake selects the toolchain's
  supported language flag; this does not promise every C++23 facility.
- Positive Python tests use an isolated interpreter copy with VCRUNTIME
  14.51.36247.0; the required MSVCP/sidecar DLLs load from System32 at that version.
  The original interpreter, with VCRUNTIME140 14.42.34438.0, remains unchanged.

兩套工具鏈從 Microsoft 官方套件下載，依 Visual Studio catalog 的 SHA-256 驗證，並核對
`cl.exe` 的 Microsoft 簽章。編譯器、標頭、CRT 靜態／匯入庫與工具均來自各自版本；
Windows SDK 共用。不把新版 MSVC 的庫補進舊工具鏈。所有產物放在隔離的 build 子目錄。

## Results / 結果

| Configuration / 組合 | Evidence / 證據 |
| --- | --- |
| 19.42 Release, Python enabled | Full source build passed; selected integration suites **21/21 passed** (91.41 s). 完整來源建置及選定整合測試通過。 |
| 19.44 Release, Python enabled | Full source build and SDK-enabled incremental build passed. 完整來源及啟用 SDK 後的建置通過。 |
| 19.44 installed/moved C++ System Apps | `sdk.system_apps` passed (204.14 s). 安裝並搬移的 C++ 系統服務案例通過。 |
| 19.44 installed/moved Python multi-App | `sdk.multi_app` passed (418.28 s), including Python startup, coexistence and failure cases. 包含 Python 的完整多 App 驗證通過。 |
| 19.44 Release full CTest, installed SDK/Python Apps | **122/122 passed** (2081.03 s), including all five installed SDK/App suites, Python and System Module cases. 全套通過，含五個安裝式 SDK／App suite、Python 與 System Module。 |
| Original Python + either extension | Rejected by unchanged preflight before `vinc._native`; no access violation. 原始 Python 被既有防護拒絕，未載入原生擴充。 |
| Isolated compatible Python + either extension | Import succeeded; loaded DLL paths and versions recorded. 隔離相容 Python 匯入成功，已記錄 DLL 路徑與版本。 |

The 21-test selection covers Message Storage, Bus protocols/ring/lifecycle,
Module System, four Python suites, SDK consumer, Python RPC/Shell and Bag.
`module.python_runtime` ran 13 internal cases: 11 passed and two optional
old-DLL/WinRT-fixture cases skipped. No WinRT/OCR replay is implied.

21 項涵蓋訊息容器、Bus、Module、四個 Python suite、SDK consumer、Python RPC／Shell
與 Bag。Runtime suite 內部 13 案例為 11 通過、2 個需額外舊 DLL／WinRT fixture 的案例
略過；不代表重跑 WinRT 或 OCR。

The complete 19.44 run includes that same Runtime suite and its two fixture
skips. All 122 CTest entries passed; this count does not imply those optional
internal WinRT cases ran. `sdk.installed_python` passed in 401.14 s,
`sdk.installed_consumer` in 44.98 s, and `sdk.installed_application` in 310.23 s.

19.44 全套中的 Runtime suite 同樣略過上述兩個可選 fixture 案例；122 個 CTest entry
皆通過，不表示那些內部 WinRT 案例有執行。安裝式 Python、consumer 與 App suite 分別
耗時 401.14、44.98、310.23 秒。

A separate 19.44 Release preflight run supplied the isolated official 14.42
MSVCP DLL: **12 passed, one WinRT-only case skipped**. The real old DLL was
rejected before native import. This check used the normal loader policy.

另外以官方隔離 14.42 MSVCP DLL 執行 19.44 Release 防護測試：**12 通過、1 個 WinRT
專用案例略過**。真實舊 DLL 在原生匯入前被拒絕，此項使用正常防護，並非下述診斷略過。

This round does not establish Debug, other CPython minors, ARM64, Linux/macOS,
or cross-toolchain C++ Plugin/SDK compatibility. Prior 19.51 framework validation
remains separate evidence in component READMEs; the later policy checks below
only rebuild Common with 19.51, not the full framework.

本輪沒有建立 Debug、其他 Python minor、ARM64、Linux／macOS 或不同工具鏈混用 C++
Plugin／SDK 的新保證。19.51 框架既有驗證仍以元件 README 為準；下方後續規則驗證僅重建
19.51 的 Common，沒有重建完整框架。

## Why the original Python still fails / 為何原始 Python 仍被拒絕

At the time of this investigation, `runtime_metadata.py` used the selected `crtversion.h` version as one
conservative minimum for every directly imported MSVC Runtime DLL. It does not
calculate an independently proven ABI floor for each DLL.

本次調查當時，生成器將 `crtversion.h` 的版本作為各個直接匯入 MSVC Runtime DLL 的共同保守門檻，
並沒有分別推導每個 DLL 的最低 ABI 版本。

**Follow-up:** the [Python deployment contract](../module_system/python/README.md#deployment-baseline-selection--部署基準選取)
now uses actual per-DLL versions from the compiler installation's selected Redist
package. The table and bypass probes below remain historical evidence. See
[validation](../module_system/python/RUNTIME_VALIDATION.md) for the new results.

**後續修正：** 已改用所選工具鏈安裝內 Redist 的逐 DLL 檔案版本，完整契約與驗證見上方。
下表及繞過防護探針保留為歷史證據，不代表目前政策或舊 DLL 已獲相容保證。

| Selected compiler | Generated minimum | Catalog's corresponding redist DLLs | Original Python VCRUNTIME140 |
| --- | --- | --- | --- |
| 19.42.34444 | 14.42.34444.0 | 14.42.34438.0 | 14.42.34438.0 |
| 19.44.35229 | 14.44.35220.0 | 14.44.35211.0 | 14.42.34438.0 |

The old-toolset redist packages in the catalog are not, by themselves, a
Microsoft support promise for every serviced compiler package. An installer
may also select a newer global redist. This version mismatch warrants review;
it is not proof that arbitrary older DLLs are safe.

Catalog 中的舊工具鏈 redist 套件不能單獨證明 Microsoft 保證它與所有修訂版編譯器相容；
安裝器亦可能另外選用更新的全域 redist。版本不一致值得檢查，不等於任意舊 DLL 都安全。

Diagnostic-only subprocesses bypassed preflight **in child-process memory only**
and ran `python/tests/message_tests.py`:

- 19.42: all four required Runtime DLLs at 14.42.34438.0; passed.
- 19.44: Python's VCRUNTIME140 at 14.42.34438.0; MSVCP140, atomic-wait sidecar and
  VCRUNTIME140_1 at 14.44.35211.0; passed.

僅供診斷的獨立子程序在自身記憶體內略過 preflight，以上兩種組合完成訊息測試。
未修改磁碟上的正式防護／metadata，也未將這些結果當成受支援部署。這只能區分門檻拒絕
與已測操作的結果，不能證明其他執行路徑安全，更不能據此接受曾導致崩潰的 14.29 MSVCP。

## Restrictions reviewed / 限制盤點

| Boundary / 邊界 | Current behavior and recommendation / 現況與建議 |
| --- | --- |
| Source build | Before the minimum policy, 19.42/19.44 built successfully. The approved policy now accepts native MSVC 19.44 or newer; 19.42 is historical evidence only. 設門檻前兩版都能建置；現在最低為 19.44，19.42 僅保留歷史實驗紀錄。 |
| Build-directory reuse | Compiler identity, paths, SDK/include/lib environment prevent mixing old objects. Keep this check and choose a separate build directory when changing tools. 保留快取混用防護，換工具鏈使用新目錄。 |
| Installed C++ SDK | Exact compiler version plus standard-library/CRT/layout fingerprint are required. This implements the current same-toolchain Plugin contract. Cross-patch relaxation needs separate link/runtime tests, including `/GL`/LTCG constraints. 不在本輪自行放寬 C++ ABI。 |
| Python ABI | Each native extension targets one CPython minor/platform/bitness/GIL configuration. Building several extensions is different from lowering the VC Runtime floor. 其他 minor 需要各自建置／驗證，不可直接共用現有 `.pyd`。 |
| Python Runtime | Uses actual per-DLL deployment baselines; unsupported old DLLs are still rejected before native import. See the Python contract linked above. 已改用逐 DLL 部署版本；保留原生匯入前的舊 DLL 拒絕。 |

**Confirmed minimum:** native MSVC 19.44 or newer, without a patch pin. Newer
development toolchains remain accepted; build discovery does not force selection
of 19.44. The [Common contract](../common/README.md#build-toolchain-policy) owns the
shared check. Installed SDK ABI matching and Python Runtime metadata remain unchanged.

**已確認門檻：**原生 MSVC 最低 19.44，不鎖定修補版；可使用較新工具鏈，不強制自動選用
19.44。共用檢查由上述 Common 契約維護。SDK ABI 配對及 Python Runtime metadata 不變。

References / 依據：

- [Microsoft C++ binary compatibility and `/GL` restrictions](https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017)
- [Microsoft Runtime deployment requirements](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/)
- [Visual Studio 2022 lifecycle](https://learn.microsoft.com/en-us/lifecycle/products/visual-studio-2022)
- [STL notes on old Runtime/mutex incompatibility](https://github.com/microsoft/STL/wiki/VS-2022-Changelog#vs-2022-1710)

## Reproduction and local evidence / 重現與本機證據

### Minimum-policy validation / 最低版本規則驗證

After adding the minimum check, on the same Windows environment:

- Real MSVC 19.42 was rejected by build.py, root CMake, standalone Common, OSAL,
  Ribosome and setup's compiler probe, with an explicit minimum-version error.
- Real 19.44 passed setup and the Release SDK-enabled incremental build.
  `common.toolchain` and `sdk.installed_consumer` passed **2/2**. The latter checks
  install/relocation/link/run, simulated old-MSVC optional lookup without imported
  targets, and rejection of a different compiler version even above the minimum.
- Real 19.51 passed setup and a standalone Common Release configure/build.
- Python build/setup/SDK-install regression: **59 passed, 14 skipped** (73 total).
  Skips cover opt-in integration and unavailable platform-specific cases; they are
  not counted as passes. Native checks above ran separately.
- No new Debug, Linux or macOS run in this policy-only change. The earlier 122/122
  Release run is retained as prior runtime evidence, not presented as a new run.

新增門檻後，於相同 Windows 環境驗證：真實 19.42 在 build.py、根 CMake、獨立 Common／
OSAL／Ribosome 及 setup 均明確拒絕；19.44 通過 setup、Release SDK 增量建置及兩項 CTest。
SDK 案例包含搬移後編譯執行、模擬舊版 optional lookup 不留下 targets、與較新但不匹配的
編譯器仍被 ABI 前置檢查拒絕。19.51 通過 setup 與 Common Release 建置。
腳本回歸 **59 通過、14 略過**；此次不重跑 Debug、Linux／macOS 或先前的 122 項 Runtime 全套。

Local policy logs: `build/toolchain-compat/minimum-policy/`; these are disposable
validation artifacts. The SDK test deliberately modifies its own installed copy
to test rejection; that copy is not a usable SDK deployment.

本機規則驗證 log 位於上述忽略目錄。SDK 測試會刻意修改自己的安裝副本測拒絕行為，
該副本不是可交付的 SDK。

Activate the selected x64 MSVC developer environment before running `build.py`;
the wrapper preserves a complete activated environment. Use a distinct directory
per compiler/configuration. Positive Python tests need a compatible interpreter
Runtime; do not replace a user's installed DLLs to reproduce these experiments.

先啟用選定版本的 x64 MSVC 開發環境，再執行下列命令；build wrapper 會沿用完整已啟用
的環境。不同工具鏈／組態使用不同目錄。Python 成功案例須使用相容的隔離環境，不覆寫
使用者安裝目錄內的 DLL。

```powershell
$vincPython = 'C:/path/to/compatible/python.exe'
py -3 build.py --config Release --build-dir build/msvc1944-release --jobs 6 `
    -D VINC_BUILD_PYTHON=ON -D "Python_EXECUTABLE=$vincPython" `
    -D VINC_INSTALL_SDK=ON -D "VINC_TEST_SDK_PYTHON_EXECUTABLE=$vincPython"
ctest --test-dir build/msvc1944-release -C Release --output-on-failure
```

Local, ignored artifacts are under `build/toolchain-compat/`: `packages.json`
contains official download URLs/hashes; `*-build.log`, `*-tests.log` and CTest
`Testing/Temporary/LastTest.log` record builds/cases; `loader-probes.json` and
`runtime-experiment.json` record the unchanged guard and diagnostic-only probes.
`prepare.py`, `run.py`, `probe.py` and `runtime_experiment.py` are disposable local
investigation tools, not supported setup/build entry points or committed tools.

本機證據與下載工具位於上述忽略目錄；不是正式發行內容，也不是要維護的新安裝介面。
