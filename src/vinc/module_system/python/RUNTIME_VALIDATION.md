# Windows Python Runtime diagnosis / Windows Python Runtime 診斷

Recorded 2026-11-02 on Windows 11 x64, build 26200. This is diagnostic evidence,
not a change to supported Runtime floors or a claim of universal compatibility.

2026-11-02 於 Windows 11 x64（26200）驗證。這是診斷紀錄，不修改 Runtime 門檻，
也不代表所有 Python／原生套件組合均受支援。

## Scope and input integrity / 範圍與輸入完整性

- Fresh official NuGet `python` packages: **3.13.12** and **3.14.10**. Both
  `python.exe` signatures validated as Python Software Foundation. After tests,
  all 1,331 / 1,385 extracted archive files remained byte-identical to their
  respective downloads. No DLL was replaced, including in the test copies.
- Original PyPI **WinRT 3.2.1** wheels for cp313/cp314: runtime, Foundation,
  Foundation.Collections, Globalization, Media.Ocr, Graphics.Imaging, Storage,
  Storage.Streams; typing_extensions 4.15.0. Downloads matched PyPI SHA-256.
  The bundled MSVCP140.dll remained identical to the wheel in both environments.
- Existing Release VINC bindings were reused with matching Python minor ABIs:
  MSVC 19.44 / toolset 14.44.35207 for cp313, and MSVC 19.51 / toolset
  14.51.36231 for cp314. No rebuild, `/MT`, preflight bypass, metadata edits,
  WinRT upgrade, global PATH change, or installed-environment update occurred.
- Each case used a fresh `python -I -B -X faulthandler` process with explicitly
  selected package directories and a 45-second parent-enforced timeout. DLL
  snapshots were taken after diagnostic standard-library imports, before either
  target package, and after each target import. They are not an OS-loader trace.

使用全新解壓的官方 Python 與原版 WinRT 3.2.1；測後逐檔比對 Python 壓縮檔，
核對 WinRT wheel 雜湊及其中的 MSVCP。重用對應 Python minor 的既有 Release 擴充，
保留原防護；每案都是獨立、有限時程序。未變更正式環境或測試副本的 DLL。

## Results / 結果

| Interpreter / 直譯器 | VINC Runtime floor / 門檻 | Direct VINC / 直接載入 | WinRT first / WinRT 先載入 | VINC first, then WinRT / VINC 先載入 |
| --- | --- | --- | --- | --- |
| Official CPython 3.13.12 | 14.44.35220.0 | Rejected: Python's VCRUNTIME140 14.42.34438.0 | WinRT adds MSVCP140 14.29.30157.0; rejection reports the already-old Python VCRUNTIME first | Rejected before WinRT; no coexistence claim |
| Official CPython 3.14.10 | 14.51.36244.0 | Passed | Rejected: WinRT's MSVCP140 14.29.30157.0 | Passed: Array operations, VINC message operations, actual OCR, message operations again, normal exit |

All four refusals were `ImportError` before `vinc._native` appeared in
`sys.modules`; no access violation was observed. Diagnostic exit code 10 means
the probe caught a failure, not the framework's bootstrap exit code.

四個拒絕案例均在原生擴充匯入前回報 ImportError，沒有發生 access violation。
Python 3.13 的 WinRT-first 案同時存在兩種舊 DLL，錯誤文字先指出 Python 的 VCRUNTIME；
DLL 清單另證實 WinRT 帶入 14.29 MSVCP，不能只靠第一行錯誤判定唯一問題。

The successful cp314 case used Python's original VCRUNTIME140 **14.51.36247.0**,
and System32 MSVCP140, MSVCP140_atomic_wait and VCRUNTIME140_1 at the same version.
No MSVCP140 from the WinRT package was loaded. `message_tests.py` exercised
generated scalar/array bindings and ownership checks before and after OCR.
The real Windows en-US OCR engine recognized **OPEN VINC 12345** from a generated
local PNG. Bitmap/stream resources were closed, and the process exited with 0.

成功案例使用官方 Python 原附的 14.51 VCRUNTIME，加上 System32 的相容 Runtime。
沒有載入 WinRT 自帶的舊 MSVCP；在 OCR 前後各執行一次既有訊息測試，並由 Windows OCR
辨識合成圖片的英文與數字，完成資源關閉及正常退出。這不是只測 import 成功。

The first restricted-sandbox OCR attempt timed out after the message checks.
The bounded rerun outside that sandbox completed OCR and exited normally;
the final rerun also repeated message checks after OCR. Preserve the initial
timeout as an environment limitation, not evidence of a VINC Runtime deadlock.

