# Application configuration / 應用功能設定

Public source/installed `vinc_add_app()` now uses the per-App selection path below.
It requires `VINC_BUILD_CONFIG`; it does not implicitly read the project's
`vinc.conf` or global `VINC_CONFIG_FILE`. The legacy source shell retains the older
single-App path during the [multi-App migration](../../apps/vinc/MULTI_APP.md).

來源／安裝式公開 `vinc_add_app()` 已接入下方各 App 選擇流程，必須提供 `VINC_BUILD_CONFIG`，
不隱式讀取專案 `vinc.conf` 或全域 `VINC_CONFIG_FILE`。遷移期間只有舊來源 shell 保留舊流程。

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
validated in this stage. Existing Unicode redirected-output concerns below remain.

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
執行；未重測互動 GUI／menu、macOS，也未解決下方既有 Unicode 診斷輸出問題。
既有 App 相關 CTest 在 Windows Debug／Release 各 **4/4** 通過；Release 安裝式應用
回歸 **1/1** 通過，涵蓋安裝、搬移、訊息重建與舊單 App 功能清理。本輪未重跑全框架測試。

## Existing single-App path / 既有單 App 路徑

Kconfig owns feature defaults and choices. CMake owns targets, actual SDK
capabilities, toolchains and build profiles. This tool selects application
composition; it does not promise complete library/dependency removal from the SDK.
Core services and `vinc/config.hpp` are unchanged. UI/Bag inclusion does not start
either Module automatically; Bag Recorder remains **enabled by default**.

Kconfig 維護功能預設與選擇，CMake 管理 targets、SDK 實際能力、工具鏈與建置組態。
這是應用組裝設定，不承諾從 SDK 徹底移除未使用函式庫／相依。
核心服務及 `vinc/config.hpp` 未搬移。UI／Bag 開啟不代表自動啟動，Bag Recorder **預設包含**。

| Symbol / 選項 | Default / 預設 | Effect / 效果 |
| --- | --- | --- |
| `CONFIG_VINC_APP_UI` | `y` | Include official webUi / 包含官方 UI |
| `CONFIG_VINC_APP_BAG` | `y` | Include bagRecorder / 包含錄製模組 |
| `CONFIG_VINC_APP_PYTHON` | `n` | Build/deploy application bindings / 建置部署 Python 綁定 |

Source and installed applications build their own Python bindings when selected,
using a matching CPython interpreter/development environment. Installed SDKs carry
the binding sources and pinned pybind11 even when the producer disables Python.
See the [Python SDK contract](../../src/vinc/module_system/python/README.md).
A prebuilt SDK
without UI rejects UI selection; it cannot grow missing libraries at configure time.

原始碼與安裝式應用均可建立自己的 Python 綁定，需要匹配的 CPython 與開發檔。
SDK 即使未啟用 Python 建置，也交付綁定來源與固定 pybind11；完整契約見上方連結。
未包含 UI 的 SDK 拒絕開啟 UI，
不能在配置時憑空補出未交付的函式庫。

## Text, CLI and editors / 文字、命令列與編輯器

By default, CMake reads `vinc.conf` in the outermost application's source root.
Use `-DVINC_CONFIG_FILE=/absolute/path/to/file.conf` for another file. Relative
paths resolve against that outer root. An explicitly named missing file fails.
A missing default file uses defaults without creating it. Configuration/build
never rewrites the user file. The full effective `.config` and `features.cmake`
are build-only outputs under `<outer-build>/vinc-config/`.

CMake 預設讀取最外層應用來源目錄的 `vinc.conf`。可用 `VINC_CONFIG_FILE` 改指定；
相對路徑以最外層來源目錄解析。明確指定卻缺檔會報錯，預設檔不存在則使用預設值，
不自動建立。configure／build 不修改使用者設定。完整有效設定與 CMake 接線檔只寫入
最外層 build 的 `vinc-config/`。

```ini
CONFIG_VINC_APP_UI=y
CONFIG_VINC_APP_BAG=y
# CONFIG_VINC_APP_PYTHON is not set
```

Run these from your application directory, substituting the tool's absolute path:

從應用專案目錄執行，將工具路徑替換為實際絕對路徑：

```sh
python /path/to/open-vinc/tools/configure/configure.py
python /path/to/open-vinc/tools/configure/configure.py --gui
python /path/to/open-vinc/tools/configure/configure.py --menu
python /path/to/open-vinc/tools/configure/configure.py --save --set VINC_APP_UI=n
```

Use `--config /path/to/vinc.conf` to select a file. Plain invocation validates and
prints the effective choices. Only `--save` or the editor's explicit Save writes
the user file; omitted choices follow this framework version's defaults. Editors
save full choices and retain upstream `.old` backup behavior. GUI Save As/Open
use the same validation hooks. Files contain only boolean options in this first
schema; unknown names, invalid values, duplicates, incompatible legacy choices,
and unavailable capabilities fail rather than silently normalize.

使用 `--config` 選檔；無其他模式時僅驗證並列出結果。只有 `--save` 或編輯器明確保存
才寫入使用者檔案。未指定項目跟隨該框架版本預設；編輯器保存完整選擇並保留上游 `.old`
備份行為。另存／開檔共用驗證。第一版只有布林設定；未知名稱、非法值、重複項目、
舊選項衝突與不可用能力都會報錯，不默默忽略。

After successful CMake configuration, `vinc_guiconfig` and `vinc_menuconfig` are
convenience targets. The standalone commands above remain usable when CMake has
failed. No command installs dependencies, configures a build or starts compilation
on the user's behalf. Missing Tk/curses affects only the requested editor.

