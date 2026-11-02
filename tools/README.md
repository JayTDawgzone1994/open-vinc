# Development tools / 開發工具

## Build entry / 統一建置入口

The root `build.bat` (Windows) and `build.sh` (Linux/macOS) delegate to the same
standard-library-only root `build.py`. Ordinary invocations **configure, then build**, defaulting
to Debug with native Ninja. They do not install dependencies, update submodules,
run npm installation, edit Kconfig files, run tests/Apps, install packages, or clean
old files. CMake remains responsible for messages, UI assets and App composition.

根目錄雙入口共用 Python 標準函式庫實作，一般呼叫**先配置、再建置**，預設 Debug／原生 Ninja。
不安裝相依、不更新 submodule、不執行 npm 安裝、不改 Kconfig、不執行測試或 App、
不安裝產物、不清除舊檔；訊息生成、UI 編譯與 App 組裝仍完全交由 CMake。

Explicit `--install-vinc-sdk` is the exception: it builds a Release SDK by default,
validates it, installs it and optionally registers it. See the
[SDK installation contract](../src/vinc/sdk/INSTALL.md#sdk-install-command).
明確指定 `--install-vinc-sdk` 時，改為預設 Release 的 SDK 建置、驗證、安裝與登記流程；
完整更新／還原契約見上述文件。`tools/sdk_install.py` owns that orchestration; CMake owns package contents.

The implementation is split by responsibility: root `build.py` owns arguments and
configure/build orchestration; `tools/build_toolchain.py` owns compiler discovery,
identity and cache compatibility; `tools/build_process.py` owns directory locking,
subprocess cleanup and bounded logs. These helpers use only Python's standard
library and are loaded from this checkout, not an installed package named `tools`.

實作依責任拆分：根目錄 `build.py` 處理參數與配置／建置流程；
`tools/build_toolchain.py` 管理編譯器偵測、身分與快取相容性；
`tools/build_process.py` 管理目錄鎖、子程序清理與容量有界的 log。
全部只使用 Python 標準函式庫，明確載入本儲存庫的 helper。

```bat
build.bat
build.bat --config Release --jobs 4
build.bat --source D:\code\my-project --target CameraApp MonitorApp
build.bat --source D:\code\my-project --config Release -Dvinc_DIR=D:/sdk/lib/cmake/vinc
```

```sh
sh build.sh
sh build.sh --config Release --jobs 4
sh /path/to/open-vinc/build.sh --source /path/to/my-project --target CameraApp MonitorApp
```

You may also call the same implementation directly. On Windows, use `py -3
build.py`; direct `.py` execution depends on file associations. On Linux/macOS,
`./build.py` uses the `python3` on PATH through its shebang and executable bit.
`VINC_BUILD_INTERPRETER` selects Python only through the bat/sh launchers, not when
Python has already been selected by a direct invocation.

也可直接執行同一份 `build.py`。Windows 建議使用 `py -3 build.py`，不依賴 `.py` 檔案關聯；
Linux／macOS 可用 `./build.py`，由 shebang 選擇 PATH 的 `python3`。
`VINC_BUILD_INTERPRETER` 僅供 bat／sh 選直譯器，直接執行時以呼叫者選用的 Python 為準。

```powershell
py -3 build.py --config Release --jobs 4
```

```sh
./build.py --config Release --jobs 4
# Alternatively: python3 build.py --config Release --jobs 4
```

| Option / 選項 | Meaning / 語意 |
| --- | --- |
| `--config Debug\|Release` | Debug for ordinary builds; Release for SDK installation. / 一般建置預設 Debug，SDK 安裝預設 Release。 |
| `--install-vinc-sdk` | Build, validate and install this checkout's full SDK. / 建置、驗證並安裝本儲存庫的完整 SDK。 |
| `--sdk-root PATH` | Dedicated single-SDK installation directory; installation mode only. / 專用 SDK 安裝目錄，同一位置只保留一份，僅安裝模式使用。 |
| `--no-register` | Install without updating CMake's user registry. / 安裝但不更新 CMake 使用者登記。 |
| `--source PATH` | Directory containing CMakeLists.txt; defaults to this checkout's `src`. / CMake 入口目錄，預設本儲存庫的 `src`。 |
| `--build-dir PATH` | Override the default binary directory. / 覆寫建置目錄。 |
| `--target A B` | Build only selected targets and their dependencies; omitted means CMake's default set. / 建置指定 targets 及相依，省略時沿用 CMake 預設集合。 |
| `--jobs N` | Positive parallel job count; omitted uses Ninja's default. / 正整數平行工作數，省略時使用 Ninja 預設。 |
| `-DNAME[:TYPE]=VALUE` | Repeatable configure cache definitions. Quote arguments containing spaces. / 可重複的 CMake cache 設定，含空白的整個參數須加引號。 |
| `--help` | No toolchain initialization or build directory creation. / 不載入工具鏈、不建立建置目錄。 |

Explicit relative `--source`/`--build-dir` paths resolve against the caller's
working directory. CMake runs with the selected source as cwd; paths inside `-D`
follow that project's CMake semantics (absolute paths are recommended). Framework
defaults are `build/windows-msvc-debug`, `build/linux-gcc-debug` (or `linux-clang`),
and `build/macos-clang-debug`, with `release` for Release. External sources instead
own their `<source>/build/<profile>`. Output locations within that tree belong to
CMake, including each App's existing deployment layout.

明確提供的相對來源／建置路徑，以呼叫當下目錄解析；CMake 執行目錄為選定來源目錄，
`-D` 內的路徑沿用該專案的 CMake 語意，建議使用絕對路徑。
預設使用固定的「平台／編譯器／組態」build 目錄，不每次新增歷史副本。
外部專案的預設產物留在外部專案自己的 build 目錄，App 部署布局不由腳本改寫。

### Toolchains and safety / 工具鏈與保護邊界

- Requires 64-bit Python 3.11+ (Python 3 only), CMake 3.20+ and Ninja. Without a saved
  selection, Windows tries `py -3`, then PATH's `python`; POSIX uses `python3`. `VINC_BUILD_INTERPRETER` names an explicit
  interpreter executable, not a command with extra flags. An invalid explicit
  selection fails without fallback. A one-shot interpreter override does **not** change the
  Module Python selection; use App Kconfig System/Custom for that, and
  `-DPython3_EXECUTABLE=...` for the existing Kconfig interpreter selection.
- Windows preserves a complete x64 MSVC developer environment. Otherwise it uses
  `vswhere` and the selected installation's `VsDevCmd.bat` in a child process.
  It does not persist PATH or change the parent shell. Partial/non-x64 developer
  environments fail. To select a specific MSVC toolset/SDK, prepare its developer
  environment first. POSIX respects explicit `CC`/`CXX` or CMake compiler `-D`
  definitions; these must name single executables, without embedded flags.
- On Windows, each build/install step runs in a hidden, separate UTF-8 console.
  The gated worker sets both console code pages to 65001 before starting
  CMake/Ninja/MSVC. The caller's console code pages stay unchanged; no global
  locale, English language pack or manual `chcp` is required by this wrapper.
  MSVC source `/utf-8` and Python output decoding alone do not control native
  `/showIncludes` output. The existing Unicode dependency probe remains mandatory.
- First-version scope is native Ninja Debug/Release, MSVC on Windows and the
  selected system GCC/Clang on POSIX. No preset inheritance parser, cross-build,
  multi-config or toolchain-file wrapper. Use CMake directly for those workflows.
  Existing `src/CMakePresets.json` remains available unchanged.
- Before configure, check cached source, generator, configuration and compiler
  paths. A versioned wrapper record compares compiler family/version, target,
  resolved executable and driver role, plus the relevant Windows SDK/toolset and
  ordered INCLUDE/LIB paths. Localized banner/copyright/usage text is not identity.
  Equivalent Windows path spelling is normalized only for comparison; actual
  environment values and search order remain unchanged. Tool query failures are
  rejected rather than recorded as compiler versions.
- Keep invocation names such as `clang++` intact. When a cached path resolves to
  the same compiler and driver role, preserve its spelling during configure so
  CMake does not reset the cache merely because an equivalent alias was selected.
- New records distinguish ordinary builds from SDK installation; conflicting
  reuse is rejected with previous/current values. Legacy records compare their
  available fields and upgrade atomically after successful configure. Missing
  historical target/purpose is reported, not inferred from `VINC_BUILD_APPS=OFF`.
  Existing cache options are retained. Invalid records stop with their path and
  recovery guidance; they are not deleted or silently replaced.
- Switching toolchains requires another build directory. No automatic clean or
  cache deletion. Records are not full dependency fingerprints: upstream pin,
  standard-library and dependency changes still need their own rebuild policy.
- An OS file lock rejects simultaneous wrapper builds in the **same resolved
  binary directory**. It releases on exit/crash; the small lock file stays. Direct
  CMake/Ninja invocations do not participate in this lock. Do not run them against
  that directory at the same time.
- Ctrl+C stops only this invocation's process tree (Windows Job / POSIX process
  group), with a five-second graceful window then forced cleanup. Build commands
  are noninteractive. Windows uses a gated worker before Job assignment so a fast
  build child cannot escape that assignment. Cancellation uses the control pipe
  to ask the worker to send Ctrl+Break inside its own console; the parent retains
  the five-second deadline and Job cleanup. No extra reader/cancellation thread
  is added. This is build supervision, not a
  sandbox for hostile custom commands that deliberately detach on POSIX.
