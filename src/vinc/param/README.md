<a id="param"></a>

# Param / 具型別參數服務

**macOS acceptance (2026-11-03):** source Debug/Release regressions passed 96/96 per configuration; subsequent fresh SDK-only producers at `9f0ec0b` passed installed consumer/application/python acceptance **3/3 each in Debug and Release**. See the [Mac SDK report](../sdk/MACOS_PARAM_VALIDATION.md) for environment, commands, Param markers and coverage limits.

**Mac 驗收已完成：** 新版 Plugin entry 13／Param wire v3 的來源及乾淨 installed SDK 已有原生證據，範圍依上述報告。下文歷史階段表格的 Mac Pending／待驗保留當輪結果，後續驗收由新報告補齊，不代表目前仍待驗，也不把不同輪次合計為一次完整 CTest。

**Restart-hint stages 1–3 and earlier redesign stages 1–4:** implementation and Windows/WSL acceptance are recorded below; native Mac follow-up is linked above. This README owns the current contracts; [the implementation record](REDESIGN.md) indexes decisions and evidence. Those rounds excluded system-setting migration; the later Publisher-default integration is linked below.

**重啟標記及先前重設計：** 實作、Windows／WSL 原始驗收見下方各階段，後續 Mac 驗收見上述報告。本文維護正式契約，實作紀錄保留索引；原輪次不包含系統設定搬移，後續 Publisher 預設接線見下方。

Param is a centrally managed typed parameter service. Modules use inherited context.param with approved name/MID, not self-created Contexts, shared regions, or Guards. Updates may be delayed/coalesced; Param neither replaces Bus nor synchronizes application across Modules.

Param 是中央維護的具型別參數服務。Module 使用繼承的 `context.param`，由中央核准的
名稱與 MID 決定身分，不自行建立 Context、共享區或持鎖物件。參數允許延遲與版本合併，
不取代 Message Bus，也不保證多個 Module 同時套用更新。

**Existing config.hpp settings are not automatically connected to Param.** The two
[Publisher defaults](../message_bus/PUBLISHER_DEFAULTS.md) are explicitly wired
through a framework consumer. The [service network settings](../shell/NETWORK_PARAMS.md)
also connect Socket Shell IP/port and UI port; other settings, including Shell timeouts,
keep their existing CLI/config path. The three [file Log settings](../log/FILE_PARAMS.md)
use Limited/App declarations and take effect only on the next App startup.

**既有 `config.hpp` 設定不會自動接 Param。** 目前將上述兩項 Publisher 預設由
框架消費端明確接入；另有上述服務網路設定接上 Shell IP／port 與 UI port。
另有三項 [檔案 Log 設定](../log/FILE_PARAMS.md) 使用 Limited／App，下次 App 啟動套用；
其餘設定（包括 Shell timeout）維持原有 CLI／config 流程。

<a id="module-api"></a>

## Module API / Module 介面

Available from on_start. The function is register_param because register is a C++ keyword.

從 `on_start()` 起可使用；`register` 是 C++ 關鍵字，因此實際函式名稱為 `register_param`。

```cpp
vinc::Result<void> on_start(const std::vector<std::string>&) override {
    auto own = context.param.register_param<std::int32_t>("gain", 1);
    if (!own) {
        return own;
    }

    auto watched = context.param.register_param<double>("camera.exposure");
    if (!watched) {
        return watched;
    }

    return context.param.on_change([this](const vinc::param::ParamMap& values) {
        const auto found = values.find("camera.exposure");
        if (found == values.end()) {
            return;
        }

        auto exposure = found->second.get<double>();
        if (!exposure) {
            // Application policy handles an incompatible watched type.
            return;
        }

        exposure_ = *exposure;
    });
}
```

| API | Behavior |
| --- | --- |
| `register_param<T>(name, default, options = {})` | Register own name/default/options (Limited + None by default); automatically watch on success |
| `register_param<T>(name)` | Watch a full foreign/own name or an own short name |
| `set<T>` / set_value | Request central mutation with exact type; no implicit save |
| update(name, destination) | Copy local cache only; missing/type failure preserves destination |
| `read_current(name)` | Read an independent committed Entry through Host; no watch, callback or cache update |
| on_change(callback) | One complete-cache callback; empty function disables it |
| watches() | Local expected type, presence, and compatibility |

| API | 行為 |
| --- | --- |
| `register_param<T>(name, default, options = {})` | 登記自己的簡稱、預設值與選項（預設 Limited + None）；成功後自動監聽 |
| `register_param<T>(name)` | 登記監聽；完整名稱可指向其他 Module，簡稱指自己 |
| `set<T>(name, value)`／`set_value(name, Value)` | 向中央請求修改，型別必須完全相符；不隱含 save |
| `update(name, destination)` | 只讀本地快取並複製至變數；不存在／型別不同回 Result，保留原變數 |
| `read_current(name)` | 經 Host 取得已提交 Entry 的獨立副本，不監聽、不派送 callback、不更新快取 |
| `on_change(callback)` | 設定一個整體快取 callback；空 function 停用 callback |
| `watches()` | 本地監聽清單，包含預期型別、目前是否存在及型別是否相容 |

Recoverable operations return Result; watches is a local query without Result. Params has no public construction/copy/replacement. Headers params.hpp, types.hpp, error.hpp are included by the SDK.

所有可能正常失敗的操作回 `vinc::Result`；`watches()` 是本地查詢，不包 Result。
`Params` 不可自行建構、複製或替換；標頭入口為 `<vinc/param/params.hpp>`、
`<vinc/param/types.hpp>` 與 `<vinc/param/error.hpp>`，亦由 SDK 整合標頭帶入。

<a id="restart-hints"></a>

### Registration options and restart hints / 註冊選項與重啟標記

```cpp
// Both permission and restart hint are optional.
auto gain = context.param.register_param<std::int32_t>("gain", 1);
auto port = context.param.register_param<std::uint16_t>(
    "port", 8080, {.restart_hint = vinc::param::RestartHint::Module});
auto shared = context.param.register_param<std::int32_t>(
    "shared", 1, {.permission = vinc::param::Permission::Public,
                  .restart_hint = vinc::param::RestartHint::App});
// Check each Result before using the registered parameter.
```

`RegisterOptions` defaults to `Permission::Limited` and `RestartHint::None`.
The previous explicit third Permission argument is replaced by the options struct;
omitting it remains supported. Watching without a default has no options.

`RegisterOptions` 預設為 `Permission::Limited` 與 `RestartHint::None`，兩者都可省略。
原本第三個位置直接傳 Permission 的 C++ 呼叫須改成 options；不傳選項的用法不變。
只監聽、不提供 default 的 overload 不接受註冊選項。

| Hint / 標記 | Meaning / 含義 |
| --- | --- |
| `None` | No declared restart requirement; not a promise of immediate application. / 未宣告重啟需求，不保證立即生效。 |
| `Module` | Author says changes require restarting the owning Module. / 作者宣告需重啟所屬 Module。 |
| `App` | Author says changes require restarting the whole App. / 作者宣告需重啟整個 App。 |

Hints are advisory registration metadata, not an applied-value tracker or a
pending-restart state. The framework does not restart anything, delay delivery,
change `set()`'s Result, or save implicitly. Only the owner registers metadata;
foreign watches and Public writes cannot replace it. Invalid enum values fail
before registration/publication. A replacement MID's registration replaces the
old hint, including clearing it to None. Internal central registration accepts
None/App only: a `vinc.*` parameter has no owning Module to restart.

標記只屬於註冊 metadata，不追蹤值是否已套用，也不是「尚待重啟」狀態。
框架不自行重啟、不延遲交付、不改 `set()` 回傳、不隱含 save。只有擁有者註冊可宣告標記，
其他 Module 的監聽或 Public 寫入不能覆寫。非法列舉值在註冊／發布前拒絕。
新 MID 重新註冊時，以新標記取代舊值，也可清為 None；中央 `vinc.*` 無所屬 Module，
其內部註冊只接受 None／App。

`Entry::restart_hint` travels through Register, central A/B snapshots, Host,
`read_current()` and pump callbacks. Stop retains it for the central lifetime.
TOML intentionally stores neither hint nor permission: restarting central loads
None until the owner registers again. Do not treat a loaded None as proof that
an inactive Module's parameter supports live updates.

`Entry::restart_hint` 經 Register、中央 A/B 快照、Host、`read_current()` 與 pump callback
完整傳遞。Module 停止後中央保留；TOML 不保存標記與權限，中央重啟載入後先為 None，
等擁有者再註冊。不能把尚未註冊的 None 解讀成「此參數可即時套用」。

Python exposes optional `permission=` and `restart_hint=` keyword arguments, and
read-only `Entry.restart_hint`. Its enum is `vinc.param.RestartHint.None_` (because
None is a Python keyword), Module, App. Supplying non-default metadata to a pure
watch raises VincError rather than silently ignoring it.

Python 可省略 `permission=`／`restart_hint=`，Entry 的標記唯讀；列舉為
`vinc.param.RestartHint.None_`、Module、App（None 是 Python 關鍵字）。只監聽時若帶入
非預設的權限或標記，回 VincError，不悄悄忽略。

Delivery stages / 實作階段：

1. **Implemented:** options, native/Python registration, cross-process snapshot
   metadata, invalid-input/version rejection and source tests. / **已實作：** 註冊選項、
   C++／Python、跨程序 metadata、非法輸入／舊版拒絕及來源測試。