首次受限沙箱執行在訊息測試後等待 OCR 逾時；以正常本機權限執行同一測試可完成。
保留初次紀錄，不把它當成已證實的 VINC deadlock。

## Initial conclusions before the correction / 修正前的結論與待確認

1. **There are two independent obstacles.** Official Python 3.13.12 already loads
   an older VCRUNTIME before WinRT is involved. This is not solely a 14.51 floor:
   the 19.44 binding's own 14.44 floor also rejects it.
2. **WinRT 3.2.1 can coexist in the tested environment.** VINC-first preflight
   selected compatible DLLs, and actual OCR/message operations passed. This does
   not promise correctness for every third-party extension or forced DLL load.
3. **No new minimum Python or WinRT version is decided.** Python 3.14.10 is a
   successful control, not a requirement. No WinRT 4.x dependency or automatic
   upgrade is justified by these results.
4. **The per-DLL minimum remains unresolved.** A guard refusal proves a policy
   mismatch, not an inevitable crash. These tests do not establish which older
   VCRUNTIME versions are safe. Review `runtime_metadata.py`'s common header-based
   floor and Microsoft deployment guarantees before considering a policy change.
5. This run did not reproduce the historical unguarded crash, obtain a native
   crash stack, run the full probe application, or test central/Bus sessions,
   Debug, Linux or macOS. The successful OCR fixture is not the probe's entire
   screenshot/OCR workflow.

兩個障礙已分離：Python 原附 DLL 先被門檻拒絕，以及 WinRT 載入順序導致的舊 MSVCP。
WinRT 3.2.1 在本次正常 Runtime、VINC-first 組合中可共存；不需要由此要求全面升級
WinRT 或 Python。下一步仍是釐清各 DLL 的部署門檻，而不是直接放寬防護。
本次未測原探針全流程、中央跨程序服務、Debug 或其他 OS，也沒有繞過防護重現舊崩潰。

## Local evidence / 本機證據

Ignored directory: `build/runtime-review-20261102/`.

- `inputs.json`: official download URLs and SHA-256 values.
- `final-evidence.json`: OS, integrity checks, source hashes, exact commands and
  final process exit codes. `matrix-results.json` retains the initial sandbox run.
- `*.stdout.jsonl` / `*.stderr.log`: DLL paths/versions, extension hash, errors and
  OCR results. Initial sandbox timeout logs are separately retained.
- `prepare.py`, `probe.py`, `run_matrix.py`, `ocr.py`, `verify.py`: disposable
  investigation tools, not maintained setup APIs. `ocr-input.png` is synthetic.

The cp313 extension SHA-256 was
`9477d47ff962059c2f6433f93130f6dde0eb6bc936ce248c2583a181ff52aa40`;
cp314 was `bfa488e75814122aa298ee04ba4850b9cf0212ed3f01ea3a191db4e6510e24eb`.
The working tree contained earlier uncommitted work; these hashes identify the
tested binaries more precisely than repository HEAD alone.

本機原始證據與臨時腳本不提交；本文件保留可審查的環境、結果及限制。

## Deployment-floor correction / 部署門檻來源修正（2026-11-02）

