<a id="互動式-shell"></a>

# Interactive Shell / 互動式 Shell

Shell is central control. Commands route Manager→control Socket→BaseModule hook; Bus payload never traverses Shell/Socket.

Shell 是中央的操作介面，模組命令仍經 `ModuleManager → control Socket → BaseModule virtual hook`。
訊息 Message Bus payload 不經 Shell 或 Socket 傳遞。

Default [Socket Shell](SOCKET.md) listens 127.0.0.1:5500 for PuTTY Raw. That guide owns CLI/recovery/capacity.

Vinc 預設開啟 `127.0.0.1:5500` 的 [Socket Shell](SOCKET.md)，可用 PuTTY Raw 連入；
啟動參數、Session 恢復與容量限制見該文件。

<a id="元件與中央程式"></a>

## Component and central application / 元件與中央程式

Internal Shell/SocketShell headers live in detail and target vinc::shell. SDK exposes socket options/errors only, not service lifecycle. Parser/router/script/Tcl integration also live in detail; apps/vinc owns central main/catalog, while this component owns reusable logic.

Shell 的框架接線標頭為 `<vinc/shell/detail/shell.hpp>`／`<vinc/shell/detail/socket_shell.hpp>`，target 為 `vinc::shell`。
SDK 只包含 `<vinc/shell/socket_shell_options.hpp>` 的設定資料與錯誤 enum，不公開服務啟停介面。
命令解析、router、script service 與 Tcl 接線標頭在 `include/vinc/shell/detail/`。
中央 main 與 Module catalog 的組裝已移到 `apps/vinc`；本元件維護可重用邏輯。