- Child output streams to the terminal and is decoded as UTF-8, replacing
  invalid bytes; old-code-page terminals may display
  replacement characters for unsupported glyphs without failing the build.
  `.vinc-build.log` stores up to 8 MiB of the latest invocation's UTF-8 output
  prefix plus a truncation marker. It is replaced on the next invocation, not
  accumulated. Incompatible-cache/lock
  failures preserve the previous log. Exit codes are nonzero on failures; a
  configure failure never starts the build. No test pass is inferred from a build.
- Source-built Tcl still has the documented Windows build-root space limitation;
  this wrapper does not patch upstream or silently relocate the tree. Installed
  SDK consumers do not compile Tcl. See [Tcl limitations](../library/tcl/README.md).

- 需要 64 位元 Python 3.11+（限 Python 3）、CMake 3.20+、Ninja；可用 `VINC_BUILD_INTERPRETER` 指定腳本
  直譯器，與 Kconfig 的 Module Python 選擇分開，不偷偷綁定。
- Windows 自動載入的 MSVC 環境只影響本次建置；已選定的完整 x64 開發環境會保留。
  POSIX 保留指定的 CC／CXX；工具不合規時報錯，不偷偷換另一套。
- Windows 建置／安裝步驟使用隱藏、獨立的 UTF-8 console；worker 在啟動工具前設定
  輸入／輸出 code page 為 65001，不改呼叫者終端、不要求手動 `chcp` 或英文語系包。
  原始碼 `/utf-8` 與 Python 解碼不等於 MSVC 診斷輸出編碼；既有 Unicode 相依探針保留。
- 第一版不包裝 preset／交叉編譯／多組態；相關需求繼續直接使用 CMake。
- 來源、生成器、組態、編譯器不相容時停止；不刪 cache、不自動 clean。
  只比較編譯器家族、版本、目標、實體路徑與 driver 用途，以及 Windows SDK／工具集、
  有順序的 INCLUDE／LIB；不比較中文說明、版權或輸出編碼。等價路徑只在比較時正規化。
- 保留 `clang++` 等執行名稱；相同實體與 driver 用途的別名沿用 cache 原有拼法，避免
  CMake 重設設定。工具查詢失敗直接報錯，不拿失敗訊息當版本。
- 新紀錄區分一般建置與 SDK 安裝；用途衝突會指出前後值。可讀舊紀錄按已有欄位核對，
  配置成功後原子升級；舊紀錄缺少的用途／目標會提示，不從 `VINC_BUILD_APPS=OFF` 猜測。
  紀錄損壞時停止，不自動清除或覆寫。這仍不是所有相依／標準庫的完整指紋。
- 同一建置目錄的 wrapper 呼叫互斥；不保證與直接執行的 CMake／Ninja 並行安全。
- Ctrl+C 先要求本次子程序停止，最多等待五秒，再強制清理；不影響其他建置目錄或網站。
  Windows 透過控制 pipe 請 worker 在自己的 console 轉送 Ctrl+Break，仍由父程序負責
  五秒期限與 Job 清理，不新增讀取／取消執行緒。
  建置命令不接收互動輸入，GUI/menu 設定等互動 target 請直接使用 CMake 執行。
- 完整輸出顯示於終端；每個 build 目錄只留最新一次、最多 8 MiB 的 log 前段與截斷標記。
  截斷不留下半個 UTF-8 字元。
  子程序失敗會傳回非零碼，配置失敗不進入編譯；建置成功不等於測試通過。

### Diagnostic colors / 診斷色彩

All three entries (`build.py`, `build.bat`, `build.sh`) automatically retain native
colors on terminals and produce plain redirected output. There is no color option.

三個入口自動判斷：終端保留工具原色，重新導向與內建 log 為純文字，不提供色彩選項。

```sh
./build.py
sh build.sh >> build-output.txt 2>&1
```

```powershell
.\build.bat
.\build.bat >> build-output.txt 2>&1
```

