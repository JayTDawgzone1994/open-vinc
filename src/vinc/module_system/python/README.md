<a id="python-module"></a>

# Python Module / Python 模組

Central Shell manages Python Modules, one system-Python process per instance. Central neither embeds/links Python nor bundles/installs interpreters/packages. pybind11 exposes the shared C++ Runtime rather than a separately implemented socket/shared-memory protocol.

Python Module 由中央 Shell 啟停，每個實例使用一個系統 Python 程序。
中央不連結或嵌入 Python，不附帶直譯器，也不自動安裝 Python／pip 套件。
使用者程式透過 pybind11 原生擴充使用同一套 C++ Runtime；沒有另寫 Socket／共享記憶體協定。

<a id="建置與部署"></a>

## Build and deploy / 建置與部署

```sh
cmake -S src -B build/my-build -DVINC_BUILD_PYTHON=ON -DPython_EXECUTABLE=/absolute/path/to/python3
cmake --build build/my-build
```

Use the chosen compiler/configuration. VINC_BUILD_PYTHON defaults OFF so C++ needs no Python development files. Enabled builds require CPython 3.9+/Development.Module, a dependency floor rather than verified coverage of every version. Only 64-bit GIL-enabled CPython; no free-threading/subinterpreters/PyPy/Python 2.

沿用所選建置目錄的編譯器與組態。`VINC_BUILD_PYTHON` 預設 OFF，讓只使用 C++ 的建置不要求 Python 開發檔。
開啟後需要 CPython 3.9 以上及其 Development.Module 檔案；這是相依最低要求，不代表所有版本已通過驗證。
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

User code lives in data/module, distinct from vinc_data persistence. Editing existing Python Modules does not rebuild central. Compiled vinc_messages type lists define vinc.messages; new .msg requires regenerating/rebuilding consumers/native extension, without runtime schema parsing. --python selects interpreter/venv; relative paths become absolute on central startup. Otherwise launch searches PATH for Windows python.exe/POSIX python3, not the build interpreter, without silently trying another incompatible environment.

`data/module` 是使用者程式的位置，與 Param／Log 的 `vinc_data` 不同。
只修改既有 Python Module 程式不需編譯中央。`vinc.messages` 由 `vinc_messages` 的
編譯期型別清單建立；新增 `.msg` 後須重新生成並建置所有使用它的程式及 Python 擴充。
不提供 runtime `.msg` 解析器或動態訊息型別。
`Vinc --python /absolute/path/to/python` 可指定 Python 或 venv，CLI 亦可接受相對路徑並在中央啟動時轉成絕對路徑。
省略時，在啟動 Module 當下依 PATH 順序尋找 Windows `python.exe` 或 POSIX `python3`。
不使用建置時選定的 Python 位置作為部署端的隱含預設，不在第一個 Python 不相容時偷偷換另一個環境。

<a id="windows-msvc-runtime-載入前檢查"></a>

### Windows MSVC Runtime preflight / Windows MSVC Runtime 載入前檢查

import vinc checks required runtimes through Python/Windows APIs before _native. Bootstrap imports Vinc before user code; external Python should import before WinRT/other native packages. This creates neither a Session nor worker.

`import vinc` 在載入 `_native` 前，以 Python／Windows API 檢查該擴充需要的 MSVC Runtime。
中央 bootstrap 本來就先 import Vinc 再載入使用者 Module；外部 Python 應在 WinRT 等原生套件前
import Vinc。這不建立框架 session，也不新增背景執行緒。

- Build-postprocessing uses dumpbin /DEPENDENTS for actual MSVCP/VCRUNTIME/CONCRT imports including Debug/sidecars. The same toolchain crtversion.h supplies a conservative four-part minimum, not System32 contents or compiler 19.xx. It is a deployment baseline, not per-function minimum; unknown metadata/dependencies fail the build.
- Generator supports standard MSVC bin/`Host<arch>`/<arch>/cl.exe layout, not unverified alternate Windows toolchains. Deploy the whole python/vinc package, not only .pyd.
- Check every loaded required same-name DLL; any below baseline/different major raises ImportError. Compatible Python-directory/other DLLs may remain; System32 placement is not mandatory.
- Missing Release imports are selected only from the Windows API system directory. Validate all, preload in order, recheck transitive loaded DLLs, and retain handles for process lifetime. Do not alter PATH/search policy.
- Debug checks actual Debug CRT names; if absent in System32, allow configured interpreter/absolute developer PATH directories, never cwd/relative PATH. Debug CRT is not bundled or replaced by Release redistributables; missing files reject.
- Missing DLL/version/manifest, corrupt metadata, or preload failure rejects before _native. Never replace/unload/patch existing DLLs, auto-install, or clean-build as a runtime remedy.
- External callers may catch ImportError and stop using Vinc. Bootstrap prints stderr and exits 70; central applies startup policy while other Modules continue.