This follow-up changes metadata to format 2. The owning selection contract is
in the [Python README](README.md#deployment-baseline-selection--部署基準選取).
The earlier six-case results above describe format 1 and remain historical.

後續已改成 format 2；選取規則由 Python README 維護。上方六案保留為修正前證據，
不應將當時 Header 推導的版本當作目前部署門檻。

### Source evidence / 來源依據

- Microsoft's [DLL deployment guidance](https://learn.microsoft.com/en-us/cpp/windows/determining-which-dlls-to-redistribute)
  identifies the VS Redist directories, compatible major/minor families and use
  of the latest deployment files. We read file versions from that deployment
  package; we do not derive DLL patch versions from compiler/header patch numbers.
- The local official VS catalog's `Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64`
  includes both `Microsoft.VC.14.44.17.14.Tools.HostX64.TargetX64` and
  `Microsoft.VC.14.44.17.14.CRT.Redist.X64`. The extracted packages were previously
  verified against catalog hashes. This supplies deployment-source evidence,
  not a claim that arbitrary old/mixed DLLs are supported.
- No Runtime files are bundled with VINC by this change. Release/Debug remain
  dynamically linked, and all installed Python/WinRT/System32 files are unchanged.

依官方部署文件與 Visual Studio component 的套件接線，讀取 Redist 實體 DLL 版本；
不再由 Header 修訂號推導。這些資料不是任意混搭舊 DLL 的相容保證。本輪沒有散布
Microsoft DLL、切換 `/MT` 或變更正式 Python、WinRT、System32。

| Build / 建置 | Previous header floor / 原門檻 | Selected deployment files / 新基準 |
| --- | --- | --- |
| MSVC 19.44.35229, Release, cp313 | 14.44.35220.0 | Four imported DLLs at 14.44.35211.0, from `Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT` |
| MSVC 19.51.36257, Release, cp314 | 14.51.36244.0 | Four imported DLLs at 14.51.36247.0, from `Redist/MSVC/14.51.36231/x64/Microsoft.VC145.CRT` |

The per-DLL values happen to agree within each of these Release packages; the
generator/loader also tests different values for different DLLs. A corrected
source can raise or lower the numerical floor. It is not an unconditional relaxation.

這兩個 Release 套件的逐 DLL 版本剛好相同；測試另外驗證不同 DLL 可以使用不同門檻。
修正來源可能降低或提高數字，不是無條件放寬。

### Validation / 驗證

- Windows 11 x64 build 26200: both Release binding builds completed. Python
  lifecycle, messages, metadata, preflight, Context services, RPC and Shell:
  **7/7 CTest entries passed with each compiler**.
- Generator unit tests: **5/5 passed**. Covered header/file patch divergence,
  package selection, absent/old packages, architecture and Debug separation,
  missing/corrupt/wrong-architecture/old DLL files.
- Preflight suite with real old-DLL and WinRT 3.2.1 fixtures: **16/16 passed,
  no fixture skips**. Covered per-DLL thresholds, malformed/old metadata,
  loaded duplicates, side-effect loads, actual imports and load-order behavior.
- A new isolated copy of official CPython 3.13.12 received the four official
  14.44.35211 DLLs **only inside this disposable copy**. Enumeration confirmed
  all four actual loaded versions/paths. `import vinc` and generated-message
  operations passed with the 19.44 binding. No guard bypass or metadata edit.
- Unmodified official CPython 3.13.12 still rejects its original VCRUNTIME140
  14.42.34438 before native import. Official 3.14.10 plus WinRT-first still rejects
  MSVCP140 14.29.30157. Both are expected ImportError refusals, not native crashes.
- Official 3.14.10 plus VINC-first passed WinRT Array operations, actual en-US OCR
  (`OPEN VINC 12345`), message tests before/after OCR and normal exit.
- MSVC 19.51 Debug CRT smoke: a small `/MDd` DLL generated Debug metadata from
  `debug_nonredist`, passed preflight and executed an exported mutex operation.
  This is **not a full Debug framework rebuild/suite**.
- MSVC 19.44 `sdk.installed_python`: **passed (395.73 s)**. Installed/moved SDK,
  external App build and relocation, custom-message additions/removal/recovery,
  central/native/Python flows and shutdown, ABI refusal and deployment repair
  passed with the installed format-2 generator/loader.

兩種 Release 工具鏈各 7/7；生成器 5/5；帶真實舊 DLL 與 WinRT fixture 的載入防護測試
16/16、無略過。測試副本的四個 14.44 DLL 實際載入後完成訊息操作；原版 Python 的
14.42、WinRT-first 的 14.29 仍被拒絕。新版官方 Python 的 VINC-first 實際 OCR 及前後
訊息測試成功。Debug 僅驗證小型 CRT 原生載入與 mutex，不宣稱完整 Debug 驗證。
19.44 的 installed Python SDK 測試亦通過：涵蓋安裝／搬移 SDK、外部 App、自訂訊息
增刪與失敗後恢復、程序互動與退出、ABI 拒絕及部署修復。

The first concurrent two-build CTest run collided on the same central instance
name in `module.python_shell`; running the suites without that overlap passed.
The first scratch OCR run omitted its PNG input; after restoring the fixture,
the same bounded run passed. These harness failures are not DLL compatibility evidence.
No Linux/macOS or full probe-application validation was performed in this Windows-only change.

首次並行跑兩個建置目錄時，Shell 測試使用同名中央而互斥；錯開後通過。首次臨時 OCR
案例少複製 PNG，補齊 fixture 後通過。這些測試配置失敗不歸因於 DLL。此輪沒有重跑
Linux／macOS，也沒有驗證完整探針應用。

Local logs, commands, generated manifests and disposable helpers are under
`build/runtime-floor-20261102/`; `integration-results.json` records the initial
matrix and `vinc-first-ocr-rerun.json` its corrected OCR rerun.