- Color is enabled only when stdout is a terminal, `TERM` is not `dumb`,
  `NO_COLOR` is not nonempty, and `CLICOLOR` is not `0`. Redirection defaults to
  plain output even if the parent set `CLICOLOR_FORCE`. Only child environment
  settings are adjusted; the caller's environment remains unchanged.
- Windows enables VT rendering when supported and restores the previous console
  mode on exit/cancellation. It does not change the caller's code pages.
  Terminal hosts exposed only as pipes receive plain output, as do log collectors.
- Child CMake/Ninja receive matching color environment settings. CMake 3.24+
  additionally receives `CMAKE_COLOR_DIAGNOSTICS=ON/OFF` for native GNU/Clang
  diagnostics. An explicit `-DCMAKE_COLOR_DIAGNOSTICS=...` takes precedence for
  compiler generation; redirected output still strips ANSI regardless of tool flags.
  Changing this cache option can cause recompilation because compiler flags
  changed; it does not clean the build directory or alter optimization settings.
- CMake 3.20–3.23 remain supported without injecting compiler color flags; an
  enabled-color invocation reports that automatic compiler coloring needs 3.24+.
  Existing project flags remain intact. No `CXX_FLAGS` rewriting, extra package,
  pseudo-terminal build wrapper, or minimum-version bump is introduced.
- Colors belong to each tool. CMake warnings and GCC/Clang diagnostics are
  forwarded, not recolored by severity. Tested MSVC 19.51 diagnostics remain
  plain; GNU/Clang flags are never added to MSVC.
- `.vinc-build.log` always strips streamed ANSI CSI/OSC sequences, including
  sequences split between reads. The 8 MiB prefix budget counts plain UTF-8
  bytes. Incomplete sequences are dropped at step completion; this is an output
  formatter, not a security sandbox for arbitrary terminal control protocols.

- 只在 stdout 是終端、未指定 `NO_COLOR`／`CLICOLOR=0`、且 `TERM` 不是 `dumb`
  時啟用；重新導向為純文字。只調整子程序環境，不改呼叫者環境。
- Windows 會啟用可用的 VT 顯示並在退出／取消後還原 console mode，不改呼叫者編碼。
  僅以 pipe 提供輸出的終端或 log 收集器，也會收到純文字。
- CMake／Ninja 使用一致的色彩環境；CMake 3.24+ 設定 `CMAKE_COLOR_DIAGNOSTICS`，
  使用者明確傳入的同名 `-D` 優先。切換可能因診斷旗標不同而觸發重編，不清除目錄。
  舊 CMake 保留可建置性與原有旗標，提示需 3.24+ 才自動設定編譯器色彩。
- 保留工具原色，不自行依嚴重度上色；實測 MSVC 19.51 診斷仍是純文字。
- log 一律去除 ANSI CSI／OSC，跨讀取區塊也能處理，8 MiB 上限按純文字 UTF-8 計算。
  不增加第三方相依，不重寫 `CXX_FLAGS`，不提高 CMake 最低版本。

