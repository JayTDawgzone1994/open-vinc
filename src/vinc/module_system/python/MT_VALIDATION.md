# Isolated Windows `/MT` experiment / Windows `/MT` 隔離實驗

## Status and scope / 狀態與範圍

This is an experiment against commit
`908e1832f45f20b6cda85b93cfb654c5a374a85b`, performed on 2026-11-02.
It is **not a production CRT switch or a promise of universal Python compatibility**.
At the time of this experiment, production source, central, native Plugins and
installed SDK remained `/MD`. The subsequent source/installed-SDK rollout and its new
validation are recorded in the [rollout report](README.md#runtime-policy-rollout).
No interpreter or third-party Runtime DLL was replaced for this experiment.

本輪僅驗證 Python 原生擴充及其靜態相依鏈使用 `/MT` 的可行性；當時正式配置仍為 `/MD`。
後續來源版切換及重新驗證見 [導入紀錄](README.md#runtime-policy-rollout)。
現有成果已先建立上述本地 commit，沒有 push。實驗改動與產物位於忽略的
`build/mt-experiment/`，此文件保留結果與限制。

## Environment and construction / 環境與建置

- Windows 11 x64, build 26200; native MSVC 19.44.35229 / toolset 14.44.35207.
- CMake 4.3.2, Ninja 1.13.2, Release `/O2`, six build jobs.
- Unmodified official CPython 3.13.12 x64, GIL enabled.
  Its VCRUNTIME140 and VCRUNTIME140_1 are **14.42.34438.0**.
- Official WinRT 3.2.1 cp313 wheels; bundled MSVCP140 is **14.29.30157.0**.
- Two build trees from the same archived commit: `md/` and `mt/`.
  Dependency source junctions use the existing pinned upstream checkouts.
- Twenty static archives are separately built for the Python `/MT` link closure.
  Existing central/test-host/native-peer targets retain `/MD`; no global CRT flip.
  The experiment's target-copy helper is not proposed as the final SDK architecture.

The copied metadata generator recognizes an experimental static mode, verifies
that the extension has no direct MSVC CRT DLL imports and emits an empty dynamic
requirement set. The copied loader accepts only that marked static format with
an empty requirement set. Ordinary format-2 `/MD` checks remain active. Python
minor/platform/bitness/GIL bootstrap checks remain in place. Production integration
would need a reviewed static metadata format and installed-SDK build wiring.

實驗只調整與靜態 CRT 不再適用的動態 DLL 檢查，不以關閉全部防護來取得通過結果。
Python ABI 仍須符合建置版本；`cp313` 不因此能用於所有 Python 3.x。

## Link and ownership review / 連結與所有權審查

`dumpbin /DEPENDENTS` and `/IMPORTS` report only these direct dependencies for
the experimental extension: `python313.dll`, `WS2_32.dll`, `KERNEL32.dll`.
There are no extension delay imports. The dependency graph records ordinary
transitive imports and Windows API-set boundaries; no VINC-owned intermediate
DLL reintroduces dynamic MSVC CRT. Python and Windows may themselves depend on
dynamic CRT DLLs. Those dependencies have **not** disappeared from the process.

`/DIRECTIVES` was inspected for all twenty `/MT` archives, with no
`MD_DynamicRelease` entries. All 61 compile commands in the extension/clone closure
use `/MT` and not `/MD`. An incorrect initial target-copy wiring
mixed CRTs and failed with LNK2038; it was corrected before successful measurements.
The successful link does not suppress runtime mismatch diagnostics.

| Boundary / 邊界 | Evidence and limitation / 證據與限制 |
| --- | --- |
| Module creation/destruction | `PythonSource::create()` supplies `new PythonModule` and its paired deleter in the same extension. Runtime is statically linked into that image. |
| Python ownership | pybind11 uses Python allocation/type APIs for Python objects. C++ holders and their destruction code remain in the extension. Reviewed `tp_alloc`/`tp_free`, `PyObject_New`/`PyObject_Free`, `PyMem_Calloc`/`PyMem_Free`, and binding function records. |
| Message storage | Local messages own C++ storage. Serialization constructs Python `bytes` by copying; Python does not free a C++ allocation. Loan wrappers invalidate retained views before releasing their shared-memory backing. |
| Context | `ContextState` controls admission and waits for outstanding calls; native teardown revokes it without holding the GIL, then acquires the GIL to clear Python resources. Retained Context wrappers cannot regain a destroyed native pointer. |
| Callbacks | Callback references acquire the GIL for use/destruction; an active callable has its own retained reference. Runtime failure reporting preserves `VincError` causes. |
| CRT-local state | No public binding passes `FILE*`, C++ allocator-owned buffers, `errno` pointers or locale objects to Python/WinRT. Script loading uses Python importlib. No assumption was found that Python's `setlocale()` or CRT environment cache updates VINC's private CRT. This is scoped source review, not an audit of every Windows/Python implementation. |
| Other C++ extensions | pybind11 distinguishes `/MD` and `/MT` platform ABI identities. Ordinary Python objects can be exchanged; arbitrary cross-extension `std::string`, `std::vector`, holders or native pointers are not a new supported ABI. |

## Functional verification / 功能驗證

The import matrix completed **5/5 expected outcomes**:

| CPython 3.13.12 scenario / 情境 | Result / 結果 |
| --- | --- |
| `/MT`, no WinRT | Import and repeated generated-message tests passed. |
| `/MT`, manually preload old MSVCP | Passed with the old DLL still loaded. |
| `/MT`, WinRT before VINC | Messages, WinRT operations and actual OCR passed. |
| `/MT`, VINC before WinRT | Messages, WinRT operations and actual OCR passed. |
| `/MD`, original Python 14.42 VCRUNTIME | Expected preflight ImportError; no guard bypass or native crash. |

OCR recognized **`OPEN VINC 12345`** from the input PNG. Enumeration after the
WinRT-first case confirmed Python's two VCRUNTIME DLLs remained 14.42 and WinRT's
MSVCP remained 14.29. Success did not depend on silently substituting newer files.

The lifecycle, services and RPC executables passed **18/18 runs**: three rounds,
each with normal bootstrap and deliberate WinRT-first bootstrap, under
`PYTHONMALLOC=debug` and `PYTHONFAULTHANDLER=1`. Coverage includes Context modes,
Shell hooks, bidirectional native/Python RPC and Bus, message retention, loan
commit/close/expired views, capacity errors, Param callbacks, structured Log,
callback exceptions and causes, restart/new MID, blocked hooks/imports and central
loss. Existing service cases also verify survival of another native Module.
The matrix was repeated with the native test hosts/peers freshly built from the
experiment's archived source, also **18/18**. These are repeated runs, not 36
different test scenarios.

### Additional checklist probes / 額外驗收探針

- **100 sequential starts/stops passed** with WinRT deliberately loaded first:
  34 active-callback stops, 33 uncommitted-loan stops and 33 callback failures.
  Three concurrent Python Context readers and a separate pump thread were used.
  Normal stops verify retained Context/Loan/view rejection after native teardown,
  preserved independently owned messages and joined worker threads.
- Callback failures verify a reported failure and nonzero exit. They do not
  require normal teardown: the existing watchdog can terminate a failed Module.
  It would be incorrect to count OS reclamation as successful C++ destruction.
- A copied binding adds test-only C++ exception probes. `std::bad_alloc` becomes
  `MemoryError`, `std::runtime_error` becomes `RuntimeError`, and stack cleanup
  runs in both cases. This is **not exhaustive allocator-failure injection**.
- UTF-8 Chinese stdout/stderr were captured and checked during every cycle.
  Callback diagnostics also travel through the extension's native `std::cerr`.
  An additional run launched a Module from `中文路徑/壓力模組.py` through OSAL,
  imported it, stopped it during a callback and verified retained-reference refusal.
- Shutdown can reject an already requested Param lock with the structured
  `ModuleSystem / ManagerClosing / AccessLock` cause. The probe checks that cause;
  it does not silently accept arbitrary pump failures.

- Static metadata admission/refusal probes passed **6/6**, including malformed
  static requirements and absent/false mode markers. CPython 3.14 was explicitly
  rejected by the cp313 bootstrap before native loading.

### Resource observations / 資源觀察

A **120-second** single-Module workload passed with concurrent Context readers,
pump, Pub/Sub, RPC, Param updates, and repeated copy/serialize/deserialize of a
4 MiB message containing nested arrays and a UTF-8 string. Every tenth iteration
also collected Python garbage. The last sample at 110.15 seconds recorded
11,762 Bus callbacks, 1,177 RPC completions and 1,177 Param callbacks.

| Sampled child resource / 子程序取樣 | Observation / 結果 |
| --- | --- |
| Private bytes | 25,219,072–25,337,856 B, approximately 24.05–24.16 MiB |
| Handles | 173 throughout all eleven samples |
| Threads | 11 initially, then 8; no increasing trend |

This run showed no sustained growth in sampled child resources. It is a two-minute
soak, not an hours-long leak qualification. Some regression cases ran concurrently
during the final part; this resource observation is not a latency/throughput benchmark.

Across the separate 100-cycle run, central handles typically returned to 134–135,
with transient samples up to 139. Central private bytes increased from 1,941,504
to 2,519,040 B (about 0.55 MiB). That initial observation did not distinguish
bounded diagnostic history from a leak. The follow-up below identifies retained
Log display history as a substantial, intentional allocation source; Private
Bytes alone is not a count of live framework allocations. The normal and fault
exits all remained observable/manageable.
Some deliberately faulted cycles also emitted a Log shutdown diagnostic with
WinSock 10054; it was not suppressed or counted as an unexpected native crash.

### Central memory attribution / 中央記憶體增長歸因

The central test executable still uses `/MD` and runs in a different process from
the `/MT` Python extension. Its memory observation is not a measurement of the
extension's static CRT overhead.

Source review identified Log's separate Shell display history in
`src/vinc/log/src/manager.cpp`, `Impl::enqueue()`. It deliberately retains recent
stdout/stderr and structured/diagnostic records after the disk queue drains and
the originating Module exits. The existing budget is 1,024 records / 8 MiB of
accounted text and metadata, shared with the queue's configuration defaults;
older records are evicted. See the [Log contract](../../log/README.md#執行緒與傳輸).
The byte budget is not a strict limit on total allocator/heap overhead.

An experiment-only observer in the archived source counted this history and
cleared **only** that container after 150 start/stop/failure cycles. It did not
change production APIs or disable logging. The same original Python 14.42 /
WinRT 14.29 setup and `/MT` extension were used. The run passed.

| Point / 觀察點 | History records / 筆數 | Accounted history bytes / 邏輯 bytes | Windows busy heap bytes / 活躍配置 | Private Bytes |
| --- | ---: | ---: | ---: | ---: |
| After 50 cycles | 426 | 132,633 | 505,488 | 2,289,664 |
| After 100 cycles | 863 | 269,087 | 668,104 | 2,539,520 |
| After 150 cycles, idle | 1,024 | 319,870 | 739,360 | 2,666,496 |
| Clear only display history | 0 | 0 | 347,001 | 2,494,464 |

Pending disk records, retained Log peers and partial text fragments were all zero
at these observations. Clearing history released **392,359 B of busy heap data
and 5,160 busy blocks**. Private Bytes decreased by only 172,032 B: these are
different metrics, and freeing objects need not return all committed heap pages
to the OS immediately. This establishes a real, bounded retention source; it
does not attribute every byte of the original 577,536 B increase.

A separate **600-cycle** run passed in 432.48 seconds, with the original central
Log implementation unchanged. It used 200 active-callback stops, 200 uncommitted-
loan stops and 200 callback failures. Only the harness measured process/heap
usage; the history observer/clear hook above was absent from this executable.

| Completed cycles / 已完成輪數 | Busy heap bytes / 活躍配置 | Private Bytes | Handles |
| --- | ---: | ---: | ---: |
| 50 | 505,206 | 2,318,336 | 134 |
| 100 | 668,126 | 2,551,808 | 134 |
| 150 | 740,134 | 2,727,936 | 134 |
| 300 | 742,740 | 2,703,360 | 132 |
| 450 | 743,700 | 2,764,800 | 131 |
| 600 | 739,135 | 2,732,032 | 131 |

Across every 50-cycle sample from 150 through 600, busy heap data stayed within
736,420–743,700 B (719–726 KiB), Private Bytes within 2,703,360–2,764,800 B
(2.58–2.64 MiB), and handles within 131–134. Busy block counts rose from 6,330
at 150 to approximately 7,055 by 250–550 while total busy bytes stayed flat;
counts alone therefore do not establish a byte-growth leak. At 600 the count
was 7,042. This is a bounded-workload plateau, not proof against arbitrarily
small leaks or leaks on other paths. The 150-cycle attribution probe overlapped
part of this run; neither result is used as a performance benchmark.

After forgetting the last Module, shutting down and destroying the central
Manager, busy data fell further to 199,830 B and Private Bytes to 1,777,664 B.
The harness still owned its OutputHub subscription and runtime infrastructure,
so this final sample is not a completely torn-down process baseline. The existing
WinSock 10054 Log shutdown diagnostic appeared again after deliberate fault exits;
the harness completed successfully and did not suppress that diagnostic.

Measurements use `GetProcessMemoryInfo` and a fixed-capacity `GetProcessHeaps` /
`HeapLock` / `HeapWalk` observer, summing `PROCESS_HEAP_ENTRY_BUSY` data. All
observations completed without heap-walk errors. Busy heap data excludes heap
overhead and non-heap allocations; sequential heap walks are not an atomic
whole-process snapshot. See Microsoft's
[HeapWalk documentation](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapwalk).

釐清結果：中央確實會保留最近的 Log 顯示紀錄，Module 結束不會立即刪除這些歷史。
150 輪對照已確認它占用的活躍配置會在清除後釋放；這部分是有界的預期行為，
不能把先前 Private Bytes 增加約 0.55 MiB 直接判定為洩漏或 `/MT` 副作用。
600 輪測試中，第 150～600 輪的活躍配置及 Private Bytes 維持平台狀態，沒有持續
累積的趨勢；已找到主要來源，不再列為完全未釐清。
也不能用這項有限測試宣稱所有路徑絕對沒有洩漏。未修改正式程式。

### Heap instrumentation completed / Heap 工具驗證完成

After the initial WinError 740, the owner approved Windows UAC elevation.
Application Verifier **10.0.26100 x64** ran on the disposable image name
`openvinc_mt_verify_908e183.exe`. Its configuration query confirmed **Heaps with
Full=true (Full Page Heap), Handles and Locks** enabled. Generic python.exe and
the native central executable were not instrumented. `PYTHONMALLOC=debug` and
`PYTHONFAULTHANDLER=1` were also enabled for the test children.

- WinRT-first, original Python 14.42 VCRUNTIME and WinRT 14.29 MSVCP: repeated
  message operations and actual OCR passed (driver reports `import 0`).
- The 100-cycle active-callback/uncommitted-loan/callback-failure workload passed
  again under Verifier (driver reports `cycles 0` and `MT_STRESS_PASSED`).
- **102 binary session logs** (driver, import/OCR process and 100 Module launches)
  were exported with `appverif -logtoxml` and parsed individually. All contain
  session headers and **no diagnostic/stop entries**. Windows reused some PIDs;
  session count is not a count of unique PID numbers.
- The first cleanup removed all verifier values but left an empty per-image
  registry key. The export step removed that empty experiment-owned key and
  confirmed it absent. No verification processes remain. The temporary renamed
  Python executable was removed after confirming it matched the original hash;
  original Python and Runtime DLLs were not changed.

Heap／Handle／Lock 指定檢查已完成，測試案例未發現 Verifier 錯誤。專用設定與臨時
執行檔已清除；102 份原始／XML 日誌保留在 `build/mt-experiment/verifier-logs/`，
解析結果為 `verifier-summary.json`，啟用與清理證據分別在 driver／export transcript。

This covers the exercised paths, not proof of absence of every memory error.
The separate Verifier Leak/LowRes/fault-injection tests were **not** enabled;
the native central was investigated separately in the memory-attribution section
above. Longer soak and exhaustive allocation-failure behavior remain separate work. See
Microsoft's [per-image verifier workflow](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/application-verifier-testing-applications).

### Checklist disposition / 清單對照

| Item / 項目 | Evidence / 證據 |
| --- | --- |
| 1. CRT consistency | 61 compile units, 20 archive directive dumps and successful link. |
| 2. DLL isolation | Import/dependency audit plus actual old-DLL enumeration before/after OCR. |
| 3–4. Allocators and lifetimes | Scoped ownership review, exception cleanup, retained/revoked objects, repeated GC and Full Page Heap/Handle/Lock verification; no stop entries in the exercised sessions. |
| 5–6. Mutex, threads, GIL/callback | 100-cycle stop/failure matrix, concurrent Context calls, pump and normal joins; stuck-code supervision in existing cases. |
| 7. Exceptions | Python/VincError cause chains, native exception translation and stack destructor probes; no exhaustive real-OOM injection. |
| 8. Loan/data integrity | Existing commit/close/expired-view/capacity cases and uncommitted-loan stop probes; large nested copies in soak. |
| 9. RPC/Param/Log | Existing typed services/RPC cases plus continuous mixed workload. This does not exercise every possible user-defined schema. |
| 10. CRT-local state | Scoped boundary review, Chinese output/path, native diagnostics; no promise of shared locale/environment caches across CRTs. |
| 11. Lifecycle/resources | Existing restart/timeout/peer-survival cases, 100 cycles and two-minute soak; 600-cycle central plateau and separate 150-cycle history-clear attribution identify bounded Log retention, without claiming byte-for-byte or exhaustive leak proof. |
| 12. WinRT/OCR | Both load orders and actual OCR passed without replacing DLLs. |

These entries describe evidence, not an unconditional production sign-off.

## Measured cost / 實測成本

Sizes below are from the successful build **before test-only exception probes**.
MiB means 1,048,576 bytes. Package totals exclude PDB, `__pycache__` and the injected
test bootstrap, and include generated Python files and license notices.

| Artifact / 產物 | `/MD` | `/MT` | Difference / 差異 |
| --- | ---: | ---: | ---: |
| `_native.pyd` | 2,072,064 B | 2,366,464 B | +294,400 B, **+0.281 MiB / 14.2%** |
| Python package | 2,166,105 B | 2,460,175 B | +294,070 B, **+0.280 MiB** |
| Additional static build archives | None | 22,882,092 B across 20 archives | **21.82 MiB** of extra build/SDK material if both variants are shipped |

The archives are build inputs, not extra application DLLs. Python.exe is not rebuilt
or bundled. Each deployed Python package receives the slightly larger extension.

| Serial wall-time measurement / 依序量測 | `/MD` | `/MT` |
| --- | ---: | ---: |
| Clean Python extension and its dependencies | 64.79 s | 61.67 s |
| Then build native lifecycle/services/RPC hosts and peers | 39.61 s | 66.95 s |
| Combined representative build | **104.40 s** | **128.62 s** |

The combined `/MT` experiment costs about **24.22 s / 23.2% more** in this sample,
because the native hosts also need the original `/MD` dependency variants.
This is one serial six-job sample with warm filesystem/compiler caches, not a
statistically stable speed comparison. The slightly faster extension-only number
does not show that `/MT` is faster. This is not a full UI/npm/App/installed-SDK build.
Incremental-build timing and other machines were not measured.

## Remaining work before a production switch / 正式切換前仍需完成

1. Review the permanent target variants and installed-SDK export/deployment wiring;
   retain `/MD` for central and C++ Plugin ABI, and validate custom generated messages.
2. Review static metadata and fail-closed malformed-mode tests as production code.
3. Validate other supported Python minor ABIs and Debug separately. `/MT` removes
   VINC's dynamic CRT dependency; it does not make CPython minor ABIs interchangeable.
4. Repeat longer soak/heap diagnostics for release qualification. A successful
   WinRT 3.2.1 fixture is not proof that all third-party native extensions are safe.
5. Static CRT security fixes require rebuilding/redeploying the extension; updating
   the machine's VC++ Redistributable does not patch this embedded runtime.

No Linux/macOS behavior, free-threaded Python, subinterpreters or full external
probe application was validated by this Windows experiment.

## Local evidence / 本機證據

Ignored directory `build/mt-experiment/` retains `prepare.py`, `mt.cmake`, `run.py`,
`benchmark.py`, `audit.py`, `probe.py`, `validate.py`, the added stress harness,
`timings.jsonl`, `audit.json`, dependency/import/directive dumps, matrix JSON and logs.
The archived source derives from the commit at the top, with experiment-only
patches applied by `prepare.py` and `add_probes.py`. Test scripts use disposable
official Python/WinRT fixtures under `build/runtime-review-20261102/` and the
isolated MSVC toolset under `build/toolchain-compat/`; they are not portable SDK tools.

Central attribution evidence is in `retention.log/json` (600 cycles),
`history.log/json` (150 cycles), `retention-summary.json`, and their preparation,
harness and runner scripts. `retention_summary.py` checks successful exits,
cycle counts and heap-walk status before summarizing. These files and the
Verifier exports are included in the local `mt-evidence.zip`.

The baseline commit was created locally; **nothing was pushed**.