- `_msvc_runtime.json` 由建置後的 `dumpbin /DEPENDENTS` 取得 MSVCP140、VCRUNTIME140、CONCRT140
  家族的實際匯入名稱，包含 Debug 名稱與 STL sidecar。以同一 MSVC 工具鏈的 `crtversion.h`
  四段版本作保守最低基準，另記工具鏈與組態；不從建置電腦 System32 或編譯器 `19.xx` 推算。
  這是部署基準，不宣稱是每個所用函式的最低可運作版本。無法辨識工具鏈資料／相依時建置失敗。
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
  本機 Debug／Release 建置快取的 Python_EXECUTABLE 已改指此隔離副本。
- `module.python_runtime` 的真實舊 DLL／WinRT 案例需要測試環境變數
  `VINC_TEST_OLD_MSVC_RUNTIME`（舊 DLL 完整路徑）與 `VINC_TEST_WINRT_DIR`（解壓套件根目錄）。
  未提供時會明確 skip，不自動下載或安裝第三方套件。一般 CTest 不依賴 WinRT。
- Linux／macOS 未實機驗證；此防護僅由 Windows MSVC 建置啟用。MSVC 19.44 尚未在本機重建
  本輪修改，外部提供的 19.44 崩潰報告不算本輪實作的驗證。

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

The exact native crash site from the external 2026-10-14 v0.1.1/MSVC 19.44/WinRT-first old-MSVCP report remains unknown without stack/dump; the UNC report was inaccessible. External tests showed new-first coexistence/OCR; isolated official wheels locally verify preflight rejection and reverse-order coexistence. [Microsoft compatibility](https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017) requires Runtime at least as new as toolchain; [STL 17.10 notes](https://github.com/microsoft/STL/wiki/VS-2022-Changelog#vs-2022-1710) document mutex null dereference from violating it. pybind11 mutex initialization is a concrete candidate, not a confirmed function diagnosis. Rejecting unsupported Runtime does not require proving the precise site. No CRT switch/compatibility macros/DLL replacements were introduced.

- 2026-10-14 外部問題的具體原生崩潰位置仍待確認：v0.1.1（8faee632）、MSVC 19.44 擴充
  在 WinRT 3.2.1 先帶入 MSVCP140.dll 14.29.30157 後 import 時發生 0xC0000005。
  外部報告確認 Vinc／新版 Runtime 先載入可通過 native tests 與 OCR；本機已下載官方 wheel
  驗證新防護對舊 DLL 先載入的拒絕，以及反向順序的共存。UNC 報告仍無法存取，尚無
  native stack／dump，不能認定封包解碼或特定 mutex 已確認有錯。
  [Microsoft 相容性要求](https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017)
  規定 Runtime 不得早於所用工具鏈；[STL 17.10 紀錄](https://github.com/microsoft/STL/wiki/VS-2022-Changelog#vs-2022-1710)
  亦說明違反版本要求可能造成 mutex null dereference。固定 pybind11 的初始化路徑使用
  std::mutex，因此屬具體候選原因，仍須原生堆疊確認。本輪已加入上述載入前拒絕與預載，
  不需要先證實是哪個 C++ 函式才能拒絕不支援的 Runtime。沒有 CRT 切換、相容巨集或第三方 DLL 替換。

Confirmed test defect from 2026-10-12: services_module asserts Publisher ID != Module MID. These are separate namespaces whose values may coincide; passing current configuration does not validate inequality. Later examples use publisher_id labels and avoid the assumption; the original test still needs identity-based verification, not cross-kind numeric comparison.

- 2026-10-12 範例 review 確認：`tests/services_module.py` 的 received_message 使用
  `publisher_instance() != self.get_mid()` 作斷言。Publisher ID 與 Module MID 是不同識別空間，
  數值可能恰好相同，不能以不相等當正確性條件。目前測試配置通過不代表此假設有效；
  建議後續改為驗證 Publisher 身分與已知發布者對應，不比較跨種類數值。
  本輪修正示範的 `publisher_id` 顯示名稱，新範例測試不採此假設；既有測試留待修正。

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
