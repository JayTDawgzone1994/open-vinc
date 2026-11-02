# Application configuration / 應用功能設定

All source and installed Apps use `vinc_add_app()` and an explicit `VINC_BUILD_CONFIG`.
The bundled App uses `apps/vinc/vinc.conf`; no global CMake feature/identity override is read.

內建、來源整合與安裝式 App 統一使用下方流程；Kconfig 管組成，建構器明確指定身份與設定檔。

## Module Python selection / Module Python 選擇

Kconfig is the only public build-time interpreter selection. Both the built-in
Vinc and `vinc_add_app()` use it. Enable Python, then choose System (default) or
Custom in the text file, GUI or menuconfig:

```ini
CONFIG_VINC_APP_PYTHON=y
CONFIG_VINC_PYTHON_CUSTOM=y
CONFIG_VINC_PYTHON_EXECUTABLE="D:/Python313/python.exe"
```

For System, use `CONFIG_VINC_PYTHON_SYSTEM=y` instead of Custom/path. The first
eligible `python.exe` (Windows) or `python3` (Linux/macOS) in the current PATH is selected,
including an activated venv. No registry search or fallback to another version
is made. Custom requires an absolute executable path; Kconfig strings escape
backslashes as `\\` and quotes as `\"`. Editors accept a not-yet-installed path;
configuration checks the actual CPython, version, ABI and development files.

