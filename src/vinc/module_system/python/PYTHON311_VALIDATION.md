# CPython 3.11 compatibility evaluation / CPython 3.11 相容性驗證

## Scope / 範圍

The initial evaluation checked **CPython 3.11 as a candidate minimum** using the existing
bindings and tests. The owner selected upstream-maintained Python branches;
that first run did not change the CMake minimum, adopt the Limited API/Stable ABI,
relax Windows Runtime preflight, or bundle an interpreter with VINC.

最初以 **CPython 3.11 作為最低版本候選**，驗證既有綁定與功能。使用者已決定不維護
Python 官方停止支援的分支；當時未修改 CMake 最低版本、不導入 Stable ABI、
不放寬 Windows Runtime 防護，也不將直譯器納入 VINC 發行內容。

The initial CMake dependency floor was 3.9, not a tested-support matrix. The
subsequently authorized minimum-policy change is recorded below; the current
contract lives in the [Common toolchain policy](../../common/README.md#build-toolchain-policy).
A `cp311` extension does **not** establish that one
binary can run on every Python version from 3.11 onward.

初次驗證時 CMake 的 3.9 是相依門檻，不是完整支援矩陣；後續獲授權的門檻更新另記於下方，
目前契約以 Common 工具鏈規則為準。
本次產物是 `cp311` 擴充，**不代表同一份二進位可直接用於所有 3.11 以上版本**。

## Environment and provenance / 環境與來源

| Item / 項目 | Value / 內容 |
| --- | --- |
| Validation date / 日期 | 2026-11-01 |
| Platform / 平台 | Windows 11 x64, build 26200 |
| VINC source / 原始碼 | `fd01072bc4f98dc1ec07fe4186e113339fa1333e` plus the current uncommitted toolchain/System Module work; not a release-tag validation |
| VINC compiler / 編譯器 | MSVC **19.44.35229**, toolset 14.44.35207, Release |
| CPython | **3.11.18**, x64, GIL-enabled, built from the official source archive |
| CPython compiler / 直譯器編譯器 | MSVC **19.51.36257**, MSBuild v145, Release; distinct from the VINC compiler |
| CMake / Ninja | 4.3.2 / 1.13.2 |
| Windows SDK | 10.0.26100.0 |
| pybind11 | 3.1.0 |
| Binding output / 綁定產物 | `_native.cp311-win_amd64.pyd` |
| Build orchestration / 建置工具直譯器 | Existing CPython 3.13.12; Module tests and bindings use the isolated 3.11.18 |

[CPython 3.11.18](https://www.python.org/downloads/release/python-31118/) is a
source-only security release. The 3.11 branch is scheduled for security support
until October 2027; the last upstream Windows installer was 3.11.9. See the
[Python version status](https://devguide.python.org/versions/). This run uses the
current security source, not an old installer or an unofficial binary download.

3.11.18 是只提供原始碼的安全更新；3.11 預計維護至 2027 年 10 月，最後一版官方
Windows 安裝包為 3.11.9。本次自行建置目前安全更新，並非舊安裝包的部署驗證。

- Archive: `https://www.python.org/ftp/python/3.11.18/Python-3.11.18.tar.xz`
- Verified SHA-256: `9120e0a653868f6e3698d36cdd99e5a58903ec9469e448f21d98f7b16d8fa290`
- Unmodified upstream `PCbuild/get_externals.bat --no-tkinter`, then Release
  `pcbuild.proj` with `PlatformToolset=v145`, `IncludeExternals=true`,
  `IncludeCTypes=true`, `IncludeSSL=true`, `IncludeTkinter=false`,
  `IncludeTests=false`, and `KillPython=false`.
- Upstream `PC/layout` supplies the isolated interpreter, headers, import
  libraries and venv support. Import checks for `ssl`, `ctypes`, `sqlite3` and
  `sysconfig` pass. This is not a full CPython standard-library test run.

The available MSBuild installation provides v145; upstream warns that this is
not its official binary build toolchain. CPython's generated `sys.version` also
contains the enclosing VINC Git label because the source tarball was built
inside this repository. **That label is not a CPython revision**; the verified
source archive hash above identifies the interpreter source.

本機 MSBuild 提供 v145，CPython 上游會提示它不是官方二進位的建置工具鏈。
另因原始碼解壓在 VINC 儲存庫內，`sys.version` 的 Git 標籤誤帶外層 VINC 資訊，
**不能當成 CPython commit**；直譯器來源以以上已核對的封裝雜湊識別。

### Runtime actually loaded / 實際載入的 Runtime

After an ordinary `import vinc`, without guard bypass:

| DLL | Version | Location / 位置 |
| --- | --- | --- |
| VCRUNTIME140.dll | 14.51.36247.0 | Isolated CPython directory / 隔離直譯器目錄 |
| VCRUNTIME140_1.dll | 14.51.36247.0 | Windows System32 |
| MSVCP140.dll | 14.51.36247.0 | Windows System32 |
| MSVCP140_atomic_wait.dll | 14.51.36247.0 | Windows System32 |
| ucrtbase.dll | 10.0.26100.9711 | Windows System32 |

The unchanged extension metadata requires a common floor of **14.44.35220.0**.
This environment passes that check. It does not establish compatibility with
older private Runtime DLLs shipped by other Python installations or packages.
No installed Python, system DLL, global PATH or registry entry was modified.
See [Windows toolchain evaluation](../../sdk/WINDOWS_TOOLCHAIN.md) for the
separate, unresolved Runtime deployment policy.

原生擴充沿用 **14.44.35220.0** 的共同檢查門檻；此環境符合要求。
這不代表其他 Python 安裝包或套件附帶的舊 DLL 也能通過。
未更動既有 Python、系統 DLL、全域 PATH 或登錄設定；舊 Runtime 部署政策仍是獨立問題。

## Initial evaluation results / 初次評估結果

The complete Release build passed. Six Python-related CTest suites passed:

| Suite | Coverage / 範圍 | Result |
| --- | --- | --- |
| `module.python` | Startup/shutdown, Manual/FrameworkLoop, fault/retry, hung import/hook, parent loss, preflight failure isolation | PASS |
| `module.python_messages` | Generated messages, nested arrays, bounds/types, serialization, lifetime | PASS |
| `module.python_runtime` | Runtime preflight, normal import, malformed metadata, actual old-DLL rejection | PASS; 12 internal cases passed, 1 skipped |
| `module.python_services` | C++/Python Bus, loans, Param snapshots/callbacks, Log, callback fault isolation | PASS |
| `rpc.python` | Typed C++/Python request/response through Context | PASS |
| `module.python_shell` | Discovery, Shell hooks, lifecycle through the framework App | PASS |

The skipped Runtime case requires a CPython 3.11 WinRT wheel fixture, which was
not supplied. The real old-DLL case did run, using isolated MSVCP140 14.42.34438:
VINC rejected it before loading `_native`. A passing suite does not turn its
skipped case into a pass. Total CTest duration: 23.82 seconds.

完整 Release 建置及六組 Python 測試通過。Runtime 子案例共 13 項，其中 12 通過、
1 因沒有 CPython 3.11 對應 WinRT wheel 而略過。真實舊 DLL 拒絕案例有執行，
確認在 `_native` 載入前拒絕隔離的 MSVCP140 14.42.34438。六組 CTest 合計 23.82 秒。

`sdk.multi_app` passed in **357.31 seconds**. Coverage includes shared native
bindings, Python-enabled/disabled App selection, real Module execution,
installation/relocation, concurrent Apps, same-name exclusion, System Module
startup failure isolation, resource repair and selection cleanup.

`sdk.multi_app` 通過（357.31 秒），涵蓋共用綁定、各 App 的 Python 選用、實際 Module
執行、安裝與搬移、不同 App 並行／同名排他、System Module 啟動故障隔離、資源修復及清理。

`sdk.installed_python` passed in **346.10 seconds**. It installs and relocates
the SDK/App, hides the original SDK/build paths, exercises custom-message
Bus/RPC/Param, adds/removes/fixes message definitions, rejects mismatched Python
ABI metadata while keeping central/native Modules alive, repairs deleted
resources, and cleans managed Python files without deleting user-owned files.

`sdk.installed_python` 通過（346.10 秒），涵蓋 SDK／App 搬移且隱藏原建置路徑後執行、
自訂訊息的 Bus／RPC／Param、訊息新增／刪除／錯誤修復、Python ABI 拒絕時的故障隔離、
部署修復，以及停用 Python 後保留使用者檔案的清理行為。

**All eight selected CTest suites passed.** The two SDK suites took 703.43 seconds
combined. A post-test process inventory found no remaining executable from the
isolated acceptance directory. No framework implementation change was needed
for these 3.11 checks. The result supports the candidate minimum in this
environment, subject to the Runtime and platform limits above and below.

**八組選定的 CTest 均通過**；兩組 SDK 驗收合計 703.43 秒。結束後檢查沒有殘留從本輪
隔離目錄啟動的程序。此次不需修改框架實作即可通過 3.11 驗證；結論限定於所記錄的
環境、Runtime 與平台範圍，不是所有安裝環境或所有較新 Python 版本的保證。

## Initial evaluation reproduction and limits / 初次評估重現與限制

Use a complete x64 MSVC 19.44 developer environment and a compatible isolated
CPython 3.11 installation with development headers/libraries. Build tooling may
use another Python; `Python_EXECUTABLE` chooses the native binding ABI.

```powershell
$vincPython = 'C:/path/to/isolated-python311/python.exe'
py -3 build.py --config Release --build-dir build/python311-release --jobs 6 `
    -DVINC_BUILD_PYTHON=ON -D "Python_EXECUTABLE=$vincPython" `
    -DBUILD_TESTING=ON -DVINC_INSTALL_SDK=ON `
    -D "VINC_TEST_SDK_PYTHON_EXECUTABLE=$vincPython"
ctest --test-dir build/python311-release -C Release --output-on-failure `
    -R '^(module[.]python.*|rpc[.]python|sdk[.]installed_python|sdk[.]multi_app)$'
```

Optional old-Runtime/WinRT fixtures use the existing
`VINC_TEST_OLD_MSVC_RUNTIME` and `VINC_TEST_WINRT_DIR` environment variables.
Without them, those individual tests explicitly skip. Do not replace DLLs in an
installed interpreter to reproduce this run.

Local ignored artifacts are in `build/python311-compat/`: `prepare.py`, `run.py`,
the official archive/source and isolated layout, `cpython-dependencies.log`,
`cpython-build.log`, `layout.log`, `vinc-build.log`, `vinc-tests.log`,
`python-tests-detail.log`, `vinc-sdk.log`, `sdk-tests-detail.log` and
`environment.json`. These are disposable investigation artifacts, not new
supported setup entry points. No new Debug, Linux, macOS, free-threading,
other-minor or cross-minor-binary validation was performed. The complete C++
CTest suite was not rerun.

本機下載、建置、原始 log 與環境快照留在上述 Git 忽略目錄。它們是可重建的驗證產物，
不是新增的正式安裝工具。本次未驗證 Debug、Linux、macOS、free-threading、
其他 Python minor，或一份擴充跨多個 minor 使用；未重跑全部 C++ CTest。

## Minimum-policy rollout (2026-11-01–02) / 最低版本規則落實

The subsequent authorized change adopts **Python 3.11, Python 3 only** for build
entry points, Kconfig and native bindings. One source policy is delivered with
the installed SDK. Missing tools point to ordinary setup; incompatible versions
report the requirement, selected version and executable. Build never installs or
upgrades tools. `--recommended` remains a future task and is not advertised as an
available command. Exact binding ABI checks and Windows Runtime protection remain.

後續獲授權將建置入口、Kconfig 與原生綁定的最低版本統一為 **Python 3.11，限 Python 3**，
並將同一份版本規則交付給安裝式 SDK。缺少工具提示一般 setup；版本不符列出需求、
實際版本與路徑，建置不自動安裝或升級。`--recommended` 留待後續實作，不提示使用者
執行尚不存在的選項；精確 ABI 比對及 Windows Runtime 防護均保留。

Environment checks no longer depend on a known Linux installation recipe.
Automatic installation still requires one. `--no-python` skips Module development
files, not the build interpreter. Default distribution packages may still be too
old; setup reports that outcome without replacing system Python/alternatives.

環境檢查與 Linux 安裝配方分開：現有工具可先檢查，缺少工具且沒有適用配方時明確失敗。
`--no-python` 僅略過 Module 開發檔，仍檢查建置直譯器。系統來源的套件可能仍低於門檻，
此時明確回報，不替換系統 Python 或 alternatives。

| Validation / 驗證 | Result / 結果 |
| --- | --- |
| Windows build / 建置 | Full Release incremental build passed with MSVC 19.44.35229 and CPython 3.11.18 for build.py, Kconfig and bindings. / 完整 Release 增量建置通過，本輪建置腳本、設定與綁定均使用 3.11.18。 |
| Python runtime suites / 執行測試 | Six CTest suites passed in 24.80 seconds. Runtime still has 12 passing internal cases and one missing-WinRT-fixture skip; actual old-DLL refusal ran. / 六組通過；Runtime 子案例 12 通過、1 因缺 WinRT fixture 略過，真實舊 DLL 拒絕有執行。 |
| Tools unit tests / 工具測試 | Windows/CPython 3.11.18 and WSL Ubuntu 24.04/CPython 3.12.3: each ran 92 cases, 74 passed and 18 platform/privilege/opt-in-integration skips. / 兩環境各 92 項，74 通過、18 項因平台／權限或未啟用選用整合測試而略過。 |
| Kconfig tests / 設定測試 | 15/15 passed on both Windows 3.11 and WSL 3.12. / 兩平台各 15 項全通過。 |
| Python SDK integration / Python SDK 整合 | `sdk.multi_app` passed in 464.02 seconds; `sdk.installed_python` passed in 475.12 seconds. The latter checks relocation, custom messages/Bus/RPC/Param, ABI failure isolation, resource repair and cleanup preserving user files. / 多 App 與安裝式 Python 全通過，涵蓋搬移、自訂訊息／Bus／RPC／Param、ABI 故障隔離、修復及保留使用者檔案的清理。 |
| Installed C++ SDK / 安裝式 C++ SDK | `sdk.installed_consumer` passed in 60.07 seconds, including relocation, message generation, execution and rejection cases. / 搬移、訊息生成、實際執行及不相容拒絕案例通過。 |
| Real setup and entry points / 實際入口 | Windows `setup --check --no-ui` passed with isolated 3.11 and the detected MSVC 19.51 toolchain; `build.bat --help` passed with 3.11. WSL interpreter-only setup probe and `build.sh --help` passed with 3.12. / 實際檢查成功，未安裝系統套件。 |

Two test findings were fixed: the Shell ABI-mismatch fixture now preserves all
generated metadata while changing only its ABI version; setup's exact cleanup
list now includes its own `msvc-version.c` probe. The latter was reproduced by a
real Windows check and covered by a cleanup regression case; no recursive cleanup
was introduced.

驗證時修正兩處：Shell 的 ABI 不匹配測試保留完整生成資訊，只覆寫要測的 ABI；
setup 的精確清理清單補上自身建立的 `msvc-version.c`。後者由真實 Windows 檢查重現，
並加入清理回歸案例，沒有改成遞迴刪除。

Old-version refusal tests simulate version discovery; they do not constitute a
run on an installed CPython 3.10. WSL validates tooling with 3.12, not Linux native
bindings built for 3.11. This round does not add macOS, Debug, Ubuntu 18.04,
free-threading or cross-minor binary validation. No system interpreter, DLL or
global environment setting was changed. Local logs use the `minimum-*` prefix
in the same ignored acceptance directory; SDK/build logs retain their names above.

舊版本拒絕案例使用版本探測替身，不代表實際安裝並執行 Python 3.10。WSL 驗證的是
3.12 工具流程，不是 Linux 3.11 原生綁定；本輪未新增 macOS、Debug、Ubuntu 18.04、
free-threading 或跨 minor 二進位驗證。未修改系統直譯器、DLL 或全域環境設定。
本輪工具 log 使用同一忽略目錄下的 `minimum-*` 前綴，SDK／建置 log 沿用上述名稱。
