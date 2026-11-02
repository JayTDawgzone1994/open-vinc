<a id="python-module"></a>

# Python Module / Python 模組

Central Shell manages Python Modules, one system-Python process per instance. Central neither embeds/links Python nor bundles/installs interpreters/packages. pybind11 exposes the shared C++ Runtime rather than a separately implemented socket/shared-memory protocol.

Python Module 由中央 Shell 啟停，每個實例使用一個系統 Python 程序。
中央不連結或嵌入 Python，不附帶直譯器，也不自動安裝 Python／pip 套件。
使用者程式透過 pybind11 原生擴充使用同一套 C++ Runtime；沒有另寫 Socket／共享記憶體協定。

The [single-App entry](../../../../apps/vinc/SINGLE_EXECUTABLE.md) changes native
Module launches only: C++ Hosts execute the App image, while Python Modules still
execute the selected system interpreter and bootstrap. Central never imports the
Python extension merely because Python support is packaged.

單一 App 入口只改變原生 Module 啟動：C++ Host 執行 App 本身，Python Module 仍執行
所選系統直譯器與 bootstrap。中央不因部署了 Python 支援就匯入原生擴充。

<a id="建置與部署"></a>

## Build and deploy / 建置與部署

For the CPython 3.11 minimum-policy environment, results and remaining limits,
see [CPython 3.11 validation](PYTHON311_VALIDATION.md). The shared
[toolchain policy](../../common/README.md#build-toolchain-policy) owns the minimum;
this does not promise cross-minor binary ABI.

CPython 3.11 最低版本的環境、測試結果與限制見
[相容性驗證紀錄](PYTHON311_VALIDATION.md)；最低版本由共用工具鏈規則維護，不承諾跨 minor 二進位相容。

Set `CONFIG_VINC_APP_PYTHON=y` in the App's `VINC_BUILD_CONFIG` file.
The bundled App uses `apps/vinc/vinc.conf`. All source/installed Apps share the same
binding recipe; compatible enabled Apps reuse one binding, while disabled Apps
have no dependency on it. The Kconfig tool interpreter (`Python3_EXECUTABLE`) is
independent of the Module interpreter selected in Kconfig.

內建／外部／安裝式 App 使用同一流程，於各自設定檔啟用 Python。啟用的 App 必須使用
相同 Python ABI；建置設定工具與執行 Module 的直譯器分開選擇。
詳見 [設定契約](../../../../tools/configure/README.md#module-python-selection--module-python-選擇)。

```sh
# Enable Python in apps/vinc/vinc.conf, then build the bundled App.
cmake -S src -B build/my-build
cmake --build build/my-build --target Vinc
```

Use the chosen compiler/configuration. CONFIG_VINC_APP_PYTHON defaults to n so C++ needs no Python development files. Enabled builds require CPython 3.11+/Development.Module (Python 3 only), a dependency floor rather than verified coverage of every version. Only 64-bit GIL-enabled CPython; no free-threading/subinterpreters/PyPy/Python 2.

沿用所選建置目錄的編譯器與組態。`CONFIG_VINC_APP_PYTHON` 預設 `n`，讓只使用 C++ 的建置不要求 Python 開發檔。
開啟後需要 CPython 3.11 以上及其 Development.Module 檔案（限 Python 3）；這是相依最低要求，不代表所有版本已通過驗證。
第一版限定 64-bit、含 GIL 的 CPython；不支援 free-threaded Python、subinterpreter 或 PyPy。
Python 2 不支援。

Native bindings and linked runtime require matching toolchain/stdlib/config/CRT. C++ Plugin ABI is unchanged. pybind11 uses CPython APIs for Python ownership, never CPython release of C++-new Modules. MSVC cannot build against MinGW Python. Minor/platform/bitness/GIL ABI must match build metadata; startup rejects incompatibility.

`vinc_python_native` 與它連入的 C++ Runtime 使用同一工具鏈／標準庫／組態／CRT。
一般 C++ Plugin 的原有 ABI 契約不變。pybind11 使用 CPython C API 管理 Python 物件；
不讓 CPython 釋放由 C++ new 建立的 Module 物件。
Windows 的 MSVC 擴充不能用 MinGW Python 建置。Python minor 版本、平台 ABI、位元數與 GIL 組態需符合建置資訊；啟動不相容時明確拒絕。

Central builds deploy:

中央建置會部署以下檔案：

```text
Vinc.exe / Vinc
python/vinc/
    __init__.py
    bootstrap.py
    _build.py
    _windows_runtime.py
    _msvc_runtime.json                 # Windows/MSVC only; generated with _native
    _native.<平台與Python版本後綴>.pyd / .so
    licenses/open-vinc/
        LICENSE
        NOTICE
        THIRD_PARTY_NOTICES.txt
licenses/open-vinc/                    # Central binary's shared notices
licenses/pybind11/LICENSE
data/module/<ModuleType>/<ModuleType>.py
```

Keep the complete `python/vinc` package, including its internal `licenses/open-vinc` directory,
when using the bindings independently of central. It carries Open-VINC's terms/attribution and
the native dependency inventory, including pybind11's complete license.
See [SDK license delivery](../../sdk/README.md#license-delivery) for binary packaging.

即使獨立於中央使用綁定，也須交付完整 `python/vinc`，包含內部的 `licenses/open-vinc`。
其中附帶 Open-VINC 條款／署名及原生相依聲明，含完整 pybind11 授權。
二進位打包契約見 [SDK 授權交付](../../sdk/README.md#license-delivery)。

User code lives in data/module, distinct from vinc_data persistence. Editing existing Python Modules does not rebuild central. Compiled vinc_messages type lists define vinc.messages; new .msg requires regenerating/rebuilding consumers/native extension, without runtime schema parsing. --python selects interpreter/venv; relative paths become absolute on central startup. Otherwise Kconfig Custom uses its absolute path, while System searches the deployment PATH for Windows python.exe/POSIX python3 without silently trying another environment.

`data/module` 是使用者程式的位置，與 Param／Log 的 `vinc_data` 不同。
只修改既有 Python Module 程式不需編譯中央。`vinc.messages` 由 `vinc_messages` 的
編譯期型別清單建立；新增 `.msg` 後須重新生成並建置所有使用它的程式及 Python 擴充。
不提供 runtime `.msg` 解析器或動態訊息型別。
`Vinc --python /absolute/path/to/python` 可指定 Python 或 venv，CLI 亦可接受相對路徑並在中央啟動時轉成絕對路徑。
省略時使用 Kconfig 的預設：Custom 使用明確設定的絕對路徑，System 在啟動 Module 當下
依部署環境 PATH 順序尋找 Windows `python.exe` 或 POSIX `python3`，不偷偷換另一個環境。
`Python_EXECUTABLE` 不再作為 VINC 的公開選擇入口，但不禁止其他 CMake 元件使用；
設定方法、舊 cache 遷移與多 App 限制見上述設定工具契約。

<a id="build-abi-probe"></a>

### Build ABI probe consolidation (2026-11-02) / 建置 ABI 探測統一

Selection and binding creation now share the validated result from Common's
[build-only Python probe](../../common/README.md#build-toolchain-policy).
The duplicated newline-based binding probe was removed. JSON fields/types and
failure diagnostics are checked at selection; each enabled App and each new
configure still probes independently. Binding metadata uses that result after
checking FindPython's interpreter identity and major/minor. Patch servicing,
venv identity, Kconfig selection, cache rejection, development-file discovery,
`/MT` policy and runtime bootstrap checks keep their existing contracts.
No new persistent probe cache, runtime service or public setting was added.

選擇階段與 binding 共用 Common 驗證後的資料，移除 binding 原有的換行格式重複探測。
各啟用 App、每次 configure 仍獨立探測，不新增持久快取。FindPython 仍找開發檔，
核對身分與 major/minor 後用統一資料生成 `_build.py`。同 ABI 的 patch 更新、venv
身分、Kconfig、cache 拒絕、`/MT` 及執行期 bootstrap 契約不變，沒有新增公開設定或服務。

Focused tests exercise real CMake helpers with interpreter-startup fault injection:
malformed/missing/wrong-type fields, unsupported ABI, nonzero exit, the real
10-second timeout, fresh per-App/reconfigure results, one VINC probe before
binding creation, discovery mismatch and patch updates. Existing PATH, Unicode
venv, foreign FindPython and missing-development diagnostics remain covered.
The missing-development test injects FindPython failure; it does not uninstall
system packages. Local logs: `build/python-abi-probe/` (not versioned).

針對性測試使用真正 CMake helper，於 Python 啟動時注入格式錯誤、缺欄位、型別錯誤、
不支援 ABI、失敗退出與實際 10 秒逾時，並驗證每個 App／configure 資料重新取得、
binding 不重跑探測、discovery 衝突及 patch 更新。既有 PATH、中文 venv、其他使用端
FindPython 與缺開發檔診斷亦保留；缺開發檔採故障注入，沒有移除系統套件。

| Environment / 環境 | Validation / 驗證 |
| --- | --- |
| Windows x64, MSVC 19.44.35229, official CPython 3.13.12, CMake 4.3.2, Release | Selection tests: 24 cases, 23 passed and 1 POSIX-only skip. Final App build and related CTests **10/10** passed. / 最終 App 建置及選擇、設定、CRT、Python 生命週期／服務／訊息／RPC 回歸通過。 |
| WSL2 Ubuntu 24.04 x86-64, GCC/libstdc++ 13.3.0, CPython 3.12.3, CMake 3.28.3, Release | Selection tests **24/24**, final App build and related CTests **7/7** passed. / 選擇與 Linux Python 回歸全部通過。 |
| Windows installed SDK, same toolchain / 同工具鏈 | Full fixture passed relocation, custom-message Bus/RPC/Param, schema incremental changes, ABI/metadata isolation, repair and disable cleanup. After the final patch-version comparison adjustment, reinstalled helpers matched source, downstream rebuild/import and Bus/RPC/Param/lifecycle passed, and `_build.py` matched the actual interpreter. / 全套案例通過；最終 patch 比對修正再交付並驗證下游建置、通訊、收尾與 metadata 一致性。 |

The initial bare `python` test invocation selected local MSYS2 Python 3.14 and
failed two existing venv fixtures before the intended Windows environment was
selected. Official CPython results above supersede that attempt; MSYS2/MinGW is
not qualified for the MSVC binding. This round did not repeat Debug, native macOS,
WSL installed SDK or OCR. The real old-MSVCP preload test passed within the Windows
suite; the optional WinRT import-order case was skipped without its wheel directory.

首輪裸 `python` 指令選到本機 MSYS2 3.14，兩個既有 venv 測試失敗；切回官方 CPython
後結果如上，不宣稱 MSYS2／MinGW 可用於 MSVC binding。本輪未重跑 Debug、macOS
原生、WSL installed SDK 或 OCR。Windows 的真實舊 MSVCP 預載測試通過；選用的 WinRT
載入順序案例未設定 wheel 目錄而略過。

This completes the duplicate-probe item from the 2026-11-02 review. Earlier fixes
remain documented in [CMake consumer coexistence](../../../../tools/configure/README.md#cmake-consumer-coexistence-correction--cmake-使用端共存修正),
[PATH selection](#system-python-path-consistency),
[OSAL loader handling](../../osal/README.md#windows-loader-error-dialogs--windows-載入器錯誤對話框)
and [Runtime build cleanup](#runtime-build-cleanup); remaining native macOS checks
stay in the root TODO.

本輪完成 Python 周邊 review 的重複探測項目；先前修正與歷史結果由上述各元件紀錄維護，
根目錄 TODO 保留尚待 macOS 原生驗證的事項。

<a id="system-python-path-consistency"></a>

### System Python PATH consistency / System Python 搜尋一致性

Module System owns ordered PATH lookup (`python.exe` on Windows, `python3` on
Linux/macOS). OSAL owns only the generic
[`is_executable_candidate()` query](../../osal/README.md#headers-and-implementation--標頭與實作位置).
Skip non-candidates; preserve unexpected query failures as ModuleSystem/Spawn with
the OSAL cause. After selecting a path, launch/bootstrap/ABI failures fail that
Module without selecting another interpreter or stopping central.
Custom and `--python` are explicit selections and never fall back to PATH.

Empty PATH entries are ignored. Explicit relative entries use central's current
working directory and are made absolute before spawning in the Module directory.
Symlinks are not canonicalized, preserving venv identity. No Python process is
started merely to search PATH; the existing bootstrap performs compatibility checks.
Build-time CMake uses the same candidate policy, independently in its own environment;
see the [configuration contract](../../../../tools/configure/README.md#module-python-selection--module-python-選擇).

Module System 負責依 PATH 選擇名稱，OSAL 只查檔案與原生權限。空 PATH 項目不代表
目前目錄；明確的相對目錄則以中央 cwd 解析，再使用絕對路徑啟動。保留 venv 連結路徑，
不為搜尋額外啟動 Python。選定後的載入、版本、ABI 錯誤只讓該 Module 失敗，
不偷偷找下一套；Custom／`--python` 也不回退。

Validation (2026-11-02): the new real-App WSL regression failed before the fix
with `Permission denied` when the first PATH candidate had no execute permission.
It reached central shutdown normally. After correction:

- Windows x64, MSVC 19.44.35229, Release, CPython 3.13.12, CMake 4.3.2:
  full source build and **12/12** related CTests passed. `module.python_shell`
  additionally passed **three consecutive runs**. Selection tests: **18 cases**,
  one POSIX-permission case skipped, no failures.
- WSL Ubuntu 24.04 x86_64, GCC/libstdc++ 13.3, Release, CPython 3.12.3,
  CMake 3.28.3: full source build and **10/10** related CTests passed;
  selection tests **18/18** passed.
- Cases cover OSAL queries, real Module startup/shutdown, failed candidates,
  directory/broken-link/non-executable candidates, empty/relative PATH, Unicode
  venv identity, explicit selections, ABI rejection, continued central script
  execution, Python services/messages/RPC and Windows Runtime guards.

The first Windows malformed-image test waited inside native launch; a separate
non-VINC probe reproduced it. That PATH-search pass used test-only suppression.
The subsequent [OSAL fix](../../osal/README.md#windows-loader-error-dialogs--windows-載入器錯誤對話框)
adds thread-scoped launch handling; the Shell test now explicitly inherits process
error mode 0 and relies on production OSAL rather than test-side suppression.
Local logs are under `build/python-path-review/` (not tracked).
Native macOS, Debug, installed SDK end-to-end and OCR were not rerun this round.

這次已補真正 App 的修正前／後對照，不只測抽出的搜尋函式。Windows 12/12、WSL 10/10
相關 CTest 通過；Windows Shell 額外連續通過三次。測試中的損壞 EXE 會受原生錯誤
對話框模式影響，該輪曾由測試程序抑制。後續 OSAL 已加入原生啟動範圍的處理，
測試現在改以模式 0 驗證正式接線；新驗證與原生限制見上方 OSAL 連結。
macOS、Debug、安裝式 SDK 全套與 OCR 未於本輪重跑，不以上述結果替代。

### Installed SDK applications / 安裝式 SDK 應用

An installed SDK always carries `share/vinc/python`: adapter sources/internal
headers, Python package, build helper and pinned pybind11 headers/CMake tools.
The producer need not build bindings or have Python development files. These
materials are not a public C++ API and contain no interpreter. Upstream sources
remain unchanged; revision verification happens at SDK production, not in the
consumer's Git-less installation.

安裝式 SDK 預設交付 `share/vinc/python`：綁定來源／內部標頭、Python 套件、共用建置
helper 與固定 pybind11 標頭／CMake 工具。製作者不必建置 Python 綁定或具備 Python
開發檔；材料不是新的 C++ 公開 API，也不附帶直譯器。上游未修改，固定版本由 SDK
製作時核對，使用端不要求 Git repository 存在。

Set `CONFIG_VINC_APP_PYTHON=y` and use ordinary `add_message_dir()` /
`vinc_add_app()`. The application builds one `_native` image against its merged
built-in/custom message list and the installed Runtime archives. Source builds
use the same `cmake/VincPython.cmake` recipe. Late message registration is supported;
schema addition/edit/removal must rebuild the extension. Plain package lookup
and Python-disabled applications do not look for Python development files or
initialize pybind11. When enabled, Vinc initializes its pinned pybind11 first;
an already-created `pybind11::module` target is rejected instead of silently mixing
versions or Python environments. Other bindings can use those targets afterward.

設 `CONFIG_VINC_APP_PYTHON=y`，沿用 `add_message_dir()`／`vinc_add_app()` 即可。
每份應用建置一個 `_native`，使用合併的內建／自訂訊息與 SDK 預編譯 Runtime。
來源版共用同一 helper。可在 app 宣告後登記訊息；增刪改 schema 會重建擴充。
單純套件查找及關閉 Python 的應用不尋找 Python 開發檔、不初始化 pybind11。
啟用時須由 Vinc 先建立固定 pybind11 targets；若已有 `pybind11::module` 則明確拒絕，
不默默混用版本或 Python 環境。之後其他綁定可使用同一組 targets。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/vinc-sdk
cmake --build build --target MyApp
cmake --install build --prefix /path/to/deployed-app
```

Build/install deploy only declared package files, including the selected extension,
ABI/CRT metadata and licenses, rather than sweeping the build directory for stale
extensions from another ABI. Ownership records remove obsolete unchanged files
when Python is disabled or the extension filename changes; untracked/modified
files are preserved. User `.py` Modules and extra dependencies remain application-
owned deployment inputs under `data/module`. Never overlay a running application.
Use separate build directories when changing Python environments to avoid stale
CMake discovery. Runtime follows the consumer App's Kconfig default or `--python`, never the SDK producer's Python path.

build／install 僅部署明確列出的擴充、ABI／CRT 資訊與授權，不掃描並複製另一 ABI
遺留的擴充。功能關閉或檔名改變時清理未被修改的舊產物，保留未納管／修改過檔案。
使用者 `.py` 與額外相依仍由應用自行部署到 `data/module`。不要覆蓋執行中的程式；
切換 Python 環境建議使用不同 build 目錄，避免沿用 CMake 探測快取。
執行時使用消費端 App 的 Kconfig 預設／`--python`，不使用 SDK 製作者路徑。

#### Installed SDK validation (2026-10-29) / 安裝式 SDK 驗證

`sdk.installed_python` passed on Windows x64/MSVC 19.51/CMake 4.3.2/Ninja 1.13.2
in Debug and Release with isolated CPython 3.13.12, and on WSL Ubuntu 24.04.5
x86-64/GCC+libstdc++ 13.3/CMake 3.28.3/Ninja 1.11.1/CPython 3.12.3 in Release.
The Windows copy uses the already-prepared compatible CRT; system Python was not
modified. This supplies a positive import path beyond the earlier old-CRT rejection.

Windows Debug／Release 與 WSL Ubuntu 24.04 Release 的 `sdk.installed_python` 均通過。
Windows 使用既有隔離 CPython 3.13.12 及相容 CRT；Linux 使用系統 CPython 3.12.3。
未修改正式 Python 安裝，這次補足先前舊 CRT 拒絕之外的正常匯入路徑。

Regression checks also passed: Windows Release installed C++ consumer/application
2/2 plus configuration/ownership/license tests 3/3; WSL Release the same five
tests 5/5. These retain default Bag/UI composition and incomplete-SDK rejection.

回歸亦通過：Windows Release 安裝式 C++ 使用端／應用 2/2，加設定／所有權／授權
3/3；WSL Release 同五項 5/5。保留預設 Bag／UI 組裝及不完整 SDK 的拒絕行為。

The producer had Python bindings disabled. The fixture moves the SDK and app,
disables Git discovery in the consumer, hides SDK/build paths during execution,
and tests custom C++/Python Bus round trips, typed RPC, Param callback/cache,
Shell lifecycle and clean shutdown. Added/changed/deleted/invalid schemas exercise
real extension rebuilds and recovery; an incompatible bootstrap Python version
rejects that Module while the native peer/central remain responsive. Missing
deployment files are repaired, and disabling Python cleans generated packages
while retaining user Module sources/untracked files. Source-build Python
enable/build/import/disable cleanup also passed on Windows Release.

SDK 製作時關閉 Python 綁定。測試搬移 SDK／應用、禁止使用端 Git 探測、執行時隱藏
SDK 與 build 原位置，驗證自訂 C++／Python 雙向 Bus、typed RPC、Param callback／快取、
Shell 啟停及正常退出。訊息增刪改／非法 schema 驗證擴充重建與恢復；錯誤 Python ABI
只拒絕該 Module，中央及原生 Module 仍可操作。遺失部署檔可補回，關閉 Python 清理
生成套件並保留使用者腳本／未納管檔案。Windows Release 來源版亦通過開啟、建置、
匯入、關閉清理循環。

Enable this opt-in test with `-DVINC_TEST_SDK_PYTHON_EXECUTABLE=/path/to/python`
on an SDK build with `BUILD_TESTING=ON`; it requires that interpreter's development
environment and working runtime dependencies. No native macOS run, real switch
between two Python minor versions in one build, PyPy/free-threaded support, or
full Python service regression is claimed by this integration test.

在 SDK 測試建置指定 `VINC_TEST_SDK_PYTHON_EXECUTABLE` 才啟用此測試，需要該直譯器
開發檔與可用 Runtime。未執行 macOS 原生驗證、同 build 內切換兩種實際 Python minor，
亦不以這項整合測試宣稱完整 Python 服務回歸或 PyPy／free-threaded 支援。

<a id="windows-msvc-runtime-載入前檢查"></a>
<a id="windows-msvc-runtime-preflight"></a>

### Windows MSVC Runtime preflight / Windows MSVC Runtime 載入前檢查

**Current source and installed SDK builds use static CRT:** Windows `_native.pyd` and its private
dependency chain use `/MT` (Debug `/MTd`). Central and C++ Plugins keep `/MD`.
Linux/macOS retain their existing system-runtime policy. Installed SDKs ship the
static archive closure even when their producer disables Python. Missing archives
or mixed CRT dependencies fail; there is no dynamic fallback.

**目前 Windows 來源版與 installed SDK 使用靜態 CRT：** `_native.pyd` 與其私有相依鏈改用 `/MT`，
Debug 為 `/MTd`；中央與 C++ Plugin 保持 `/MD`。Linux／macOS 不變。
installed SDK 即使製作者關閉 Python，也會交付靜態相依庫；缺檔或混用 CRT 明確失敗，不退回 `/MD`。

**macOS decision (2026-11-02):** retain the system dynamic C++ runtime. The audited
extension uses system libc++, with no evidence of the Windows-style private
runtime conflict. The decision, rationale and compatibility limits are owned by
the [Common runtime policy](../../common/README.md#macos-system-runtime-decision).
**macOS 已確認維持現狀：** 使用系統動態 C++ Runtime，不因 Windows 的舊 DLL
衝突改為靜態；原因與尚待驗證的相容性界線統一記錄於上述 Common 政策。

- Static builds verify actual imports after linking. Only the selected CPython
  DLL and reviewed Windows API DLLs are allowed; CRT imports (including UCRT and
  sidecars) or another native DLL fail generation and require review.
- Static format 3 records mode, native filename and SHA-256. `_build.py` selects
  the expected mode. Preflight checks the manifest, Python's actual extension
  selection and image hash before importing `_native`; stale/missing/mixed
  artifacts reject. The hash detects deployment mistakes, not malicious rewriting
  of the complete package. Redeploy the complete package after signing/modifying
  its image and regenerate the matching metadata.
- Static preflight neither inspects nor preloads MSVC CRT DLLs. Old DLLs already
  owned by Python/WinRT do not impose VINC's compiler floor on those libraries.
  CPython and third-party code still own their compatibility requirements.
- Python ABI matching remains mandatory. Python objects use Python allocation
  APIs; VINC's C++ resources are created/destroyed inside the extension. Arbitrary
  C++ pointers, containers or holders crossing unrelated extensions are unsupported.
- Static CRT security updates require rebuilding/redeploying VINC's extension;
  updating the system Redistributable cannot patch its embedded CRT.

靜態建置逐一檢查實際 DLL 相依，拒絕 CRT／UCRT／sidecar 或未審查 DLL。Format 3 保存
模式、檔名與 SHA-256；匯入前核對 Python 選中的檔案，混搭、缺檔、損壞均拒絕。
雜湊防止誤部署，不是簽章或安全沙箱；簽署／修改映像後也需重新產生相符 metadata。
靜態路徑不檢查、不預載 MSVC DLL，不替換 Python／WinRT 的舊檔。Python minor ABI
仍必須符合；不增加跨 extension 的 C++ ABI 承諾。靜態 CRT 安全修正須重建及重新部署。

<a id="runtime-build-cleanup"></a>

#### Runtime build cleanup (2026-11-02) / Runtime 建置整理

The MSVC recipe no longer contains the unreachable dynamic Redist discovery
branch. Static metadata still tracks its generator, loader and `crtversion.h`;
the header supplies informational toolset data, not a dynamic DLL version floor.
The legacy dynamic metadata tool, loader and tests are retained unchanged.
The root and tools guides now distinguish current static behavior from historical
dynamic deployment requirements. Repeated ABI probing was handled separately in
the [build ABI probe consolidation](#build-abi-probe).

已移除 MSVC recipe 不可達的動態 Redist 搜尋分支。靜態 metadata 保留生成器、loader
及 `crtversion.h` 相依；Header 提供工具鏈診斷資料，不作為動態 DLL 版本門檻。
舊動態 metadata 工具、loader 與測試維持原樣。根目錄與工具指引已區分現行靜態行為
與歷史動態部署要求；重複 ABI 探測已於後續完成，見
[建置 ABI 探測統一](#build-abi-probe)，兩輪驗證範圍分別記錄。

Validation / 驗證：

| Environment / 環境 | Evidence / 結果 |
| --- | --- |
| Windows x64, MSVC 19.44.35229, CPython 3.13.12, Release | Source build and related CTests **8/8** passed: CRT policy, Shell/bootstrap, lifecycle, messages, metadata, loader, services and RPC. / 來源建置與相關回歸通過。 |
| Same Windows build / 同上 | Touching the metadata generator and loader rebuilt matching format-3 metadata; deleting the manifest repaired it. Filename/SHA-256 matched the actual image, and focused tests passed **3/3** afterward. Ninja link inputs retain both files and `crtversion.h`, without Redist DLL inputs. / 增量及缺檔補回通過，靜態 metadata 相依保留。 |
| Windows installed SDK, same compiler/Python, Release / 同工具鏈 | `installed_python.cmake` passed: relocated SDK/App, downstream custom Bus/RPC/Param messages, schema edits/recovery, ABI and metadata failure isolation, resource repair and Python-disable cleanup. The installed recipe matches the source file. / 安裝式 SDK 下游建置、搬移部署、增量、故障隔離及清理全數通過，交付的 recipe 與來源一致。 |
| WSL2 Ubuntu 24.04, GCC/libstdc++ 13.3.0, CPython 3.12.3, Release | Source build and Python Shell/lifecycle/messages/services/RPC CTests **5/5** passed. / Linux 來源建置與五項 Python 回歸通過。 |

Metadata unit tests passed **7/7**. Loader tests passed **20**, with **1** WinRT
import-order case skipped because its isolated wheel directory was not configured.
The separate real old-MSVCP preload case ran successfully. This round did not
repeat OCR, Debug, native macOS or the WSL installed-SDK suite. Local logs are in
`build/python-runtime-cleanup/` (not versioned).

Metadata 單元測試 7/7；loader 測試 20 項通過、1 項 WinRT 載入順序因未設定隔離 wheel
目錄而略過，獨立的真實舊 MSVCP 預載案例有執行並通過。本輪未重跑 OCR、Debug、macOS
原生或 WSL installed SDK 全套。紀錄位於上述本機 build 目錄，不納入 Git。

Ribosome deliberately generates messages on every build, so an otherwise
unchanged build may relink the extension and refresh metadata. This existing
policy was preserved; timestamp stability is not an acceptance criterion here.

Ribosome 現有契約為每次 build 都生成訊息，因此未改來源也可能重新連結擴充、更新
metadata。本輪保留該政策，不以時間戳不變作為驗收條件。

#### Retained dynamic-mode guard / 保留的動態模式防護

The following describes the retained format-2 guard and historical deployment
policy. Newly built MSVC source/installed bindings select static mode above;
their mode is generated by CMake, not a runtime user override. The current CMake
recipe does not locate Redist packages or track their DLLs. Dynamic generation
remains available in the internal metadata tool and covered by its tests; this
does not offer a supported CMake switch back to dynamic bindings.

以下為保留的 format-2 動態防護及歷史部署契約；新建置的 MSVC 來源／安裝式綁定
使用上方靜態模式，由 CMake 決定，不提供使用者於執行期切換。現行 CMake recipe
不搜尋 Redist 或追蹤其 DLL；內部 metadata 工具的動態生成與相關測試仍保留，
但不代表提供切回動態 binding 的 CMake 選項。

import vinc checks required runtimes through Python/Windows APIs before _native. Bootstrap imports Vinc before user code; external Python should import before WinRT/other native packages. This creates neither a Session nor worker.

`import vinc` 在載入 `_native` 前，以 Python／Windows API 檢查該擴充需要的 MSVC Runtime。
中央 bootstrap 本來就先 import Vinc 再載入使用者 Module；外部 Python 應在 WinRT 等原生套件前
import Vinc。這不建立框架 session，也不新增背景執行緒。

- Build-postprocessing uses dumpbin /DEPENDENTS for actual MSVCP/VCRUNTIME/CONCRT imports including Debug/sidecars. Format 2 records each imported DLL's actual file version from the selected Visual Studio deployment package. Header patch numbers are not DLL floors; System32 is not a build-time baseline. See the deployment selection contract below.
- Generator supports standard MSVC bin/`Host<arch>`/<arch>/cl.exe layout, not unverified alternate Windows toolchains. Deploy the whole python/vinc package, not only .pyd.
- Check every loaded required same-name DLL; any below baseline/different major raises ImportError. Compatible Python-directory/other DLLs may remain; System32 placement is not mandatory.
- Missing Release imports are selected only from the Windows API system directory. Validate all, preload in order, recheck transitive loaded DLLs, and retain handles for process lifetime. Do not alter PATH/search policy.
- Debug checks actual Debug CRT names; if absent in System32, allow configured interpreter/absolute developer PATH directories, never cwd/relative PATH. Debug CRT is not bundled or replaced by Release redistributables; missing files reject.
- Missing DLL/version/manifest, corrupt metadata, or preload failure rejects before _native. Never replace/unload/patch existing DLLs, auto-install, or clean-build as a runtime remedy.
- External callers may catch ImportError and stop using Vinc. Bootstrap prints stderr and exits 70; central applies startup policy while other Modules continue.

- `_msvc_runtime.json` 由建置後的 `dumpbin /DEPENDENTS` 取得 MSVCP140、VCRUNTIME140、CONCRT140
  家族的實際匯入名稱，包含 Debug 名稱與 STL sidecar。Format 2 逐一記錄選定 Visual Studio
  部署套件中各 DLL 的實際檔案版本，不再拿 Header 修訂號當 DLL 門檻，也不拿 System32
  當建置基準。這是部署基準，不宣稱是每個函式的最低 ABI 版本；選取契約見下方。
- 目前生成器針對標準 MSVC `bin/Host<arch>/<arch>/cl.exe` 目錄布局；不假裝支援未驗證的
  Windows 替代工具鏈。更新原生擴充時須一起部署整個 `python/vinc`，不能只換 `.pyd`。
- 已載入的必要同名 DLL 全部檢查；只要其中一份低於基準或主版本不同，即拋 `ImportError`。
  符合基準的 Python 目錄／其他位置 DLL 可沿用，不強制所有 DLL 都在 System32。
- 未載入的 Release DLL 只從 Windows API 取得的系統目錄選取，先驗證全部候選，再依序預載，
  每次載入後重新檢查依賴帶入的 DLL；handle 保留至程序結束。不修改全域 PATH 或 DLL 搜尋設定。
- Debug 依實際 Debug CRT 相依檢查；系統目錄沒有時，允許已配置的 Python 執行檔目錄與
  開發環境絕對 PATH 目錄，排除工作目錄及相對 PATH。Debug CRT 是開發相依，不由 Vinc 打包，
  也不以一般 Release Redistributable 取代。缺少時明確拒絕。
- 缺少 DLL、讀不到版本、metadata 缺少／損壞、預載失敗都明確拒絕載入 `_native`。
  不替換、卸載或修補已載入的 DLL，不自動安裝套件或 clean build。
- 外部程式可捕捉 `ImportError` 並停止使用 Vinc。中央啟動時，bootstrap 將診斷寫到 stderr，
  以 70 退出；中央沿用啟動失敗政策，其他 Module 繼續運作。

**Python can itself preload an old VCRUNTIME 140.** Import ordering then cannot fix it; merely installing a newer system copy leaves the chosen old copy active. Deployment must repair/update Python dependencies and restart. Diagnostics show path/version/requirement; Vinc does not modify Python installations.

**Python 本身也可能先載入較舊 VCRUNTIME140。** 此時只調整 `import vinc` 順序不能補救，
也不能只安裝系統新版卻留下 Python 實際選用的舊副本；需由部署者修正／更新 Python 的 Runtime
相依並重新啟動程序。錯誤顯示實際路徑、版本與需求。Vinc 不自動修改 Python 安裝。

Preflight is compatibility defense, not a sandbox. Direct link-time imports do not cover all later dependencies, concurrent native loader races, or later forced full-path loads. Import before starting threads. Maintain system/third-party deployment normally; native crashes still require a native stack to identify the function.

<a id="deployment-baseline-selection--部署基準選取"></a>

#### Historical dynamic deployment baseline / 歷史動態部署基準

The compiler's `VC/Tools/MSVC/<toolset>` determines the **same VC installation's**
`Redist/MSVC` directory. Select its newest numeric package directory with the same
major and an equal/newer minor than the CRT header family. Microsoft documents
the [deployment files and compatible toolset families](https://learn.microsoft.com/en-us/cpp/windows/determining-which-dlls-to-redistribute).
Directory/header patch numbers need not equal serviced DLL file versions.
For example, the 14.44.35112 directory contains 14.44.35211 DLLs.

- Read the native image's PE architecture and each corresponding CRT file's PE
  architecture/version, without loading those files. Release selects the desktop
  CRT directory; Debug selects `debug_nonredist/.../DebugCRT`.
- Missing/ambiguous package directories, missing imports, unreadable versions,
  wrong architecture or older-family files fail the build. No environment PATH,
  System32, unrelated VS installation, arbitrary version override or download is
  used to supply a build baseline. A custom extracted toolchain needs its official
  Redist files in the same VC layout.
- The former dynamic CMake recipe recorded the selected directory and tracked its DLL files as link inputs.
  Reconfigure after changing Redist selection. A changed selection discovered
  during metadata generation fails rather than silently using a new package.
- Metadata contains per-DLL versions, toolset/configuration, informational header
  version and a VC-relative source path; it contains no producer-specific absolute
  path. Source and installed SDK consumers use the same generator. Deploy metadata,
  loader and native extension together; old/malformed formats are rejected.
- This is a supported deployment baseline, not proof that every older DLL crashes
  or that arbitrary mixed DLL cohorts are safe. A newer selected package may raise
  the baseline. This change neither promises old Python-bundled CRT compatibility
  nor requires WinRT 4.x, and it does not distribute Microsoft's DLLs.

依所選編譯器找到同一 VC 安裝下的 `Redist/MSVC`，選取主版本相同、次版本不低於 Header
家族的最新數字目錄。目錄／Header 修訂號與 DLL 實際檔案版本不必一致。依原生映像的 PE
架構選 Release CRT 或 Debug 非散布 CRT，逐檔驗證架構與版本，不載入這些檔案。
套件缺失／不明確、缺 DLL、版本無法讀取、架構錯誤或家族過舊均使建置失敗；不從 PATH、
System32 或其他 VS 安裝補猜，也不下載／自動安裝。自行解壓工具鏈須保留官方 Redist 布局。
先前的動態 CMake recipe 追蹤 DLL 檔案變更；當時改選 Redist 後須重新配置。
保留的動態 metadata 生成器若發現選取已改變，仍會明確失敗。
Metadata 只保留逐檔版本、工具鏈／組態、供診斷的 Header 版本及 VC 相對來源，不洩漏本機
絕對路徑。來源建置與 installed SDK 使用同一生成器，部署須同步更新整套 Python binding。
這不是所有舊 DLL 的最低 ABI 推導；新版部署套件也可能提高門檻，不承諾舊 Python CRT
混搭、不要求 WinRT 4.x、不散布 Microsoft DLL。實測見 [Runtime 驗證](RUNTIME_VALIDATION.md)。

這項檢查是啟動相容性防護，不是程序級安全沙箱。依賴清單是連結時的直接匯入，不涵蓋使用者
日後動態載入的所有相依；不能阻止其他 native thread 並行載入／卸載 DLL，也不保證第三方稍後
以完整路徑強行載入另一份 DLL 的行為。建議在啟動其他工作執行緒前 import Vinc。
仍需正常管理系統／第三方套件部署，原生崩潰的具體函式要以 native stack 確認。

<a id="本輪驗證2026-10-14"></a>

#### Recorded preflight validation (2026-10-14) / 本輪驗證（2026-10-14）

- Windows x64/MSVC 19.51 full Debug/Release builds; lifecycle/messages/preflight/services/Python RPC/Module Shell/Python Shell passed **7/7** each.
- Release protection **13/13** using isolated official winrt-runtime 3.2.1 cp313 wheel/typing_extensions4.16.0. Vinc-only succeeds; WinRT-first rejects; Vinc/new-runtime-first coexist; manual old MSVCP rejects before native import. Local probe OCR was not rerun.
- Debug **11 passed/2 Release-only skipped**; simulated failure policies and actual Debug-CRT enumeration/version/import passed, without a real old-Debug-CRT test.
- Real central verifies exit 70 with peers responsive, bootstrap manifest errors on stderr, and missing-manifest incremental regeneration without clean.
- Installed CPython 3.13.12/VCRUNTIME 14.42.34438 was correctly rejected by baseline 14.51.36244. Success used isolated build/runtime-investigation/python with VS 14.51.36247; installed Python/WinRT were unchanged. Those caches selected the isolated copy.
- Real old-DLL/WinRT tests require VINC_TEST_OLD_MSVC_RUNTIME/VINC_TEST_WINRT_DIR; absent inputs explicitly skip, without download/install. Normal CTest needs no WinRT.
- No native Linux/macOS; preflight is Windows MSVC only. That change was not locally rebuilt with 19.44; external crash evidence is not implementation validation.

- Windows x64／MSVC 19.51：Debug、Release 完整建置成功；Python lifecycle、messages、Runtime
  防護、services、RPC Python、Module Shell、Python Shell，兩種組態各 7/7 CTest 通過。
- Release 防護測試 13/13 通過，使用解壓到測試目錄的官方 `winrt-runtime 3.2.1`
  cp313-win_amd64 wheel 與 typing_extensions 4.16.0。A：Vinc 與訊息測試成功；B：WinRT
  先載入被明確拒絕；C／D：Vinc／預載先執行，WinRT Array 與 Vinc 訊息操作在同程序成功；
  E：手動載入舊 MSVCP140 後，被拒於 `_native` 匯入前。未在本機重跑探針 OCR。
- Debug 防護測試 11 項通過、2 項 Release 專用 WinRT／舊 DLL 案例跳過；Debug 的失敗政策
  有模擬測試，實際 Debug CRT 的列舉、版本與正常匯入亦通過。沒有真實舊 Debug CRT 測試證據。
- 中央測試確認 preflight 失敗的 Module 以 70 退出，原本運作中的 Module 仍可回應命令。
  metadata 損壞的診斷由 bootstrap stderr 保留。刪除生成 manifest 後，Ninja 增量建置可補回
  相同內容；無需 clean build。
- 本機原始 CPython 3.13.12 自帶 VCRUNTIME140 14.42.34438，實測被新工具鏈的 Release 基準
  14.51.36244 明確拒絕。成功路徑使用 `build/runtime-investigation/python` 的隔離 CPython
  3.13.12 副本，搭配 VS 提供的 14.51.36247 Release Runtime；正式 Python／WinRT 安裝未修改。
  當時本機 Debug／Release 建置快取的 Python_EXECUTABLE 指向此隔離副本；目前選擇入口
  已改為 App Kconfig，不應依這份歷史紀錄設定 CMake 變數。
- `module.python_runtime` 的真實舊 DLL／WinRT 案例需要測試環境變數
  `VINC_TEST_OLD_MSVC_RUNTIME`（舊 DLL 完整路徑）與 `VINC_TEST_WINRT_DIR`（解壓套件根目錄）。
  未提供時會明確 skip，不自動下載或安裝第三方套件。一般 CTest 不依賴 WinRT。
- 此防護僅由 Windows MSVC 建置啟用；Linux／macOS 不套用此 DLL 檢查。
  後續 MSVC 19.44 本機建置與 Python／Runtime 驗證已完成，見
  [Windows 工具鏈報告](../../sdk/WINDOWS_TOOLCHAIN.md)。外部崩潰報告不等於本機驗證。

<a id="掃描與-moduletype"></a>

## Discovery and ModuleTypes / 掃描與 ModuleType

- After Shell reservations/compiled catalog, scan executable-adjacent data/module once.
- Sorted direct physical subdirectories only, no recursion/symlink following.
- Same-name .py entry with valid Module naming; discovery never imports/executes it.
- Missing root is empty success; read/name/entry/duplicate issues diagnose/continue.
- Never overwrite C++/existing ModuleTypes. New directories require central restart; no hot scan.
- Default on_name consumes --name, without numeric suffixes; authors may override.
- Type/instance names cannot collide; instances have unique names/central MIDs.

- 中央在保留 Shell 命令、登記編譯期 catalog 後，掃描執行檔旁的 `data/module` 一次。
- 只處理一層實體子目錄，依目錄名排序；不遞迴掃描、不跟隨目錄 symlink。
- 每個子目錄必須有同名 `.py`，名稱遵守 Module 命名規則。掃描不 import 或執行 Python。
- 無此根目錄是正常空集合。讀取失敗、名稱不合法、缺入口、重複種類只回報問題；中央繼續運作。
- 不覆蓋 C++ 或既有 ModuleType。新增目錄需重新啟動中央；尚未提供熱掃描。
- 預設 `on_name()` 消耗 `--name <實例名稱>`，不自動加尾碼；使用者可覆寫命名 hook。
- 種類與實例不能同名；多實例各自有唯一名稱與中央頒發的 MID。

<a id="使用介面"></a>

## Usage API / 使用介面

See [Shell examples](../../../../examples/python/README.md). Export class Module:

見 [完整 Shell 範例](../../../../examples/python/README.md)。腳本匯出 `Module` 類別：

```python
import vinc

class Module(vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop):
    def on_start(self, args):
        print(f"ready: {self.get_module_name()}")

    def module_interface(self, args):
        if args == ["status"]:
            print("running")
            return 0
        print("Usage: status")
        return 1
```

FrameworkLoop runs pumpLoop and forbids main override; Manual requires main, choosing pumpOnce/pumpLoop or a user pump thread. Lifecycle success returns None, failure raises. Shell hook returns int32, where nonzero is command failure rather than Module fault. Completion returns string lists; naming may consume mutable args. Other hook args are local lists excluding the Shell type/instance token.

FrameworkLoop 由框架執行 `context.pumpLoop()`，不能覆寫 main。Manual 必須實作 main；
可自行呼叫 pumpOnce／pumpLoop 或建立自己的 pump thread。
生命週期 hooks 的成功回傳是 None；失敗使用 Python exception。Shell hook 明確回 int32，非零代表命令失敗，不是 Module 故障。
`module_autoComplete(args)` 回傳字串列表。`on_name(args)` 回傳實例名稱並可消耗 mutable args；
其他 hooks 的 args 是本地列表。Python 的 args 不包含 Shell 前面的種類／實例名稱。

Context provides living/pumps/Bus/Param/Log/RPC; BaseModule provides name/MID. Context has no public constructor; import gives no session/identity. During construction/naming services remain unbound and pump returns ResourcesUnavailable; start background work only in on_start after binding. Stop sets living false/stopped pump state. Runtime teardown revokes proxies; later calls raise VincError rather than access freed native pointers.

Context 提供 `living()`、`pumpOnce()`、`pumpLoop()`、Message Bus、Param、Log 與 RPC；
BaseModule 提供 `get_mid()`、`get_module_name()`。
Context 無公開建構子，來自框架建立的 BaseModule；成功 import 不會建立控制連線或取得身分。
初始化／命名時 Context 尚未接上服務，pump 會回報 ResourcesUnavailable。
建構與命名 hook 不得啟動背景工作；待 on_start 時身分及服務才已完成接線。
停止時 living 為 false；pump 可返回 stopped。Runtime 銷毀時撤銷代理，之後呼叫會拋 VincError，不能使用已釋放的原生 pointer。

<a id="錯誤執行緒與退出"></a>

## Errors, threads, and exit / 錯誤、執行緒與退出

- C++ expected failures remain Result; Python returns values/None or raises VincError.
- exception.error retains numeric domain/code/operation/native/causes/truncated. error_to_string/str provides diagnostics; cause records are independent copies.
- SDK never auto-recovers. Catch VincError/ValueError etc according to application policy.
- Escaping hooks preserve traceback/PythonException and existing VincError causes, bounded by 32 KiB python_traceback_bytes.
- Escaping Python Shell/completion exceptions latch Module failure even if Manual ignores pump Results; C++ Shell command failure remains distinct. Malformed external requests do not become hook exceptions.
- Callbacks run on pump; native waits release GIL, Python access acquires it. Heartbeat/watchdog neither run Python nor acquire GIL; one native control service/process.
- User-thread exceptions are not guaranteed detected; manage/join threads in on_stop.
- Successful Manual main uses Finishing/Completed, no restart; failures follow ModulePolicy. Hook/call-drain/object cleanup stays supervised and may be terminated when stuck.
- After supervised cleanup, os._exit avoids unbounded interpreter finalization. No atexit/final-GC promise; required cleanup belongs in on_stop. Python stdout/stderr are UTF-8/unbuffered. Before native import/local watchdog, central startup deadlines still protect launch.

- C++ 預期失敗仍使用 Result；Python 成功直接取得值／None，失敗拋 `vinc.VincError`。
- `exception.error` 保留 domain、code、operation、native、causes、truncated；數字不因語言轉接丟失。
  `vinc.error_to_string(error)` 或 `str(exception)` 顯示診斷。原因紀錄是獨立副本。
- Python SDK 不自動替使用者復原。使用者可捕捉 VincError、ValueError 等例外。
- 逃出受管理 hook 的例外保留 Python traceback，ModuleSystem 回報 PythonException 原因；
  若原本是 VincError，同時保存其結構化原因鏈。診斷文字上限是 config::python_traceback_bytes（32 KiB）。
- Python Shell 命令／補全的未處理例外會鎖定 Module 故障，即使 Manual 忽略 pump 失敗也會終止；
  C++ Shell hooks 保留原有命令失敗契約。外來畸形請求不因這項 Python 政策變成使用者 hook 例外。
- callback 只在 pump 呼叫端執行；原生等待期間釋放 GIL，接觸 Python 物件時取得 GIL。
  heartbeat 與 watchdog 不執行 Python、不取得 GIL；每個程序只有一份原生控制服務。
- 使用者自建 Python thread 的未處理例外不保證被框架偵測；必須自行管理與在 on_stop 中 join。
- main 正常返回走既有 Finishing／Completed 協定，不自動重啟；失敗依 ModulePolicy 處理。
  on_stop／借用呼叫排空／物件清理仍在原生監督期限內，卡住則依既有退出政策處理。
- 框架完成受監督的收尾後，以 os._exit 結束 Python 程序，避免重新進入無期限的 interpreter finalization。
  不承諾 atexit 或最後垃圾回收會執行；必要的應用清理必須放在 on_stop。stdout／stderr 採 UTF-8、無 Python 緩衝。
  Python 啟動與原生擴充 import 之前尚無本地 watchdog，中央的啟動期限仍保護這段流程。

<a id="接線與責任"></a>

## Integration responsibilities / 接線與責任

Private ModuleSource creates native Plugins through paired create/destroy or Python wrappers through PythonSource. Shared Runtime prepares authentication/fatal bridge/Bus/Param/Log/watchdog before user loading; after object destruction stop services, clear Source before bridge expiry. The extension owns one runtime, not an extra forwarding Host. Initial Python integration did not change native entry; current RPC Context uses entry 8/bootstrap 13. No-argument Python wrapper construction receives no central default name; on_name still decides.

`ModuleSource` 是 Module System 私有建立介面；native_source.cpp 保留 Plugin create／destroy 配對，
PythonSource 提供 Python 物件適配。共同 Runtime 先準備認證、fatal bridge、Bus／Param／Log 接線及 watchdog，
再載入使用者程式。Module 銷毀後停止服務，最後 clear source，仍早於 fatal bridge 失效。
Python 擴充包含唯一 Runtime owner；不另啟動一個 ModuleHost 幫 Python 轉送。
Python 初次接入時未改變 Native Plugin 入口；目前 Context 使用 Plugin entry 10／bootstrap v16。
Python Source 以無參數 create 建立 wrapper，不從中央取得預設實例名稱；on_name 契約不變。

<a id="驗證與後續"></a>

## Historical validation and follow-up / 驗證與後續

Recorded 2026-10-12 Windows x64/MSVC/official CPython 3.13.12:

2026-10-12：Windows x64／MSVC／官方 CPython 3.13.12：

- Full Debug/Release builds; all 72 CTests passed including reruns after test-setting fixes. Self-building Common/SDK tests need developer environments; restart tests use permitted delay values.
- Final focused Module/Context/Plugin/Python **6/6** each.
- Real discovery without execution, duplicate rejection, both modes/custom pump threads, command statuses/completion.
- SDK catches, unhandled causes/restart limits/new MID, revocation, stuck hook/import cleanup, central disconnect/heartbeat expiry.
- Real central Unicode deployment/different cwd/PATH/explicit Python/ABI refusal/raw output/traceback. C++ external SDK remains buildable without Python.

- Debug／Release 完整建置成功；全套各 72 項 CTest 均已通過，包含修正測試設定後的失敗項目重跑。
  會自行建置的 Common／SDK 測試需在 MSVC 開發環境執行；新增重啟測試使用既有政策允許的延遲值。
- 最後的 Module／Context／Plugin／Python 重點回歸兩種組態各 6/6 通過。
- 實測掃描不執行程式、重複名稱拒絕、FrameworkLoop／Manual／自行建立 pump thread、Shell 狀態碼及補全。
- 實測 SDK 錯誤捕捉、未處理例外的原因鏈、重啟上限與新 MID、Context 撤銷、卡住的 hook／import 回收、
  中央斷線及停止 heartbeat 後的子程序退出。
- 真正的中央 Shell 驗證中文部署路徑、不同工作目錄、PATH／指定 Python、ABI 不相容拒絕、
  原始輸出與 traceback；外部 C++ SDK 消費端仍可不啟用 Python 建置。

The historical machine PATH began with incompatible MinGW 3.14. The documented explicit CPython 3.13 path was machine-specific; select your own compatible interpreter. Vinc changes no PATH and never silently substitutes one. No native Linux/macOS validation in that round.

原驗證主機 PATH 第一個為不相容 MinGW 3.14，曾明確指定本機 CPython 3.13 路徑；那是主機專屬位置，使用時須選擇自己的相容直譯器。框架不修改 PATH、不默默替換 Python。該次 Linux／macOS 未實機驗證。

<a id="訊息與-message-bus"></a>

## Messages and Message Bus / 訊息與 Message Bus

```python
from vinc import messages, message_bus

message = vinc.make_message(messages.test)
message.data().x = True
message.data().y.assign([1, 2, 3])
publisher = self.context.advertise(messages.test, "camera/samples")
publisher.push(message)

loan = publisher.get_next()
loan.data().y.assign([4, 5])
publisher.commit(loan)

subscriber = self.context.subscribe(messages.test, "camera/samples", self.received)
# self.received(message) returns None and runs on the pump caller.
latest = subscriber.get_last()
```

- Options/StartPosition live in vinc.message_bus; pass options to advertise/subscribe. C++ minimum 2 slots/capacity/contention/loss apply; stats returns independent values.
- subscribe(Type, topic, callback=None, options=...) may use get_last only, no public next. Callbacks transfer mutable local ownership; get_last leaves callback cursors unchanged.
- list_topics(after="") returns TopicPage(topics, next) with standard paging.
- get_next loans fixed shared capacity. Successful commit, Publisher close/destruction, or teardown revokes all loan views. Dropping an uncommitted loan retains OutstandingLoan, never auto-cancels/publishes/releases.
- push accepts matching local types without consuming them; capacity failures raise VincError.
- Fields use C++ schemas/layout, not native lists in shared memory. Arrays support len/negative integer indexing/iteration/set/assign/resize/reserve/clear/push/pop/shrink/capacity/copy/at, not slices. Nested [] borrows; at copies; failed assign is not partial.
- Python boundary raises IndexError/TypeError/OverflowError/ReferenceError, without changing C++ contract-based indexing.
- MessageString stores bytes; assign accepts bytes/UTF-8 str, to_bytes preserves content, decode is strict UTF-8. NUL/non-UTF8 are retained, never guessed.
- message.copy retains content/metadata; assign retains destination metadata. Serialization images omit transport metadata. copy.copy(record view) makes a value copy without colliding with legal copy fields; constants are read-only mappings/raw bytes. Keywords use getattr/setattr.
- Six metadata getters are read-only. Unix/monotonic unsigned 64 nanoseconds use Python int. format_utc rejects negative/>uint64 inputs. Framework fills metadata, with no setters; hash is compile-time.
- Local views keep owners, not deleted/reused nodes. Python references do not extend shared writable leases. No raw pointer/memoryview/NumPy zero-copy API.
- Concurrent operations on a Pub/Sub or messages during push/commit return OperationInProgress. Native waits release GIL without extra pump/heartbeat/control threads.

- PublisherOptions／SubscriberOptions、StartPosition 位於 `vinc.message_bus`；傳給 advertise／subscribe 的 `options`。
  容量、至少兩個 slot、碰撞等待及丟失統計沿用 C++ Bus。`stats()` 回傳獨立統計物件。
- `subscribe(Type, topic, callback=None, options=...)` 可只用 get_last；不提供公開 next。
  callback 交付本地訊息所有權，可保存或修改。get_last 不推進 callback 游標。
- `context.list_topics(after="")` 回傳 TopicPage（topics、next），沿用原有分頁契約。
- `get_next()` 取得固定容量 loan，`commit()` 成功後撤銷 loan 和所有借用欄位。
  Publisher close／銷毀與 Module teardown 也撤銷仍存在的 loan。
  丟棄未提交 loan 仍讓該 Publisher 保持 OutstandingLoan；不偷偷丟棄、提交或解除占用。
- `push()` 只接受匹配型別的本地訊息，不消耗它。超過上限拋 VincError。
- 欄位使用與 C++ 相同的儲存 schema 和布局；沒有 Python 原生 list 直接落入共享記憶體。
  陣列支援 len、整數索引／負索引、迭代、set、assign、resize、reserve、clear、
  push_back、pop_back、shrink_to_fit、capacity、copy、at。尚未提供 slice。
  `[]` 的巢狀資料是借用 view；`at()` 是獨立副本。陣列整批 assign 失敗不部分覆寫目的地。
- Python 型別轉換先檢查宣告寬度：越界 IndexError，型別不合 TypeError，數值溢位 OverflowError，
  失效借用 ReferenceError。這是 Python 語言邊界；沒有改動 C++ 的 contract-based `[]`。
- 原始字串以 bytes 保存；MessageString.assign 接受 bytes 或以 UTF-8 編碼的 str，to_bytes 原樣取回，
  decode 使用嚴格 UTF-8。不存在自動猜測編碼。NUL 與非 UTF-8 bytes 可保留。
- `message.copy()` 複製內容及接收 metadata；assign 複製內容並保留目的地 metadata。
  serialize／deserialize 使用既有驗證過的訊息映像，不包含傳輸 metadata。
  巢狀 record view 用 `copy.copy(view)` 取得獨立副本，避免與合法的 `copy` 欄位撞名。
  常數放在 `Type.constants` 唯讀 mapping；字串常數是保留原始內容的 bytes。
  Python keyword 欄位可用 getattr／setattr。
- 發布 metadata 為唯讀：topic_name、publisher_name、publisher_instance、get_message_timestamp、
  get_message_monotonic_timestamp、get_topic_count。Unix／單調時間均為無號 64-bit 奈秒，以 Python int 交付。
  `vinc.format_utc(message.get_message_timestamp())` 產生 UTC 文字，負數／超過 uint64 輸入拒絕。
  發布時仍由框架填入，Python 無 setter。schema_hash 是編譯期 hash。
- 本地訊息 view 保留其 owner；移除的巢狀節點不因索引再利用而復活。
  共享 loan 的 view 不因保存 Python 參照而延長發布有效期。沒有 raw pointer、memoryview 或 NumPy 零拷貝入口。
- 單一 Publisher／Subscriber 的同時操作，以及 push／commit 進行中的訊息存取，回報 OperationInProgress。
  原生等待釋放 GIL；沒有增加 pump、heartbeat 或控制服務執行緒。

<a id="param-與-log"></a>

## Param and Log / Param 與 Log

```python
from vinc.param import Type, Permission  # 或使用 vinc.param.Type
```

param/log/messages/message_bus support both package import and vinc.* attributes.

`param`、`log`、`messages`、`message_bus` 同時支援 package import 和 `vinc.*` 屬性存取。

```python
self.context.param.register_param("gain", vinc.param.Type.Double, 1.25)
self.context.param.register_param("other.gain", vinc.param.Type.Double)
self.context.param.on_change(self.parameters_changed)
value = self.context.param.update("gain", vinc.param.Type.Double)
```

- Explicit Param Type selects fixed widths; Python int never guesses. None means omitted default/watch, not a value.
- FrameworkOnly is default; Open permits all Modules. Typed set goes to central; update reads cache only.
- on_change runs in pump with an independent read-only mapping snapshot/immutable Entry fields, safe to retain across pumps. Missing names stay absent; watches checks presence/compatibility; None clears callback.
- Log severity/write uses native service with framework identity/time. enabled avoids unnecessary eager f-string formatting. Capacity/backpressure raises VincError; Python strings are never reinterpreted by fmt. print stays raw.
- Escaping Param/Bus callbacks retain traceback/causes and latch Module fault. Caught exceptions are application responsibility; central/peers continue.
- Teardown revokes Context/drains entered calls before GIL-held callback/Bus cleanup, breaking bound-method cycles. Stuck work follows native supervision/deadlines.

- Type 明確選擇 Bool、Int8／UInt8、Int16／UInt16、Int32／UInt32、Int64／UInt64、Float、Double、String。
  不由 Python int 自動推測寬度。None 作為省略 default 的監聽形式，不是參數值。
- register_param 的 permission 預設 FrameworkOnly；Open 允許所有 Module 修改，沿用中央權限驗證。
  set(name, Type, value) 請求中央更新；update(name, Type) 只讀 Context 快取，不同步去中央撈。
- on_change(callback) 在 pump 執行；傳入獨立複製的唯讀 mapping，Entry 的 value/default_value、
  permission、version、timestamp、type 皆唯讀。可保留 snapshot，不隨下次 pump 改變。
  未出現的監聽參數不在 map 中；watches() 可查 present／compatible。None 清除 callback。
- `context.log.error/warn/info/debug(text)` 或 write(Level, text) 使用同一套原生 Log，
  module／MID／時間由框架填入。enabled(Level) 可避免不必要格式化；先用 Python f-string 建立文字。
  大小限制與回壓仍回報 VincError；不把 Python 格式字串交給 fmt 再解讀。
  print 仍走原有 raw output 路徑。
- 逃出 Param／Bus callback 的例外保留 traceback／VincError 原因鏈並鎖定 Module 故障。
  使用者捕捉的例外由使用者處理；中央及其他 Module 不因此關閉。
- Teardown 先撤銷 Context 並排空已進入的呼叫，再持有 GIL 清除 callback 引用及 Bus 資源，
  打斷 bound-method 對 Module 的循環引用。卡住的使用者工作仍由既有 native watchdog／中央期限處理。

<a id="第二輪驗證2026-10-12"></a>

## Second-stage validation (2026-10-12) / 第二輪驗證（2026-10-12）

Windows x64/MSVC/CPython 3.13.12 full builds. Debug **74/74**; Release all 74 passed after deploying corrected import aliases/rerunning failures. Final four Python suites passed **4/4** each. Coverage includes nesting/bool/integer bounds/raw bytes/expired views/capacity/C++↔Python/commit-close/uncommitted loans/Param snapshots/Log/traceback/peer survival. PythonTest fixtures/private _test_messages aliases exist only with testing, not SDK. Shared private bindings register common container types once. Serialize same-VINC_NAME central tests across configurations to avoid exclusivity refusal. No native Linux/macOS run.

Windows x64／MSVC／CPython 3.13.12：Debug／Release 完整建置成功。
Debug 全套 74/74 通過；Release 全套執行後，更新部署的 Python 子模組匯入別名並重跑失敗項，
74 項皆已通過。最後版本的四個 Python 測試在兩種組態分別重跑，均為 4/4。
包含本地巢狀訊息、bool 陣列、整數邊界、原始字串、失效 view、容量失敗、
C++↔Python 雙向跨程序收送、loan commit／close、未提交 loan、Param 快取／唯讀快照、
Log、callback traceback／原因鏈及其他 Module 存活。
測試 schema 使用 PythonTest 前綴，Frame／Types 別名只在 BUILD_TESTING 啟用，
位於私有 `_test_messages`，不屬於 SDK API。容器 view 在共用私有綁定目錄建立一次，
避免不同訊息清單使用同一種陣列時重複註冊 Python 型別。
不同建置組態的 Shell 測試須依序執行：它們使用相同 VINC_NAME，並行啟動會被單一實例鎖拒絕。
Linux／macOS 未實機驗證。

<a id="待處理事項"></a>

## Outstanding issues / 待處理事項

Current pending validation is tracked in the [root TODO](../../../../TODO.md).
The records below preserve completed rollout stages and historical diagnosis;
they are not an uncompleted implementation checklist. Current Windows bindings
use static CRT; the retained dynamic-mode guard does not impose its DLL version
floor on static bindings. See [the current policy](#windows-msvc-runtime-preflight).

目前待驗證項目見根目錄 TODO。下方保留已完成的導入階段與歷史診斷，不代表這些實作
仍未完成；現行 Windows binding 使用靜態 CRT，保留的動態防護門檻不套用於靜態綁定。

<a id="runtime-policy-rollout"></a>

### Runtime policy rollout / Runtime 政策導入

Stage one adds component-owned Windows static-CRT archives and
`cmake/VincPythonRuntime.cmake` to select their Runtime/catalog targets.
The [common policy](../../common/README.md#build-runtime-policy) owns CRT rules and
variant boundaries. Stage two selects these libraries in source-built Windows
bindings; stage three exports them in installed SDKs and uses the same static
metadata and loader validation for downstream custom-message bindings.

第一階段建立建置政策及靜態相依庫；第二階段已切換 Windows 來源版 binding、metadata
與載入驗證；第三階段將相同政策接入 installed SDK 與下游自訂訊息綁定。
SDK 保留「下游才啟用 Python」的能力；Linux／macOS 保留系統 Runtime。

The following records preserve each stage's scope. Subsequent formal-entry,
multi-App and System Module acceptance is maintained in the
[SDK integration results](../../sdk/INSTALL.md#runtime-final-acceptance);
the [root TODO](../../../../TODO.md) tracks the owner's native macOS validation.

以下保留各階段的實測範圍；後續正式建置入口、多 App 與 System Module 整合結果統一見
[SDK 驗收紀錄](../../sdk/INSTALL.md#runtime-final-acceptance)，macOS 實機驗收進度見根目錄 TODO。
下方保留初次阻塞紀錄與通知快照修正後的 Mac 實測。

<a id="macos-runtime-validation-2026-11-02"></a>

#### Native macOS validation (2026-11-02) / macOS 原生驗證

以下先保留初次編譯阻塞的歷史證據；通知快照修正後的結果見
[後續功能驗證](#macos-snapshot-repair-validation)。初次失敗不代表目前仍無法建置。

Source: `50a88325245a02ce93d33f985e763284ed3f56bc`. macOS 26.3.1(a), OS build
25D771280a, arm64; Apple Clang 21.0.0 (clang-2100.1.1.101), Apple libc++ 21.1.6,
SDK 26.5, CMake 4.4.4, Ninja 1.13.2, Node 24.22.0/npm 11.19.1. Tests selected the
existing Codex-bundled, GIL-enabled arm64 CPython 3.12.14 through System/PATH;
this is not a claim that the host's `/usr/bin/python3` 3.9.6 meets the 3.11 floor.
No system interpreter was installed or replaced.

使用全新 `build/macos-validation-20261102/source-release`，從正式 `build.py` 配置
Release，明確啟用 Python／UI／Bag／App／範例／測試與 `VINC_INSTALL_SDK=ON`。
Kconfig System 正確選到上述 3.12.14，Python／Development.Module 與 pybind11 配置成功。
但完整建置因 [Message Bus 通知快照編譯問題](../../message_bus/README.md#macos-notification-snapshot-blocker)
失敗；不能宣稱 Python 擴充、中央 App 或 SDK Runtime 整合已通過。

| Check / 檢查 | Result / 結果 |
| --- | --- |
| Build graph / 建置圖 | 293 compile-command entries contained no Windows `/MT`, `/MTd`, `/MD` or `/MDd` flags; no `_python_runtime` targets were declared. This checks configuration, not a completed image's dependencies. / 未混入 Windows CRT 旗標或額外靜態版本；不是已連結產物的動態相依稽核。 |
| Independent native CTests / 獨立原生測試 | **41/41** passed: Common, OSAL (including `osal.notification` and `osal.notification_native`), Concurrency, Storage, OutputHub and Control. Targets were built separately after the full build failed. / 完整建置失敗後，另行建置並執行上述可獨立的測試。 |
| Kconfig/SDK selection / 設定選擇 | **6/6** passed: `sdk.configuration`, `sdk.python_selection`, `sdk.app_selection`, `sdk.owned_files`, `sdk.license_inventory`, `sdk.package_inventory`. App selection uses real tiny Plugins without a complete Runtime. / 小型 Plugin 驗證不等於多 App 執行驗收。 |
| Build/setup/SDK tooling / 工具測試 | 117 cases: **94 passed, 23 skipped**, no failures. A separately enabled real CMake build-entry integration case also passed, covering clean/incremental build, paths with spaces and incompatible-cache rejection. / 另補跑真實建置入口測試一項通過，跳過案例未當作通過。 |
| Frontend / 前端 | Lockfile dependencies installed; `npm run typecheck` passed. No interactive browser or complete App/UI runtime validation. / 型別檢查通過，未宣稱瀏覽器或完整 App 執行通過。 |

**初次嘗試時仍待完成（後續結果見下節）：** 來源 Python import／生命週期／訊息／Bus／RPC／Param、
單一 App／Host 入口、SDK 製作者關閉 Python 後由下游啟用、安裝搬移、
多 App 執行與失敗隔離、部署補檔／停用清理，以及擴充的 `otool` 相依／載入路徑稽核。
因此根目錄 TODO 的 Mac 驗收項目保留未完成。此輪不測 Windows 專用 CRT DLL／WinRT
案例，也不宣稱 Debug、Intel Mac 或其他 Python minor 已通過。

Reproduction uses the existing tools on PATH, with the selected Python directory
before the old system interpreter and `build/tools/bin` providing CMake/Ninja:

```sh
python3 -B build.py --config Release --build-dir build/macos-validation-20261102/source-release --jobs 4 \
  -DVINC_CONFIG_FILE=/absolute/path/to/python-system.conf \
  -DPython3_EXECUTABLE=/absolute/path/to/python3 \
  -DVINC_INSTALL_SDK=ON -DVINC_TEST_SDK_PYTHON_EXECUTABLE=/absolute/path/to/python3 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

`python-system.conf` sets `CONFIG_VINC_APP_UI=y`, `CONFIG_VINC_APP_BAG=y`,
`CONFIG_VINC_APP_PYTHON=y` and `CONFIG_VINC_PYTHON_SYSTEM=y`. It lives in the ignored
validation directory; no application config or production code was changed.
Evidence is under `build/macos-validation-20261102/`: `source-release-build.log`,
`native-ctest.log`, `configuration-ctest.log`, `app-selection-ctest.log`,
`tools-tests.log`, `build-entry-integration.log`, `frontend-typecheck.log` and
`validation.json`. The 47 CTests are disjoint subsets of the configured 125 tests;
the unbuilt remainder was not executed and is not counted as passing.

<a id="macos-snapshot-repair-validation"></a>

##### Follow-up after the snapshot repair / 通知快照修正後的功能驗證

Same environment and base commit as above, plus the uncommitted
`codex/notification-snapshot-mutex` changes. The platform-independent repair and
snapshot contracts are owned by the [Message Bus record](../../message_bus/README.md#macos-notification-snapshot-blocker).
The previously configured Python-enabled Release tree now builds completely,
including the native extension, App, Plugins and SDK header checks. The
`message_bus.cleanup` fixture also needed its direct `<thread>` include for
`std::this_thread::sleep_for`; production notification failure handling is unchanged.

| Functional check / 功能檢查 | Result / 結果 |
| --- | --- |
| Release full CTest run | **125 passed, 1 failed out of 126**; the sole failure was the Python services temporary-path fixture described below. No skipped tests. / 初次完整執行保留唯一 fixture 失敗，不描述成一次全綠。 |
| Final Release rerun, original environment | **3/3 passed**: `module.python_services`, `message_bus.cleanup`, `message_bus.notification_snapshots`. The rebuilt services fixture passed without overriding `TMPDIR`. Together with the unaffected full-run results, all **126 distinct Release suites have passing evidence**. / 修正後回到原本環境重驗通過，合併未受影響的完整回歸，126 項皆有通過證據。 |
| Debug related regression | **8/8 passed**: six Bus suites, Module instance and Context. / 非完整 Debug 矩陣。 |
| Release SDK/App coverage | `sdk.system_apps`, `sdk.multi_app`, `sdk.installed_python`, `sdk.installed_consumer`, `sdk.installed_application` and `sdk.external_consumer` passed, as did all `app.*` tests. Covers source/installed entry, real subprocesses, relocation, inventories, Python/native operation, schema rebuild, ABI refusal and deployment cleanup. / 不包含 Python-OFF SDK producer。 |
| Release service coverage | OSAL **24/24**, Bus, Module/Python, Control, Storage, Concurrency, Param, Log, RPC, Shell, UI and Bag suites passed within the matrix above. / 不代表互動瀏覽器、其他架構或效能驗證。 |

**Runtime audit:** the 294 compile-command entries contain no Windows CRT flags
or separate `_python_runtime` variants. `otool -L` reports only system libc++ and
libSystem for `_native.cpython-312-darwin.so`; it has no `LC_RPATH`. Central `Vinc`
does not link a Python dylib. The built extension records deployment minimum 26.0;
this run does not establish support for an older macOS release.

**Validation boundary:** Debug built selected targets and passed eight Bus/Module
CTests; it did not validate the full Python/App/SDK matrix. The Release SDK producer
in this run has Python enabled. The separate producer-Python-OFF/downstream-ON
acceptance subsequently passed in the
[clean macOS SDK round](../../sdk/INSTALL.md#macos-clean-sdk-acceptance).
Other CPython minor versions, Intel Mac and Windows/Linux reruns of this snapshot
revision remain outside this evidence. Benchmarks were explicitly excluded
by the user; no latency or throughput comparison is claimed.

Evidence is under `build/macos-snapshot-mutex-20261102/`: build/CTest logs,
JUnit XML, dependency/load-command audits and `validation.json` with changed-source
SHA-256 hashes. Installed fixture details remain beneath the Release build's
`system-apps-test`, `multi-app-test`, `sdk-python-test` and other SDK test directories.

The first full Release run found a separate fixture issue in
`tests/services_tests.cpp`: macOS's temporary path traverses `/var` → `/private/var`,
so Log correctly rejected a symlink ancestor before Python service checks began.
The unchanged binary passed with `TMPDIR=/private/tmp`, confirming the path issue.
The fixture now canonicalizes the existing temporary root, matching the existing
Log test fixture. Production Log path validation and Python Runtime behavior were
not relaxed. The original failure and diagnostic rerun are retained in the logs.
The final original-environment run is `final-ctest.log` / `final-ctest.xml`;
`final-fixture-build.log` records the rebuild. No test timeout was relaxed.

**Clean SDK follow-up (2026-11-02):** a fresh Release producer with Python, Apps,
examples and tests disabled passed native SDK delivery, downstream Python/custom
messages, installed single-executable App and source/installed multi-App fixtures.
Commands, environment, runtime audit and validation limits are maintained in the
[SDK acceptance record](../../sdk/INSTALL.md#macos-clean-sdk-acceptance).
後續已補齊 Python-OFF producer、下游才啟用 Python 的交付流程；本段先前的
Python-enabled 結果仍保留原範圍，不把兩輪描述成同一次全套測試。

#### Historical rollout validation / 歷史導入驗證

The following stage records retain their original scope; references to pending
macOS validation describe those earlier runs. The native follow-up and remaining
Mac acceptance work are recorded above.

下列保留各導入階段當時的範圍；其中「macOS 待驗證」是歷史狀態，後續原生結果與
尚未完成的 Mac 驗收項目以上節為準。

Validation commands (MSVC developer environment, existing configured build with
`BUILD_TESTING=ON`):

```sh
cmake --build build --target vinc_check_python_runtime --config Release
ctest --test-dir build -C Release -R "^common.runtime_policy$" --output-on-failure
```

Use a matching Debug build for `/MTd`. These checks validate the CMake policy and
native link closure, not production Python/installed SDK. The earlier Python/WinRT
experiment is recorded separately below. Native macOS verification remains pending.

Stage-one validation (2026-11-02) / 第一階段驗證：

| Environment / 環境 | Evidence / 結果 |
| --- | --- |
| Windows x64, MSVC 19.44.35229, Ninja, Release and Debug | Built the real static Runtime/catalog closure and ran `vinc_check_python_runtime` in both configurations. Static images have no MSVC CRT DLL imports; native test Hosts retain dynamic CRT. / 兩種組態的靜態連結、匯入表及 bootstrap/catalog 探針通過，原生 Host 仍用動態 CRT。 |
| Same Windows builds / 同上 | Audited 20 static archives per configuration: all use `/MT` or `/MTd`. Compile commands contain 57 static and 250 native translation units with the expected separate CRT flags. Default native Release build did not build these extra archives. / 各 20 個靜態庫及編譯參數核對通過；預設原生建置不額外編譯它們。 |
| Windows Release regression / 回歸 | **67/67** related Common, OSAL, Concurrency, Storage, Control Channel, Bus, Module, Param, Log and RPC CTests passed (234.08 s). / 相關原生元件回歸全數通過。 |
| Windows CRT policy / 政策測試 | `common.runtime_policy` passed in Release and Debug: native/static execution, standalone declaration, default-build exclusion, indirect CRT mismatch, undeclared archive and invalid policy. / 兩組態正反向測試通過。 |
| Windows native SDK consumer / 原生 SDK 使用端 | Existing source-tree `sdk.consumer` passed; this is not installed-SDK static deployment validation. / 既有 source-tree 消費端通過，不代表 installed SDK 靜態交付已驗收。 |
| Standalone OSAL/Common, Windows Release | Built successfully without declaring Python variants. Encoding warnings are tracked in the [OSAL outstanding issues](../../osal/README.md#待處理事項). / 獨立建置成功，沒有額外 Python 相依；既有編碼警告另行記錄。 |
| WSL2 Ubuntu 24.04, GCC/libstdc++ 13.3.0, Release | Module Runtime built; Common time/toolchain/error/error-chain and Concurrency tests passed **6/6**. No static-Python variants were declared. / 原生建置及六項測試通過，未引入新的靜態 Runtime 政策。 |

Stage one did not repeat the earlier Python/WinRT coexistence experiment, switch
the production extension, or qualify installed-SDK static deployment. New native
macOS validation is still required for these CMake changes; previous macOS results
for unrelated functionality are not withdrawn.

第一階段未重跑先前的 Python／WinRT 實驗、未切換正式擴充，也未完成 SDK 靜態庫交付驗收。
這批 CMake 變更仍待 macOS 實機驗證，不撤銷既有其他功能的 macOS 驗證紀錄。

Stage-two validation (2026-11-02) / 第二階段驗證：

| Environment / 環境 | Evidence / 結果 |
| --- | --- |
| Windows 11 x64, MSVC 19.44.35229, official CPython 3.13.12 | Source binding built in Release `/MT` and Debug `/MTd`; Python lifecycle, messages, metadata, loader, Context services and RPC CTests passed **6/6 in each configuration** (26.54 s / 32.74 s). `PYTHONMALLOC=debug` and faulthandler enabled. / 兩種組態六組測試全過。 |
| Same Windows Python, WinRT 3.2.1 | Clean import, old DLL first, WinRT first, Vinc first, and WinRT-first lifecycle/services/RPC: **7/7** isolated cases passed. Both import orders ran real OCR and message operations. Python retained VCRUNTIME **14.42.34438.0**; WinRT-first retained MSVCP **14.29.30157.0**. No DLL replacement/unloading. / 舊 DLL 留在程序內，Vinc 靜態擴充仍正常運作。 |
| Windows x64, isolated CPython 3.11.18, same MSVC, Release | Built the matching cp311 extension; six CTests passed **6/6** (26.51 s), including old-DLL-first import. WinRT order test skipped because no cp311 WinRT fixture was supplied. / 最低 Python 版本回歸通過；未重跑 3.11 的實際 OCR。 |
| Windows artifacts / 產物 | Release/Debug each audited **20** static archives and **62 static / 255 native** compile-command entries. `_native.pyd` imports only its matching Python DLL and reviewed Windows APIs, with no direct MSVC CRT DLL import. Static metadata records the checked image's SHA-256. / 靜態與動態 CRT 編譯旗標分離；載入前核對產物。 |
| Windows native regression / 原生回歸 | `message.generated`, `module.plugin`, and source-tree `sdk.consumer`: **3/3** passed (8.77 s). / 既有原生訊息與 Plugin 接線通過。 |
| Dynamic-loader control / 動態路徑對照 | Current loader with the earlier `/MD` image and a compatible Python environment: **20 passed, 1 static-only skip**. Old DLL rejection remains effective. This is a loader regression, not new installed-SDK acceptance. / 保留 SDK 過渡期的動態防護，不以此宣稱 SDK 靜態交付完成。 |
| WSL2 Ubuntu 24.04, GCC/libstdc++ 13.3.0, CPython 3.12, Release | Built the normal shared extension; Python lifecycle/messages/services/RPC CTests **4/4** passed (14.69 s). No static-Python variants declared. / Linux 維持原系統 Runtime，功能回歸通過。 |

The Windows generator tests cover unreviewed imports and static metadata without
a Redistributable directory. Loader tests reject missing/malformed metadata,
mode mismatches, stale image hashes and mismatched image selection before native
import. Static mode never invokes dynamic CRT preparation. Tests keep existing
dynamic-mode rejection coverage. Shared-memory/message formats and C++ resource
ownership APIs are unchanged.
Deleting only the generated manifest in the isolated Release build and rebuilding
`vinc_python_native` regenerated static metadata; a subsequent native import passed.

Windows 測試涵蓋未知 DLL 相依、metadata 缺漏／格式錯誤、靜動態模式混用、產物雜湊
及選檔不符；皆在原生匯入前拒絕。靜態模式不載入或替換動態 CRT，原有動態模式
防護仍保留。共享記憶體格式與 C++ 資源所有權介面未改。
另已驗證隔離 Release build 刪除生成 manifest 後，重新建置會補回 metadata，匯入成功。

Evidence is under ignored `build/runtime-policy/`: `stage2-*-test.log`,
`stage2-release-test.log`, `stage2-audit.json`, `coexist.json`/`coexist-*.log`,
`dynamic-control.log`, `stage2-metadata-rebuild.log` and `linux-python.log`. These are local validation artifacts,
not required SDK files. Native macOS, installed-SDK static export/deployment and
full App/UI qualification remain outside this stage. The earlier Application
Verifier experiment was not repeated; passing these cases does not prove absence
of every possible CRT/third-party defect. Static CRT servicing requires rebuilding
and redeploying the extension; Python minor ABI matching remains required.

本輪未做 macOS 實機、installed SDK 靜態交付、完整 App／UI 或重新執行 Application
Verifier；不宣稱任意 Python／第三方套件都相容。靜態 CRT 修補需重新建置與部署，
Python minor ABI 配對限制不變。

The [isolated `/MT` experiment](MT_VALIDATION.md) records the 2026-11-02 static-CRT
dependency audit, old-Python/WinRT coexistence, ownership/lifecycle probes and
measured build/package cost. That historical experiment did not switch production;
the source-build switch is recorded here. Installed-SDK integration is recorded in
the [SDK validation](../../sdk/INSTALL.md#static-python-sdk-validation).
Final integration acceptance is recorded separately in the
[SDK integration results](../../sdk/INSTALL.md#runtime-final-acceptance).

[`/MT` 隔離實驗](MT_VALIDATION.md) 記錄舊 Python／WinRT 共存、所有權與生命週期驗證，
以及建置時間與大小成本。該實驗當時未切換正式接線；來源版切換記錄於本節，
installed SDK 實測見 [SDK 驗證](../../sdk/INSTALL.md#static-python-sdk-validation)；
後續整合驗收另見上述 SDK 紀錄，不以舊實驗代替新接線驗收。

The following diagnosis describes the earlier dynamic-CRT implementation,
not the source/installed static extension above.

以下診斷是先前動態 CRT 實作的紀錄，不是上方來源／安裝版靜態擴充的限制。

The [2026-11-02 Runtime diagnosis](RUNTIME_VALIDATION.md) separates the clean
official-Python refusal from WinRT 3.2.1's old-MSVCP load order. Official 3.13.12
is refused even by the 19.44 binding; official 3.14.10 with VINC first passes
actual OCR and message operations. The follow-up in the same report replaces the
header-derived floor with actual per-DLL deployment versions; old-DLL rejection,
dynamic CRT linkage and the lack of a WinRT 4.x requirement remain.

[2026-11-02 Runtime 診斷](RUNTIME_VALIDATION.md) 已分離官方 Python 原附 DLL 的門檻拒絕，
與 WinRT 3.2.1 舊 MSVCP 的載入順序問題。乾淨 3.13.12 連 19.44 綁定也會拒絕；
乾淨 3.14.10 的 VINC-first 實際 OCR／訊息操作通過。同份報告的後續修正改用逐 DLL
實際部署版本；保留舊 DLL 防護及動態 CRT，不要求 WinRT 4.x。

The [Windows toolchain evaluation](../../sdk/WINDOWS_TOOLCHAIN.md) separates
source/compiler support from Python ABI and Runtime deployment. Its historical
header-floor and bypass experiments are not the current deployment policy.
The dynamic 19.44 deployment baseline still rejects Python's 14.42.34438 VCRUNTIME;
passing limited probes with older files is not a compatibility guarantee.

[Windows 工具鏈評估](../../sdk/WINDOWS_TOOLCHAIN.md) 區分來源建置、Python ABI 與 Runtime
部署問題。其 Header 門檻及繞過防護實驗屬歷史紀錄；保留的動態防護使用上述部署基準，
不是目前 Windows 靜態 binding 的 DLL 門檻。
動態 19.44 基準仍拒絕 Python 的 14.42.34438 VCRUNTIME，有限探針通過不等於相容性保證。

The exact native crash site from the external 2026-10-14 v0.1.1/MSVC 19.44/WinRT-first old-MSVCP report remains unknown without stack/dump; the UNC report was inaccessible. External tests showed new-first coexistence/OCR; isolated official wheels locally verify preflight rejection and reverse-order coexistence. [Microsoft compatibility](https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017) requires Runtime at least as new as toolchain; [STL 17.10 notes](https://github.com/microsoft/STL/wiki/VS-2022-Changelog#vs-2022-1710) document mutex null dereference from violating it. pybind11 mutex initialization is a concrete candidate, not a confirmed function diagnosis. Rejecting unsupported Runtime does not require proving the precise site. No CRT switch/compatibility macros/DLL replacements were introduced.

- 2026-10-14 外部問題的具體原生崩潰位置仍待確認：v0.1.1（8faee632）、MSVC 19.44 擴充
  在 WinRT 3.2.1 先帶入 MSVCP140.dll 14.29.30157 後 import 時發生 0xC0000005。
  外部報告確認 Vinc／新版 Runtime 先載入可通過 native tests 與 OCR；本機已下載官方 wheel
  驗證新防護對舊 DLL 先載入的拒絕，以及反向順序的共存。UNC 報告仍無法存取，尚無
  native stack／dump，不能認定封包解碼或特定 mutex 已確認有錯。
  [Microsoft 相容性要求](https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017)
  規定 Runtime 不得早於所用工具鏈；[STL 17.10 紀錄](https://github.com/microsoft/STL/wiki/VS-2022-Changelog#vs-2022-1710)
  亦說明違反版本要求可能造成 mutex null dereference。固定 pybind11 的初始化路徑使用
  std::mutex，因此屬具體候選原因，仍須原生堆疊確認。當時已加入上述載入前拒絕與預載，
  不需要先證實是哪個 C++ 函式才能拒絕不支援的 Runtime。沒有 CRT 切換、相容巨集或第三方 DLL 替換。

<a id="publisher-identity-regression"></a>

Resolved test defect (2026-11-02): `tests/services_module.py` formerly asserted
Publisher ID != Module MID, although these are separate namespaces. On Windows,
the unmodified assertion was reproduced failing with reply Publisher ID 2 and
receiving Python MID 2. Production identity allocation and Runtime are unchanged.

`module.python_services` now obtains the expected reply Publisher ID from paged
Bus diagnostics through the native fixture's test-only command hook. It checks
topic, publisher name and Publisher ID, including copied messages and `get_last()`.
The six scenarios cover coincident IDs, distinct IDs, intentionally wrong expected
identity, and the existing Param/Bus/reentrant-pump callback failures. The equal-ID
scenario runs first and explicitly verifies equality, so fixture drift cannot
silently remove this regression coverage. Wrong identity must fail the Python
Module while the native peer remains Running; shutdown and reference release are
also checked.

Validation: Release `module.python_services` passed all six scenarios on Windows
x64 / MSVC 19.44.35229 / CPython 3.13.12 and WSL Ubuntu 24.04 x86_64 /
GCC 13.3 / CPython 3.12.3, with `PYTHONMALLOC=debug` and `PYTHONFAULTHANDLER=1`.
This change has not been rerun on macOS. Reproduce with
`ctest --test-dir <build> -V -R "^module[.]python_services$"`.

- 已修正（2026-11-02）：舊測試錯把 Publisher ID 與 Module MID 當成必須不同的數值。
  Windows 實際製造兩者皆為 2 時，確認舊斷言會誤判；正式身分配置與 Runtime 未更動。
  現在透過測試原生 Module 的命令 hook，從 Bus 分頁診斷取得預期 Publisher ID，
  再驗證收到的 topic、發布者名稱、ID，以及 copy／get_last 的身分保留。
  六個案例涵蓋同號、異號、故意錯誤的預期身分及既有三種 callback 故障。
  同號案例明確檢查測試前提；錯誤身分仍會使 Python Module 失敗，原生 Module 保持存活。
  上述 Windows／WSL Release 環境皆通過，包含正常收尾與參考釋放；本輪未重跑 macOS。

<a id="python-rpc2026-10-14"></a>

## Python RPC（2026-10-14） / Python RPC (2026-10-14)

Context register_rpc/call_rpc/rpc_stats share native Host/shared-memory protocols. Immediate admission raises; completion gets reply/error: (RpcStatus, Response)/None or None/VincError. Handlers return RpcStatus, completions None. Pump runs callbacks; escaping exceptions retain diagnostics/fault the Module.

Context 現在提供 register_rpc、call_rpc、rpc_stats，與 C++ 共用 Host／共享記憶體協定。
同步 admission 失敗拋 VincError，非同步完成使用 completed(reply, error)；框架失敗時 error
是 VincError，成功 reply 為 (RpcStatus, Response)。Handler 回傳 RpcStatus，completion 回傳 None。
Callback 都在 pump 執行；未處理 Python 例外保留診斷並使該 Module 失敗。

See [RPC](../../rpc/README.md) and [comparison examples](../../../../examples/rpc/README.md).

API 與限制見 [RPC](../../rpc/README.md)，對照範例見
[examples/rpc](../../../../examples/rpc/README.md)。

## Notification bootstrap update / 通知啟動接線更新

The Python native adapter uses the same **v16** bootstrap and Host notification
lifetime as the C++ ModuleHost. Central creates the endpoint; Runtime opens both
notification handles before importing the user's Module source. Failure rejects
that Module's startup and preserves the OSAL cause. Rebuild the native adapter
with central/Host. No Python API or extra thread is added. pumpLoop now uses the same Host notification wait and 10 ms fallback as C++; see [Pump wakeup](../PUMP_WAKEUP.md).
See [Module notification ownership](../INSTANCE.md#notification-ownership-2026-10-28--通知資源所有權).

Python 原生接線共用 v16 bootstrap；使用者 Module 載入前先開啟中央建立的通知資源，
失敗只拒絕該 Module 啟動。須與中央／Host 一起重建；Python API 不變；pumpLoop 共用 Host 通知等待及 10 ms 補查，不增加執行緒。