Both searches skip missing/non-file candidates and POSIX files without execute
permission. Empty PATH entries are ignored; explicit relative entries are resolved
against that process's current working directory. Venv symlink spelling is retained.
Once selected, a launch/probe/version/ABI failure is reported without trying another
candidate. CMake uses local CMP0109 NEW and disables bundle/root-path redirection
for this host-interpreter search; it does not change the parent's search settings.
See [runtime and validation](../../src/vinc/module_system/python/README.md#system-python-path-consistency).

建置與執行都跳過不存在、非一般檔案及 POSIX 無執行權限的候選；忽略 PATH 空項目，
明確的相對目錄依各自程序工作目錄解析。選定後若啟動／版本／ABI 失敗，不繼續搜尋。
此處統一的是選擇規則，並不要求建置機與部署機的 PATH 或實際路徑相同。

The App embeds only Custom's path. System resolves the deployment PATH, not a
saved build-machine location. `--python PATH` overrides either mode for that
invocation. A missing/incompatible runtime fails the affected Python Module;
central and other Modules remain available. The binding still requires matching
CPython minor/platform/bitness/GIL ABI; this is not cross-minor compatibility.

Enabled Apps in one build must resolve to the same interpreter; System and Custom
may coexist when they do. Different environments require separate build directories.
Venv symlinks are not collapsed to their base executable. Reconfiguration rejects
changes to the selected interpreter or ABI without deleting anything. A serviced
patch version with the same path and ABI is allowed.

`Python_EXECUTABLE` is not a VINC selection input. `build.py` forwards it like any
other permitted `-D` definition; VINC neither rejects nor deletes it. Other CMake
consumers may use it. VINC passes its Kconfig selection to FindPython locally;
CMake decides whether an unused command-line definition warrants a warning.
Only VINC's own identity/ABI record guards reconfiguration. A legacy VINC-generated
Python package without that record requires another build directory: shared
FindPython cache entries cannot establish which interpreter built it. No files
are automatically cleaned. `Python3_EXECUTABLE` and the Python running `build.py` select
configuration/build tools only. Old setup `module_python` records are ignored;
setup no longer saves one. There is no automatic interpreter installation.

Kconfig 是 Module Python 唯一的公開建置設定來源；內建 Vinc 與多 App 共用。
預設 System 按目前 PATH 選第一個 Python，包含 venv，不查登錄檔、不改找其他版本。
Custom 僅接受絕對路徑，建置時檢查 CPython／ABI／開發檔；App 也以該路徑為執行預設。
System 在部署環境重新依 PATH 尋找；`--python` 可覆寫本次執行，未更動設定檔。
執行環境不合只拒絕該 Python Module，不關閉中央。多 App 必須選到同一直譯器；
切換環境／ABI 需另一個 build 目錄，不自動清除。`Python_EXECUTABLE` 不再選擇 VINC 的
Module Python；`build.py` 如一般 `-D` 原樣轉交，不拒絕、不刪除，其他 CMake 元件可以使用。
VINC 僅在函式內將 Kconfig 選擇交給 FindPython。未使用參數是否警告由 CMake 判斷，不保證必有警告。
快取保護依 VINC 自己的身份／ABI 紀錄；若舊目錄已有 VINC Python 生成套件卻沒有該紀錄，
要求另用建置目錄，不以通用 FindPython 快取猜測原環境。設定工具的 `Python3_EXECUTABLE` 保留。
setup 的舊 `module_python` 不再生效，也不再保存這項預設。

Generated strings use literal CMake arguments and escaped UTF-8 C++ bytes; paths
are data, not shell commands, CMake expressions or generator expressions. Disabled
Python/Custom choices do not retain an active runtime path. Upstream editor choice
changes are supported without treating their previous selection as a conflict.
Control characters and CMake's `$<` generator-expression introducer are rejected
in executable paths before they reach the build graph.

生成路徑只當資料處理，不執行其中的 CMake／shell 表達式；關閉 Python 或切回 System
不會留下有效的 Custom 預設。文字與編輯器共用驗證，明確衝突的選擇會拒絕。
執行檔路徑不得包含控制字元或 `$<`，避免被後續建置生成器解讀。

### Selection validation (2026-11-02) / 選擇接線驗證

#### CMake consumer coexistence correction / CMake 使用端共存修正

The blanket `Python_EXECUTABLE` rejection and the inference of VINC ownership
from FindPython's private cache were removed. On Windows x64, MSVC 19.44,
CPython 3.13.12, CMake 4.3.2 and Release, the source build passed with a nonexistent
external `-DPython_EXECUTABLE` while Kconfig selected the actual binding interpreter.
Selected Python/configuration CTest: **8/8**. Selection/cache tests: **13/13** on
Windows and **13/13** on WSL Ubuntu 24.04, CPython 3.12.3/CMake 3.28.3. Windows
build/setup/profile tests: **117**, with 22 platform skips and no failures.

The Windows installed-SDK fixture now finds a separate venv before `find_package(vinc)`;
the caller's variable and interpreter target remain unchanged, while VINC builds
with its Kconfig interpreter. Relocation, custom-message Bus/RPC/Param, incremental
schema changes, ABI failure isolation, deployment repair and disable cleanup passed.
Legacy generated packages without VINC identity records are tested as explicit
new-build-directory failures. No native macOS, Debug, or full WSL SDK rerun is claimed
for this correction. Local logs are under ignored `build/python-input-review/`.

已移除全面拒絕與共用 FindPython 快取的歸屬誤判。Windows Release 來源建置帶入不存在的
外部 `Python_EXECUTABLE` 仍成功，實際 binding 依 Kconfig 選擇；相關 CTest 8/8 通過。
Windows／WSL 選擇與快取測試各 13/13；Windows 工具測試 117 項、22 項平台跳過，無失敗。
安裝式 SDK 已實測另一個 venv 先執行 FindPython，保留對方變數與 interpreter target，
並通過搬移、自訂訊息通訊、增量更新、ABI 隔離、補檔與停用清理。舊 VINC 生成套件缺少
身份紀錄時明確要求新目錄。本次未重跑 macOS、Debug 或 WSL 完整 SDK 驗收。

#### Initial Kconfig rollout / 初次 Kconfig 接線

Windows x64 uses MSVC 19.44 (toolset 14.44.35207), CMake 4.3.2, Ninja 1.13.2,
official GIL-enabled CPython 3.13.12 and Release. WSL Ubuntu 24.04 x86-64 uses
GCC/libstdc++ 13.3, CMake 3.28.3, Ninja 1.11.1 and CPython 3.12.3, Release.

- Both platforms: 20 parser/editor-save/UTF-8/literal-path cases and 9 real CMake
  selection/cache cases passed. Version-floor and missing-development diagnostics
  use a deliberately raised test policy and a FindPython failure double; these
  are not claims of running an old interpreter or removing system headers.
- Windows: source build passed; Python/Context/messages/Runtime/Shell/App and
  configuration CTest 10/10 passed. Build/setup/profile tools: 117 tests,
  22 platform-inapplicable skips, no failures. PowerShell setup syntax passed.
- WSL: source build passed; final selected CTest 11/11 passed, including installed
  Python SDK, multi-App, Python/services/messages, configuration, Shell and App entry.
- Windows installed SDK: fresh consumer, custom-message Bus/RPC/Param flow,
  relocation, schema add/edit/remove, ABI rejection, missing `--python` override,
  central/native survival, repair and Python-disable cleanup passed. Normal flows
  now omit `--python` to verify the Kconfig default.
- Windows multi-App: System/Custom shared a venv under a Chinese/space-containing
  path. Three-App execution, same-name exclusion, concurrent isolation, relocation,
  failure isolation, resource repair and selection cleanup passed.

兩平台解析／編輯器保存測試各 20 項、CMake 選擇／快取各 9 項通過。Windows 來源建置與
相關 CTest 10/10、WSL 來源建置與最終選定 CTest 11/11 通過（含 installed SDK、多 App、
Python／服務／訊息、設定、Shell 與 App 入口）。Windows 安裝式測試已確認搬移、
自訂訊息、Bus／RPC／Param、失敗隔離與清理；正常流程不帶 `--python`，直接驗證設定預設。
Windows 多 App 測試另以中文／空白路徑的 venv 驗證 System／Custom 共用、三 App 執行、
同名排他、並行隔離、搬移、故障隔離、資源修復及選擇清理，全部通過。
最低版本／缺開發檔的診斷使用刻意調高的測試門檻及 FindPython 替身，並非實際移除系統套件。

Reproduce the fast checks with:

```sh
python -B tools/configure/tests/test_configuration.py
python -B tools/configure/tests/test_python_selection.py
ctest --test-dir <build> --output-on-failure -R '^sdk\.(configuration|python_selection|app_selection)$'
```

Installed runtime checks are `sdk.installed_python` and `sdk.multi_app` in an SDK
producer configured with `BUILD_TESTING=ON` and the test-only
`VINC_TEST_SDK_PYTHON_EXECUTABLE`. That variable selects the fixtures' Kconfig
content; it is not an App setting. No native macOS, Debug rerun or manual
GUI/curses visual acceptance is claimed here; macOS handoff is in root TODO.

安裝式測試的直譯器參數只用來產生測試 Kconfig，不是新的 App 設定入口。
本輪不宣稱 macOS、Debug 重測或人工 GUI／curses 畫面驗收；macOS 已列入根目錄 TODO。

<a id="stage-1-module-selection"></a>

## Stage 1 Module selection / 第一階段 Module 選擇

`vinc_register_app_configuration()` is an internal CMake composition helper, not
a new user-owned App constructor. It captures an existing target's explicit config
path, tool path and SDK capabilities. At outermost configure completion,
`vinc_resolve_app_configuration()` sees late/subdirectory Module registrations,
sets `VINC_APP_MODULE_TARGETS`/`VINC_APP_PYTHON`, and adds selected dependencies.
The App composer consumes these properties for each catalog. Per-App deployment is implemented in stage 3; see the Multi-App plan.
An internal read-only `--preflight` pass resolves Python before deferred execution:
CMake cannot add pybind11 subdirectories during that phase. This pass temporarily
accepts Module symbols from the file, but does not validate membership. The final
registered manifest is mandatory and rejects unknown Modules before generation.

`vinc_register_app_configuration()` 是內部 CMake 組裝 helper，保存既有 target 的明確設定檔、
工具路徑及 SDK 能力，不是供使用者自行建立 App 的新入口。最外層 configure 結束後，
解析器包含晚登記／子目錄 Module，寫入選擇 properties 並建立相依；App 組裝器使用結果
產生各自 catalog，第三階段已接上各 App 獨立部署。內部唯讀 `--preflight` 先解析 Python，因 CMake
不允許在 deferred 階段加入 pybind11 子目錄。此預讀暫時接受檔案中的 Module 符號，
不代表種類已有效；最終仍必須使用實際登記清單驗證，未知 Module 會阻止生成。

Per-App files live at `<build>/vinc-config/<App>/`: `modules.json` lists available
custom ModuleTypes, `.config` saves effective choices and `features.cmake` passes
the result to CMake. These are generated files, not user configuration. Module
symbols are `CONFIG_VINC_MODULE_<exact ModuleType>` (case-sensitive, default `n`),
independent of target naming or registration order. Duplicate types and App output
directory case collisions fail. Removed/unknown selections fail rather than vanish.
Official Modules are automatically selected from framework registration metadata;
this schema exposes only Python and custom Module switches. UI/Bag capabilities
are required. It does not accept the old `CONFIG_VINC_APP_UI/BAG` switches.

每個 App 的生成檔放在 `<build>/vinc-config/<App>/`，與使用者設定分離。
選項保留 ModuleType 原始大小寫，不依 target 名稱或登記順序編號；預設 `n`。
重名、App 輸出目錄大小寫碰撞、移除後仍引用的選項都報錯。
官方 Module 依框架登記自動選入，新 schema 只提供 Python 與自訂 Module 開關，
要求 UI／Bag 能力且不接受舊 UI／Bag 選項。既有單 App 解析器暫時保留到後續遷移。

Editor targets are `<App>_vinc_guiconfig` and `<App>_vinc_menuconfig`. Both carry the
same config, Module manifest and capability arguments as CMake. Saving updates
only the selected user file; the next build regenerates its selections. If a bad
config prevents generation, the diagnostic identifies the standalone editor
arguments. For example, using the actual generated manifest and tool paths:

```sh
python /path/to/configure.py --modules /path/to/build/vinc-config/MyApp/modules.json \
    --config /path/to/my-app.config --save --set VINC_MODULE_Camera=y
```

各 App 有獨立 GUI／終端編輯 target，與 CMake 共用設定來源、Module 清單及能力限制。
保存只寫指定檔案，下次 build 自動重新配置；設定錯誤阻止生成時，可依診斷提供的參數
獨立執行工具。設定輸出不是 Runtime registry，不增加執行期工作。

Stage 1 validation (2026-10-31): Windows x64/MSVC 19.51, CMake 4.3.2, Python
3.13.12 passed 14 parser cases and real CMake selection builds in Debug/Release.
WSL Ubuntu 24.04/GCC 13.3, CMake 3.28.3, Python 3.12.3 passed the same 14 cases and
the Release selection build. The fixture verifies unselected source never builds,
shared Plugin reuse, late registration, target customization, explicit catalog
selection, isolated saving, automatic regeneration and failure/recovery for
missing/unknown inputs. These are build-graph tests, not production multi-App tests.
Editor save/load hooks and the arguments shared by editor targets were tested;
interactive GUI/menu sessions, native macOS and full multi-App runtime are not
validated in this stage. The Unicode redirected-output concern recorded then was
subsequently resolved; see [the correction below](#redirected-unicode-diagnostics--unicode-重導診斷).

Existing App regression also passed on Windows: `sdk.configuration`,
`sdk.app_selection`, `module.shell` and `app.entry` were **4/4 in both Debug and
Release** after rebuilding the App targets. Release `sdk.installed_application`
passed **1/1**, including SDK resource installation, relocated execution, message
regeneration and the still-existing single-App feature cleanup path. No full
framework regression or native macOS run was performed for this stage.

The evidence uses base commit `4315cfa` plus the existing uncommitted workspace
and stage 1 changes, not a published release. Reproduce focused checks with:

```sh
python -B tools/configure/tests/test_configuration.py
ctest --test-dir <configured-build> -C <Debug-or-Release> \
    -R "^sdk[.](configuration|app_selection)$" --output-on-failure
```

第一階段已通過 Windows Debug／Release 與 WSL Release 的實際小型 Plugin 建置驗證；
兩平台設定解析各 14 項通過。涵蓋未選不建置、共用不重建、晚登記、target 後續設定、
catalog 範圍、個別保存、重新配置及錯誤恢復。驗證範圍是建置圖與設定，不是完整多 App
執行；當時未重測互動 GUI／menu、macOS。當時保留的 Unicode 診斷輸出問題已於後續
修正，見下方「Unicode 重導診斷」。
既有 App 相關 CTest 在 Windows Debug／Release 各 **4/4** 通過；Release 安裝式應用
回歸 **1/1** 通過，涵蓋安裝、搬移、訊息重建與舊單 App 功能清理。本輪未重跑全框架測試。

## Unified App configuration / 統一 App 設定

All Apps use the Module composition schema. UI and Recorder are mandatory System
Modules, not Kconfig switches. The bundled App uses `apps/vinc/vinc.conf`; external
Apps supply `VINC_BUILD_CONFIG` explicitly. All example Modules remain selected
in the bundled default; user Modules default off.

所有 App 共用 Module 組裝 schema；UI／Recorder 固定包含。內建設定檔位置如上，
外部 App 明確提供設定檔。設定工具不配置或編譯專案。

## Text, CLI and editors / 文字、命令列與編輯器

Each App explicitly supplies `VINC_BUILD_CONFIG`, relative to its calling CMakeLists.
The bundled App uses `apps/vinc/vinc.conf`. A named missing file fails; configure/build
never edits the source config. Generated output is `<build>/vinc-config/<App>/`.

各 App 明確提供設定檔，路徑相對於呼叫端 CMakeLists；缺檔即報錯，不自動建立或覆寫。
內建 App 的 GUI/menu targets 是 `Vinc_vinc_guiconfig`／`Vinc_vinc_menuconfig`：

```sh
cmake --build build/debug --target Vinc_vinc_guiconfig
cmake --build build/debug --target Vinc_vinc_menuconfig
```

For standalone editing after configuration, use its generated Module manifest:
配置後也可直接使用產生的 Module 清單編輯；只在 Save 時更新設定檔：

```sh
python tools/configure/configure.py --config apps/vinc/vinc.conf --modules build/debug/vinc-config/Vinc/modules.json --gui
python tools/configure/configure.py --config apps/vinc/vinc.conf --modules build/debug/vinc-config/Vinc/modules.json --save --set VINC_APP_PYTHON=y
```

Without `--modules`, standalone editing exposes Python choices only; it cannot
infer registered CMake ModuleTypes. Text editing remains available before the first
configure. Unknown names, invalid values, duplicate entries and unavailable capabilities
fail explicitly. Tk/curses is needed only for its respective editor. No editor installs
packages or starts a configure/build.

未提供 `--modules` 時只提供 Python 選擇，不猜測 CMake 登記的 ModuleType；首次配置前可
直接編輯文字檔。未知名稱、非法值、重複項目及能力缺失都明確報錯；不自行安裝或建置。

## Build integration / 建置整合

Central application composition requires Python 3.11+ (Python 3 only) to resolve configuration,
even when Python Modules are disabled. This is an interpreter-only build tool;
Tkinter is optional for GUI, curses for terminal UI. CMake uses `Python3_EXECUTABLE`
for this tool; Module Python selection belongs to App Kconfig.
Plain `find_package(vinc)`/linking does not run the configurator or require Python.
SDK-only source builds with `VINC_BUILD_APPS=OFF` retain their existing build options.

中央應用組裝需要 Python 3.11+（限 Python 3）解析設定，即使未開啟 Python Module 亦然。
只需要直譯器；GUI 額外需要 Tkinter，終端選單需要 curses。
設定工具使用 `Python3_EXECUTABLE`，Module 綁定直譯器由 App Kconfig 選擇。
單純 find_package／連結不執行工具、不要求 Python。
`VINC_BUILD_APPS=OFF` 的 SDK-only 來源建置保留原有建置開關。

Removed CMake feature inputs: `VINC_BUILD_PYTHON`, `VINC_BUILD_BAG`, `VINC_BUILD_UI`,
`VINC_BUILD_EXAMPLES`, `VINC_NAME`, `VINC_CONFIG_FILE`. They are not read/rejected/deleted;
CMake owns unused-input warnings. App identity and file locations are explicit
`vinc_add_app()` arguments. Compiler, configuration, tests and SDK installation
remain CMake concerns. Official SDK production always includes UI capability.

上述舊變數不再控制 VINC；不為它們增加拒絕或相容轉接。身份與設定檔位置由 App 建構器
明確指定，Module／Python 選擇屬 Kconfig。工具鏈、測試、SDK 安裝保留 CMake 入口。
下方為遷移前各階段的歷史驗證，不代表舊入口仍受支援。

The completed App unification and current platform validation are maintained in
[the App report](../../apps/vinc/README.md#app-composition-unification--app-組裝統一).
組裝統一與最新平台驗證集中於上述文件；以下早期紀錄不代表舊入口仍受支援。

### Legacy Python/Bag switch removal (2026-11-02) / 舊開關移除驗證

Windows x64 Release (MSVC 19.44/toolset 14.44.35207, CPython 3.13.12,
CMake 4.3.2) built the source App with both obsolete switches OFF and Kconfig
features ON; selected Python/RPC/Bag/configuration CTest passed **10/10**.
The inverse full-source configuration (old switches ON, Kconfig OFF) produced
neither a Python binding nor a Recorder Module target. Configuration tests passed
**21/21** on Windows and **21/21** on WSL Ubuntu 24.04/CPython 3.12.3/CMake 3.28.3.
Windows build/setup/install tests: **118**, 22 platform skips, no failures.

The SDK-only producer ignored old Python ON/Bag OFF inputs, built successfully,
and retained its Python-capable archives/material. The installed Python fixture
passed relocation, custom Bus/RPC/Param, schema updates, failure isolation, repair
and disable cleanup. CMake itself reported the old variables as unused. Logs are
under ignored `build/kconfig-legacy-review/`. No macOS, Debug, or full WSL SDK
rerun is claimed for this removal. App composition unification was implemented
afterward; its separate validation is recorded in the
[App report](../../apps/vinc/README.md#app-composition-unification--app-組裝統一).

Windows 來源建置與相關 CTest 10/10 通過；完整配置另驗證舊值 ON／Kconfig OFF 不會建立
Python binding 或 Recorder Module。Windows／WSL 設定測試各 21/21，Windows 工具測試
118 項、22 項平台跳過，無失敗。純 SDK 與 installed Python App 驗收通過，涵蓋搬移、
自訂訊息通訊、增量更新、失敗隔離、補檔與停用清理。未重跑 macOS、Debug 或 WSL 完整 SDK。

Installed SDKs ship this directory at `share/vinc/configuration`, with pinned
Kconfiglib and `capabilities.json`. Use its `configure.py` directly; no pip/network
is required. CMake validates against imported targets, not user-edited capabilities.
Modifying configuration or adding/removing the default file triggers regeneration
on the next build. The source and installed paths share this parser and schema.

安裝式 SDK 將工具、固定 Kconfiglib 及能力資訊放在 `share/vinc/configuration`，
不需 pip／網路。CMake 以實際 imported targets 驗證能力。設定修改及預設檔新增／刪除
會在下一次 build 觸發重新配置。兩條路徑共用解析器及功能定義。

Legacy source deployment records generated Module/assets in `.vinc-owned-files`. A removed
feature's formerly managed files are deleted only if their recorded hashes still
match; modified files are preserved with diagnostics. Untracked files, user plugins,
data/module, params, logs and recordings are never claimed by this manifest.
Pre-migration outputs without a record are not guessed or swept. Do not deploy
over a running application. Source compiler caches/intermediates may remain.

部署記錄生成檔及雜湊，功能關閉只刪除仍符合紀錄的舊檔。修改過的檔案保留並提示；
未記錄檔案、使用者 plugin、data/module、參數、log、錄製資料不納管。
導入前沒有紀錄的舊產物不猜測刪除；不要覆蓋執行中的應用。
來源建置的編譯器快取／中間產物可保留。

## Validation / 驗證

Validated on 2026-10-29:

| Environment / 環境 | Checks / 檢查 |
| --- | --- |
| Windows x64, MSVC 19.51, Release, CMake 4.3.2, Python 3.13.12 | Eight configuration unit cases; configuration, ownership and license CTests; source external application build/run and UI/Bag off/on cycle; installed application build/run, relocation and off/on deployment cycle. All passed. / 八項設定案例、三項專用 CTest、來源外部應用與安裝式應用建置執行、搬移及功能開關循環均通過。 |
| WSL Ubuntu 24.04.5, GCC/libstdc++ 13.3, Release, CMake 3.28.3, Python 3.12.3 | Eight configuration unit cases; five SDK CTests (installed consumer, installed application, configuration, ownership, licenses). All passed. / 八項設定案例及五項 SDK CTest 均通過。 |
| Windows Tk 8.6.15 | Real guiconfig widgets/event loop and upstream Save callback exercised in a hidden window; saved choices reloaded successfully. / 隱藏視窗實測元件、事件迴圈及上游 Save callback，保存結果可重新讀取。 |

The application tests verify that changing, creating and deleting configuration
triggers regeneration; disabled official Plugins/assets disappear from managed
deployment, while user files survive; enabling them again restores operation.
Missing installed Python build material is rejected. Cleanup tests reject traversal
before deleting any files and preserve modified/untracked files.

應用測試涵蓋修改、新增、刪除設定後重新配置；關閉官方功能會清理已記錄產物，
使用者檔案保留，再開啟可恢復運作。缺少安裝式 Python 建置材料時明確拒絕。
清理測試涵蓋越界路徑在任何刪除前即遭拒絕，以及修改過／未納管檔案保留。

The additional Windows source-Python enable/build/deploy/disable cycle passed;
disabled package files were removed and a user-created file was preserved.
The import check was blocked by the existing
runtime guard: this machine's CPython bundles VCRUNTIME140 14.42.34438.0, while
the MSVC 19.51-built extension required at least 14.51.36244.0. This was not a
successful Python runtime check. The Python installation was not changed. This
historical result concerns the former dynamic-CRT binding, not the current static
Windows extension; see the [current Runtime policy](../../src/vinc/module_system/python/README.md#windows-msvc-runtime-preflight).

補充的 Windows 原始碼版 Python 開啟、建置、部署、關閉循環通過，套件產物移除且
使用者新增檔案保留。匯入測試被既有載入檢查擋下：本機 CPython 自帶
VCRUNTIME140 14.42.34438.0，低於此次綁定要求的 14.51.36244.0。
這不算當時的 Python 執行驗證通過；未更動系統 Python。此為舊動態 CRT 綁定的歷史
結果，不是目前 Windows 靜態擴充的限制；現行政策見上述連結。

Subsequent installed-Python SDK work validated successful imports and Module flows
using the existing isolated Windows Python, plus Ubuntu's system Python. See the
[Python SDK validation](../../src/vinc/module_system/python/README.md#installed-sdk-validation-2026-10-29--安裝式-sdk-驗證).

後續安裝式 Python SDK 工作已用既有 Windows 隔離環境及 Ubuntu 系統 Python 完成正常
匯入與 Module 流程驗證；結果見上述 Python SDK 紀錄，不更動前述舊 CRT 拒絕證據。

This round did not repeat Debug builds, run native macOS integration, exercise an
interactive curses terminal, or perform manual visual GUI acceptance. That historical
round declared Python 3.9 but ran on 3.12/3.13; the current minimum follows the
[Common toolchain policy](../../src/vinc/common/README.md#build-toolchain-policy). Earlier platform validation
is not replaced by these results; upstream support is not local test evidence.

本輪未重跑 Debug、未執行 macOS 原生整合、未操作 curses 互動終端，亦未完成人工 GUI
視覺驗收。該歷史輪次宣告最低 Python 3.9，但實測為 3.12／3.13；目前最低版本已依共用工具鏈規則更新。這些紀錄不取代先前平台驗證，
也不把上游支援聲明當作本機測試證據。

## Redirected Unicode diagnostics / Unicode 重導診斷

Resolved in the Kconfig Python selection work (2026-11-02): `configure.py`
emits UTF-8 and its CMake callers explicitly decode UTF-8. The parser suite
checks a real config path containing U+E918/Chinese while the inherited
`PYTHONIOENCODING` is cp950. The earlier failure was diagnostic encoding, not
a Module self-launch failure. This does not repair incorrectly decoded input
from an external script; the caller must still supply the intended path.

2026-11-02 已修正設定工具與 CMake 呼叫端的 UTF-8 輸出契約；測試以 cp950 環境
重導含 U+E918／中文的實際設定檔路徑。這解決診斷編碼失敗，不會替外部腳本猜測
已經誤解碼的路徑，也不是 Module 啟動架構修正。