Directory/files use shell, internal class vinc::Shell, helpers namespace vinc::shell, library vinc_shell/vinc::shell. Executable target shell outputs Vinc/Vinc.exe, optionally renamed. [Application settings](../../../apps/vinc/README.md#打包應用程式的啟動腳本) owns embedded scripts/CLI policy.

元件目錄與主檔名統一為 `shell`，框架內部類別為 `vinc::Shell`。
輔助型別的 `vinc::shell` 命名空間、`vinc_shell`／`vinc::shell` library target 保持原名。
中央程式 target 為 `shell`，預設輸出檔名為 `Vinc.exe`／`Vinc`；可由應用配置更名。
內嵌啟動腳本的 CMake 入口與 CLI 政策見 [中央應用設定](../../../apps/vinc/README.md#打包應用程式的啟動腳本)。

<a id="本機命令流程閱讀入口"></a>

### Local command reading order / 本機命令流程閱讀入口

- command_line validates size, quotes/escapes, then argument count; LineEditor owns history/drafts/UTF-8 tail editing.
- Router resolves builtins/legacy/Module/ModuleType with named help/dispatch/completion helpers; completion keeps its own search order.
- Shell submit separates Session-local/asynchronous jobs; collect handles output/script/shutdown/command/completion results; handle_key handles events. Shutdown waits continue collecting with no new global deadline.
- Construction stages output subscription/reserved Tcl names/job storage. Members clean on failure; Manager retains reservations until its destruction.

- `src/command_line.cpp`：先檢查輸入大小，再解析引號／跳脫，最後檢查參數數量；
  `LineEditor` 獨立處理歷史草稿與 UTF-8 行尾。
- `src/command_router.cpp`：`execute()` 依序選擇 builtin、legacy command、Module、ModuleType；
  help 組裝、各類命令執行、補全候選收集分別由具名 helper 處理。補全保留自己的原有查找順序。
- `src/shell.cpp`：`submit()` 區分本機 Session 命令與非同步工作；`collect()` 依序收取
  Module 輸出、腳本結果、關閉要求與命令／補全結果；`handle_key()` 負責終端事件。
  `wait_shutdown()` 持續收取結果並等待程序退出，沒有新增整體關閉逾時。
- Shell 建構的輸出訂閱、Tcl 命令保留、工作儲存配置分成具名階段；訂閱由成員持有並於
  建構失敗時回收，名稱保留仍由 ModuleManager 持有，直到 manager 析構。

Config centralizes line/args/history/jobs/batches/deadlines/polling plus Socket/script settings, retaining values. Shell publicly depends on Common; session identities/display constants remain local.

本機輸入／參數／歷史／工作數上限、輸出批次、命令／補全期限與輪詢間隔集中於
`<vinc/config.hpp>`；`vinc::shell` 直接公開相依 `vinc::common`。
數值沿用原實作；Session 身分與終端顯示常數留在 Shell。
SocketShell 與腳本設定也集中於同一標頭，沿用原值；詳細流程如下。

<a id="遠端與腳本流程閱讀入口"></a>

### Remote and script reading order / 遠端與腳本流程閱讀入口

- Socket construction validates options/address/listener/poller/session storage before worker; members own failure cleanup. loop advances results/interests/disconnect deadlines then handles read/send/errors. Session commands handle recovery/disconnect/scripts/output; general work remains async.
- Remote collection takes command results, script results, then Module output before shutdown completion. Network thread owns session state, atomic flags coordinate stop. Partially sent reply items retain their full queue budget.
- ScriptService serializes completed work/owner mailboxes; take consumes collected output, retaining active owners and final tails.
- TclRuntime preserves interpreter/guard order. Named setup/argument/after/puts/dispatch helpers separate native bridges. Published interpreter pointer is withdrawn under the same mutex before destruction/thread finalization.
- Socket/Tcl/integration/console/terminal/helper tests have named scenarios with original scopes/order/assertions/policies.

- `src/socket_shell.cpp`：建構依序驗證設定、解析位址、開啟 listener、登記 poller、
  保留 Session 儲存，最後啟動 worker；資源由成員持有並於建構失敗時解構。
  `loop()` 驅動 `advance_session()` 與 `handle_ready_event()`；前者收取結果、
  更新收送關注及斷線期限，後者依序處理讀取、傳送及連線錯誤。
  `handle_session_command()` 處理恢復、斷線、腳本狀態與輸出控制；一般命令仍非同步派送。
- `collect()` 固定依序收取命令結果、腳本結果、模組輸出，最後發出關機完成訊息。
  各 Session 的狀態由網路執行緒維護；跨執行緒關機要求使用原有 atomic。
  回覆佇列仍計入正在部分傳送的完整項目，不因已送出部分內容而提早釋放預算。
- `src/script_service.cpp`：控制操作持有 `mutex_` 收取已完成工作、處理 owner 信箱；
  `take()` 消費已收取內容，但執行中的 owner 保留信箱。工作結束後再次收取尾端輸出。
- `src/tcl_runtime.cpp`：`run()` 保持 interpreter 與 guard 的建立／銷毀順序；
  `install_commands()`、`install_arguments()` 負責環境設定，`handle_after()`、
  `handle_puts()`、`dispatch_command()` 區分原生命令橋接責任。
  interpreter 的公開指標仍在同一 mutex 保護下借出／解除，先解除再銷毀，最後清理 Tcl thread。
- `tests/`：Socket、Tcl、整合測試依情境拆成具名函式；Console、Terminal、Module helper
  與 InputPipe 展開控制流程和清理物件，保留既有測試順序、工作負載、斷言與失敗政策。

Default 8 live connections imply poller capacity 9, retained Sessions up to 16 plus one recovery-only entrance, and 17 script mailboxes including local owner. These name existing budgets, not runtime Param options.

設定中的連線上限、poller 容量、保留 Session 數與腳本信箱數有相依關係：
預設 8 條連線，poller 額外登記 1 個 listener；一般 Session 最多保留連線數的 2 倍，
另留 1 個僅供恢復的入口。腳本信箱最多 17 個，涵蓋保留的遠端 owner 與本機 owner。
這些是既有容量政策的具名化；沒有增加執行時 Param 功能。

<a id="分工與所有權"></a>

## Responsibilities and ownership / 分工與所有權

- apps main initializes Tcl/Manager/Log/Param, reserves Shell names, registers catalog/scans Python, and handles startup scripts.
- CommandRouter dispatches/results/completion; Shell owns help/exit/srun/scancel.
- TclRuntime creates one isolated interpreter per run on its worker, with cancellation/bounded output.
- ScriptService shares one job across local/remote owners and routes output/results/disconnect cancellation.
- SocketShell owns OSAL networking, whole-line input, recovery, send bounds/subscriptions.
- command_line handles quotes,128 history entries,4096-byte input/UTF-8 tail editing.
- Shell owns terminal loop/jobs/results/complete shutdown.
- OSAL Terminal owns native modes/keys/UTF-8/EOF/stop signals. Input/stop handling activates before startup scripts and restores on exit.

- `apps/vinc/src/main.cpp`：初始化 Tcl、建立 ModuleManager／Shell、先保留命令名稱再登記 CMake 模組目錄，處理 `-S`。
- `CommandRouter`：共用命令派送、完整錯誤與補全結果；Shell 管理 `help`／`exit`／`srun`／`scancel`。
- `TclRuntime`：每次腳本建立獨立 Tcl interpreter，工作執行緒持有、執行及銷毀；支援取消與有界輸出暫存。
- `ScriptService`：本機與遠端共用一個 Tcl 工作；以 Session ID 路由輸出、結果與斷線取消。
- `SocketShell`：OSAL TCP／Poller 收送、整行輸入、Session 恢復、有界傳送與輸出訂閱。
- `command_line`：引號與參數解析、128 筆歷史、4096-byte 輸入上限與 UTF-8 行尾編輯。
- `Shell`：終端事件迴圈、命令工作與補全結果顯示、完整關閉流程。
- OSAL `Terminal`：Windows console／POSIX terminal 的模式管理、按鍵、UTF-8、EOF、SIGINT／Ctrl+C。
  終端與停止事件處理在啟動腳本之前啟用，離開時還原。

Startup order is Tcl→Manager→Log/Param persistence→Shell/name reservations→catalog/Python discovery→Socket Shell→startup script/interaction. Scripts/network are defaults; CLI selects IP/port/timeouts, never public init/start calls or enable switches. SDK include and Module children start no central Shell.

中央啟動順序為 Tcl → Manager → Log／Param 保存設定 → Shell／命令保留 → catalog／Python 掃描 → Socket Shell → 啟動腳本／操作。腳本與網路是預設功能，使用者以 CLI 調整 IP／port／timeout，不呼叫初始化或啟停服務、不增加服務開關。SDK include 與子程序不啟動中央 Shell。

Internal APIs remain in detail without old public forwarding headers. Shell::create returns an owner borrowing Manager/output, both longer-lived. Tcl initialization returns internal Result on missing resources, no singleton. One control thread serializes Shell UI/process_line/run/destruction; Module work uses internal workers. Only one OSAL Terminal may own stdin; second open is Busy.

上述函式保留作框架內部接線，標頭位於 detail，不提供舊公開標頭的相容轉接。
`Shell::create(manager, output)` 回傳 Result owner，明確借用 manager 與輸出；兩者必須
比 Shell 活得久。中央先初始化 Tcl，缺少資源可由內部 Result 回報；沒有 singleton。
Shell 的 UI／process_line／生命週期操作由同一控制執行緒使用，不可與另一個 run／析構並行。
模組工作仍在框架內部工作執行緒進行。
全程序同時只允許一個 OSAL Terminal 擁有標準輸入，第二個 open 回 Busy。

Manager retains builtin/Tcl/autoload name reservations without callbacks into Shell. Conflicting types reject registration; source on_name proposals reject before approval, without suffix escape. Construct Shell before types for early collision checks. Script proc names should not shadow Modules. Recreated Shell never calls dead callbacks. Legacy micro-programs remain trusted central native callbacks required to return, not isolated processes.

ModuleManager 保留框架命令及初始化後的 Tcl 全域命令／autoload 名稱，直到 manager 析構；不持有指向 Shell 的 callback。
ModuleType 使用保留字時在種類登記時拒絕；實例名稱由 Module source 的 `on_name` 提案，
使用保留字時在中央核准名稱前拒絕，不靠 `_1` 避開。既有名稱衝突會在 Shell 初始化時回報。
先建構 Shell 再登記模組可讓種類衝突在登記當下回報。腳本自己的 `proc` 等名稱仍應避免遮蔽模組命令。
重新建立 Shell 不會呼叫已析構 Shell 的函式；ModuleType／實例名稱不會與這些內建命令衝突。
既有 `register_micro_program` 仍可用，其 callback 是中央原生程式碼，需自行保證會返回；
本次未將 legacy micro-program 改成獨立程序。

<a id="命令與參數"></a>

## Commands and arguments / 命令與參數

```text
help
exampleModule
exampleNode hello
messageBusExample --name subscriber sub demo
messageBusExample --name publisher pub demo
publisher push 42
subscriber last
exit
```

Interactive Module submissions immediately restore input; startup prints Started <approved-name>. Before Ready, commands fail not-ready. Help lists user types and instance state/MID/PID, not every nonterminal state as Running. System Modules and retained errors are available with `help --verbose`.

互動式送出模組命令後會立即回到輸入。模組啟動完成會印出 `Started <完整節點名>`；
在 Ready 前操作該節點會得到未就緒錯誤。`help` 顯示使用者種類與實例的狀態、MID、PID；System Module 與最後錯誤用 `help --verbose` 查看，
不把所有非終止狀態都標成 Running。

### Compact help / 精簡說明

Usage is the first line. Commands use the same three-column layout as ModuleTypes;
Socket Shell's Remote heading also occupies its own line above the command names.

Usage 位於最前面，Commands 採與 ModuleTypes 相同的三欄對齊；Socket Shell 的
Remote 標題也獨立一行，再列出指令名稱。

ModuleTypes use a separate heading and three left-to-right columns, padded to each
column's longest name with at least two spaces between columns. Names are not
truncated; an empty list shows `(none)`. Local, remote and verbose help share this
layout; narrow terminals may still wrap long rows.

ModuleTypes 標題獨立一行，每行由左至右最多三個名稱，各欄依該欄最長名稱對齊，
欄間至少兩格；不截斷名稱，空清單顯示 `(none)`。本機、遠端及詳細 help 共用排版，
窄終端仍可能自行折行。

Column-layout validation (2026-11-01): Windows x64 / MSVC 19.51, Debug and Release
each passed `shell.unit`, `shell.integration` and `shell.socket`. Cases cover an
empty list, unequal column widths, sorted multi-row output, a partial final row,
verbose help, local input and TCP line endings. Both App builds succeeded; actual
Debug Vinc TCP output confirmed the seven built-in/example types in three rows.
Linux/macOS were not run for this formatting change.
The same three suites passed again in both configurations after extending columns
to Commands/Remote and moving Usage first; actual Debug Vinc TCP output was checked.

三欄排版驗證（2026-11-01）：Windows x64／MSVC 19.51，Debug、Release 各通過
`shell.unit`、`shell.integration`、`shell.socket`，涵蓋空清單、不同欄寬、排序換行、
不足三欄的末行、詳細 help、本機輸入與 TCP 行尾。兩種 App 建置成功，另實際啟動
Debug Vinc，確認七個官方／範例種類分為三行。本輪排版變更未執行 Linux／macOS。
後續將 Commands／Remote 納入欄位排版並把 Usage 移至最前面後，兩種組態再次通過
相同三組測試，亦確認實際 Debug Vinc 的 TCP 輸出。

`help` shows command names, user ModuleTypes and Modules. Only Socket Shell adds the
remote command overview. Use `<builtin> -h` or `<builtin> --help` for syntax;
`help --verbose` includes retained primary failures, contract reports and cleanup
failures with their original MIDs. Debug/waiting markers remain in the compact view.

`help` 只列指令名稱、使用者 ModuleTypes 與 Module 狀態；只有 Socket Shell 額外列遠端指令。
完整語法使用 `<內建指令> -h` 或 `--help`，不使用 `help log`。
`help --verbose` 保留主要故障、契約報告與收尾故障及各自 MID；精簡版仍保留 DEBUG／等待標記。

Official `ui` and `recorder` are builtins shared by local Shell, Socket Shell and
Tcl. `ui enable|disable` toggles HTTP only; `ui status` and `recorder status` can
report central recovery state even when the process is not Running. Once Running,
status/Recorder operations are forwarded through the existing hook. `recorder
record ...` and `finish` never control Module lifetime. Both support `-h`/`--help`.
Ordinary help/completion hides their ModuleTypes/instances; `help --verbose` adds
`[System Modules]`, desired UI state, MID/PID, retained faults and retry timing.
See [System lifecycle](../module_system/README.md#system-modules) for protection,
restart and shutdown contracts. Native `ui`/`recorder` names are reserved commands.

官方 `ui`／`recorder` 是本機、Socket Shell、Tcl 共用的內建指令。UI 開關只控制 HTTP；
兩者 status 在程序尚未 Running 時仍可查中央恢復狀態，就緒後才轉交既有 hook。
`recorder record ...`／`finish` 不控制程序生命週期，兩者皆支援 `-h`／`--help`。
一般 help／補全隱藏官方種類與實例；詳細 help 另列 System 區段、UI 意圖、MID／PID、
保留錯誤及重試時間。上述兩個命令名稱由框架保留。

Help interception requires exactly two tokens and a framework builtin name. It
does not execute the operation (including exit, disconnect or script cancellation).
ModuleType, Module and registered-command flags pass to their existing hooks
unchanged; no generated Module help is promised. `srun file.tcl --help` passes the
flag to Tcl `argv`. A recovery-only remote entrance can show `resume --help`, but
still rejects other commands under its existing capacity policy.

只有剛好兩個 token 且第一個是框架內建指令時才攔截說明旗標，不執行原操作
（包括退出、斷線與取消腳本）。ModuleType、Module 及使用者登記指令的旗標原樣交給既有
hook，框架不替模組生成說明。`srun file.tcl --help` 的旗標仍傳入 Tcl `argv`。
容量滿載時的恢復專用入口可顯示 `resume --help`，其餘命令仍遵守原有拒絕政策。

Validation (2026-11-01): Windows x64, MSVC 19.51, Debug and Release each passed
11 suites: the ten `shell.*` tests and `app.entry`. Targeted builds included the
actual Vinc executable and the entry fixtures. Coverage includes both help aliases,
no shutdown/disconnect/script-cancel side effects, actual ModuleType/Module hook
forwarding, Tcl script argv, compact failed state and retained verbose diagnostics.
An actual Debug Vinc run also checked `help`, `log --help`, plain redirected output
and normal exit. No Linux/macOS execution for this help change.

驗證（2026-11-01）：Windows x64／MSVC 19.51，Debug、Release 各通過 11 組
（十組 `shell.*` 與 `app.entry`），並建置實際 Vinc 與入口驗收程式。涵蓋兩種旗標、
不關機／斷線／取消腳本、真實 ModuleType／Module hook 轉發、腳本 argv、精簡失敗狀態
及詳細診斷保留。另外實際執行 Debug Vinc，確認 `help`、`log --help`、導向輸出的純文字
及正常退出。本輪 help 修改未在 Linux／macOS 執行。

### Session errors / Session 錯誤回應

Interactive command, parsing, completion and script failures show concise summaries.
`shell -e` / `shell --error` inspects this Session's latest observed failure;
`shell -h` / `shell --help` shows usage. `shell` is a reserved framework name.
An empty Session reports `No recorded error.` Success and inspection do not clear
the slot. Concurrent jobs update it in result-observation order, not submission order.

互動命令、解析、補全及腳本失敗顯示簡短摘要；`shell -e`／`shell --error` 查看該
Session 最後觀察到的失敗，`shell -h`／`--help` 查看用法。`shell` 為框架保留名稱。
尚無紀錄時顯示 `No recorded error.`；成功指令與查看紀錄不會清除它。並行工作按
結果被收取的順序更新，不承諾依送出順序保存。

`SessionFeedback` is Shell-internal presentation state owned by the local control
loop or a remote Session's network loop; workers never mutate it. It retains the
original numeric Error/cause chain, or an application status without converting
that status into a framework Error. Only the command target is retained, not the
full submitted argument list. Targets/script filenames reuse `shell_max_line_bytes`
(4096 bytes); Tcl detail uses `shell_script_result_bytes` (256 KiB). Queries reserve
`shell_script_result_suffix_reserve` (32 bytes) within the Socket payload budget for
framing; truncated text is marked. No new thread, queue or mandatory Log write is added.

`SessionFeedback` 是 Shell 內部呈現狀態，由本機控制迴圈或各遠端 Session 的網路迴圈
持有，工作執行緒不修改它。保存原始 Error／原因鏈，或應用狀態碼；不把非零狀態碼
改成框架錯誤。只保留命令目標，不另外保存整串參數。目標／腳本檔名沿用 4096-byte
輸入限制，Tcl 細節沿用 256 KiB 診斷限制，查詢回覆預留既有 32-byte framing 空間並
標記截短；沒有新增執行緒、佇列或強制 Log 寫入。

Local and remote slots are independent. A disconnected Session retains its slot
through the existing resume grace period, including failures collected while
disconnected; expiry/destruction releases it. A fresh connection has no error.
Script failures belong to the initiating Session, with file/line/reason in the
summary and full bounded Tcl details available on inspection. Tcl `catch`,
`return -options`, native `error` and framework numeric error options are unchanged;
`shell -e` inside Tcl returns SessionRequired instead of reading interactive state.
Tcl's own diagnostic text may quote script source/values; this is not a secret-redaction facility.

本機與遠端紀錄互不影響；斷線後在既有恢復期限內保留，包括斷線期間才收取的失敗，
Session 過期／銷毀後釋放。全新連線沒有紀錄。腳本失敗只屬於啟動者，摘要包含
檔名／行號／原因，查詢可看有界的詳細 Tcl 資訊。Tcl `catch`、`return -options`、
原生 `error` 與框架數值錯誤選項不變；腳本內 `shell -e` 回 SessionRequired，
不讀取互動狀態。Tcl 自身診斷可能包含腳本來源／值；本功能不是機密遮罩機制。

Unknown execution outcomes remain explicit: a timeout/cancel is never presented
as proof that the operation did not execute or that retrying is safe. Unrecognized
codes receive a generic summary but remain numeric in `shell -e`. Existing
background fault notices, `log status`, `param status`, startup/shutdown diagnostics
and `help --verbose` retain their diagnostic contracts; they are not command-error history.

「執行結果未知」仍明確標示，不把逾時／取消說成操作沒發生或可安全重試。未知代碼
使用一般摘要，數值仍可查。背景故障通知、`log status`、`param status`、啟停診斷及
`help --verbose` 維持各自契約，不當作所有命令錯誤的歷史。

Validation (2026-11-01): Windows x64 / MSVC 19.51, Debug and Release each passed
all 11 selected suites (`shell.*` and `app.entry`). Coverage includes concise
errors, unchanged numeric Results, unknown causes, uncertain outcomes, bounded
details, Session isolation, reconnect retention (including a script failing while
disconnected), local dispatch and Tcl regressions. A separately launched Debug
Vinc also passed actual TCP checks for `ls`, invalid Param/Log arguments,
`shell -e` / `shell --error`, retention after successful `help`, and normal `exit`.
This round did not run Linux/macOS tests or perform a new GUI visual inspection.

驗證（2026-11-01）：Windows x64／MSVC 19.51，Debug、Release 各通過選定的
11 組測試（`shell.*` 與 `app.entry`），涵蓋簡短錯誤、原始數值 Result、未知原因、
執行結果未知、診斷容量限制、Session 隔離、重連保留（含斷線期間腳本失敗）、
本機派送與 Tcl 回歸。另實際啟動 Debug Vinc，以 TCP 驗證 `ls`、錯誤 Param／Log
參數、兩種錯誤查詢語法、成功 `help` 後保留錯誤及正常 `exit`。
本輪未執行 Linux／macOS 測試，也未新增 GUI 目視驗收。

Quotes preserve spaces/empty args; double quotes escape quote/backslash, single quotes retain all text. Outside quotes, backslash escapes space/quotes/backslash, otherwise remains for Windows paths. Adjacent quoted/unquoted fragments join; unmatched quotes reject.

引號保留空白與空參數，雙引號支援 `\"`、`\\`；單引號內保留所有文字。
引號外可用反斜線跳脫空白、引號或反斜線；其他反斜線保留，方便 Windows 路徑。
引號可以與同一參數的未引用片段相接。未關閉引號拒絕執行。

```text
node open "D:\Some Folder\a.txt"
node set label ""
node set path '\\server\share\file'
```

Interactive syntax treats #,//,$ literally, not comments/expansion. Limit 4096 bytes/256 args; reject entire overlong lines without executing prefixes.

互動解析不把 `#`、`//` 視為註解，也不解釋 `$`，故 URL 等參數原樣保留。
單行上限 4096 bytes、256 個參數；超長行拒絕整行，不執行被截斷的前半段。

<a id="本次保留的-hook-相容性"></a>

### Native hook argument compatibility / 本次保留的 hook 相容性

Native Shell start args retain ModuleType as first token; command/completion args retain instance name. Direct Manager callers get their vector unchanged. Bus examples accept both. Python source adapts to its documented target-free lists. Future native removal needs coordinated migration.

尚未變更 hook 的既有位置約定：Shell 送入 `on_start` 的參數包含 ModuleType 名，
送入 `module_interface`／`module_autoComplete` 的參數包含完整 Module 名。
直接呼叫 ModuleManager 的 API 則原樣傳遞呼叫端提供的 vector，不擅自增刪。
Message Bus 範例仍兼容兩種呼叫來源。若未來統一移除目標名稱，需同步遷移各 hook。
Python Source 會轉接為該語言文件定義的不含目標名稱之 args，不能將 native hook 的約定直接套到 Python。

<a id="命令執行與錯誤"></a>

## Execution and failures / 命令執行與錯誤

- At most 16 active command/completion jobs, then Busy; no unbounded waiting queue.
- Reject overlap per target; distinct targets may run concurrently. Manager permits only one pending remote command/instance.
- Wait 5 s for commands,1 s completion, startup_timeout+1 s for launch, on workers so keyboard/help/exit stay responsive.
- Timeout means unknown execution, no automatic resend/cancel. Manager keeps Busy until late reply/exit to avoid accumulating stuck work.
- Interactive failures use concise summaries; `shell -e` retains full framework/native diagnostics. Nonzero hook status keeps its number.
- Tab is asynchronous; discard responses after input changes. Show all candidates; insert single candidates with quoting, not only global node names.

- 互動介面最多 16 個執行中的命令／補全工作；滿載立即回 Busy，沒有未執行命令的無界佇列。
- 同一目標仍有 Shell 工作時拒絕重疊送出；不同目標可以並行。
  ModuleManager 另外確保同一實例只能有一個未完成的遠端請求。
- 節點命令等待 5 秒，補全等待 1 秒，啟動等待該模組 startup_timeout 加一秒。
  等待在工作執行緒，鍵盤、內建 help 與退出仍可處理。
- Timeout 表示命令結果未知，不代表未執行或已取消；不自動重送。
  ModuleManager 會保留該請求的 Busy 狀態直到遲到回覆或該次程序退出，避免繼續往卡住的模組排隊。
- 互動失敗顯示簡短摘要，`shell -e` 可查看完整錯誤鏈與原生 OS 資訊；
  遠端 hook 非零回傳仍顯示實際回傳碼，不杜撰原因。
- Tab 同樣非同步；結果抵達時，若輸入已改變便丟棄舊結果。
  多候選全部顯示，不要求候選本身是全域節點；單候選會依引號規則填入。

OutputHub captures bytes and Shell displays batches/restores input; TextRenderer escapes terminal controls. Bounded queues drop old Module output with gaps, while command/Tcl output retain separate routing. Arbitrary stdout is broadcast, never a request-specific reply; hooks return statuses. Subscribe by constructing Shell before Modules to show startup output.

模組 stdout／stderr 由 [OutputHub](../output_hub/README.md) 擷取，Shell 在事件迴圈中
批次顯示，再還原輸入列。模組輸出由 OutputHub TextRenderer 清除終端控制序列後呈現，不直接執行 ANSI 序列。
Shell 佇列有容量上限，過慢時略過最舊輸出並提示；命令結果與 Tcl 輸出仍走原本路由。
模組的任意 stdout 不會被當作某個請求的專屬回覆；命令 hook 的結果仍是狀態碼。
Shell 建構時即訂閱，因此應先建立 Shell，再啟動需要顯示初始化輸出的模組。

<a id="ctrlcexit-的完整關閉流程"></a>

## Complete Ctrl+C/exit shutdown / Ctrl+C／exit 的完整關閉流程

First Ctrl+C/exit cancels the script and starts one shutdown; repeats never skip cleanup/_Exit. With default network service, noninteractive stdin EOF only disables local input. Internal Shell-only tests without network/scripts still shut down on EOF.

第一次 Ctrl+C 或 `exit` 啟動同一流程，先取消執行中的腳本；重複 Ctrl+C 不跳過清理，也不直接 `_Exit`。
中央預設啟用 Socket Shell，stdin 非互動終端的 EOF 只表示沒有本機輸入，框架繼續運作。
框架內部測試若單獨建立未啟用網路／腳本的 Shell，輸入 EOF 仍走完整關閉流程。

OSAL separate process groups let central coordinate Ctrl+C so children do not skip on_stop. Windows CREATE_NEW_PROCESS_GROUP/POSIX SETPGROUP create no new console or whole-tree termination policy.

模組透過 OSAL 建立獨立程序群組，讓終端的 Ctrl+C 由中央接收並協調停止，
避免子程序先被 OS 預設處理器結束而跳過 `on_stop()`。Windows 使用 `CREATE_NEW_PROCESS_GROUP`，
POSIX 使用 `POSIX_SPAWN_SETPGROUP`；沒有建立新 console，也沒有加入整棵程序樹的終止政策。

**English**

1. Close registration/start admission and new commands.
2. Mark all instances stopping, disable restart.
3. Notify existing channels for normal hook/destructor cleanup.
4. After per-Module stop timeout, supervision requests OS termination.
5. Continue observing actual exit; a termination request is insufficient.
6. Drain accepted Bus exit work and Shell jobs, then show completion/restore terminal.

**繁體中文**

1. 關閉中央的新註冊／啟動入口，Shell 停止接收新命令。
2. 同時標記所有受管理實例停止，停用其自動重啟。
3. 經既有控制通道通知模組，讓 `on_stop()` 與析構正常完成。
4. 超過每個模組的 stop_timeout，監督執行緒透過 OSAL 要求 OS 終止。
5. 持續觀察 OS 的退出結果；只提出 terminate 請求不算完成。
6. 等待 Message Bus BusManager 已排入的退出回收工作完成，收完 Shell 工作，才顯示關閉完成並還原終端。

begin_shutdown is repeatable; wait_shutdown timeout does not stop supervision. Shell repeatedly reports remaining names/PIDs/termination errors until real completion. OS refusal/unresponsive scheduling can extend waits; repeated Ctrl+C never abandons children. Scope excludes user-created unmanaged processes.

`ModuleManager::begin_shutdown()` 為可重複呼叫的停止入口；`wait_shutdown(deadline)` 的 Timeout
不會停止監督。Shell 每次短暫等待後顯示尚未退出的名稱、PID 與終止錯誤，持續等到真正完成。
OS 拒絕終止或程序無法被排程時可能持續等待，沒有「第二次 Ctrl+C 直接丟下子程序」的捷徑。
範圍是 ModuleManager 持有的程序，不包含使用者自行建立且未交給框架管理的程序。

Internal composition explicitly uses the two-stage API, with Manager destruction as last defense. Successful Shell shutdown means all managed children have confirmed exit, not a guess based on destructor deadlines.

框架內部接線應明確使用上述兩階段 API；ModuleManager 原有析構仍是最後的防護路徑。
Shell 成功返回關閉流程時，所有受管理程序都已確認退出，無須以析構的時間限制猜測成功。

<a id="終端行為與平台邊界"></a>

## Terminal/platform behavior / 終端行為與平台邊界

Framework-owned local Shell output uses OSAL `Terminal::write_console()` when its
destination is `std::cout` attached to a terminal. This includes startup status,
prompts, command/script results and rendered Module output. Injected ostream
destinations and redirected stdout retain the ostream path; no global stream buffer,
CRT locale or third-party printf is replaced. Formatting startup/shutdown summaries
uses temporary strings; ordinary text passes directly to OSAL. A native console
write failure disables further local display for this Shell and submits one wrapped
diagnostic to the existing bounded Log. Input, remote Shell and central services
continue; this does not promise Log delivery or automatic console recovery.

框架自己的本機 Shell 輸出（啟動摘要、prompt、命令／腳本結果及格式化 Module 輸出）
在目的地是終端的 `std::cout` 時，統一使用 OSAL `Terminal::write_console()`。
注入的 ostream 與重導向 stdout 維持原串流路徑，不替換全域串流、CRT locale 或第三方 printf。
啟動／關閉摘要組合時使用暫存字串，一般文字直接交給 OSAL。原生 console 寫入失敗後，
停止該 Shell 的後續本機顯示並將一筆包裝錯誤送入既有有界 Log；輸入、遠端 Shell 與
中央服務繼續運作，不承諾 Log 送達或自動恢復 console。

- The local ANSI prompt uses Open-VINC blue `#246BDF` and restores the default foreground before input text. Redirected/non-ANSI output and Socket Shell remain plain text; exact RGB rendering requires a true-color terminal.
- Up/down history restores the pending draft, omitting consecutive duplicate commands.
- Tail entry/backspace deletes a UTF-8 code point, not arbitrary grapheme clusters. No middle insertion/cursor movement/full emoji editing.
- Windows ReadConsoleInputW handles surrogate pairs; POSIX parses UTF-8/key escapes. Standalone Escape has a bounded recognition delay.
- Render line tails with conservative two-column non-ASCII widths, not universal glyph accuracy.
- Redirected UTF-8 input distinguishes EOF/error without spinning; redirected output omits ANSI prompts. Async line dispatch is not synchronous scripting.
- Ctrl+C/SIGINT only signals stop; ordinary threads clean up. OS forced shutdown/terminal closure may impose deadlines and prevent cleanup.

- 上／下取回歷史，返回最新位置時還原尚未送出的草稿；不記錄連續重複命令。
- 本機 ANSI 提示符使用 Open-VINC 品牌藍 `#246BDF`，輸入文字前恢復預設前景色。重導向／非 ANSI 輸出與 Socket Shell 維持純文字；精確 RGB 顯色需要支援 true color 的終端。
- 支援行尾輸入與 Backspace；刪除一個完整 UTF-8 code point，避免留下半個中文字元。
  本次未實作中間插入／游標左右移動、完整 grapheme cluster（組合字／複合 emoji）編輯。
- Windows 使用 ReadConsoleInputW 並處理 UTF-16 代理對；POSIX 解碼 UTF-8 與方向鍵 escape sequence。
  單獨 Escape 有短暫辨識期限，不會無限等後續按鍵。
- 顯示長行末端，以非 ASCII 每字最多兩欄保守預留；不宣稱精確覆蓋所有終端的字形寬度。
- 重導向的 UTF-8 stdin 區分 EOF 與錯誤，不會空轉；stdout 非終端時不輸出 ANSI prompt。
  這個入口沿用互動的非同步派送，不能將逐行輸入當成同步腳本執行保證。
- Ctrl+C 按鍵與程序 SIGINT／Windows CTRL_C_EVENT 只傳達停止事件；清理在一般執行緒進行。
  OS 強制關機／終端強制關閉另有 OS 時限，不承諾被 OS 強殺後仍能完成清理。

Native references: [GetConsoleMode](https://learn.microsoft.com/en-us/windows/console/getconsolemode), [termios](https://www.man7.org/linux/man-pages/man3/termios.3.html), [poll](https://www.man7.org/linux/man-pages/man2/poll.2.html).

Windows Terminal selection: OSAL Terminal disables inherited `ENABLE_MOUSE_INPUT`
because its API delivers text/key events only. Mouse selection/copy belongs to the
terminal host, not Shell. Legacy Console Quick Edit remains disabled to avoid
selection pausing console I/O; this does not enable legacy Quick Edit selection.
The original input mode is restored on Terminal destruction, including its mouse flag.

Windows Terminal 文字選取：OSAL Terminal 的 API 只交付文字／按鍵事件，因此明確關閉
繼承的 `ENABLE_MOUSE_INPUT`，讓滑鼠選取／複製由終端管理。傳統 Console 的 Quick Edit
仍維持關閉，避免選取期間暫停 Console I/O；本次未啟用傳統 Quick Edit 選取。
Terminal 解構時還原原本輸入模式，包括滑鼠旗標。

Windows console 模式依據 [Microsoft GetConsoleMode](https://learn.microsoft.com/en-us/windows/console/getconsolemode)，
POSIX 模式及等待依據 [termios](https://www.man7.org/linux/man-pages/man3/termios.3.html) 與
[poll](https://www.man7.org/linux/man-pages/man2/poll.2.html)。

<a id="tcl-腳本"></a>

## Tcl scripts / Tcl 腳本

Legacy ScriptInterpreter/custom syntax/run_script/-sc are removed. Use standard Tcl 9.1/.tcl/UTF-8. Ordinary interactive input remains Shell syntax, not Tcl evaluation.

舊 `ScriptInterpreter`、自訂語法、`run_script` API 與 `-sc` 已移除。使用標準 Tcl 9.1，慣用副檔名 `.tcl`，檔案採 UTF-8。
互動 Shell 仍使用上面的命令列語法，不將一般輸入當作 Tcl 程式執行。

From repository root:

從專案根目錄啟動：

```powershell
.\build\windows-msvc-debug\Vinc.exe -S examples/scripts/startup.tcl
```

Inside Shell:

Shell 中：

```text
srun examples/scripts/maintenance.tcl
srun examples/scripts/cancel-demo.tcl
```

Startup -S and srun share execution, with argc/argv/argv0. Each run has isolated variables/procs/events and only one script runs. source nests inside it; nested srun returns Busy. Internal ScriptSource owns either path/text across async jobs with shared mailbox/cancellation/error behavior. Embedded argv0/info script is diagnostic embedded:startup.tcl, with empty argv; all scripts expose ::vinc::app_dir. No automatic cwd change. Source paths relative to a script can use:

`Vinc -S file.tcl [args...]` 與 `srun file.tcl [args...]` 共用執行器。
參數透過 Tcl `argv`／`argc` 提供，`argv0` 為腳本路徑。
每次執行使用獨立變數、proc 與事件環境；一次只執行一份腳本。
`source` 可在同一次環境引入檔案，嵌套 `srun` 則回 Busy。
框架內部 `ScriptSource` 可攜帶檔案路徑或擁有的內嵌文字；兩者共用 ScriptService mailbox、
工作執行緒、取消、派送及錯誤原因鏈。文字跨非同步邊界由工作持有，不借用呼叫端暫存內容。
內嵌入口以 `embedded:startup.tcl` 提供 `argv0`／`info script` 及錯誤來源，並非磁碟檔案。
每份腳本都可透過 `::vinc::app_dir` 取得 EXE 所在目錄；內嵌入口無 argv 參數。
路徑相對於程序目前工作目錄，不自動 `cd`。相對於腳本所在目錄引入檔案可寫：

```tcl
source [file join [file dirname [info script]] common.tcl]
```

Module names dispatch directly after Tcl variable/list/quote parsing into argv, without string reassembly/Shell reparsing:

模組命令直接依名稱派送，Tcl 的變數／清單／引號先解析成參數陣列，不再拼成字串重做 Shell 解析：

```tcl
set action hello
exampleModule
exampleNode $action
if {[catch {exampleNode status} reason]} {
    puts "Command failed: $reason"
}
```

- Script commands wait sequentially; startup waits Ready. Success returns0 and prints the approved name.
- `param set` requires an existing typed parameter. On a clean first run, start
  the Module with its author-defined args (for example `userExampleModule --gain 2.0`)
  before adjusting it through `param set`. Shell does not synthesize parameter
  flags or wait for owner validation when saving. See [Param startup/save](../param/README.md#startup-and-correction).
- `param set` 必須已有具型別的參數。乾淨首次啟動，先用 Module 作者定義的 args
  （例如 `userExampleModule --gain 2.0`）啟動，再用 param set 調整。Shell 不生成
  參數旗標，save 也不等擁有者審核；完整契約見上述 Param 文件。
- Duplicate instances fail without suffixes; source naming hooks choose options and scripts use approved names, never rewritten targets.
- Nonzero hooks/unknown/start/timeout become catchable Tcl errors.
- Unhandled errors show file/line/stack and stop only that script, including -S, leaving central/Modules alive.
- Local foreground scripts ignore ordinary input but keep Ctrl+D/C; prior async jobs can still execute, so scripts are neither transactions nor global locks. Remote Sessions remain usable; remote scripts do not occupy local input and scancel cancels the shared job.
- Ctrl+D/scancel request cancellation, distinct from EOF; no-job just reports. Scripts may cancel themselves.
- return ends the file; no-arg Tcl exit requests complete framework shutdown, never bypasses cleanup.
- TCL_CANCEL_UNWIND cannot be swallowed by ordinary catch. Pure loops/after/framework command/start waits cooperate; already-sent actions are not undone and remain Busy until reply/exit. No rollback or automatic Module stop.
- Native extensions/micro-programs/blocking files/external processes may delay cancellation. Never kill central threads; put long work in Modules and do not compete for stdin.
- Standard Tcl remains trusted in-process deployment code, not a sandbox; timers die with that interpreter.
- Tcl stdout/stderr puts routes through Shell with 256 KiB pending output/truncation; file channels retain Tcl behavior.

- 命令依序等待完成；啟動等 Ready 後才執行下一行。成功回傳 `0`，啟動時另外顯示完整名稱。
- 不再自動添加尾碼；同名實例直接報錯。模組可在 `on_name(args)` 解析自己的命名選項，腳本呼叫核准後的完整名稱；框架不改寫腳本中的目標。
- 非零 hook 回傳、未知命令、啟動失敗、逾時轉為 Tcl error，可由 `catch` 處理。
- 未處理錯誤顯示檔名、行號及 Tcl 呼叫堆疊；包括 `-S` 在內，都只終止該次腳本，框架與已啟動模組繼續運作。
- 本機 Shell 前景等待腳本時，忽略一般輸入，保留 Ctrl+D／Ctrl+C；完成後恢復 prompt。
  先前已送出的非同步命令仍可能執行，因此腳本不是交易或全域互斥區段。其他 Socket Shell 可繼續操作；
  遠端腳本不占住本機輸入，本機仍可操作模組或用 `scancel` 取消全域腳本。
- Ctrl+D 與 `scancel` 都要求取消腳本，沒有腳本時只提示；Ctrl+D 與 stdin EOF 是不同事件。
  本機前景執行期間使用快捷鍵；腳本也可用 `scancel` 取消自己。
- `return` 正常結束目前腳本檔案。Tcl 的 `exit` 改接框架完整關閉，只接受無參數形式，不能直接跳過清理。
- 取消採 Tcl `TCL_CANCEL_UNWIND`，一般 `catch` 不能攔截；純 Tcl 迴圈、`after ms` 與框架提供的模組命令／啟動等待可響應取消。
  已送出的命令不撤銷，遲到回覆／程序退出前仍保持 Busy；已完成操作不回滾，已啟動模組不自動停止。
- 原生擴充、原生 micro-program、阻塞檔案／外部程序操作仍可能延遲取消；不強殺中央執行緒。
  腳本應將長時間工作交給模組，且不從 stdin 與 Shell 搶讀輸入。
- Tcl 標準功能保留，腳本是受信任的程序內部署程式，不是隔離的安全沙箱。
  定時 callback 只在該次 Tcl 環境存在期間有效，不會在腳本結束後繼續運作。
- Tcl `puts` 的 stdout／stderr 經 Shell 輸出，每次尚未消費資料上限 256 KiB，超量標記截斷；寫檔仍使用 Tcl 原有行為。

<a id="建置與部署"></a>

### Build and deploy / 建置與部署

Unmodified pinned Tcl is integrated in [library/tcl](../../../library/tcl/README.md): single-config Ninja, separate Debug/Release, MSVC/NMAKE on Windows and configure/make/zlib on POSIX. Core is static; initialization/encoding/licenses deploy beside Vinc in lib/tcl9.1, no system Tcl. Deploy follows actual runtime output including parent changes and a build-only lock serializes shared destinations. Keep assets together. Movable Tcl resources do not make build-absolute Module catalogs relocatable.

Tcl 使用未修改的固定上游，透過 [library/tcl](../../../library/tcl/README.md) 整合。
使用單一組態的 Ninja 建置，Debug／Release 分開目錄；Windows 使用 MSVC／NMAKE，Linux／macOS 路徑使用上游 configure／make 與 zlib 開發套件。
Tcl 核心靜態連結，初始化腳本、編碼資料及授權放在 `Vinc` 旁的 `lib/tcl9.1`，不要求安裝系統 Tcl。
框架建置的 `vinc_deploy_tcl(target)` 預設跟隨 target 的實際輸出目錄，包含父專案後設的
`RUNTIME_OUTPUT_DIRECTORY`；多個 target 寫入同一目的地時在建置期間以檔案鎖序列化複製。
部署時須一併保留該目錄。現有自動生成的模組目錄仍記錄建置時的模組絕對路徑；Tcl 資源可搬移不代表整套模組部署已支援任意搬移。

<a id="驗證"></a>

## Recorded validation / 驗證

Windows x64 / MSVC 19.51.36257, Windows SDK 10.0.26100.0, CMake 4.3.2,
Ninja 1.13.2 (2026-11-01): full Debug and Release builds passed. In each
configuration, the ten `shell.*` tests and `app.entry` passed (11 distinct tests,
across targeted runs). Debug initially hit LNK1168 on the running Bag DLL; after
the owner authorized normal Socket Shell `exit`, central and Module shutdown
completed and the full build succeeded. No forced termination was used.

2026-11-01，Windows x64／上述工具鏈：完整 Debug、Release 建置成功，兩種組態各通過
十項 `shell.*` 與 `app.entry`（分批執行，共 11 項不同測試）。Debug 最初因使用中的 Bag DLL
遇到 LNK1168；使用者授權後經 Socket Shell `exit` 正常關閉中央與 Module，再完成建置，
未強制終止程序。桌面工具仍回報 Vinc 沒有可擷取視窗，目視驗收限制見下方待辦。

Native output regression (2026-11-01): before the fix, a hidden real Windows
console with Tcl initialized and CRT locale `.950` failed at
`actual console lost success check mark`. The same Shell/stdout test now passes
under `.950`, `.1252` and `.UTF8`, reading back check/cross/Chinese characters and
console color attributes, with no literal `[39m` and no foreground-color leak.
It also checks locale/code-page preservation, invalid UTF-8 before any write,
UTF-8 conversion across the 4096-byte boundary, and native failure diagnostics.
Legacy console readback substitutes U+FFFD for the tested supplementary emoji
even with direct `WriteConsoleW`; that case compares against native UTF-16 output,
and does not claim pixel/font verification. Production-entry tests cover Unicode
through redirected pipes and files, with no ANSI. CMake captures explicitly use
UTF-8 so the test runner does not reinterpret correct bytes through its locale.

原生輸出回歸（2026-11-01）：修復前在真正的隱藏 Windows console、初始化 Tcl 並設定
CRT `.950` 後，實際 Shell/stdout 測試失敗於「畫面缺少成功勾號」。修復後 `.950`、
`.1252`、`.UTF8` 均能讀回正確的勾叉、中文及顏色屬性，沒有 `[39m` 殘留或正文染色。
另驗證 locale／code page 保留、非法 UTF-8 寫入前拒絕、4096-byte 分段及原生錯誤。
舊 console 讀回 API 對測試用補充平面 emoji 會回傳 U+FFFD，直接 `WriteConsoleW`
也相同；該項比對原生 UTF-16 寫入結果，不宣稱已驗證字型或像素。正式入口測試另涵蓋
Pipe／檔案 UTF-8 重導向且不含 ANSI；CMake 明確以 UTF-8 解碼，避免測試工具自行誤轉碼。

Mouse input mode fix, 2026-11-01: Windows x64/MSVC 19.51 Release build and
`shell.terminal`, `shell.integration`, `shell.console_input` passed (3/3).
The hidden-console case explicitly inherits mouse reporting, verifies it is disabled
while Terminal is open and restored afterward, and checks Unicode/edit keys,
Ctrl+C/Ctrl+D, OS Ctrl+C with normal Module cleanup, and prompt redraw.
`shell.console_input` skips only the separate central-App deployment scenario;
the full `shell.console` remains available. Actual Windows Terminal drag-to-clipboard
interaction was not automated; no Linux/macOS run (the change is Windows-only).
Debug binaries were not rebuilt because the user's Debug App was still running.

滑鼠輸入模式修正（2026-11-01）：Windows x64／MSVC 19.51 Release 建置成功，
`shell.terminal`、`shell.integration`、`shell.console_input` 共 3/3 通過。
隱藏 Console 案例明確繼承開啟的滑鼠輸入旗標，驗證 Terminal 開啟時停用、結束時還原，
並驗證 Unicode／編輯按鍵、Ctrl+C／Ctrl+D、OS Ctrl+C 與模組正常收尾、提示符重繪。
`shell.console_input` 僅略過獨立的中央 App 部署案例，完整 `shell.console` 仍保留。
尚未自動驗證 Windows Terminal 實際滑鼠拖曳到剪貼簿；本次只更動 Windows 路徑，
未執行 Linux／macOS。使用者的 Debug App 仍在執行，因此未重建 Debug 產物。

Brand prompt, 2026-10-31: Windows x64/MSVC 19.51 Release built successfully.
`shell.integration` and `shell.socket` passed; `shell_console_tests <Vinc.exe> --redraw-only`
passed in a hidden native console, checking RGB prompt bytes, input redraw, and foreground reset
before submitted commands. This targeted mode skips unrelated App deployment scenarios.
The full `shell.console` run failed at `headless_deployment`'s Tcl marker check before reaching
the prompt scenario; another Debug Vinc was running (possible instance exclusion interference).
Rerun the full suite with that App stopped before claiming full Console coverage. The Debug
build hit LNK1168 on the occupied Bag DLL. No Linux/macOS validation in this round.

品牌提示符（2026-10-31）：Windows x64／MSVC 19.51 Release 建置成功；
`shell.integration`、`shell.socket` 通過，`shell_console_tests <Vinc.exe> --redraw-only`
在獨立隱藏原生 Console 通過 RGB 提示符、輸入列重繪、送出命令前恢復前景色的檢查。
此模式略過無關的 App 部署情境。完整 `shell.console` 在 `headless_deployment` 的 Tcl
標記檢查失敗，尚未到達提示符案例；當時有另一份 Debug Vinc 執行中，可能受同名排他影響。
須先停止該 App 再重跑，才能宣稱完整 Console 通過。Debug 建置則遇到 Bag DLL 被占用的
LNK1168。本輪未驗證 Linux／macOS。

Embedded-script Windows Debug/Release **12** related tests each (2026-10-14), including source/cancel/lines/regressions, are in [central evidence](../../../apps/vinc/README.md#驗證).

內嵌腳本（2026-10-14）的 Windows Debug／Release 各 12 項整合驗證，包含既有 Shell
回歸及新增來源／取消／行號測試，見 [中央驗證紀錄](../../../apps/vinc/README.md#驗證)。

- unit: quoting/empty/escapes/paths/completion roundtrip/Unicode deletion/history/lifetime/reservations/statuses/candidates.
- terminal: chunked UTF-8/CRLF/keys/Escape/stop/invalid bytes/EOF/exclusive release.
- integration: real process args/completion/timeout-Busy-late replies/healthy peers while stuck/normal and forced Ctrl+C shutdown/exit/EOF.
- Windows hidden console: injected Chinese/surrogates/keys/modes/code pages and actual broadcast CTRL_C_EVENT with on_stop/normal exit; not manual hardware interaction.
- Tcl: flow/source/argv/Unicode/spaces/empty/locations/isolated runs/reservations, catch-resistant loop/after/command cancellation, reruns/foreground/start failure/stop during startup.
- Console also covers Ctrl+D, relocated Tcl, Unicode -S, failed headless deployment remaining alive until OS Ctrl+C.

- `shell.unit`：引號／空字串／跳脫、Windows 路徑、補全 roundtrip、中文刪除、歷史草稿、
  Shell 重建的 callback 生命週期、命名保留、原生回傳碼與候選清單。
- `shell.terminal`：重導向 UTF-8 分段、CRLF、方向鍵、孤立 Escape、Ctrl+C、非法 UTF-8、EOF、獨佔與釋放。
- `shell.integration`：真實 Module 程序的參數與補全，命令逾時後 Busy／遲到回覆，
  卡住命令期間仍操作其他模組；Ctrl+C 等待正常清理並強制終止卡住程序，確認退出；exit／EOF 共用流程。
- Windows 的 `shell.console`：在獨立隱藏 console 注入原生鍵盤事件，驗證中文、代理對、Ctrl+C、
  方向鍵、Backspace，以及 input/output mode 與 code page 還原；另外實際廣播 OS CTRL_C_EVENT，
  確認中央收到事件，模組經框架完成 `on_stop()` 並以正常退出碼結束。不是人工實機操作紀錄。
- `shell.tcl`：標準 Tcl 控制流程、source、argv、中文／空白／空參數、錯誤位置、獨立環境、保留字、
  catch 內無限迴圈／after／模組等待取消、取消後再次執行、前景輸入、啟動失敗保留框架、啟動期間 Ctrl+C 與真實模組退出。
- `shell.console` 另驗證原生 Ctrl+D、搬移後的 Tcl 資源、Unicode `-S` 路徑與參數、無 stdin 的失敗部署保持存活及 OS Ctrl+C 關閉。

Historical initial validation: Windows x64/MSVC 19.51.36257/SDK 10.0.26100.0/CMake 4.3.2/Ninja 1.13.2, **20/20** each. Linux/macOS backends were not natively built/run in that round.

初版歷史紀錄：當時在 Windows x64／MSVC 19.51.36257、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2
完成建置與測試：Debug 20/20、Release 20/20 全數通過。
後續各平台的建置與測試範圍見[平台驗證報告](../../../README.md#validation-and-limits)。

<a id="shell-命名統一驗證2026-10-04"></a>

### Shell rename validation (2026-10-04) / Shell 命名統一驗證（2026-10-04）

Nine C++ files had only naming/path changes, without style/behavior refactoring.

本輪只統一目錄、主檔名、類別與中央程式 target；9 個受影響 C++ 檔案比對確認
僅包含名稱／路徑替換，未進行程式風格重構或改變執行行為。

Windows Debug full build/SDK headers and **9/9** related tests (17.20 s); Release standalone targets/headers and **6/6** (12.70 s). No Linux/macOS.

- Windows x64 MSVC Debug：完整建置與 SDK 標頭編譯成功；Shell、SDK、
  Module／Message Bus 中央程式測試共 9/9 通過（17.20 秒）。
- Windows x64 MSVC Release：相關獨立測試目標與 SDK 標頭編譯成功；
  Shell Socket／Tcl／Unit／Terminal／Integration、SDK 測試 6/6 通過（12.70 秒）。
- Linux／macOS 本輪未驗證。

<a id="本機-shell-可讀性整理驗證2026-10-04"></a>

### Local readability validation (2026-10-04) / 本機 Shell 可讀性整理驗證（2026-10-04）

Parser/editor/router/local loop/tests retained dispatch/lifetimes/exceptions/diagnostics/shutdown. Added adjacent quotes, escape states/tabs/control rejection, input/argument bounds, completion positions, and preserving input after rejected long edits.

本輪整理命令解析／編輯、CommandRouter、本機 Shell 主迴圈與單元測試，保留既有
派送順序、工作／腳本生命週期、例外邊界、診斷訊息及關閉等待政策。
補充解析與編輯的邊界案例：相鄰引號、不同引號狀態的跳脫、escaped tab、控制字元拒絕、
輸入／參數數量上限、補全插入位置，以及拒絕超長編輯後原內容不變。

Both Windows full builds/SDK headers; related Shell/SDK/Module/Bus **9/9** (17.51 s/16.85 s). Earlier Release process occupancy ended, enabling central/Console/Module/Bus reruns. No Linux/macOS/other component full suite.

- Windows x64 MSVC Debug／Release：完整建置與 SDK 個別標頭編譯成功。
- Shell Socket／Tcl／Unit／Terminal／Integration／Console、SDK、Module／Message Bus
  中央程式測試：Debug 9/9 通過（17.51 秒），Release 9/9 通過（16.85 秒）。
- 上輪占用 Release 執行檔的程序已結束；本輪已完成 Release 中央程式重建，
  並補驗 `shell.console`、`module.shell`、`message_bus.shell`。
- Linux／macOS 本輪未驗證；本次未重跑所有其他元件測試。

<a id="完整-shell-可讀性整理驗證2026-10-04"></a>

### Complete Shell readability validation (2026-10-04) / 完整 Shell 可讀性整理驗證（2026-10-04）

SocketShell/ScriptService/TclRuntime/headers plus seven helpers/tests were expanded without changing priorities/recovery/backpressure/cancel/locks/destruction/exceptions/APIs. No error migration in that stage. Assertion-text comparisons supplement, not replace, behavior tests.

本輪補齊 SocketShell、ScriptService、TclRuntime 的實作與標頭，以及七個尚未整理的
測試／輔助檔案。命令優先順序、Session 恢復、傳送背壓、腳本取消、鎖範圍、
資源銷毀順序、例外邊界與既有 API 保持原有政策；未進行錯誤介面遷移。
測試按情境拆分，原有斷言診斷文字比對保留；這項靜態比對不取代行為測試。

Windows full builds/headers; **9/9** each (16.74 s/16.03 s). Real TCP segmentation/refusal/recovery/ownership/8-client capacity/IPv6/slow clients/shutdown tails, Tcl cancel/console events passed. No Linux/macOS/PuTTY GUI/unrelated suites. Path/InputPipe concerns then pending were fixed later.

- Windows x64 MSVC Debug／Release：完整建置與 SDK 個別標頭編譯成功。
- Shell Socket／Tcl／Unit／Terminal／Integration／Console、SDK、Module／Message Bus
  中央程式測試：Debug **9/9** 通過（16.74 秒），Release **9/9** 通過（16.03 秒）。
- 驗證包含真實 TCP 的輸入分段／拒絕、Session 恢復與歸屬、8 條連線容量、IPv6、
  慢客戶端、遠端關機等待模組退出與尾端輸出，以及 Tcl 取消與 Console 控制事件。
- Linux／macOS 本輪未驗證，PuTTY GUI 未實測；未重跑其他不相關元件的完整測試。
- 當時仍有路徑轉型疑慮及 InputPipe 建構失敗回收待辦；後續狀態見下方修正紀錄。

<a id="複掃缺口修正與驗證2026-10-04"></a>

## Follow-up scan fixes (2026-10-04) / 複掃缺口修正與驗證（2026-10-04）

Named 34 wait/delay values across five test files by purpose, retaining units/test locality. Expanded-constant token comparison/clang-format preserved values/order/assertions. Windows full builds and seven OSAL/six Shell tests **13/13** each; no Linux/macOS. Production wait_for(0 ms) stays a nonblocking probe. Paths/InputPipe were later fixed.

- `tests/terminal_tests.cpp`、
  `tests/tcl_tests.cpp`、`tests/socket_shell_tests.cpp`、`tests/integration_tests.cpp`、
  `tests/console_tests.cpp` 前次列出的 34 處非零等待／延遲值已依正常操作、刻意逾時、
  取消、觀察及清理用途命名，chrono 後綴保留時間單位。測試常數留在測試內。
- 將新常數展開後，與本輪修改前快照逐 token 比對一致：數值、等待位置、操作與斷言
  順序不變。前輪 clang-format 設定重跑後亦保持一致。
- Windows x64 Debug／Release 完整建置成功，七個 `osal.*` 與六個 `shell.*` 測試
  兩種組態各 **13/13** 通過。Linux／macOS 未執行。
- 正式實作的 `wait_for(0ms)` 是非阻塞就緒檢查，維持原樣；當時的路徑轉型與 InputPipe 待辦已於後續工作修正，見下方紀錄。

<a id="共用錯誤介面"></a>

## Common error interface / 共用錯誤介面

Shell owns its common Result/Error enums. It wraps Module/OSAL/OutputHub failures with bounded causes/common text. Static metadata has no Tcl dependency.

Shell 使用 `vinc::Result<T>`／`vinc::Error`，錯誤與操作 enum 由 `<vinc/shell/error.hpp>` 維護。
ModuleSystem、OSAL 與 OutputHub 失敗在本層轉換並保留有界原因鏈；文字統一由
`error_to_string()` 查詢。Shell metadata provider 不依賴 Tcl runtime。

- Internal create/init/network entries return Result.
- Successful process_line value is application status; nonzero hooks are not framework transport failure.
- Command/script failures diagnose and return to operation, retaining framework/peers.
- Runtime Socket failures isolate connection/listener/worker, not central shutdown; see [network policy](SOCKET.md#容量與生命週期).
- Unrecoverable local terminal failure returns from run only after full shutdown; explicit exit/stop also clean fully.
- Missing startup Tcl/bind/listen retains startup failure before scripts, distinct from post-start isolation; not every error is central fatal.
- Existing STL/third-party/application exception boundaries remain, not global exception disabling.

- `Shell::create()`、`SocketShell::create()`、`initialize_tcl()` 與網路啟動為內部 Result 入口。
- `process_line()` 的成功 value 保留應用命令回傳碼；非零 hook 回傳碼並非框架傳輸錯誤。
- 命令／腳本失敗顯示診斷後返回操作迴圈，保留框架與其他模組。
- Socket 執行中故障按連線、listener、worker 分層隔離，不單獨要求全框架關閉；
  退避、停用與診斷節流見 [Socket Shell 故障政策](SOCKET.md#容量與生命週期)。
- 本機 terminal 不可恢復故障由 `run()` 在完整關閉後回報；明確 exit／Ctrl+C 仍完整收尾。
- 啟動期缺少 Tcl 資源或 bind/listen 失敗保持既有啟動失敗政策，腳本尚未執行。
  這與服務運作後的網路故障隔離不同，不將所有錯誤視為中央程序致命故障。
- 既有標準函式庫、第三方及應用例外邊界維持；未全面停用 C++ exceptions。

ScriptReport separates Result from bounded Tcl file/line/text. Numeric causes live in Tcl return options for catch/return -options; later ordinary errors do not reuse handled old framework faults. Invalid options remain script failures. Bridges retain native command tokens so Tcl rename cannot recurse through the wrong command.

`ScriptReport` 分開保存 Result 結果與有界 Tcl 檔名／行號／文字診斷。框架錯誤的數值原因鏈
放在 Tcl return options：`catch` 可以處理它，`return -options` 可重新傳遞；後來發生的
一般 Tcl 錯誤不會誤用先前已處理的框架錯誤。非法 options 回到一般腳本失敗，不信任任意數值。
命令橋接使用保存的原生命令 token，腳本 rename 原始命令後亦不會遞迴呼叫錯誤的名稱。

<a id="後續修正紀錄"></a>

## Follow-up fixes / 後續修正紀錄

Strict OSAL UTF-8 replaces char→char8_t reinterpretation for scripts/Tcl executable paths, without code-page guessing. InputPipe test-helper construction rolls back native resources, covered by fault injection/stdin restoration. Socket isolation and Tcl rename/options tests live in faults/tcl. SDK actual build results are separate from older evidence.

- UTF-8 路徑：腳本路徑與 Tcl executable 路徑使用 OSAL 的嚴格轉換；不猜字碼頁，
  不再把 char 儲存重解釋為 char8_t view。Unicode 路徑限制見 OSAL 與中央入口文件。
- InputPipe：建構失敗先回收已取得的原生 handle／fd；測試 `shell.faults` 有故障注入
  與 stdin／資源回復檢查。此處屬測試輔助物件，沒有改變正式 stdin 所有權政策。
- Socket 故障隔離與 Tcl rename／return-options 的測試分別位於 `shell.faults`、`shell.tcl`。
  本輪 SDK 的實際重建與測試結果另列，不以舊歷史紀錄冒充本輪驗證。

<a id="module-契約故障顯示2026-10-05"></a>

## Module contract-fault display (2026-10-05) / Module 契約故障顯示（2026-10-05）

Local/remote collect Manager diagnostic revisions and show Module fault/name/MID/PID/actual exit code-signal/termination-request status/reason/index-size/detection file-line-function, without another thread. output off does not hide central faults; `help --verbose` displays the latest retained diagnostic.

本機與 SocketShell 在原有收集迴圈檢查 ModuleManager 的診斷 revision；變更時顯示
`[Module fault]`、名稱、MID、PID、實際退出 code／signal、中央是否曾要求 OS 終止，
以及原因、適用的 index／size、框架偵測 file／line／function。沒有新增診斷 thread。
Module 的 `output off` 不關閉中央故障通知；`help --verbose` 也顯示已保留的最後一筆。

Escape C0/DEL/ESC and mark truncation before display. Malformed/incomplete records show errors, not partial fields. Distinguish actual exit from whether termination was requested. Fatal deadline expired means observed deadline expiry, not successful OS kill.

子程序文字在顯示前替換 C0／DEL（含 ESC），避免嵌入終端控制序列；截短文字明確標示。
不完整或非法共享紀錄只顯示對應錯誤，不解讀部分欄位。診斷不宣稱中央已殺掉模組，
而是分開列出已確認的 OS 退出及是否曾發送終止要求。
`fatal exit deadline expired` 另行顯示中央觀察到有效退出期限已到，不表示 OS 終止請求成功。

Per-Session cursors avoid repeat display; disconnection/backpressure retain existing behavior without TCP-screen-delivery guarantee. [Module System](../module_system/README.md#契約故障共享紀錄) owns retained scope/fault boundaries.

每個 Session 各自持有通知 cursor，正常收集不重複顯示同一 revision；遠端斷線或背壓仍適用
既有 SocketShell 政策，沒有 TCP 畫面送達保證。中央保留範圍與故障邊界見
[ModuleSystem](../module_system/README.md#契約故障共享紀錄)。

Recorded Windows local/real-TCP tests verified notices/help/output-off; see [Module evidence](../module_system/README.md#共享故障紀錄驗證). No Linux/macOS in that round.

本輪已在 Windows Debug／Release 驗證本機與真實 TCP 的通知、help 重查及 output off 行為；
完整結果見 [ModuleSystem 驗證](../module_system/README.md#共享故障紀錄驗證)。Linux／macOS 未驗證。
<a id="待處理事項"></a>

## Outstanding issues / 待處理事項

- **Startup output visual acceptance (2026-11-01, resolved on Windows):** after
  the console character/color readback tests, the owner visually inspected the
  application and confirmed that the display is normal. This is user acceptance,
  not an assistant screenshot review. Linux/macOS validation of the new
  `write_console()` path remains outstanding.
- **啟動輸出目視驗收（2026-11-01，Windows 已解決）：** 真實 console 字元／顏色測試後，
  使用者已親自檢視並確認畫面正常。此為使用者驗收，並非助手截圖驗證；新
  `write_console()` 路徑的 Linux／macOS 驗證仍待完成。

Remote input redraw/login are deferred in root TODO/SOCKET. Native Linux/macOS build/deployment/cancel/exit and PuTTY GUI remain unverified. Native Tcl blocking work may delay cancel; no central-thread force-abort promise.

- 遠端背景輸出期間的輸入列重繪、密碼登入維持延後決策，見根目錄 TODO 與 SOCKET。
- Linux／macOS 的原生建置、部署、取消與退出行為仍待驗證；PuTTY GUI 亦未實測。
- 原生 Tcl 擴充或阻塞操作可能延遲取消，不承諾強制中止中央執行緒。

<a id="sdk-邊界與文件恢復2026-10-06"></a>

## SDK boundary/document restoration (2026-10-06) / SDK 邊界與文件恢復（2026-10-06）

This page had been overwritten by Storage material. Restored Shell text was reconciled with Results/strict paths/isolation/automatic startup, retaining Module faults and removing misplaced Array/singleton claims. Services remain central-owned with detail headers. Evidence: [SDK](../sdk/README.md#sdk-重審修正2026-10-06).

本頁先前被 Message Storage 主體覆蓋。本次以本機保留的 Shell 重工前快照為文字來源，
逐段核對現行 Result、嚴格路徑轉換、故障隔離及使用者的自動啟動決策；保留真正屬於
Shell 的 Module 故障顯示說明，移除誤置的 MessageArray 契約及已過時的 singleton 說明。
中央自行準備預設服務；Shell／SocketShell 接線標頭移到 detail，既有執行順序與政策不變。
本輪驗證結果見 [SDK](../sdk/README.md#sdk-重審修正2026-10-06)。

<a id="module-除錯命令"></a>

## Module debug commands / Module 除錯命令

These commands control user Modules. Protected System types/instances reject
debug start and stop; use `ui`/`recorder` service commands instead.

以下操作適用使用者 Module。System 種類／實例拒絕 debug start／stop，請用官方服務命令。

- debug start TYPE [args] waits at a pre-load Host gate and prints MID/PID/Plugin path. Native argv starts with TYPE. Wait uses shell_command_timeout; timeout/script cancel does not stop the created Host—inspect/help/stop explicitly.
- debug continue MID queues release immediately, not Ready or debugger operations.
- debug stop MID uses normal bounded Module stop, even before name registration.
- Help marks DEBUG/pre-load wait; debug is reserved.
- Local commands use async workers to keep Ctrl+C responsive; remote/Tcl share the router, with complete shutdown for debug Modules too.

- `debug start <ModuleType> [args...]`：等待 Host 載入前 gate，顯示 MID、PID、Plugin 路徑。
  Module argv 沿用一般啟動語意，第一個元素為 ModuleType。取得 gate 的等待最多為既有
  `config::shell_command_timeout`；逾時／腳本取消不停止已建立的 Host，使用 help 查閱並停止。
- `debug continue <MID>`：排入啟動放行，立即返回；不等應用 Ready，不操作 debugger 本身。
- `debug stop <MID>`：使用既有有界 Module 停止流程，也能處理尚未註冊名稱的 Host。
- `help` 在 Modules 列表標示 DEBUG 與載入前等待。`debug` 成為保留命令名稱。
- 本機 debug 命令走既有非同步工作，避免等待阻塞 Ctrl+C；Socket Shell 與 Tcl 共用 router。
  明確 Ctrl+C／exit 對除錯 Module 仍執行完整 shutdown。

Debugger/symbol workflow and limits: [debugging guide](../module_system/DEBUGGING.md).

步驟及 debugger／符號限制見 [Module 除錯指南](../module_system/DEBUGGING.md)。

<a id="param-命令2026-10-08"></a>

## Param commands (2026-10-08) / Param 命令（2026-10-08）

Reserved list/get/set/save/status share local/Tcl/remote router. Central may write Limited; errors retain Shell→Module→Param causes without central exit. Persistence/type-change notices are independent of OutputHub subscriptions. [Param](../param/README.md) owns format/cache/save policy; listener settings were not moved.

新增保留命令 `param list`、`param get NAME`、`param set NAME VALUE`、`param save`、
`param status`；本地、Tcl、Socket Shell 共用 CommandRouter。中央權限可改 Limited
參數。錯誤以 Shell → ModuleSystem → Param 原因鏈呈現，失敗不要求框架退出。
本機與遠端會顯示新的 Param 保存／型別更新診斷；不是 Module OutputHub 訂閱的一部分。
完整格式、保存政策與快取時機由 [Param README](../param/README.md) 維護。
本輪不更動 Shell IP／port／timeout 的既有設定來源。

Param get/list display declared restart metadata; successful set adds a reminder
without restarting or saving. Same-value writes still show the declaration;
failed writes do not show success reminders. Local/Socket/Tcl share the reply;
see [Param restart hints](../param/README.md#restart-hints) for semantics and evidence.
Param get／list 顯示作者宣告的重啟需求，set 成功後提醒但不重啟、不保存；同值寫入也提示，
失敗不顯示成功提醒。本機／Socket／Tcl 共用回覆，契約與測試統一見上述 Param 文件。


<a id="log-指令2026-10-08"></a>

## Log commands (2026-10-08) / Log 指令（2026-10-08）

Log status/reopen/level select source by name/MID/all and severity. Each interactive Session has independent log show filters/module selection, default off without changing persistence/raw output. Tcl central commands work, but show requires an interactive owner. Commands/puts/input are not automatically logged; see [Log](../log/README.md).

新增 log status、log reopen、log level MODULE_OR_MID_OR_all error|warn|info|debug。
本地／遠端 Session 各自支援 log show on|off|error|warn|info|debug，及 log show module NAME_OR_MID_OR_all。
show 預設 off，僅影響顯示；output on/off 原意不變。Tcl 可用中央指令，show 需互動 Session。
不把命令結果、Tcl puts 或使用者輸入全部保存。見 [Log](../log/README.md)。