2. **Implemented:** Shell get/list/set reminders and native/Python examples. / **已實作：**
   Shell get/list/set 提醒及 C++／Python 範例。
3. Fresh installed-SDK acceptance for this ABI change is tracked in
   [stage 3](#restart-hints-stage-three). Earlier SDK results below predate restart hints.
   / 此次 ABI 變更的乾淨 installed SDK 驗收見[第三階段](#restart-hints-stage-three)；
   下方較早的 SDK 結果不能替重啟標記背書。

Stage 1 validation (2026-11-03, this working tree, Release):

| Environment / 環境 | Result / 結果 |
| --- | --- |
| Windows x64, MSVC 19.44.35229 / STL 14.44.35207, CMake 4.3.2, Ninja 1.13.2, CPython 3.13.12 | **7/7 passed**, 45.32 s |
| WSL Ubuntu 24.04 x86_64, GCC 13.3 / libstdc++, CMake 3.28.3, Ninja 1.11.1, CPython 3.12.3 | **7/7 passed**, 25.53 s |
| Native macOS / 原生 macOS | Pending / 待驗 |

The selection is `param.values`, `param.protocol`, `param.process`, `module.plugin`,
`module.python`, `module.python_messages`, `module.python_services`. It covers
omitted/partial options, all three hints, malformed enum/truncated registration,
previous-version rejection, native/Python current reads and callback metadata,
Public writes preserving metadata, stopped-owner retention, replacement registration,
and central restart restoring None from TOML. Existing permission, cancellation,
queue, Bus/RPC and Module cleanup checks also run. These are targeted source
Release tests, not a full-project or fresh installed-SDK acceptance run.

本次執行三套 Param 與四套 Plugin／Python 測試，涵蓋省略／部分選項、三種標記、非法列舉／
截斷註冊、舊版拒絕、C++／Python 讀取及 callback、Public 寫入保留標記、停止後保留、
重新註冊替換，以及中央重啟從 TOML 載入 None。保留並跑過既有權限、取消、佇列、
Bus／RPC 與退出回收檢查。這是來源 Release 定點驗收，不是完整專案或乾淨 SDK 驗收。
Mac 須以同版程式重跑此組；Debug 與新版 installed SDK 未於此階段驗收。

Local logs (Git-ignored): `build/param-restart/windows-final.log` and
`build/param-restart/linux-final.log`. WSL retains existing missing-field-initializer
warnings in test fixtures; neither build failed.
本機 logs 不上 Git；WSL 仍有測試 fixture 既有 aggregate 欄位省略警告，兩平台建置均成功。

<a id="restart-hints-stage-two"></a>

Stage 2 validation (2026-11-03, same Windows/WSL toolchains above, Release):

- Source CTest selection: the seven stage-1 tests plus `shell.unit`,
  `shell.integration`, `shell.socket`, `shell.tcl`. Windows **11/11** (78.70 s),
  WSL **11/11** (38.58 s). After the final test-script adjustment, `param.process`
  was rebuilt and passed again on both platforms.
- Local router, actual TCP Shell and Tcl exercise Module/App hints in get/list/set;
  None remains compact. Same-value writes report metadata, invalid/missing writes
  retain errors and do not publish, existing owners keep running, and saved values
  remain unchanged without explicit save.
- The actual repository C++/Python smoke runs eight isolated central lifetimes,
  retaining the original startup/save/correction cases. New label cases verify
  that an edit changes neither active startup_label nor MID, unsaved edits vanish
  after App restart, and explicit save/restart applies the updated label. Each
  run verifies orderly child shutdown. This is source-App acceptance, not the
  separate fresh installed-SDK stage recorded below.
- The TCP fixture originally mistook an asynchronous `[Param]` diagnostic prompt
  for its command reply; it now skips that diagnostic within the same deadline.
  The first smoke draft incorrectly parsed a MID from Tcl's result; Tcl correctly
  returns 0 and emits text separately. The revised test does not guess MIDs or
  alter Tcl behavior. Earlier failed logs are retained, not relabeled as passes.

第二階段沿用上方 Windows／WSL 工具鏈，Release 各 **11/11 通過**；最後調整測試腳本後，
另重建並通過兩平台 param.process。驗證本機 router、真實 TCP、Tcl 三條路徑，包含
Module／App 提醒、None 不多印、同值寫入、失敗不發布／不提示成功、不自動重啟或保存。
正式 C++／Python 範例 smoke 實跑八次中央生命週期，驗證 active label／MID 不偷偷變更、
未保存值於 App 重啟後消失，以及 save 後重啟才套用。每次確認子程序正常退出。

首次測試暴露兩個 fixture 假設：非同步 Param 診斷也會重畫 prompt，Tcl 成功回傳是 0。
測試已修正且重驗，不為了測試修改正式 Shell／Tcl 契約，也不猜 MID。早期失敗 log 保留。
本階段不替代原生 macOS 或乾淨 installed SDK；後者的新版驗收另見下方第三階段。

Evidence (Git-ignored) / 本機證據：
`build/param-restart-stage2/windows-final.log` and `linux-verified.log` contain
11-suite CTest summaries; `windows-verified.log` and `linux-smoke-final.log`
contain the final targeted test and revised eight-lifetime smoke results.
前兩份包含十一套摘要與較早的 smoke 失敗；後兩份才是最終定點測試及新版範例結果。

No config.hpp setting migration, new thread, new queue or automatic restart is
included. Registration and snapshots add one encoded u32 hint per entry; existing
capacity/timeout/cancellation contracts remain.
本輪不搬 config.hpp、不新增 thread／queue、不自動重啟。註冊及快照每筆多編碼一個 u32，
既有容量、逾時、取消契約不變。

<a id="restart-hints-stage-three"></a>

### Restart hints stage 3: installed SDK / 重啟標記第三階段：安裝式 SDK

2026-11-03, `codex/param-redesign-contract` working tree, Release, using the stage-1
Windows/WSL toolchains recorded above:

| Environment / 環境 | Clean SDK producer | Installed consumer / application / python |
| --- | --- | --- |
| Windows x64, MSVC 19.44, CPython 3.13.12 | Passed / 通過 | **3/3 passed / 通過** |
| WSL Ubuntu 24.04 x86_64, GCC 13.3, CPython 3.12.3 | Passed / 通過 | **3/3 passed / 通過** |
| Native macOS / 原生 macOS | Pending / 待驗 | Pending / 待驗 |

Both producers start from new build directories with SDK installation enabled,
Apps/testing disabled, and pinned dependencies. Each script installs and moves its
own SDK before building consumers. No system SDK installation is modified. This
stage changes only fixtures/documentation; it does not repeat full CTest or Debug
acceptance, migrate config.hpp settings, or change Runtime/ABI protection.

兩平台均使用新目錄建置完整 SDK，再由各腳本安裝、搬移及建置消費端，不修改系統 SDK。
本階段只補 fixture／文件，是 Release 定點整合驗收，不代表重新跑過完整 CTest／Debug，
也不搬 config.hpp、不改 Runtime／ABI 保護。

Local evidence (Git-ignored): `build/param-restart-stage3/{windows,linux}-producer.log`
and `-{consumer,application,python}.log`. Detailed Python results include
`sdk-python-test/flow-41.log`, `flow-42.log`, `missing-python-override.log` and
`wrong-abi.log`. Producers are `build/param-restart-stage3/windows-release` and
`/home/vinc-test/build/param-restart-stage3-sdk-release`. These are test artifacts,
not published SDK releases.

上述 logs／SDK 僅供本機驗收，不上 Git；正式 fixture 保留於 repository，便於三平台重跑。

The maintained `sdk/tests/installed_python` fixture builds a C++ Plugin and Python
binding using only the installed SDK. Both check omitted registration options
(Limited + None); native Module and Python App hints cross the Host protocol and
current-read path. Python also checks callback metadata and foreign Public writes
preserving None. The relocated App's Tcl script exercises Shell get/list/set;
the harness requires both hints and the explicit App-save reminder. Existing
custom-message Bus/RPC, ABI failure isolation and orderly shutdown remain required.

正式 SDK fixture 從安裝套件建置 C++ Plugin 與 Python binding，驗證兩種語言省略選項
時的 Limited + None，以及 Module／App 標記經 Host 跨程序讀取、callback 保留標記、
Public 寫入不更改標記。搬移後由 Tcl 執行 Shell get/list/set，驗收腳本必須看到
兩種重啟提醒與 App 重啟前的 save 提示；既有自訂訊息 Bus／RPC、ABI 錯誤隔離及
正常退出也必須通過。相同值 set 仍提示宣告的需求，不表示框架追蹤待重啟狀態。

#### Supplemental full Debug validation / 補充：Debug 完整驗證

2026-11-03, same working tree/toolchains as stage 3, with new Debug build trees,
`BUILD_TESTING=ON`, `VINC_INSTALL_SDK=ON` and an explicit SDK test interpreter.
The existing local parent project composes the bundled C++ App and a Python-enabled
`ParamValidation` App. CTest runs without a name filter, serially; the actual
C++/Python example smoke is run separately against ParamValidation.

| Environment / 環境 | Full Debug build | Initial CTest / 首次全套 | Eight-lifetime example smoke |
| --- | --- | --- | --- |
| Windows x64, MSVC 19.44, CPython 3.13.12 | Passed | **129/130**, 1769.73 s | Passed |
| WSL Ubuntu 24.04, GCC 13.3, CPython 3.12.3 | Passed | **136/136**, 1887.91 s | Passed |

Windows' sole initial failure was the
[Python selection diagnostic-wrap assertion](../../../tools/configure/README.md#python-probe-diagnostic-wrapping).
The real timeout was reported correctly; the test assumed `limit 10s` could not wrap.
The assertion was subsequently fixed and the complete selection suite passed on
both platforms (CTest 1/1 each). The table retains the original full-suite result;
no second full-project run or runtime change is claimed.
All Windows Param, Shell, installed SDK and Python service cases passed. WSL passed
every configured CTest case, including Python selection. Both separate eight-lifetime
example runs passed (Windows 26.77 s, WSL 11.58 s).

本次以新 Debug 目錄建置，跑未篩選、依序執行的完整 CTest，並另外執行 Python 啟用的
ParamValidation 範例八次生命週期。Windows 唯一失敗是 CMake 診斷換行造成測試誤判，
Param、Shell、installed SDK 與 Python 服務均通過。後續已修正該斷言，兩平台重跑整套
Python selection 各 CTest 1/1 通過；上表保留原始全套結果，不改記為一次全綠。
正式程式未改，也未再次跑完整專案、Release 全套或 Mac 原生驗收。
WSL 全部 136 項通過，包含 Python selection；兩平台另跑的八次生命週期範例也都通過。

Evidence: `build/param-full-validation/{windows,linux}-{configure,build,ctest,examples}.log`,
`*-list.json` (test inventory) and `*-results.json` (commands/exit status/timing).
Local build artifacts are Git-ignored. Windows build is `windows-debug` under
that directory; WSL build is `/home/vinc-test/build/param-full-validation-debug`.

Native macOS follow-up checklist / 原生 macOS 補驗清單（已完成，見 [新報告](../sdk/MACOS_PARAM_VALIDATION.md)）：

1. Use the same topic commit; record OS/CPU, Clang/libc++, CMake, Ninja and CPython.
   Rebuild App, Plugins and binding together (Plugin entry 13, bootstrap 16,
   Param wire 3). Earlier Mac results predate this change.
2. Run the eleven source suites listed in stage 2 and the repository C++/Python
   example smoke, now **eight** central lifetimes, not its previous five-run form.
   Check startup-only label application, save/no-save and child shutdown.
3. Use new Release producer/install/consumer directories. Configure
   `VINC_INSTALL_SDK=ON`, `VINC_BUILD_APPS=OFF`, `BUILD_TESTING=OFF`, build, then
   run `installed_consumer.cmake`, `installed_application.cmake` and
   `installed_python.cmake`. Use matching `COMPILER`/`CONFIG`/`PYTHON`; pass
   `SDK_BUILD`, `FIXTURE`, `GENERATOR`, `MAKE_PROGRAM`, plus
   `WORK=<SDK_BUILD>/sdk-install-test` for consumer and `WITH_UI=ON` for application.
4. Retain all three script results and Python `flow-41.log`/`flow-42.log`.
   Verify `NATIVE_PARAM_RESTART_OK`, `PYTHON_PARAM_RESTART_OK`, Module/App Shell
   reminders, missing/wrong Python isolation and normal shutdown. Do not mark Mac
   passed using Windows/WSL evidence; record Debug separately if tested.

Mac 必須以同一 commit 重建，跑第二階段十一套來源測試與目前八次生命週期範例，
再使用全新 Release SDK／consumer 跑三個安裝驗收腳本。保留環境、成功／失敗 logs、
兩種語言標記、Shell 提醒、Python 錯誤隔離及退出證據。舊 Mac 基線仍保留，
不算這次重啟標記驗收；Debug 若有執行須另列結果。

<a id="current-reads"></a>

### Current reads / 目前值讀取

```cpp
auto current = context.param.read_current("gain");
if (!current) {
    return std::unexpected(current.error());
}

auto gain = current->get<std::int32_t>();
if (!gain) {
    return std::unexpected(gain.error());
}
// Use *gain for initialization; current also owns default/permission/restart hint/version/time.
```

`Result<Entry> read_current(std::string_view name) const` is available from
`on_start()`, including immediately after successful registration or set. A short
name means this Module; a complete name may read another Module, including
Limited values, without registering a watch. Missing values return `NotFound`;
invalid names fail before Host access. Type checking is explicit through
`Entry::get<T>()`. Changing or retaining the returned Entry cannot mutate central
state, the pump cache, or a later result.

`read_current()` 可於 `on_start()` 登記成功後立刻使用，不必先 pump。簡稱指自己；
完整名稱可讀別的 Module（含 Limited），不必先監聽。不存在回 `NotFound`，非法名稱
在呼叫 Host 前拒絕；用 `Entry::get<T>()` 明確檢查型別。Entry 是獨立副本，包含
value／default／permission／restart_hint／version／timestamp；保留或修改副本不改中央或快取。

Reads always request a complete current shared snapshot via the existing Host
Access interface. They do not send a value-query RPC, alter notifications, use
pump's known revision, or suppress later callback delivery. The Host lazily
attaches/maps once, serializes its reusable copy buffer, and decodes outside the
shared lock. This currently copies/decodes the complete Param image before
selecting one Entry; it may allocate and is not a cheap cache getter.
Admission adds one condition variable and a busy flag per Host client; standard
library condition-variable bookkeeping may allocate. There is no additional
worker, polling loop, or shared-memory resource.

讀取沿用 Host 的完整共享快照，不新增查值 RPC，不更改通知或 pump revision，
也不吃掉下次 callback。首次讀取才 attach／map；Host 共用的複製 buffer 序列化使用，
解碼在共享鎖外。目前會複製／解碼整份 Param 映像後取出單筆，可能配置記憶體，
不能當作便宜的本地 getter。
Host 每個參數 client 增加一個條件變數與使用中旗標；標準庫的條件變數內部可能配置
記憶體，沒有新增工作執行緒、輪詢或共享資源。

Concurrent current reads and the single pump are permitted. Host read admission
waits at most `param_request_timeout` and observes stop; initial attachment has
its existing request timeout, and shared-lock acquisition uses
`param_lock_timeout`. These are per-stage waits, not one end-to-end deadline or
a hard bound on native mapping/allocation/decoding. Stop cancels queued admission
and transport waits; Runtime also stops ModuleLocks to cancel its active waits.
An in-flight read can complete before a racing stop/change is observed. Returned
values describe the copied snapshot, not a reservation of future state.

可與其他讀取及單一 pump 並行。Host 讀取使用權等待上限為 `param_request_timeout`，
支援停止取消；首次 attach 沿用請求期限，共享鎖沿用 `param_lock_timeout`。
這些是分階段期限，不是整個 API 的硬即時上限，也無法中斷原生 mapping／配置／解碼。
Runtime 停止時亦停止 ModuleLocks 以取消其 active wait；已進行中的讀取可能先於
並行停止或修改完成。快照不保證後續不再改變。`read_current()` 不改 callback 執行緒。

Failures use outer operation `ReadCurrent` (14); shared snapshot failures use
`ReadSnapshot` (15), preserving underlying causes. Admission/attachment waits use
`Timeout`/`Cancelled`, not mutation outcome-unknown codes. Shared-lock failures
retain the existing `StorageFailed` classification with lower-layer causes. The current-read change itself introduced no new error codes, wire fields, Access
virtual functions or Plugin layout. Subsequent [restart hints](#restart-hints)
use wire v3 and Plugin entry 13; TOML remains format 1. Updated SDK headers/archives and the
Python binding must be rebuilt together; see the [installed-SDK record](#param-redesign-stage-four).

失敗最外層 operation 為 `ReadCurrent`（14），底層快照為 `ReadSnapshot`（15），
保留原因鏈；讀取排隊／attach 逾時或取消不使用修改結果未知的錯誤。
共享鎖失敗沿用 `StorageFailed` 並保留底層原因。read_current 當時未新增錯誤碼、協定欄位或 Host／Plugin 布局；
後續[重啟標記](#restart-hints) 已改為 wire v3／Plugin entry 13，TOML 仍為 format 1；
SDK 標頭、靜態庫及 Python binding 須一併重建，乾淨安裝驗收見第四階段紀錄。

<a id="startup-and-correction"></a>

### Startup arguments and business validation / 啟動參數與業務驗證

The Module owns argument parsing and validation; the framework does not generate
`--parameter` flags. The recommended sequence is: parse args, register a fixed
code default, `read_current()`, submit an explicit override with ordinary `set()`,
validate the effective value, then initialize resources. An override is not a new
default and does not save. Without an override, use the central current value;
first-time registration supplies the default when no record exists.

Module 自行解析及驗證 args，框架不生成 `--參數名稱`。建議順序是：解析 args、註冊
固定程式預設、`read_current()`、用普通 `set()` 提交明確覆寫、驗證有效值，最後才建立
資源。覆寫不改 default，也不隱含 save；沒有覆寫時使用中央目前值，首次無紀錄時才
由註冊提供預設。

The [C++/Python user examples](../../../examples/userExampleModule/README.md#startup-parameters)
implement `--gain` with finite range 0..10 and fixed default 1.0. Malformed or
out-of-range startup args reject startup. Typed but unsuitable persisted/runtime
values are diagnosed and replaced by the owner through normal `set()`. A failed
startup correction fails startup; a failed callback correction is reported and
the example retains its last effective gain. Resource failure is separate: a
valid port failing to bind is not grounds to rewrite that parameter.

[C++／Python 範例](../../../examples/userExampleModule/README.md#startup-parameters)
示範 `--gain`、有限範圍 0..10、固定預設 1.0。錯誤啟動 args 拒絕啟動；已保存或執行中
的值若型別正確但不適用，由擁有者回報原因並以普通 `set()` 修正。啟動修正失敗即啟動
失敗；callback 修正失敗則回報並保留最後有效值。合法 port 遇 bind 失敗是資源問題，
不能當成非法值而偷偷改參數。

Correction can overwrite a newer concurrent edit. Neither a small queue nor
human input speed removes this accepted limitation. There is no expected-version
write API, implicit cached-version check, owner-approval registry or extra queue.
Successful set acknowledges central commit, not application or persistence.

修正可能覆蓋並行的新修改；小佇列與人類輸入速度都不消除此已接受的限制。
不提供 expected-version 寫入、不暗帶快取版號、不增加審核 registry 或額外佇列。
set 成功代表中央提交，不代表已套用或已保存。

<a id="命名型別與權限"></a>

### Names, types, and permissions / 命名、型別與權限

- Full names are module.param with exactly one dot; Context qualifies short names with the approved instance. Segments allow ASCII letters/digits/_/-, no slash/space/Chinese; full limit 256 bytes including Module/dot. vinc is central-reserved.
- Types: bool, fixed-width signed/unsigned 8/16/32/64, float, double, string. Native aliases follow their fixed-width type. No arrays/custom objects. Strings are valid UTF-8 <=256 bytes; values/paths may contain Chinese.
- Limited is the default: central and the registered owning MID may write; other Modules may only read. Public permits **all Modules** to write, without guaranteeing application correctness under competing changes. Both policies permit foreign reads.
- Central derives identity from authenticated Sessions; Modules cannot choose owners or register foreign schema/defaults. Foreign watches/Public writes are permitted.
- Same MID/name repeated registration returns AlreadyRegistered. A new MID with the same Module name may update default/type/permission/restart hint. Stop deletes neither parameters nor Public policy. A replacement MID must register before it can write Limited values; retired MIDs and a different Session claiming an admitted MID are rejected.
- Missing watches succeed without invented map values; later snapshots can add them. If actual type differs, map retains the actual Entry, get/update returns TypeMismatch, and watches.compatible is false; no coercion.
- Local watching is reserved before sending registration and retained on remote failure, covering uncertain committed outcomes. Missing entries remain absent; rewatch resends notification subscription.

- 完整名稱為 `module.param`，恰一個 `.`；簡稱依實際核准的 Module 名稱補全。
  Module／參數段限 ASCII 字母、數字、`_`、`-`，不接受 `/`、空白或中文名稱。
  完整名稱（含 Module、點）上限 256 bytes；`vinc` 保留給中央。
- 支援 bool、int8/uint8、int16/uint16、int32/uint32、int64/uint64、float、double、string。
  C++ 使用對應 `std::int*_t`／`std::uint*_t`，平台原生型別若為其別名，遵循相同型別。
  無陣列或自訂物件。字串限 256 bytes 且為有效 UTF-8；字串內容及檔案路徑可含中文。
- 預設 `Permission::Limited`，中央與已登記的所屬 MID 可寫，其他 Module 唯讀。
  `Permission::Public` 允許**所有 Module**修改；兩種權限都允許其他 Module 讀取，不保證多方修改的業務意圖。
- 中央由認證 Session 導出 MID／名稱。Module 不傳入自選 owner；其他 Module 的完整
  名稱只能監聽或依 Public 權限寫入，不能冒名註冊其 schema/default。
- 同一 MID 對同一參數重複登記回 `AlreadyRegistered`；新 MID 的同名 Module 可重新
  登記並更新 default／type／permission。停止不刪參數，也不撤銷既有 Public 設定。
  新 MID 須重新登記才能寫入 Limited；舊 MID 與冒用已接入 MID 的不同 Session 均被拒絕。
- 監聽尚不存在的參數成功，快取 map 不建立假值。重新取得快照後自然出現。
  監聽型別與後來的實際型別不同時，map 保留實際 Entry，`get<T>`／`update` 回
  `TypeMismatch`，`watches().compatible` 為 false；不把值強制轉型。
- 登記先保留本地監聽，再送出中央請求；遠端失敗時保留這份監聽，避免逾時但已提交的
  參數失去追蹤。不存在的值仍不出現在快取 map；重試監聽會重新送出通知訂閱。

System Modules use the same Context and permissions as ordinary Modules. Their
parameters belong to their approved instance name (for example, `ui.port`), not
a nested `vinc.ui` namespace. UI port is wired as described in the network contract above.

System Module 與一般 Module 使用相同 Context／權限，不加專用寫入後門；其參數歸屬
自己的實例名稱（例如 `ui.port`），不增加 `vinc.ui` 巢狀命名。UI port 已依上述網路契約接線。

<a id="pump快取與-callback"></a>

### pump, cache, and callbacks / pump、快取與 callback

Central commit, local cache refresh, and persistence are separate events. Successful set/registration acknowledges commit, not immediate cache mutation. Only a successful later pump makes values visible to update/callback. Intermediate 10→20→30 may coalesce into one delivery of 30.

中央提交、Module 快取更新與檔案保存是不同事件。`set`／登記成功表示中央提交完成，
不表示本地快取立即變動；使用者在下次成功 pump 後才能從 update／callback 取得新值。
只有 pump 更新快取。`10 → 20 → 30` 可合併成一次交付 30。

The default serial pump fixes Bus batch boundaries, handles ready RPC work,
refreshes Param/callbacks, then Shell hooks and Bus callbacks. With opt-in
[parallel Topic workers](../module_system/PARALLEL_PUMP.md), RPC/Param/Shell remain
mutually serial on the pump caller while Topics may overlap them. Only this caller
mutates the Param cache. No user timer API is added; one public pump remains active.

預設單執行緒 pump 先擷取 Bus 批次界線，再依序處理 RPC、Param、Shell、Bus callback。
選用 Topic worker 後，控制 callback 仍在同一 pump caller 依序執行，只有它能改快取；
Topic 可與控制 callback 並行。跨 thread 保留參數須複製，不保留借用 map。
沒有新增 timer API，同一 Context 仍只允許一個公開 active pump。

- Central hints only Modules watching changed names; hints contain no values and have no delivery guarantee. Every pump checks shared revision through Host; a successful pump begun after a change sees that version or later. No synchronized pumping or intermediate-version queue.
- Callback receives const ParamMap& containing the complete currently existing watched cache; Entry holds value/default_value/permission/version/timestamp. At most one Param callback/pass, counted in PumpStats.callbacks.
- Map/Entry references last only through the callback. Copy to retain; use update/watches elsewhere rather than saving internal pointers across threads.
- No shared/cache/control mutex is held during user callbacks. update/set/register are allowed and do not mutate the borrowed map in place. New watches/callback replacements apply in later pumps; old captures are destroyed outside locks.
- Callbacks return void; applications handle business errors. Escaping exceptions latch ModuleSystem HookException and fault exit even if pump Result is ignored; central/other Modules continue.
- on_stop must join Context-using user threads. Stop/disconnection closes new requests/cancels Host waits, without undoing prior central commits. Stuck callbacks follow stop deadlines/OS termination policy.

- 中央僅向監聽變動名稱的 Module 發送提示；提示不含參數值，也不作交付保證。
  每次 pump 均經 Host 檢查共享 revision；變更後開始且成功完成的 pump 取得該版本或較新版本。
  不要求每個 Module 同步 pump，也不保留中間版本佇列。
- 一次 callback 收到 `const ParamMap&`，是該 Context 監聽中**目前存在**的完整快取。
  Entry 包含 value、default_value、permission、version、timestamp。
  每輪最多一次 Param callback；`PumpStats.callbacks` 包含這次派送。
- map 與 Entry 的參考只保證在該次 callback 期間有效。需保留時自行複製。
  callback 之外使用 `update`／`watches`，不要保存內部 map pointer 給其他執行緒。
- 使用者 callback 執行時不持有共享鎖、快取 mutex 或控制通道 mutex。
  可呼叫 update／set／register_param；這些操作不就地修改 callback 所借用的 map。
  新登記或替換 callback 於後續 pump 生效，舊 callback 捕捉的物件於鎖外釋放。
- callback 回 void，業務錯誤由應用處理。未處理例外轉為 ModuleSystem HookException，
  鎖定該 Module 的故障退出，忽略 pump Result 也不能清除故障；中央與其他 Module 繼續。
- on_stop 必須 join 使用 Context 的自建執行緒。停止／失聯封閉新請求並取消 Host 等待；
  已執行的中央修改不因停止而撤銷。callback 若卡住，沿用 Module 停止期限與 OS 終止政策。

<a id="toml-與保存"></a>

## TOML and persistence / TOML 與保存

Central reads the following after startup and before Modules/deployment scripts:

中央 Vinc 啟動後，在啟動 Module／部署腳本前讀取：

```text
<Vinc executable directory>/vinc_data/<INSTANCE_NAME>/params.toml
```

Paths are executable-relative, not cwd, with one directory per INSTANCE_NAME.
First writable startup creates the file before registration adds framework
Publisher defaults; build generates none. Private ManagerAccess::configure_params
loads persistence and registers those defaults before Module startup/registration.

路徑相對執行檔位置，不依啟動時 current directory。不同 INSTANCE_NAME 使用不同目錄。
正常可寫環境首次執行建立檔案，再註冊並加入框架 Publisher 預設，不由 build 產生。
私有 `ManagerAccess::configure_params` 接入根目錄、載入及註冊框架預設；
它必須在 Module 啟動與使用者參數登記前呼叫。

```toml
format = 1

[params.'camera.gain']
type = 'int32'
default = 1
value = 4

[params.'camera.serial']
type = 'uint64'
default = '0'
value = '18446744073709551615'
```

The quoted full name is one key, not nested Module tables. uint64 uses decimal strings to preserve its range; other integers use TOML integers, float/double floating values, strings TOML strings. Explicit type is required, never guessed. Floating values retain round-trip precision; NaN/inf are representable without NaN-payload-bit guarantees.

完整名稱是帶引號的一個 key，不是 TOML 巢狀的 Module table。
`uint64` 用十進位字串表示以保留完整範圍；其他整數用 TOML integer，float／double
用浮點數，字串用 TOML string。型別欄位必須明寫，不從數字外觀猜測。
浮點保留 round-trip 精度；NaN／inf 可表示，不承諾保存 NaN payload bits。

Central separates registered default, file saved value, and runtime live value:

中央區分三種值：註冊 default、檔案 saved value、執行期 live value。

**English**

1. New parameters use the registered default for both default/saved and are added to the file.
2. Same-type default changes update saved only if it equaled the old default; customized saved values remain. Current live values follow the same rule independently, without persisting unsaved live changes.
3. Type changes reset only that entry default/saved/live with a diagnostic.
4. Entries not registered this run or belonging to stopped Modules remain, including on later save.
5. Explicit param save copies all current live values to saved; exit does not save automatically.

**繁體中文**

1. 新參數：default 與 saved value 都用登記值，補進檔案。
2. 同型別 default 改變：原 saved value 等於舊 default 才跟隨新 default，否則保留自訂值；
   本次中央仍存在的 live value 亦獨立依同樣規則處理，不把未保存 live value 寫入檔案。
3. 型別改變：只將該參數 default／saved／live 重設為新 default，發出診斷。
4. 舊參數本輪沒登記／Module 未啟動：繼續保留，包含下次 save。
5. `param save` 才把當下所有 live value 更新成 saved value。退出不自動 save。

Automatic file maintenance for new parameters/default changes does not save unrelated live adjustments. Users may edit saved value and restart; no live file watching/reload. Remove historical entries only while central is stopped; registered entries reappear next run.

因此「新增另一參數」或「更新 default」的自動檔案維護，不會順便保存其他未 save 的調整。
使用者可直接改檔案的 value，重新啟動後載入；執行中不監看檔案，也不自動 reload。
需要刪除歷史參數時，停止中央後編輯檔案；仍被 Module 登記的參數下次會補回。

Save runs in the same serialized central queue as mutations and captures a
consistent committed snapshot. It does not await owner validation or callbacks.
If save precedes a business correction, the file retains the prior value until
another explicit save. Correction, startup overrides and exit never imply save.
A business-invalid but correctly typed value is not whole-file corruption.

save 與修改走同一個中央序列化佇列，保存一致的已提交快照；不等待擁有者審核或 callback。
先 save、後業務修正時，檔案保留修正前的值，直到再次明確 save。修正、啟動覆寫及退出
都不隱含 save。型別正確但業務上不適用的值，不是刪除整份 TOML 的理由。

<a id="檔案失敗"></a>

### File failures / 檔案失敗

- Invalid TOML syntax/version/structure/typed values invalidates the entire file: discard, delete, recreate clean, and diagnose. Defaults arrive at registration; ordinary type/default updates follow per-entry rules above.
- Permission/read/write/replace failure diagnoses/disables file I/O for this run without assuming corruption or deleting the original. Live service continues; status reports why; restart retries loading.
- Oversized files remain intact with I/O disabled/capacity error.
- Write/flush/close a same-directory .tmp, then rename over the target without deleting it first. Failure preserves the previous complete file; later writes may replace remnants. No power-loss durable-transaction claim.
- Automatic-save failure does not undo committed shared state. A bounded latest-diagnostic slot feeds local/Socket Shell cursors; quick diagnostics may coalesce, and status retains the latest file/type-change diagnostic.

- TOML 語法、格式版本或結構／型別值不合法：整份不採用，刪除後建立乾淨檔案，提示原因。
  Module 註冊時才有 defaults。一般的「Module default/type 更新」依上述個別條目規則。
- 權限不足、一般讀寫／替換失敗：提示並停用本次 file I/O，不以 I/O 錯誤推論檔案損壞，
  不刪原檔。live 服務仍運作；`param status` 可查原因，重新啟動後才重新嘗試載入。
- 檔案超過讀取容量：保留原檔並停用 I/O，回報容量錯誤。
- 同目錄 `.tmp` 完整寫入／flush／close 後 rename 取代，不先刪目的檔。
  失敗保留上一次完整檔；下次寫入可覆寫殘留 temp。**不宣稱斷電持久交易保證**。
- 自動保存失敗不否定已成功的共享提交。中央保留有界的最新診斷槽，本地與 Socket Shell
  各依游標提示；多筆快速發生的診斷可合併，`param status` 留最後檔案錯誤／型別更新提示。

<a id="shell"></a>

## Shell / Shell 操作

Local/Socket/Tcl all use the same router:

本地、Socket Shell 及 Tcl 共用相同 router：

```tcl
param list
param get camera.gain
param set camera.gain 3
param save
param status
```

param is reserved against Module names. Set parses the registered type and leaves values unchanged on failure; central may write Limited. Follow the active Shell/Tcl quoting rules for spaces; get/list escape controls. Large lists remain bounded by Socket replies and fail rather than silently truncate; use individual get. No Socket login/encryption exists; API permissions are not a same-account native-code sandbox.

`param` 為保留命令，Module 不能使用該名稱。set 依登記型別解析，失敗不修改值；中央可改
Limited。字串含空白時使用所處 Shell／Tcl 的引號規則；get/list 將控制字元跳脫顯示。
大型 list 仍受 Socket Shell 既有回覆容量限制，可改查個別 get；不悄悄截斷成成功回覆。
Socket Shell 尚未有登入／加密；這些權限是框架 API 邊界，不是對同帳號惡意 native code 的沙箱。

`get`/`list` append `[restart: Module 'camera']` or `[restart: App]` only when
registered metadata declares it. None adds no annotation and makes no promise of
immediate application. Successful `set` retains the commit/not-saved message and
adds a restart reminder; App additionally reminds the operator to `param save`
before restarting. A same-value successful set still reports the declaration,
not a newly detected pending restart. A failed set returns its usual error without
success/reminder text. `save`, current-value delivery and lifecycle are unchanged.

get／list 只在有標記時附上 `[restart: Module 'camera']` 或 `[restart: App]`；None 不加文字，
也不宣稱立即生效。set 成功保留 committed／not saved 回覆，再提醒重啟對象；App 另提醒
重啟前用 param save 保留修改。同值 set 成功也會提示，因為這是宣告，不是新增的待重啟狀態。
失敗時只回原有錯誤，不帶成功／重啟文字；保存、交付與生命週期行為不變。

```text
VINC > param set camera.port 8081
Parameter committed (not saved)
Restart hint: Module 'camera' must be restarted to apply this parameter.
```

The same text is shown by local Shell, Socket Shell and Tcl. Tcl prints command
messages separately and still returns 0 on success; no return-value contract changes. Notices go to the requesting
command, not a new broadcast/log channel. The [C++/Python label example](../../../examples/userExampleModule/README.md#restart-hints)
shows explicit application on Module startup and the difference between Module
and central restarts.
本機、Socket、Tcl 顯示同一回覆文字；Tcl 分開輸出命令訊息，成功回傳仍為 0，不改回傳契約。
提示屬於請求者命令回覆，不新增廣播／Log 通道。上述 label 範例示範啟動才套用，
以及 Module 重啟與中央重啟的差別。

Tcl `param set` is supported after the parameter exists; it does not infer a type
or create a missing entry. For a first-ever Module launch, use Module-defined
startup args for settings needed before resource initialization. Later script
writes may override loaded values; success does not wait for Module application.

Tcl 可對已存在參數執行 `param set`，不猜型別建立不存在的參數。首次啟動前尚無紀錄時，
初始化資源需要的設定使用 Module 自訂 args；不能把「先 set、再 start」當通用首次部署
流程。後續腳本寫入可能覆蓋載入值，成功不等待 Module 套用。

<a id="接線同步與協定"></a>

## Integration, synchronization, and protocol / 接線、同步與協定

```text
Plugin Context.param (vinc::param)
    └─ Host ModuleParams (vinc::param_host)
         ├─ Socket: register / set / watch / attach; watchdog 唯一接收者
         └─ ModuleLocks lease: 唯讀共享快照 → 本地 buffer → decode
中央 ModuleManager
    ├─ ParamManager: 一條有界工作佇列 + 一條共用 worker
    ├─ Storage: OSAL SharedMemory + Concurrency 的 A/B 發布位元
    └─ Persistence: toml++，只有中央 target 相依
```

- vinc::param: types, Context facade, threadless codecs; no control service/TOML.
- vinc::param_host: shared images/reads using OSAL/Concurrency.
- vinc::param_manager: central worker/permissions/revisions/TOML, composed through designated Module detail interfaces.
- Central worker sleeps when idle. Shared waits/TOML/disk work run outside heartbeat/Manager monitoring. No Module Param thread; Host uses its watchdog.
- Commit encodes/allocates the full candidate locally before acquiring central ownership, writes the inactive slot, then atomically flips selector/releases through publish_and_unlock. Uncommitted failed/dead candidates stay invisible.
- Host preallocates a 1 MiB buffer. Under lock it checks header/copies bytes; decoding/map allocation/callback follow outside. Equal revision skips copying; images contain no native pointer/container layout.
- Each slot header is u64 revision/byte length; payload encodes wire version/global revision/count and name/type/value/default/permission/restart hint/entry version/steady timestamp with explicit Control integers.
- Revision 0 is the initial empty image; published revisions never wrap and UINT64_MAX is the uncached sentinel. Timestamp is same-host steady nanoseconds, not calendar/cross-restart ordering.
- Domain=4, wire v3: Attach 0x101, Register 0x102, Set 0x103, Changed 0x104, Watch 0x105. Validate IDs/operations and reject duplicate/unknown IDs, malformed versions, truncation/trailing bytes, bad types/permissions/restart hints. Result envelopes explicitly encode causes.
- Permission wire values are Public=1 and Limited=2; retired central-only value 0 is invalid. Param v1/v2 requests/snapshots and pre-entry-13 Plugins are incompatible. TOML format remains 1 because it stores defaults/values, not permissions or restart hints. Rebuild central, Host, Plugins and SDK consumers together.
- Before queueing, central caps request size from fixed fields/name/string limits (default 536 bytes), not the entire generic frame allowance.
- Changed carries revision hints only; shared checks recover missed hints. Unknown errors remain numeric.
- Current Plugin entry/bootstrap follow [Module System](../module_system/README.md). The Param metadata change originally used entry 13/bootstrap v16. Rebuild all connected artifacts with matching toolchain/stdlib/config/layout/CRT.
- Reclaim mutexes only after OS-confirmed exit, never heartbeat timeout. Central crash invalidates the epoch; a new [Instance](../module_system/INSTANCE.md) creates new resources, not resets old mappings.

- `vinc::param`：型別、Context facade 與無 thread 的 codec；不連入控制服務／TOML。
- `vinc::param_host`：共享映像／讀取契約，依賴 OSAL 與 Concurrency。
- `vinc::param_manager`：中央 worker、權限、版本、TOML；Module System 用指定 detail 介面組裝。
- 中央 worker 閒置時睡眠；共享鎖等待、TOML、磁碟 I/O 不在 heartbeat 或 Manager monitor 上執行。
  不新增 Module Param thread。Host 接收仍使用原有 watchdog。
- 每次提交先在本地完成整份編碼與必要配置，再取中央 owner 鎖、覆寫未發布槽，最後以
  `publish_and_unlock()` 同一 CAS 切換槽並解鎖。失敗／死亡前未提交的候選不可見。
- Host 預先配置 1 MiB buffer，持鎖期間只檢查 header／複製完整位元組，解鎖後再 decode、
  配置 map 與派送 callback。相同 revision 不複製 payload。image 不保存 C++ pointer／容器布局。
- 每個槽的 header 是 revision u64、payload bytes u64；payload 有 wire version、全域 revision、
  筆數，以及每筆 name、type/value/default、permission、restart_hint、entry version、steady-clock timestamp。
  整數使用 Control codec 明確編碼；不直接傳 native struct。
- 0 是初始空映像 revision；有效發布版本不回繞，UINT64_MAX 保留為未有快取的 sentinel。
  timestamp 是同主機 steady-clock 奈秒，不是可跨重啟排序的日曆時間。
- Control Domain=4、Param wire version=3；Attach=0x101、Register=0x102、Set=0x103、
  Changed=0x104、Watch=0x105。請求／回覆 ID 及 operation 配對，拒絕重複／未知 ID、
  畸形版本、截斷、尾端資料、非法型別／權限／重啟標記。原因鏈沿用明確欄位的 Result envelope。
- 權限編碼 Public=1、Limited=2；舊的中央唯寫值 0 無效。拒絕 Param v1／v2 請求／快照與 entry 13 以前的 Plugin。
  TOML 只保存 default／value，不保存權限或重啟標記，因此格式仍為 1；中央、Host、Plugin 與 SDK 使用者須一起重建。
- 中央在排入工作佇列前限制請求大小；上限依固定欄位與名稱／字串上限推導，預設 536 bytes。
  不讓單一畸形請求占用控制通道允許的最大 payload 容量。
- Changed 僅包含 revision 提示；丟失提示由共享版本補查恢復。未知錯誤代碼保留數值。
- 目前 Plugin entry／bootstrap 以 [Module System](../module_system/README.md) 為準；此 Param metadata 改動當時為 entry 13／v16。中央、Host、Plugin、SDK
  須一起重建，仍不承諾跨工具鏈／CRT／Debug-Release ABI。
- 上層只在 OS 確認 Module 退出後回收其鎖；heartbeat timeout 不足以回收。中央 crash
  使整世代失效，新中央依 [INSTANCE](../module_system/INSTANCE.md) 建新區，不重設舊映射。

<a id="錯誤與成本"></a>

### Errors and costs / 錯誤與成本

ErrorDomain::Param uses common Result, fixed causes, and static catalog. Outer operations reflect the API; distinct inner semantics remain causes. Capacity/type/permission/missing-value failures return Result, not framework exceptions. STL/allocator boundaries remain; worker contains standard/allocation exceptions, while callbacks follow Module fault policy.

錯誤分類為 `ErrorDomain::Param`，使用 common Result、固定原因鏈與靜態 catalog。
最外層 operation 對應呼叫端使用的 API；同元件 helper 有不同操作語意時保留於原因鏈。
容量不足、型別不符、權限不足、缺少值均為 Result，不引入 framework exception。
STL／配置器例外維持既有邊界；中央 worker 收斂配置／標準例外，使用者 callback 另依 Module 故障政策。

Mutation waits after sending return TimeoutOutcomeUnknown/CancelledOutcomeUnknown: remote execution may have happened and there is no resend. Query waits can use ordinary Timeout/Cancelled. Worker jobs retain framework-owned values/Sessions, not caller-stack references.

送出修改後的等待逾時／取消回 `TimeoutOutcomeUnknown`／`CancelledOutcomeUnknown`，
不保證遠端未執行，不自動重送；查詢等待可回一般 Timeout／Cancelled。
worker 保存的工作只包含框架擁有的值／Session，不保存呼叫端的 stack reference。

The bounded central queue commits in its actual processing order, not a global
ordering of caller timestamps. Full admission returns CapacityExceeded; rejected
work is not deferred, and admitted work is not silently dropped. This is not a
per-Module delivery queue and does not preserve every intermediate callback value.

既有中央有界佇列依實際處理順序提交，不保證跨發送端的呼叫時間排序。滿載明確回
CapacityExceeded，拒絕的工作不延後偷偷執行，已接受工作不默默丟棄。它不是各 Module
的逐筆交付佇列，不因此保留每個中間 callback 值。

| config | Default |
| --- | --- |
| param_name_bytes / param_string_bytes | 256 bytes each |
| param_entry_limit | 1024 central entries / watches per Context |
| param_slot_bytes | 1 MiB per slot plus 16-byte header; two slots |
| param_file_bytes | 4 MiB TOML read/write |
| param_pending_limit | 32 Host requests |
| param_queue_limit | 256 central jobs |
| param_request_timeout | 10 s |
| param_lock_timeout | 2 s, cancellable |
| param_wait_poll_interval | 10 ms cancellation checks for active central waiters |

| config 設定 | 預設 |
| --- | --- |
| `param_name_bytes`、`param_string_bytes` | 各 256 bytes |
| `param_entry_limit` | 1024 個中央條目／每 Context 監聽上限 |
| `param_slot_bytes` | 每槽 1 MiB，另有 16 bytes header；共兩槽 |
| `param_file_bytes` | TOML 讀寫 4 MiB 上限 |
| `param_pending_limit` | 每 Host 32 個並行請求 |
| `param_queue_limit` | 中央 256 個待處理工作 |
| `param_request_timeout` | 請求等待 10 秒 |
| `param_lock_timeout` | 共享鎖等待 2 秒，可取消 |
| `param_wait_poll_interval` | 中央命令的 active waiter 每 10ms 檢查取消 |

Two shared slots serve all parameters and consume one module_lock_limit mutex. Each reading Host keeps a 1 MiB copy buffer/map; central retains live/saved/candidate state. Updates reencode the full image; pumping checks global revision. This version favors simplicity, stability, and inspectability, not high-frequency/no-allocation claims. Context caches only watched entries.

共享區固定兩槽，不會為每個參數建立 OS 資源；占用中央 `module_lock_limit` 中一把 mutex。
每個開始讀取的 Host 有 1 MiB 複製 buffer 與本地 map；中央持有 live／saved 狀態，提交時
另配置候選。更新會重編碼全映像，泵送也會檢查全域 revision；第一版偏重簡單、穩定與可檢查性，
未宣稱高頻更新或無配置。只對使用中的監聽集合保留 Context 快取。

<a id="驗證與待處理事項"></a>

## Recorded validation and outstanding issues / 驗證與待處理事項

<a id="param-redesign-stage-one"></a>

### Redesign stage 1: permissions and identity / 重設計第一階段：權限與身分

Validated on 2026-11-03 from the `codex/param-redesign-contract` working tree
(based on `25919bd`, changes not yet committed). These are targeted source-build
results, not a new full-suite or installed-SDK certification.

| Environment / 環境 | Configuration / 組態 | Result / 結果 |
| --- | --- | --- |
| Windows x64, MSVC 19.51.36257, MSVC STL 14.51.36231 | Debug, native | Param three suites + `module.plugin`: **4/4** |
| Windows x64, MSVC 19.44.35229, MSVC STL 14.44.35207, CPython 3.13.12 | Release, Python static CRT | Param three suites + Plugin + Python lifecycle/messages/services: **7/7** |
| WSL Ubuntu 24.04 x86_64, GCC 13.3 / libstdc++, CPython 3.12.3 | Release | Same seven suites: **7/7** |

The tests cover owner/central Limited writes, foreign Limited rejection, Public
writes, retained values across restart, replacement-MID registration, old-MID
rejection before and after enqueue, and refusal to switch an authenticated MID
to another live Session. Wire tests reject version 1 descriptors, requests and
snapshots, retired permission 0, and unknown permission values; Plugin tests
reject entry version 11. Existing persistence, coalescing, Tcl/Socket Shell and
shutdown tests also run through the changed permissions.

The Windows C++/Python user-example assertions also passed after completing the
deployment in an external fixture variant. The original fixture still omits
Plugin/UI resources; [its outstanding issue](../../../examples/userExampleModule/README.md#macos-deployment-fixture-gap)
records this distinction. It is not counted among the seven passing CTests.

本次驗證權限、停止／重啟後接手、已排隊舊 MID 失效及 Session 不得冒用；
C++／Python 真實跨程序測試均通過。Param wire／共享映像已升為 v2，Plugin entry
升為 12；TOML format 仍為 1。中央、Host、Plugin 與 SDK 消費端須一起重建。
普通 `set()`、快取及合併 callback 語意保留，未增加版本條件或新佇列。

Reproduce against the matching build with:

```sh
ctest --test-dir <build> -C Release -R '^(param[.].*|module[.]plugin|module[.]python|module[.]python_services|module[.]python_messages)$' --output-on-failure
```

The Release validation used a separate Python-enabled App configuration; the
bundled App profile was not changed. Windows MSVC 19.51 Python validation was
blocked by the existing static-import policy, recorded in the
[Python outstanding issue](../module_system/python/README.md#msvc-1951-static-import-review).
No Runtime guard was relaxed. Native macOS validation remains pending;
subsequent installed-SDK integration is recorded in [stage 4](#param-redesign-stage-four).

Release 使用隔離的 Python App 設定，未改內建 App profile。MSVC 19.51 的 Python
產物遭既有靜態匯入檢查拒絕，改用 19.44 完成驗收；相關疑慮已記錄於 Python README，
未放寬 Runtime 防護。Mac 原生驗收待補，後續乾淨 installed SDK 整合見第四階段；
上述是第一階段紀錄；第二階段 `read_current()` 見下節，沒有搬移 `config.hpp` 系統設定。

<a id="param-redesign-stage-two"></a>

### Redesign stage 2: current reads / 重設計第二階段：目前值讀取

2026-11-03, same topic working tree and source-build environments as stage 1.
Final validation after the reader-admission helper and fixture correction:

| Environment / 環境 | Configuration / 組態 | Result / 結果 |
| --- | --- | --- |
| Windows x64, MSVC 19.51.36257, MSVC STL 14.51.36231 | Debug, native | Param three suites + `module.plugin`: **4/4**, 30.83 s |
| Windows x64, MSVC 19.44.35229, MSVC STL 14.44.35207, CPython 3.13.12 | Release, Python static CRT | Param three suites + Plugin + Python lifecycle/messages/services: **7/7**, 45.95 s |
| WSL Ubuntu 24.04 x86_64, GCC 13.3 / libstdc++, CPython 3.12.3 | Release | Same seven suites: **7/7**, 25.13 s |

Use the stage-1 CTest selection above for Release; native Debug selects
`^(param[.].*|module[.]plugin)$`. These are targeted source-build tests, not a
full-project test run or an installed-SDK certification.

以上為最終程式碼的針對性來源建置驗收，並非全專案測試或 installed SDK 認證。
Release 沿用上一節的 CTest 選取規則；原生 Debug 只選 Param 三套與 Plugin。

The focused tests now also exercise real Host/Plugin and Python current reads:

- Before the first pump: current/default/permission/type/version are available;
  the ordinary cache remains missing. After set: current sees the new commit
  while update still sees the prior cache until pump.
- Own short/full names and unwatched foreign Limited reads, independent Entry
  ownership, missing/invalid names, unbound access, rejected snapshot sentinel,
  and structured Host/native error causes with `ReadCurrent` operation text.
- Concurrent readers, Python reads alongside pump with GIL release, reentrant
  callback reads, unchanged watch counts and subsequent callback delivery.
- Pending Attach cancellation, cancellation of a queued reader independently
  of the active lock waiter, post-stop rejection, and an unanswered Attach
  reaching the real `param_request_timeout` without outcome-unknown semantics.

The Python fault fixture now accepts its exact expected callback fault even if
it occurs before `wait_started()` observes Running; it still verifies Failed,
the expected operation, traceback, and survival of the peer Module. No Runtime
failure policy was changed and no arbitrary startup failure is accepted.

第二階段驗收包含：第一次 pump 前讀取、set 後快取尚未更新、未監聽的其他 Module
參數、獨立副本、型別／預設／權限／版本、錯誤鏈、並行／callback 內讀取、停止取消
及真實 Attach 逾時。Python 故障 fixture 修正了「刻意 callback 故障必須晚於
wait_started 成功」的時序假設，仍核對故障 operation、traceback 與其他 Module 存活，
沒有放寬正式 Runtime 政策。

Native macOS checks remain pending. Subsequent installed-SDK acceptance is recorded in stage 4;
usage/default-override/save workflows are recorded in stage 3. The existing MSVC 19.51
static-import issue and example deployment-fixture gap remained separate stage-1
records in stage 2; the fixture is subsequently repaired in stage 3 below.

Mac 原生驗收仍待補；後續乾淨 installed SDK 見第四階段，啟動覆寫／保存使用流程見第三階段。
第二階段當時未更動 MSVC 19.51 靜態匯入與範例 fixture；fixture 的後續修正見下方第三階段。

<a id="param-redesign-stage-three"></a>

### Redesign stage 3: startup and persistence / 重設計第三階段：啟動與保存

2026-11-03, `codex/param-redesign-contract` working tree based on `25919bd`.
The C++/Python user examples now parse `--gain`, retain a fixed 1.0 default,
read current state before opening resources, and show owner-side correction.
No central persistence algorithm, queue policy, runtime parameter configuration,
Plugin ABI or wire version changed in this stage. A private test friend admits
gated jobs to the existing worker; it adds no production state or scheduling.

本階段更新 C++／Python 使用範例、Tcl／Shell 說明與測試；中央保存演算法、佇列政策、
系統設定、Plugin ABI 及 wire 版本均未更動。private 測試 friend 只讓 fixture 對既有
worker 放入閘門工作，沒有新增正式狀態或排程機制。

| Environment / 環境 | Focused validation / 針對性驗收 |
| --- | --- |
| Windows x64, MSVC 19.51.36257 / STL 14.51.36231, Debug | Param **3/3**, 19.46 s; updated C++ example built |
| Windows x64, MSVC 19.44.35229 / STL 14.44.35207, Release, official CPython 3.13.12 | Param + Plugin + Python lifecycle/messages/services **7/7**, 46.28 s; repository C++/Python smoke **passed**, five central lifetimes |
| WSL Ubuntu 24.04 x86_64, GCC 13.3 / libstdc++, Release, CPython 3.12.3 | Same seven suites **7/7**, 25.56 s; repository C++/Python smoke **passed**, five central lifetimes |

These are source-build results, not full-project or clean installed-SDK results.
Repeat the stage-1 Release CTest selection above; Debug selects `^param[.].*`.
以上為來源建置的針對性驗收，不是全專案或乾淨 installed SDK 認證；Release 的 CTest
選取規則沿用第一階段，Debug 選 `^param[.].*`，完整範例執行方式見下方。

Coverage includes:

- `param.process`: clean first startup rejects set-before-registration; args
  override current but not default/saved; same-central restart retains live;
  new-central restart restores saved. A stopped owner permits save of a typed
  but business-invalid value; owner startup corrects live, and only another save
  changes the file. Existing historical-entry/default-update/coalescing tests remain.
- `param.protocol`: a promise gate holds the worker while exactly
  `param_queue_limit` jobs are admitted; set/save then fail with CapacityExceeded.
  Accepted jobs drain in order, rejected writes never appear, and admission
  resumes. Guarded release/join protects test captures on assertion failure.
- The actual `examples/userExampleModule/tests/smoke.py` runs C++ and Python in
  five central lifetimes. It covers invalid args, explicit overrides, saved
  values, offline edits, startup and callback correction, no implicit save,
  explicit correction save and inactive-entry preservation. Param ordering uses
  completed commands and pump batches, not assumed sleep durations. Preexisting
  Bus/Manual smoke checks retain their short scheduling waits.
- The smoke deployment now includes Plugin directories and UI assets only;
  it never copies developer params/logs. This closes the earlier fixture omission
  for the validated platforms, not a runtime deployment change.

驗收涵蓋首次註冊、啟動覆寫／default 不變、live／saved 分離、停止後 save、啟動修正、
再次明確保存、歷史條目保留、既有 default 維護及 callback 合併。佇列測試以 promise
閘門固定順序，驗證滿載拒絕、已接受工作不遺失、拒絕寫入不延後出現及恢復接單。
完整範例實跑五次中央生命週期，兩種語言均測首次讀取、非法 args、離線改檔與 callback
修正；Param 不以 sleep 假設順序。原有 Bus／Manual smoke 的短排程等待仍保留。

The Release example run uses an isolated source App selecting both native
examples and Python, invoking the unchanged repository smoke script directly:

```sh
<matching-cpython> examples/userExampleModule/tests/smoke.py <App-executable> examples/userExampleModule
```

Release 使用隔離來源 App 選入兩個 C++ 範例與 Python，直接執行儲存庫內的 smoke.py，
未使用外部修補版，未改內建 App profile。Windows Python 必須使用與 binding 匹配的
官方 CPython；本機 PATH 的 MinGW 3.14 會被既有 ABI 防護拒絕。WSL 共用 Windows 來源
目錄的 node_modules 時，另補入 lockfile 同版本的 Linux Rolldown／LightningCSS
原生套件；未改 package.json／lockfile。這些環境準備不放寬框架檢查。

Native macOS must rerun the three Param suites, four Plugin/Python suites and
the updated smoke fixture. Clean installed-SDK acceptance is recorded in stage 4.
The separate MSVC 19.51 static-import issue is still open.

macOS 須重跑三套 Param、四套 Plugin／Python 及新版 smoke fixture；乾淨 installed SDK
見第四階段。MSVC 19.51 Python 靜態匯入檢查疑慮仍未解決，本階段沿用 19.44 驗收。

<a id="param-redesign-stage-four"></a>

### Param redesign stage 4: installed SDK / 第四階段：安裝式 SDK

2026-11-03, `codex/param-redesign-contract` working tree (not yet committed):

| Environment / 環境 | Result / 結果 |
| --- | --- |
| Windows x64, MSVC 19.44.35229 / STL 14.44.35207, CMake 4.3.2, Ninja 1.13.2, CPython 3.13.12 | Fresh Release SDK build and all three installed scripts **passed** |
| WSL Ubuntu 24.04 x86_64, GCC 13.3 / libstdc++, CMake 3.28.3, Ninja 1.11.1, CPython 3.12.3 | Fresh Release SDK build and all three installed scripts **passed** |
| Native macOS / 原生 macOS | Pending for this Param change / 本次 Param 變更待驗 |

Both producers use new build directories, `VINC_INSTALL_SDK=ON`,
`VINC_BUILD_APPS=OFF`, `BUILD_TESTING=OFF`, and existing pinned dependencies.
The three maintained CMake scripts run directly against freshly installed/moved
SDKs: `installed_consumer`, `installed_application`, `installed_python`. This is
targeted Release integration acceptance, not a new full-project CTest run. Source
Debug/Release evidence above remains applicable: stage 4 changes only fixtures
and documentation, not runtime implementation or config.hpp.

兩平台使用全新 SDK-only Release build，沒有沿用前輪 SDK 產物；安裝、搬移後直接執行
三套現有 CMake 驗收腳本。這是針對性整合驗收，不冒充重新跑完整 CTest。來源
Debug／Release 結果沿用上方第一至三階段證據；本階段只更新 fixture／文件，
沒有改正式執行邏輯或 config.hpp，也沒有放寬 Runtime／ABI 防護。

Coverage also retains relocated custom-message Bus/RPC, UI HTTP and Recorder,
incremental/invalid/recovered message generation, missing/mismatched Python
isolation, resource repair and managed-file cleanup. Native peers survive Python
startup rejection and all successful App runs report orderly child shutdown.
Windows additionally rejects missing static Python archives and mixed CRT usage.

既有驗收同時涵蓋搬移後自訂訊息 Bus／RPC、UI HTTP／Recorder、訊息增量／無效／修復、
Python 路徑／ABI 錯誤隔離、部署修復及受管理檔案清理。Python 啟動遭拒時 C++ Module
仍可回應，成功流程確認子程序正常退出；Windows 另測缺失靜態庫與混用 CRT 拒絕。

Local evidence is ignored by Git: `build/param-stage4/{windows,linux}-producer.log`
and the corresponding `-{consumer,application,python}.log` summaries. Detailed
Python logs are in each producer's `sdk-python-test/`, including `flow-41.log`,
`flow-42.log` and `wrong-abi.log`. Windows producer: `build/param-stage4/windows-release`;
WSL producer: `/home/vinc-test/build/param-stage4-sdk-release`. These are local
validation artifacts, not published SDK releases. The MSVC 19.51 static-import
concern remains separate; these Windows results use 19.44.

上述 logs 與 SDK 均為 Git 忽略的本機驗收產物，未發布；Windows 使用 19.44，
不代表已處理另列的 MSVC 19.51 靜態匯入檢查疑慮。

The acceptance fixture in `sdk/tests/installed_python` compiles a C++ Plugin and
Python binding against the installed package. Both read current parameters before
their first pump. It verifies empty pump caches, independent Entry copies, owner
writes to Limited parameters, rejection of foreign Limited writes, and foreign
Public writes. Both success markers are required in the relocated App flow.

`sdk/tests/installed_python` 從安裝套件建置 C++ Plugin 與 Python binding，兩者都在
第一次 pump 前讀取參數。驗證快取未被填入、Entry 獨立副本、自己可寫 Limited、其他
Module 寫 Limited 遭拒、其他 Module 可寫 Public；搬移後 App 必須輸出兩種語言的成功標記。

At the original stage-4 checkpoint, native macOS was pending. Subsequent source and
Debug/Release clean-SDK acceptance are recorded in the [Mac report](../sdk/MACOS_PARAM_VALIDATION.md),
covering the later restart-hint ABI as well. Earlier results retain their tested revisions.

第四階段原始輪次尚缺 Mac；後續已補來源及 Debug／Release 乾淨 SDK，並涵蓋較新的重啟標記 ABI，
見上述報告。舊紀錄保留原版本與範圍，不改寫為當輪已通過。

### Earlier validation and retained limits / 先前驗收與既有限制

- param.values/protocol/process cover types/limits/TOML preservation, versions/truncation/causes/replay, A/B commit, stop cancellation, real Hosts/Plugins, permissions/missing values/coalescing, stop/restart/default/type changes, Tcl/real TCP Shell.
- Windows x64/MSVC 19.51: Debug full **64/64** (224.37 s), then Param **3/3** after final request-bound/operation changes; final Release full **64/64** (189.44 s). No native Linux/macOS verification in that round.
- Synchronous OS/filesystem I/O may block indefinitely; caller waits are bounded and do not occupy heartbeat, but worker join may wait for native I/O. No isolated disk process or unproven hard deadline was introduced.
- File rewrites lose manual comments/formatting. No runtime reload, deletion API, version history, or cross-Module atomic application.
- FrameworkLoop treats pump errors through main Result failure policy; Manual may handle recoverable Result. Callback exceptions latch faults and cannot be ignored away.

- 已增加 `param.values`、`param.protocol`、`param.process`：型別／容量、TOML 往返與失敗保留、
  版本／截斷／原因鏈／重放、A/B 提交、停止取消、真實 Host/Plugin、權限、缺少參數、
  合併 callback、Module 停止／中央重啟、type/default 更新、Tcl 與真實 TCP Shell。
- Windows x64／MSVC 19.51：Debug 全套 CTest **64/64 通過**（224.37 秒）；
  最後補強請求容量與外層 operation 後，重新建置並重跑 Param **3/3 通過**。
  Release 最終完整建置及全套 CTest **64/64 通過**（189.44 秒）。
  後續平台建置與行為驗證範圍見[平台報告](../../../README.md#validation-and-limits)。
- 已確認限制：作業系統／檔案系統 I/O 卡住時，C++ 同步磁碟工作無法安全強制取消；
  呼叫者等待有界且不占 heartbeat，但 join worker 的關閉仍可能等候原生 I/O 返回。
  未將 worker 另放隔離程序，也沒有以無證據的硬期限宣稱處理此 OS 限制。
- 已確認限制：檔案重寫不保留手工註解／排版；無執行期 reload、參數刪除 API、保存歷史版本
  或跨 Module 同步套用交易。後續擴充須另行設計與授權。
- 已確認限制：FrameworkLoop 的 pump 若回可恢復錯誤，沿用 `main()` Result 故障政策；
  Manual 可自行處理 Result 再繼續。callback 例外已鎖定故障，不能以忽略 Result 規避。