CMake 配置成功後可用 `vinc_guiconfig`／`vinc_menuconfig` targets；配置失敗時仍可獨立
執行工具修正設定。工具不自動安裝相依、不替使用者配置或編譯專案。
缺少 Tk／curses 只影響要求的編輯介面。

## Build integration / 建置整合

Central application composition requires Python 3.9+ to resolve configuration,
even when Python Modules are disabled. This is an interpreter-only build tool;
Tkinter is optional for GUI, curses for terminal UI. CMake uses `Python3_EXECUTABLE`
for this tool; Python Module ABI selection remains `Python_EXECUTABLE`.
Plain `find_package(vinc)`/linking does not run the configurator or require Python.
SDK-only source builds with `VINC_BUILD_APPS=OFF` retain their existing build options.

中央應用組裝需要 Python 3.9+ 解析設定，即使未開啟 Python Module 亦然。
只需要直譯器；GUI 額外需要 Tkinter，終端選單需要 curses。
設定工具使用 `Python3_EXECUTABLE`，Module 綁定仍使用 `Python_EXECUTABLE`。
單純 find_package／連結不執行工具、不要求 Python。
`VINC_BUILD_APPS=OFF` 的 SDK-only 來源建置保留原有建置開關。

The legacy source path's `VINC_BUILD_BAG`, `VINC_BUILD_PYTHON`, and
`VINC_BUILD_UI=OFF` feed the same resolver; a contradictory explicit file value
fails. `VINC_BUILD_UI=ON` describes source SDK capability and does not force UI
into the app. Legacy cache values from older builds are still explicit inputs;
remove them (e.g. `cmake -U VINC_BUILD_PYTHON ...`) when migrating to file ownership.
There is no hidden last-writer-wins order. The new per-App constructor instead
requires an explicit file and does not accept the old installed `NO_UI`/`NO_BAG` API.

舊入口透過同一解析器轉接；與檔案明確值相反即報錯。`VINC_BUILD_UI=ON` 表示 SDK
可建置 UI，不強迫應用包含它。舊 build cache 也屬明確輸入，改由檔案管理時應先清除
相關舊值，例如 `-U VINC_BUILD_PYTHON`。不採隱含覆蓋順序。新各 App 建構器則要求明確
設定檔，不接受舊安裝式 `NO_UI`／`NO_BAG` API。

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
the MSVC 19.51-built extension requires at least 14.51.36244.0. This is not a
successful Python runtime check. The Python installation was not changed; use a
compatible deployment as described in the [Python SDK guide](../../src/vinc/module_system/python/README.md).

補充的 Windows 原始碼版 Python 開啟、建置、部署、關閉循環通過，套件產物移除且
使用者新增檔案保留。匯入測試被既有載入檢查擋下：本機 CPython 自帶
VCRUNTIME140 14.42.34438.0，低於此次綁定要求的 14.51.36244.0。
這不算 Python 執行驗證通過；未更動系統 Python，後續需依 Python SDK 指南準備相容環境。

Subsequent installed-Python SDK work validated successful imports and Module flows
using the existing isolated Windows Python, plus Ubuntu's system Python. See the
[Python SDK validation](../../src/vinc/module_system/python/README.md#installed-sdk-validation-2026-10-29--安裝式-sdk-驗證).

後續安裝式 Python SDK 工作已用既有 Windows 隔離環境及 Ubuntu 系統 Python 完成正常
匯入與 Module 流程驗證；結果見上述 Python SDK 紀錄，不更動前述舊 CRT 拒絕證據。

This round did not repeat Debug builds, run native macOS integration, exercise an
interactive curses terminal, or perform manual visual GUI acceptance. Python 3.9
is the declared minimum, but these runs used 3.12/3.13. Earlier platform validation
is not replaced by these results; upstream support is not local test evidence.

本輪未重跑 Debug、未執行 macOS 原生整合、未操作 curses 互動終端，亦未完成人工 GUI
視覺驗收。宣告最低 Python 3.9，但本輪實測為 3.12／3.13。這些紀錄不取代先前平台驗證，
也不把上游支援聲明當作本機測試證據。

## Outstanding issue / 待處理問題

Windows redirected console encoding can reject a valid Unicode project path.
During the 2026-10-30 SDK relocation checks, a local PowerShell script decoded its
UTF-8 path incorrectly, producing a path containing U+E918. Configuration failed
with `cp950 codec can't encode character` when `configure.py` printed the path.
The tool's final `print()` uses the inherited stdout encoding even though its
configuration files use UTF-8. Correctly encoded Chinese/space paths passed the
subsequent SDK tests; arbitrary Unicode paths are not thereby covered.

Status: confirmed diagnostic-output limitation, not a Module self-launch failure.
Follow-up: define UTF-8 output consistently between the Python tool and its CMake
caller, and test redirected output with a path outside the active code page. This
is recorded separately from the single-executable change; no encoding policy has
been changed in that work.

Windows 重導輸出的主控台編碼可能拒絕合法 Unicode 專案路徑。2026-10-30 的 SDK 搬移
檢查中，本機 PowerShell 腳本誤讀 UTF-8 後產生含 U+E918 的路徑，`configure.py` 印出
路徑時發生 `cp950 codec can't encode character`。設定檔雖用 UTF-8，末尾 `print()`
仍沿用 stdout 編碼。修正腳本編碼後的中文／空白路徑通過，不代表所有 Unicode 路徑皆通過。

狀態：已確認的診斷輸出限制，不是 Module 自我啟動失敗。建議後續統一 Python 工具與
CMake 呼叫端的 UTF-8 輸出，並測試重導輸出及目前 code page 之外的路徑。本次單一
執行檔工作只另行記錄，沒有更動編碼政策。
