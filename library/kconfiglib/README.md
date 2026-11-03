# Kconfiglib

- Source / 來源: https://github.com/zephyrproject-rtos/Kconfiglib
- Version / 固定版本: **v14.1.0**, commit `061e71f7d78cb057762d88de088055361863deff`.
- License / 授權: **ISC**, retained in [upstream/LICENSE.txt](upstream/LICENSE.txt), together with per-file attributions.
- `upstream/` is an unmodified Git submodule. `revision.txt` and the parent gitlink pin the same commit.
- `upstream/` 為未修改的 Git submodule；`revision.txt` 與主專案 gitlink 固定同一 commit。

## Scope / 用途與邊界

Used for build-time application feature configuration, including parsing, terminal
menus and the Tkinter GUI. Open-VINC's wrapper and contracts live in
[tools/configure](../../tools/configure/README.md), outside the unmodified upstream.
This adds no runtime dependency.

用於建置期應用功能設定，包含解析、終端選單與 Tkinter GUI。
Vinc 封裝與契約由上述 tools/configure 文件維護，未修改上游，也不新增執行期相依。

The upstream repository is now maintained under Zephyr. At selection time
(2026-10-29), v14.1.0 remained the latest final release; v14.1.1a* tags were
prereleases. This pins the historical final release, not Zephyr's newer main
branch. Its age is not proof of compatibility; validation below bounds our claim.

上游現由 Zephyr 組織接手。選版時（2026-10-29），v14.1.0 仍是最新正式版，
v14.1.1a* 為預發布版。本次固定歷史正式版，並非較新的 Zephyr main 分支。
版本年齡不代表相容性保證，實際驗證範圍如下。

## Restore and use / 取得與使用

From the repository root / 從儲存庫根目錄執行：

```sh
git submodule update --init library/kconfiglib/upstream
python library/kconfiglib/upstream/guiconfig.py /path/to/Kconfig
python library/kconfiglib/upstream/menuconfig.py /path/to/Kconfig
```

Select an installed Python 3 executable (`python3` on systems using that name).
No global pip installation is needed for the parser. GUI use requires a working
Tkinter installation; terminal use requires curses (normally windows-curses on
Windows). These UI dependencies are not installed or bundled by this import.
Headless configuration needs neither GUI nor curses.

請選用已安裝的 Python 3；部分系統命令名稱為 `python3`。解析器不需全域 pip 安裝。
GUI 需要可用的 Tkinter；終端介面需要 curses（Windows 通常為 windows-curses）。
本輪未安裝或附帶這些 UI 相依；無介面配置不需要 Tkinter 或 curses。

Run tools from a separate build/work directory. By default the menus load/save
`.config` there; set `KCONFIG_CONFIG` to choose another file. Do not run test
outputs into `upstream/`. For Vinc applications use the wrapper linked above;
the raw commands require a caller-provided definition. Preserve the complete
license and per-file copyright notices when redistributing the tools.

在獨立 build／工作目錄執行；選單預設讀寫該處的 `.config`，可透過
`KCONFIG_CONFIG` 指定其他檔案。不要把測試產物寫入 `upstream/`。
上例為上游原始入口；Vinc 應用請使用上方連結的封裝。散布工具時須保留完整授權與各檔署名。

## Validation and pending work / 驗證與待辦

Initial import checks on 2026-10-29, Windows x64 / CPython 3.13.12 / Tcl-Tk 8.6.15
(historical scope; see [current application integration results](../../tools/configure/README.md#validation--驗證)):

- Passed: defaults, dependency resolution, full/minimal configuration save/load,
  C header generation, and the upstream `genconfig.py` CLI.
- Passed: importing `guiconfig` and initializing/destroying a hidden Tk root.
- Not tested at initial import: interactive GUI editing, curses terminal UI, the complete upstream
  Linux-kernel comparison suite, Linux/macOS execution, or Vinc CMake integration.
- Local smoke script: ignored `build/kconfiglib-smoke/check.py`.

Windows 基本解析、相依、完整／最小設定存取、標頭生成與 CLI 已通過；GUI 模組載入與
隱藏 Tk root 初始化／銷毀亦通過。這不等於完整互動 GUI 驗證。
初次引入時未驗證終端選單、上游完整 Linux kernel 對照測試、Linux／macOS 與 Vinc CMake 整合。
上述為歷史紀錄；後續 Windows／WSL 設定接線與驗證結果由 tools/configure 文件維護。