References: [CMake color diagnostics](https://cmake.org/cmake/help/latest/variable/CMAKE_COLOR_DIAGNOSTICS.html),
[CMake CLI color environment](https://cmake.org/cmake/help/latest/envvar/CLICOLOR_FORCE.html),
[Ninja output tests](https://github.com/ninja-build/ninja/blob/master/misc/output_test.py),
[MSVC diagnostic options](https://learn.microsoft.com/en-us/cpp/build/reference/diagnostics-compiler-diagnostic-options).

### Build wrapper validation / 建置腳本驗證

Run policy/subprocess tests with the command below. Set `VINC_BUILD_TEST_CMAKE=1` to additionally compile the
small external CMake fixture; this requires a real native toolchain. The fixture
checks two selected targets, space-containing paths/cache values, incremental
build and incompatible-cache rejection. Other cases check failure propagation,
locking after owner death, argument preservation and grandchild cleanup.

測試預設不建置框架；設定 `VINC_BUILD_TEST_CMAKE=1` 才編譯小型外部 CMake fixture。
涵蓋兩 target、空白路徑／參數、增量建置、cache 拒絕、失敗傳遞、鎖持有人死亡後回收及
中斷清理孫程序。

```sh
python3 -B -m unittest discover -s tools/tests -p "test_build*.py" -v
```

On Windows, use `py -3` or the selected Python executable in place of `python3`.
Windows 請將 `python3` 換成 `py -3` 或選定的 Python 執行檔。

#### Recorded build-entry checks (2026-10-31) / 本輪驗證

| Environment / 環境 | Results / 結果 |
| --- | --- |
| Windows x64 build 26200; MSVC 19.51.36257; CMake 4.3.2; Ninja 1.13.2; CPython 3.13.12 | **15/15** wrapper tests, including the real external CMake fixture. Full framework Debug/Release builds and incremental invocations passed from a normal terminal, including UI/messages. / 15 項腳本測試全通過，含真實外部 CMake fixture；一般終端的完整 Debug／Release 建置與增量呼叫通過，包含 UI／訊息生成。 |
| WSL Ubuntu 24.04 x86_64; GCC 13.3.0; CMake 3.28.3; Ninja 1.11.1; CPython 3.12.3 | **15/15** wrapper tests. Clean full Debug framework build and incremental build passed in the Linux filesystem. A separately installed current Debug SDK built `First`/`Third`, including Third's Python bindings; `Second`/`SecondModule` were not built. / 15 項全通過；Linux 檔案系統中的完整乾淨 Debug 與增量建置通過。另行安裝當前 SDK 後，指定兩個 App 建置成功，包含 Third 的 Python 綁定，未額外編譯 Second／SecondModule。 |

Tests include an injected interrupt while a silent grandchild ignores graceful
signals, process-lock owner termination, cp950 console output, and invocation
from another cwd through both root launchers. Full framework CTest/runtime suites
were **not rerun**: this round changes build tooling and documentation, not Runtime
code. Native macOS execution, Ubuntu 22.04 deployment, other Python versions and
an interactive terminal's Ctrl+C UI remain outside this validation. Existing
macOS framework evidence is preserved in the root README; it does not certify
these new launchers.

涵蓋安靜且忽略正常訊號的孫程序在注入中斷後被回收、鎖持有人被終止、cp950 輸出，
以及從其他目錄經兩個入口建置。**未重跑框架完整 CTest／Runtime 套件**；本輪只改工具與文件。
本輪新腳本的 macOS 原生執行、Ubuntu 22.04 部署、其他 Python 版本，以及人工互動終端的
Ctrl+C 顯示流程尚未驗證；不抹除根目錄既有 macOS 框架驗證，也不將其當作新腳本驗證。

Local, ignored evidence is in `build/script-validation/`: `windows-tests.log`,
`wsl-tests.log`, `windows-debug-incremental.log`, `windows-release-incremental.log`,
`windows-target-console.log`, `wsl-debug-console.log`, `wsl-sdk-console.log` and
`wsl-multi-app-final.log`. Iterations before the final checks may retain earlier
development failures. The framework's existing Ribosome regeneration can recompile
consumers on an incremental build; the wrapper does not promise a no-op framework
rebuild or change that policy. The small standalone fixture verifies a true no-op
second build independently.

本機驗證紀錄在上述忽略目錄，未隨 Git 交付；較早的開發紀錄可能仍保留修正前失敗。
框架既有 Ribosome 每次生成可能讓使用端重新編譯，腳本不改這項政策，
也不宣稱框架第二次建置完全不做事；小型獨立 fixture 另有驗證第二次無需編譯。

Root relocation follow-up (2026-10-31): the same 15 tests passed on Windows and
WSL after moving `build.py` to the repository root. Checks include the default
source location, direct invocation from another cwd, bat/sh forwarding, and real
external CMake builds. WSL directly executes the executable shebang entry.
Logs: `build/script-validation/windows-root-build-tests.log` and
`build/script-validation/wsl-root-build-tests.log`. No full framework rebuild was
needed for this path-only change; native macOS remains untested for these scripts.

根目錄搬移補驗（2026-10-31）：Windows／WSL 各 15 項通過，涵蓋預設來源定位、
其他工作目錄的直接執行、bat／sh 轉交與真實外部 CMake 建置；WSL 已直接使用 shebang
執行 `build.py`。紀錄如上。本次僅搬移路徑，未重建完整框架；新腳本仍待 macOS 原生驗證。

#### Toolchain review follow-up (2026-10-31) / 工具鏈檢查修正補驗

| Environment / 環境 | Results / 結果 |
| --- | --- |
| Windows; MSVC 19.51.36257; CMake 4.3.2; Ninja 1.13.2; Python 3.13.12 | Build tests: 25 passed, 1 POSIX skip, including real C++ compile/link/run. SDK tests: 20 passed, 2 platform/privilege skips. The existing Debug framework build passed with `PYTHONUTF8=1` and the legacy record, without cleaning. / 建置測試 25 通過、1 略過；SDK 測試 20 通過、2 略過；原 Debug build 在 UTF-8 模式下沿用舊紀錄完成建置，未清除。 |
| WSL Ubuntu 24.04; GCC 13.3.0 and Clang 18.1.3; CMake 3.28.3; Ninja 1.11.1; Python 3.12.3 | Each compiler: 25 build tests passed, 1 Windows skip. The real fixture links the C++ standard library, runs, and checks a no-op second build. GCC SDK tests: 21 passed, 1 Windows skip. / 兩套編譯器各 25 通過、1 略過；範例實際連結標準庫、執行與確認第二次無需編譯；GCC SDK 測試 21 通過、1 略過。 |

Checks cover legacy MSVC decoding/path variants, meaningful version/SDK/purpose
changes, failed queries, corrupt records, atomic-save failure, UTF-8 log boundaries
and existing subprocess cleanup. Real Release SDK updates in isolated Windows/WSL
destinations passed staging and final link/run verification with `--no-register`.
WSL also checked that migration preserved cached compiler spelling, flags and SDK
feature settings. Clang coverage is the external fixture, not a full framework
build. No native macOS check or full Runtime/CTest rerun is claimed here.

涵蓋舊 MSVC 紀錄的編碼／路徑差異、實際版本／SDK／用途變更、工具查詢失敗、損壞紀錄、
原子寫入失敗、UTF-8 截斷與既有程序清理。Windows／WSL 在隔離位置完成 Release SDK
更新、暫存及最終連結／執行驗證，使用 `--no-register`；WSL 另核對舊 cache 的編譯器拼法、
flags 與 SDK 設定未被重設。Clang 僅驗證外部範例，非完整框架；本輪未重跑完整 Runtime／
CTest，也未新增 macOS 原生驗證。

Final local evidence: `build/script-validation/windows-toolchain-tests.log`,
`windows-toolchain-sdk-tests.log`, `windows-toolchain-migration.log`,
`windows-toolchain-sdk-migration.log`, `wsl-toolchain-tests.log`,
`wsl-clang-driver-tests.log`, `wsl-toolchain-sdk-tests.log` and
`wsl-toolchain-sdk-migration.log`. These ignored logs are not release artifacts.

#### Native console encoding follow-up (2026-10-31) / 原生 console 編碼補驗

The preceding cp950 output tests covered Python rendering, not native MSVC output
from a CP950 console. A reported configure failure exposed this coverage gap:
the same MSVC emits a CP950 Unicode-header path under CP950 and UTF-8 under
65001. The wrapper now isolates the native console as described above, without
relaxing `VincMsvcDependencies.cmake` or cleaning build caches.

前輪 cp950 測試只涵蓋 Python 顯示，沒有涵蓋 CP950 console 下的原生 MSVC 輸出。
使用者的配置失敗揭露此缺口：同一編譯器在 CP950／65001 輸出不同編碼的中文標頭路徑。
本次以獨立 console 修正，不放寬 CMake 相依探針、不清除 build cache。

- Windows/MSVC 19.51.36257: **28 passed, 1 POSIX skip**. Both CP950 and 65001
  callers retain their original input/output code pages. A real CMake fixture
  includes the production dependency helper and a Unicode-named header; changing
  only that header changes the executable's result from 1 to 2 after incremental
  build. Graceful cancellation and forced cleanup of an uncooperative child and
  grandchild are tested. SDK installer tests: **20 passed, 2 platform/privilege skips**.
- The original `build/windows-msvc-debug` configured and built successfully from
  a CP950 caller with `PYTHONUTF8=0`; both caller code pages remained 950.
- WSL Ubuntu 24.04/GCC 13.3.0: **25 passed, 4 Windows skips**, including the real
  external CMake fixture. Native macOS and a manual terminal Ctrl+C session were
  not exercised; no full framework Runtime/CTest rerun is claimed.

- Windows 建置測試 **28 通過、1 略過**；兩種 code page 均保留呼叫者設定，真正修改中文
  標頭後，增量建置程式結果由 1 變 2；驗證正常取消與不配合子／孫程序的強制清理。
  SDK 測試 **20 通過、2 略過**。
- 使用 CP950、`PYTHONUTF8=0` 重跑原本 Debug 目錄成功，呼叫者輸入／輸出仍為 950。
- WSL **25 通過、4 略過**，含真實 GCC 範例；本輪未驗證 macOS、人工終端 Ctrl+C 操作，
  也未重跑完整框架 Runtime／CTest。

Local logs: `build/script-validation/windows-console-tests.log`,
`windows-console-sdk-tests.log`, `windows-cp950-framework.log` and
`wsl-console-regression.log`. Cancellation follows the
[Windows same-console signal rule](https://learn.microsoft.com/en-us/windows/console/generateconsolectrlevent).

#### Color follow-up (2026-10-31) / 色彩補驗

- Final interface: no `--color` option. Tests reject the removed option and check
  that help no longer advertises it. Windows/MSVC 19.51.36257, CMake 4.3.2,
  Ninja 1.13.2: color tests **9/9**. Pipe capture, append-to-file redirection and
  logs are plain. VT console mode restoration is checked on an exception path.
- WSL Ubuntu 24.04, GCC 13.3.0 and Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1:
  each color suite **8 passed, 1 Windows skip**. With no color arguments, real
  compiler warnings retain ANSI through Ninja under a PTY, while pipe capture,
  append-to-file redirection and logs remain plain.
- Split-sequence stripping is tested at every split point, including OSC links,
  UTF-8 byte limits and interrupted sequences. The older-CMake version branch is
  unit-tested only; no native CMake 3.20–3.23 or macOS color run is claimed.
  This round does not rerun full framework Runtime/CTest suites.

最終介面不提供 `--color`，測試確認舊選項被拒絕、help 已移除。
Windows 色彩 **9/9**；WSL GCC／Clang 各 **8 通過、1 略過**。
包含真實編譯警告、PTY 的自動色彩、重新導向純文字、Windows VT 還原、跨區塊色碼及
log 容量邊界；舊 CMake 僅測版本分支，macOS 與完整 Runtime／CTest 未在本輪重跑。

Final local evidence: `build/script-validation/windows-auto-colors.log`,
`wsl-gcc-auto-colors.log`, `wsl-clang-auto-colors.log` (ignored).
After removing the option, Windows build regression passed 28 cases with 1 skip;
SDK installer regression passed 20 with 2 skips. Logs:
`windows-auto-build-regression.log` and `windows-auto-sdk-regression.log`.
移除選項後，Windows 建置回歸 28 通過、1 略過；SDK 安裝回歸 20 通過、2 略過。
Earlier color implementation checks remain in `windows-colors.log`,
`windows-color-framework.log` and the color regression logs; those predate removal
of the selectable modes and do not describe the final CLI.

## Dependency setup / 相依環境準備

Run from any working directory; paths are resolved relative to these scripts:

可從任意工作目錄執行，專案路徑以腳本所在位置定位：

```bat
D:\path\to\open-vinc\tools\setup.bat
```

```sh
bash /path/to/open-vinc/tools/setup.sh
```

Setup installs/checks tools, restores pinned Git submodules, and runs `npm ci` for
the UI. It prepares Open-VINC dependencies, not additional packages required by
arbitrary user Modules. **It never configures CMake, builds Open-VINC or a consumer project, runs
project tests, or downloads Playwright browsers.** It does compile/link tiny,
standalone temporary probes for C++23/toolchain/SDK and POSIX zlib availability;
they do not include framework headers or create a project build directory.

腳本只安裝／檢查工具、還原固定版本 submodule，並為 UI 執行 `npm ci`。
範圍是 Open-VINC 本身的相依，不包含使用者自訂 Module 的額外套件。
**不配置 CMake、不編譯 Open-VINC 或使用端專案、不執行專案測試，也不下載 Playwright 瀏覽器。**
工具鏈與 POSIX zlib 檢查會在暫存目錄編譯／連結獨立的小程式，
不引用框架標頭、不建立專案 build 目錄。

## Options / 選項

| Option | Behavior / 行為 |
| --- | --- |
| default / 預設 | Prepare native, UI, and optional Python SDK dependencies; this does not enable Python in the consumer's CMake configuration. / 準備原生、UI 與選用 Python SDK 相依，不更改使用端的 CMake 選項。 |
| `--check` | No installs, downloads, submodule updates or `npm ci`; temporary compiler probes still run. / 僅檢查，不安裝、下載、更新 submodule 或執行 `npm ci`；仍執行暫存工具鏈探針。 |
| `--recommended` | Keep compatible tools; use the recommended installation candidates below when needed. May combine with `--check`. / 保留相容工具，缺少或不合格時使用下列建議安裝候選；可搭配 `--check`。 |
| `--no-ui` | Skip Node/npm and frontend packages. / 略過 Node／npm 與前端套件。 |
| `--no-python` | Skip Module development files; still check the build interpreter. / 略過 Module 開發檔，仍檢查建置工具的直譯器。 |
| `--help` | Print usage without preparing anything. / 僅顯示說明。 |

## Platform behavior / 平台行為

Default setup uses the existing platform recipes below; it does not guarantee
that the distribution's default package meets the minimum. In particular,
Ubuntu 22.04's default Python 3.10 fails the Python 3.11 floor after installation.
Select a separately prepared compatible interpreter with `VINC_SETUP_PYTHON`.
Setup never replaces the system Python/alternatives. Build does not install or
upgrade dependencies: missing tools point to `tools\setup.bat` or
`bash tools/setup.sh`; incompatible tools report required/found versions and
the selected path, with `--recommended` as the preparation hint for incompatible versions.
Neither mode silently replaces an explicitly selected incompatible interpreter/compiler.

預設 setup 沿用下列平台配方，不保證套件庫預設版符合最低要求。例如 Ubuntu 22.04 的
預設 Python 3.10，安裝後仍會被 3.11 門檻拒絕；須自行準備相容版，再以
`VINC_SETUP_PYTHON` 選用。setup 不替換系統 Python／alternatives。
build 不安裝或升級；缺少工具時提示一般 setup 指令，版本不符時列出要求、實際版本與路徑。
版本不合格時提示 `--recommended`；兩種模式都不偷偷替換明確指定但不相容的直譯器／編譯器。

### Recommended candidates / 建議安裝候選

These are installation choices, **not new minimums or exact-version build gates**.
Already-compatible tools stay selected. Installation availability and native capability
probes can still fail; no third-party Python repository, version manager, system-Python
replacement or whole-OS upgrade is added. WinGet/Homebrew must already be present.

這些是安裝候選，**不是新的最低版本或精確版號門檻**。已合格的環境繼續使用。
套件可用性與實際能力檢查仍可能失敗；不加入第三方 Python 套件來源、版本管理器，
不替換系統 Python，不升級整個 OS。WinGet／Homebrew 須事先存在。

| Platform / 平台 | Missing tools in recommended mode / 建議模式缺少工具時 |
| --- | --- |
| Windows x64 | VS 2022 Build Tools 17.14 C++ workload and Windows SDK; MSVC floor 19.44. WinGet CPython 3.14, CMake 4.4.4, Ninja 1.13.2, Node 24.22.1 with bundled npm; Git via WinGet. / VS C++ 工作負載及 SDK，其他工具使用 WinGet。 |
| Ubuntu 24.04 | GCC/G++ 13 and distribution standard library; Python 3.12 and matching dev files via apt. Official CMake 4.4.5 and Ninja 1.13.2 archives, SHA-256 checked, under the user's data directory. Official Node 24 archive as below. / apt 補編譯器與 Python，官方壓縮包補 CMake／Ninja／Node。 |
| Ubuntu 22.04 | GCC/G++ 12 via apt; same CMake/Ninja/Node route. If no compatible Python exists, stop and recommend manually prepared CPython 3.12 + dev files with `VINC_SETUP_PYTHON`; do not add a PPA. / Python 不合格時明確提示手動準備，不新增 PPA。 |
| macOS | Apple CLT/Xcode; Homebrew stable CMake/Ninja/Git, `python@3.14`, `node@24`. An incompatible Apple compiler/SDK needs an explicit CLT/Xcode update, not silent LLVM substitution. / Apple 工具鏈不合格時提示更新，不偷換 LLVM。 |

Other Linux environments can check/use compatible tools; there is no guessed install
recipe. An incompatible **existing** Visual Studio installation is diagnosed with the
recommended MSVC version; update its C++ workload in the Visual Studio Installer and
rerun setup. No supported Python minor version implies a universal `.pyd`: rebuild
bindings for that minor. The Windows native Runtime guard remains enforced; installing
a global VC++ Redistributable does not replace a Python/package-private old DLL.

其他 Linux 可檢查並使用合格環境，沒有配方時不猜安裝方式。**既有** Visual Studio 過舊時，
會提示建議的 MSVC 版本；請在 Visual Studio Installer 更新 C++ 工作負載後重跑。
Python minor 改變仍須重建綁定；全域 VC++ Runtime 安裝不會取代 Python／套件私帶的舊 DLL，
既有 Runtime 防護不會放寬。

### Remembering selections / 記住本機工具選擇

After every complete successful setup (ordinary or recommended), save one literal
data file under `.vinc/toolchains/<platform>-<architecture>.conf`, ignored by Git and
outside `build/`. WSL distributions have separate `wsl-<distribution>` names.
Only absolute tool paths, selected Windows toolset/SDK or Apple developer directory,
and diagnostic versions are stored—not the full environment. Replacement is atomic;
failure preserves the previous record. `--check` never writes it. Packages already
installed before a later failure are not uninstalled.

一般／建議 setup 全部檢查成功後，才原子更新 `.vinc/toolchains/` 的本機紀錄。
檔案被 Git 忽略、位於 build 外；Windows、Linux、各 WSL 發行版、macOS 與 CPU 架構分開。
只存工具絕對路徑、Windows toolset／SDK 或 Apple developer directory，以及診斷版號，
不存整包環境。失敗保留舊紀錄，`--check` 不寫入；不回滾前面已成功安裝的系統套件。

- Root `build.bat`/`build.sh` read the saved Python path **before** starting Python.
  A direct `python build.py` invocation keeps the already-running interpreter.
  Windows bootstrap and setup discovery disable Python launchers' automatic
  installation in their process environment, including `--check`.
- Explicit choices override saved defaults: `VINC_BUILD_INTERPRETER`, complete x64
  Developer environment, `CC`/`CXX`, compiler/configuration-Python CMake `-D` options, and
  `VINC_BUILD_CMAKE`, `VINC_BUILD_NINJA`, `VINC_BUILD_NODE`, `VINC_BUILD_NPM` (single
  executable paths, no embedded arguments). Keep Node/npm from the same installation.
- Setup's checked Python becomes the default build/Kconfig-tool interpreter only.
  App Kconfig owns Module Python; old `module_python` profile fields are ignored.
  Setup no longer records a Module interpreter. `-DPython_EXECUTABLE` is rejected.
- Missing saved tools fail with a repair hint; no silent fallback. Explicit overrides
  do not rewrite the record. Rerun setup to refresh it, or remove just this platform's
  record to restore discovery. Version text is diagnostic, not an exact patch lock.
- Windows reconstructs its developer environment each run; compiler choices go through
  CMake without injecting an absolute `CC` command into Tcl/NMAKE. Existing compiler
  cache checks stay enforced; a previously used Module Python path/minor ABI change
  requires another build directory. Nothing is automatically cleaned.
- Only this checkout's default `src` build uses this profile. External `--source`
  projects and direct CMake invocations do not inherit it.

根目錄 bat／sh 在啟動 Python 前讀取紀錄；直接呼叫 Python 則尊重已選的直譯器。
Windows 入口與 setup 偵測會在自己的程序環境關閉 Python 啟動器的自動下載功能，
避免 build／check 意外安裝 Runtime。
明確環境／`-D` 設定優先，一次覆寫不改紀錄。setup 檢查過的 Python 預設共用於
build 與 Kconfig 工具；Module Python 改由 App Kconfig 選擇，舊 `module_python` 紀錄忽略，
setup 不再寫入這個欄位。`-DPython_EXECUTABLE` 已移除，功能開關不受 setup 更動。
工具消失就失敗，不改找另一套；可重跑 setup 更新，或刪除本平台紀錄恢復自動偵測。
既有建置目錄的編譯器與實際使用過的 Python ABI 不相容時要求另選目錄，不自動清除。
外部 `--source` 專案與直接 CMake 不套用本 checkout 的紀錄。

- **Windows x64:** `setup.bat` invokes `setup.ps1` through Windows PowerShell 5.1.
  WinGet installs missing Git, CMake, Ninja, MSVC C++ Build Tools (including
  recommended Windows SDK components), Node LTS, or CPython 3.13. Setup imports
  the Visual Studio developer environment into its own process and checks `cl`,
  linker, resource compiler and NMAKE. It does not change the parent terminal.
  The root build wrapper prepares that environment automatically. For direct
  CMake commands, use an x64 Developer terminal; setup prints its `VsDevCmd.bat` path.
- **Ubuntu 22.04/24.04, x86_64/arm64:** apt supplies native tools, `make`, zlib
  development files and Python development files. Existing compilers are probed
  first; if none passes, candidates are GCC 12 on 22.04 and GCC 13 on 24.04.
  These are setup candidates, not a guarantee that every framework feature
  builds. Missing/incompatible Node uses an official Node 24 binary under
  `${XDG_DATA_HOME:-$HOME/.local/share}/open-vinc/node`, verified against the
  official HTTPS SHA-256 list. No third-party apt repository or system-wide
  compiler/Node alternative is installed. Setup prints the private Node PATH
  needed by later shells; it does not edit profiles.
- **macOS, arm64/x86_64:** use Apple Command Line Tools and a preinstalled
  Homebrew. Missing tools use Homebrew; incompatible/missing Node selects
  `node@24`, Python selects `python@3.13`. Initial Apple tools installation can
  require a GUI; finish it and rerun. An incompatible Apple compiler/SDK needs
  an Xcode/Command Line Tools update or explicit compatible `CC`/`CXX`; setup
  does not silently replace Apple Clang. This does not establish macOS/Intel
  deployment compatibility.

- **Windows x64：** bat 使用 Windows PowerShell 5.1，透過 WinGet 補齊工具、
  C++ Build Tools／Windows SDK、Node LTS 或 CPython 3.13。
  MSVC 環境僅在腳本程序內載入；根目錄 build 入口會自行準備。
  直接執行 CMake 時，請使用 x64 Developer 終端。
- **Ubuntu 22.04／24.04：** 使用 apt；優先保留通過探針的編譯器，必要時安裝 GCC 12／13
  候選工具鏈。缺少 Node 時下載官方 Node 24、核對 SHA-256，安裝到目前使用者資料目錄。
  不新增第三方 apt 來源、不切換系統預設編譯器、不修改 shell profile。
- **macOS：** 使用 Apple Command Line Tools 與既有 Homebrew。Apple 工具初次安裝可能需要
  互動，完成後須重跑；不相容的 Apple SDK／編譯器要更新或明確選用其他工具鏈。

Package managers may need administrator privileges, sudo, network access,
license acceptance or a restart. Setup never reboots, uninstalls tools, changes
system alternatives or upgrades the entire OS. Failure exits nonzero with the
failed step. Homebrew/WinGet must already be available when needed. Other Linux
distributions/releases may check existing tools. Missing dependencies require a
known installation recipe; unsupported recipes fail without guessing commands.
This is not a distribution-version restriction on build eligibility.

套件安裝可能需要系統權限、sudo、網路、授權同意或重啟；腳本不自動重開機、移除工具、
切換系統 alternatives 或升級整個 OS。失敗會回傳非零退出碼並指出階段。
需要時須先具備 Homebrew／WinGet；其他 Linux 發行版不猜測安裝指令。
其他 Linux 發行版或 Ubuntu 版本可檢查既有工具；只有需要安裝、但沒有配方時才拒絕。
build 不因發行版版本拒絕，仍須滿足工具、標準庫及 OS 能力需求；允許嘗試不代表正式驗證支援。

## Checks and source protection / 檢查與來源保護

- CMake minimum is 3.20; Node follows current Vite engines: 20.x from 20.19, or
  22.12 and later. The C++ probe checks `std::expected`, threads, 64-bit pointers
  and lock-free 64-bit atomics, plus Windows SDK linking. It does not replace
  a real framework build or behavioral tests.
- Python must be 64-bit, GIL-enabled CPython 3.11+ (Python 3 only) with matching `Python.h` and,
  on Windows, its x64 import library. `VINC_SETUP_PYTHON` selects an explicit
  executable; an incompatible choice is reported rather than replaced. Setup
  prints its path for your Kconfig Custom Python selection. Windows tries the
  Python launcher's interpreter before PATH candidates. `--no-python` skips
  development-file requirements, but still checks the build interpreter.
  The shared [minimum policy](../src/vinc/common/README.md#build-toolchain-policy)
  is used by setup, build, configuration and the installed SDK.
- POSIX `CC`/`CXX` name compiler executables, without embedded flags. A rejected
  explicit `CXX` is not replaced. Selected tools are printed; the caller owns
  its CMake entry point, build directory, configuration and feature switches.
- Submodules must have no tracked/untracked edits. Missing/mismatched pins are
  restored normally and rejected in check-only mode. No `--force`, `--remote`,
  clean, reset or root-branch update is used. A Git checkout is required;
  source archives without `.git` cannot restore pinned submodules.
- `npm ci --include=dev` restores the lockfile, even with `NODE_ENV=production`.
  It replaces this UI's `node_modules`, without updating package/lock files or
  installing global packages. npm's lifecycle-script policy is respected.
  `--check` uses `npm ls --include=dev --all`; this checks the dependency tree,
  not the integrity/security of every installed file. Both modes additionally
  import the UI's build/runtime dependencies to detect unusable installations.

明確指定的 Python／編譯器不合規時直接回報，不偷偷改選。
submodule 有已追蹤或未追蹤修改時停止，不強制覆寫；只還原 Git 固定版本。
UI 的 `npm ci` 會依 lockfile 重建 `node_modules`，包含建置工具，不更新 package／lockfile。
`--check` 確認相依樹，不代表逐檔完整性或安全稽核。
兩種模式也會實際載入 UI 建置／執行相依，檢查安裝內容是否可用。

## Validation and outstanding coverage / 驗證與待補覆蓋

Offline policy tests are independent of the framework's CMake entry point:

離線政策測試不依賴框架的 CMake 入口：

```sh
python3 -m unittest discover -s tools/tests -v
```

Use a selected CPython executable on Windows. Tests cover unknown options,
check-only installation refusal, failure propagation, optional-service skipping,
and real temporary Git submodule edits/pin mismatch. Platform-specific cases
are explicitly skipped elsewhere.

Windows 請改用選定的 CPython。測試涵蓋未知選項、僅檢查模式不安裝、失敗傳遞、
略過選用依賴，以及真正的暫存 Git submodule 修改／版本不符；跨平台案例明確略過。

### Recorded checks (2026-10-29) / 驗證紀錄

| Environment / 環境 | Results / 結果 |
| --- | --- |
| Windows x64; Windows PowerShell 5.1; Visual Studio 18 Build Tools; CMake 4.3.2; Ninja 1.13.2; CPython 3.13.12; Node 24.19.0 / npm 11.17.0 | Normal setup and check-only passed with existing native tools. `npm ci` and frontend dependency imports passed. Offline tests: **9 passed, 9 platform skips**. / 現有原生工具的正常／僅檢查流程通過，npm 安裝與載入檢查通過；離線測試 9 項通過、9 項跨平台略過。 |
| Ubuntu 24.04.5 / WSL 2 x86_64; GCC 13.3.0; CMake 3.28.3; Ninja 1.11.1; CPython 3.12.3; Node 24.22.2 / npm 11.19.1 | Normal setup/check-only passed in a separate Linux checkout, using locally available pinned Git objects. Missing Node was downloaded from the official source and checksum-verified; npm dependencies were installed and imports passed. Offline tests: **10 passed, 8 platform skips**. No CMakeCache was created. / 獨立 Linux checkout 使用本機固定 Git 來源；實際下載／驗證缺少的 Node、安裝 npm 套件與載入檢查通過，離線測試 10 項通過、8 項略過，沒有產生 CMakeCache。 |

Local logs: `build/setup-windows-validation.log` and
`build/setup-linux-validation.log` (ignored, not release artifacts).
Checking a Windows CRLF submodule checkout from WSL reported tracked changes and
correctly stopped; Linux validation used its own LF checkout. These tests did
not exercise real WinGet/apt/Homebrew installations of every native tool or
network cloning of each upstream. npm reported unapproved `vue-demi` lifecycle
scripts; dependency imports passed without weakening the user's script policy.

本機 log 位於上述忽略的 build 路徑，不隨版本交付。
WSL 檢查 Windows 換行格式的 submodule 時正確停止，Linux 改用獨立 LF checkout 驗證。
本輪未實際經各套件管理器安裝全部原生工具，也未逐一從上游網路 clone。
npm 提示 `vue-demi` lifecycle script 尚未獲准；未降低使用者政策，套件載入檢查通過。

Native macOS and fresh-machine installer paths remain unverified. Windows/WSL
results do not imply all installers or platforms were tested. Component
validation stays in each component's README.

macOS 原生執行與全新電腦的各安裝器路徑仍待驗證。
Windows／WSL 結果不代表每個安裝器或三平台全數通過；元件驗證仍由各元件 README 維護。

### Recommended setup and saved tools (2026-11-02) / 建議安裝與工具紀錄驗證

| Environment / 環境 | Actual checks / 實際驗證 |
| --- | --- |
| Windows x64, PowerShell 5.1; MSVC 19.51.36257, SDK 10.0.26100.0; CMake 4.3.2, Ninja 1.13.2; existing CPython 3.13.12, Node 24.19.0/npm 11.17.0 | Recommended setup/check passed and kept existing compatible versions. Real profile hashes stayed unchanged after check-only and intentional invalid-Python failure. Tool tests **100 passed, 19 platform skips** (119 total), including real CMake integration. / 保留既有合格工具；成功保存，檢查／失敗不覆寫；含真實 CMake 的工具測試 100 通過、19 略過。 |
| Windows, same native toolchain; isolated official CPython NuGet 3.14.10 | Full Release build through saved selections passed with explicit Module Python override; Python Module/messages/Runtime/services/RPC/Shell CTest **6/6** passed. No private Runtime replacement or guard relaxation. / 完整 Release 與 Python 功能 6/6 通過，未替換私帶 Runtime 或放寬防護。 |
| Ubuntu 24.04 / WSL 2 x86_64; GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1, Python 3.12.3, Node 24.22.2/npm 11.19.1 | Recommended setup/check and saved-profile native Common target build/incremental build passed. Tool tests **98 passed, 21 platform skips** (119 total), including real CMake integration. Isolated official CMake 4.4.5/Ninja 1.13.2 downloads, checksums and executable startup passed. / setup、紀錄、Common 原生／增量建置通過；工具測試 98 通過、21 略過；官方下載與 SHA-256 實測通過。 |

The real Windows build caught and fixed an integration regression: exporting the
saved compiler path as `CC` broke Tcl's NMAKE syntax. Saved Windows compilers now
use CMake selections; a regression test protects the child environment. Real
PowerShell 5.1 launcher tests also verify embedded quotes, spaces and trailing
backslashes. Profiles contain literal data; they are never sourced/evaluated.

Windows 實際建置抓到並修正保存的 `CC` 完整路徑干擾 Tcl/NMAKE 的問題，已加入回歸案例。
PowerShell 5.1 真實入口測試另涵蓋引號、空白與尾端反斜線；設定檔不當 shell 程式執行。

Local ignored evidence: `build/setup-recommended/` (`all-tests-windows.log`,
`all-tests-wsl.log`, `setup-windows.log`, `setup-wsl.log`, `build-windows.log`,
`build-wsl.log`, `python314-tests.log`, `download-wsl.log`). Windows WinGet candidates
were queried, not all reinstalled on a fresh machine. Ubuntu 22.04's no-PPA failure
policy is covered by isolated tests, not a fresh 22.04 installation. macOS installer,
launcher and profile execution remain pending native validation; previous component
macOS results are unchanged. This test does not claim full Linux framework revalidation
or cross-minor Python binary compatibility.

本機證據位於上述忽略目錄。WinGet 已查核候選版本，未在乾淨 Windows 逐一重裝；
Ubuntu 22.04 不加 PPA 的失敗政策以隔離案例驗證，未重裝 22.04。
macOS 本輪安裝器／入口／紀錄仍待原生驗證，不影響既有元件的 macOS 驗證紀錄。
這輪也不宣稱重新驗證完整 Linux 框架或 Python 跨 minor 二進位相容。

Sources: [Microsoft scripted installation](https://learn.microsoft.com/en-us/visualstudio/install/use-command-line-parameters-to-install-visual-studio),
[Homebrew prerequisites](https://docs.brew.sh/Installation),
[Node 24 distribution](https://nodejs.org/dist/latest-v24.x/),
[CMake downloads](https://cmake.org/download/),
[Ninja 1.13.2 assets](https://github.com/ninja-build/ninja/releases/tag/v1.13.2).

## Windows compiler minimum / Windows 編譯器最低版本

`build.py` and `setup.ps1` check native MSVC against the
[shared Common policy](../src/vinc/common/README.md#build-toolchain-policy):
**19.44 or newer**, without a patch pin. Setup queries the selected `cl.exe`
before its C++23 feature probe. An old installed toolset produces an actionable
error; setup does not silently replace an existing compiler installation.
Build-directory reuse and installed SDK ABI checks remain unchanged.

`build.py` 與 `setup.ps1` 共用 Common 的 **MSVC 19.44 起**規則，不鎖定修補版。
Setup 先查詢實際 `cl.exe`，再執行 C++23 探針；既有工具鏈過舊時提示更新，不偷偷替換。
建置目錄重用與安裝式 SDK 的 ABI 檢查保持不變。

## Python minimum validation / Python 最低版本驗證

The shared Python minimum, source/installed SDK checks and setup diagnostics were
validated with Windows CPython 3.11 and WSL CPython 3.12. See the
[minimum-policy rollout report](../src/vinc/module_system/python/PYTHON311_VALIDATION.md)
for actual tool versions, passing/skipped tests and remaining platform coverage.
This does not validate one native extension across different Python minor versions.

共用 Python 最低版本、來源／安裝式 SDK 檢查及 setup 診斷，已使用 Windows 3.11 與
WSL 3.12 驗證。實際工具版本、通過／略過案例及平台限制見上述紀錄；
這不代表同一份原生擴充可跨不同 Python minor 使用。
