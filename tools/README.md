# Dependency setup / 相依環境準備

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
| `--no-ui` | Skip Node/npm and frontend packages. / 略過 Node／npm 與前端套件。 |
| `--no-python` | Skip Python SDK requirements; setup itself does not need Python in this mode. / 略過 Python SDK 需求，此模式下 setup 本身也不需要 Python。 |
| `--help` | Print usage without preparing anything. / 僅顯示說明。 |

## Platform behavior / 平台行為

- **Windows x64:** `setup.bat` invokes `setup.ps1` through Windows PowerShell 5.1.
  WinGet installs missing Git, CMake, Ninja, MSVC C++ Build Tools (including
  recommended Windows SDK components), Node LTS, or CPython 3.13. Setup imports
  the Visual Studio developer environment into its own process and checks `cl`,
  linker, resource compiler and NMAKE. It does not change the parent terminal.
  Build your own project in an x64 Developer terminal; setup prints its
  `VsDevCmd.bat` path.
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
  MSVC 環境僅在腳本程序內載入，後續建置請使用 x64 Developer 終端。
- **Ubuntu 22.04／24.04：** 使用 apt；優先保留通過探針的編譯器，必要時安裝 GCC 12／13
  候選工具鏈。缺少 Node 時下載官方 Node 24、核對 SHA-256，安裝到目前使用者資料目錄。
  不新增第三方 apt 來源、不切換系統預設編譯器、不修改 shell profile。
- **macOS：** 使用 Apple Command Line Tools 與既有 Homebrew。Apple 工具初次安裝可能需要
  互動，完成後須重跑；不相容的 Apple SDK／編譯器要更新或明確選用其他工具鏈。

Package managers may need administrator privileges, sudo, network access,
license acceptance or a restart. Setup never reboots, uninstalls tools, changes
system alternatives or upgrades the entire OS. Failure exits nonzero with the
failed step. Homebrew/WinGet must already be available when needed. Other Linux
distributions are rejected rather than running guessed package commands.

套件安裝可能需要系統權限、sudo、網路、授權同意或重啟；腳本不自動重開機、移除工具、
切換系統 alternatives 或升級整個 OS。失敗會回傳非零退出碼並指出階段。
需要時須先具備 Homebrew／WinGet；其他 Linux 發行版不猜測安裝指令。

## Checks and source protection / 檢查與來源保護

- CMake minimum is 3.20; Node follows current Vite engines: 20.x from 20.19, or
  22.12 and later. The C++ probe checks `std::expected`, threads, 64-bit pointers
  and lock-free 64-bit atomics, plus Windows SDK linking. It does not replace
  a real framework build or behavioral tests.
- Python must be 64-bit, GIL-enabled CPython 3.9+ with matching `Python.h` and,
  on Windows, its x64 import library. `VINC_SETUP_PYTHON` selects an explicit
  executable; an incompatible choice is reported rather than replaced. Setup
  prints its path for your `Python_EXECUTABLE` selection. Windows tries the
  Python launcher's interpreter before PATH candidates.
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

Sources: [Microsoft scripted installation](https://learn.microsoft.com/en-us/visualstudio/install/use-command-line-parameters-to-install-visual-studio),
[Homebrew prerequisites](https://docs.brew.sh/Installation),
[Node 24 distribution](https://nodejs.org/dist/latest-v24.x/).
