<a id="vinc-log"></a>

# Vinc Log / Vinc 紀錄服務

Central collects Module stdout/stderr and structured context.log records. Log depends on Common, Control Channel, OSAL, OutputHub; Module System owns lifetime. It does not supervise processes/fatal faults and uses neither Message Bus nor pump for delivery.

中央保存 Module 的 stdout／stderr，以及 `context.log` 的結構化紀錄。
此元件依賴 Common、Control Channel、OSAL、OutputHub；Module System 管理其生命週期。
Log 不負責生命週期或 fatal 監督，也不經 Message Bus 或 pump 傳輸。

<a id="module-api"></a>

## Module API / Module 介面

```cpp
auto logged = context.log.info("Camera {} started", device);
if (!logged) {
    // Decide whether this application needs to report or count rejected logs.
}

(void)context.log.warn("Dropped {} frames", dropped);
(void)context.log.error("Device {} failed", device);
(void)context.log.debug("Exposure = {}", exposure);
```

- error/warn/info/debug use fmt compile-time format strings; write(Level, string_view) accepts existing text.
- Default source level Info retains Error/Warn/Info and skips Debug. Filtered logs succeed without formatting; C++ arguments are still evaluated before the call.
- Formatted text is limited to config::log_message_bytes (8 KiB); excess returns TooLarge, not silent truncation. Fixed output buffers run user formatters immediately, without retained arguments/background formatting.
- `Result<void>` success means local send acceptance only, not central receipt, file write, or power-loss durability. Full capacity returns Backpressure without waiting; Closed/ChannelFailed retain available causes.
- Host binds identity/transport; Plugins cannot supply source name/MID/PID or own control service. context.log binds after name approval/before on_start, through normal Module destruction; constructors are unbound.
- Concurrent calls need no pump; Host watchdog applies source levels. STL/allocator/user-formatter exception policy is not globally changed.
- Error severity itself neither terminates nor restarts Modules. Returning a Log failure from a hook invokes that hook's existing failure policy.

- `error`、`warn`、`info`、`debug` 使用 fmt 的編譯期格式字串；`write(Level, string_view)` 寫入既有文字。
- 預設來源等級 Info：Error／Warn／Info 收集，Debug 略過。來源過濾回傳成功，且不做格式化；
  C++ 函式引數仍會在呼叫前求值。
- 每筆格式化後文字最多 `config::log_message_bytes`（8 KiB）；超過回報 `TooLarge`，不靜默裁切。
  格式化使用固定輸出 buffer，使用者 formatter 當場執行；不保存引數或把 formatter 延後到背景執行緒。
- 回傳 `Result<void>`：成功只代表本機傳送佇列已接收，未承諾中央收到、寫檔或斷電持久性。
  容量滿回報 `Backpressure`，不等候空位；Closed／ChannelFailed 保留可取得的下層原因。
- ModuleHost 綁定身份與傳送 adapter。Plugin 不可指定來源名稱／MID／PID，不持有 control service。
  `context.log` 在名稱核准後、`on_start()` 前綁定，有效至正常 Module 解構完成；建構期尚未綁定。
- 可多執行緒呼叫；不需 pump。來源等級由既有 Host watchdog 套用。
  STL／allocator／使用者 formatter 的例外政策未全面改變。
- Error 等級本身不終止或重啟 Module。使用者若把 Log 的失敗直接回傳為 hook 失敗，
  仍會依該 hook 的原本故障政策處理。

<a id="執行緒與傳輸"></a>

## Threads and transport / 執行緒與傳輸

```text
Plugin context.log → Host Access → 獨立 Log Session ─┐
Module printf → OSAL PipeService → OutputHub ──────┼→ 中央 collector → 有界 disk queue → disk worker → spdlog sink
中央框架診斷 ─────────────────────────────────────┘                └→ 獨立 Shell display history
```

- Central has shared collector and disk-worker threads; Modules gain no Log thread.
- Log and lifecycle Sessions have separate queues on existing Host/central Control I/O threads. CPU contention remains; this is neither hard real-time nor total resource isolation.
- Host connection waits at most log_connect_timeout (1 s), retaining failure while Module startup continues. No reconnect/replay. Bad protocol closes only the Log Session.
- Error/Warn reserves are 8 frames/32 KiB at Host and 64 records/512 KiB centrally. These protect only those queue points, not end-to-end delivery, and do not reorder FIFO.
- Central Log uses DropNewest receive overflow with independent counters; normal control retains CloseSession.
- Disk queue is 1024 records/8 MiB logical text/metadata; full queues drop new records. Separate same-sized display history retains newer records, so slow Shell does not block disk work.
- Collector processes at most 16 records/source/pass, polling every 20 ms. One mutex protects identities/queues/stats, never file I/O or user callbacks.

- 中央新增 collector、disk worker 兩個共用執行緒；Module 不新增 Log 執行緒。
- Log Session 共用 Host／中央既有 Control Service I/O 執行緒，與生命週期 Session 分開排隊。
  共用 I/O 執行緒仍會有 CPU 競爭，不宣稱硬即時或完全資源隔離。
- Host Log 連線最多等待 `log_connect_timeout`（1 秒）；失敗保留錯誤，照常繼續 Module 流程。
  第一版不自動重連、不重播。錯誤格式或版本關閉 Log Session，不以此要求停止 Module。
- Host 傳送容量預留 8 frames／32 KiB 給 Error／Warn；中央 disk queue 預留 64 筆／512 KiB。
  這是這兩個排隊點的容量保留，不是端到端保證，也不重排 FIFO 中既有紀錄。
- 中央 Log Session 選用 Control Channel `DropNewest` 接收政策，損失計數不依賴紀錄佇列。
  一般控制 Session 維持原本 `CloseSession`。
- 中央 disk queue 上限 1024 筆／8 MiB（文字與紀錄 metadata 的邏輯預算）；滿時丟棄新紀錄。
  display history 另有同樣上限，保留較新的紀錄；慢 Shell 不會阻塞 disk worker。
- 收集每來源每輪至多 16 筆，輪詢間隔 20 ms。相同中央 mutex 保護 identities、queues、stats，
  檔案操作不持此 mutex；collector 不執行使用者 callback。

<a id="紀錄與損失"></a>

## Records and losses / 紀錄與損失

Each record includes Vinc name/startup identity, Module name/MID/PID, central collection sequence, source, level, time, and text. Framework diagnostics use module=vinc and MID/PID 0 rather than impersonating a Module.

每筆含 Vinc 名稱、此次中央啟動識別、Module 名稱、MID、PID、中央收集序號、來源、等級、
時間與文字。框架自身診斷使用 `module=vinc`、MID/PID 0 表示中央，而非假冒 Module。

- Structured records carry producer wall-clock microseconds; all records have receive wall/monotonic times. Display uses local time/UTC offset; numeric units do not promise OS precision.
- printf has no producer timestamp/severity: produced_us=unknown/RAW, separate stdout/stderr. Raw chunk capture supplies receive time; a multi-chunk line uses its final delivered event time.
- Before name approval, source is starting:MID. Rename flushes prior fragments with their original name; historical identity is not rewritten.
- Central sequence is collection order, not cross-process timestamp sorting.
- Controls/newlines/backslashes are escaped; other bytes retain their value without guessing stdout encoding. Long raw lines use [fragment]; tails/rename fragments [tail]. Normal fragmentation does not split UTF-8 scalars.
- log status exposes disk drops, OutputHub lost chunks/bytes, transport drops, Host rejects, file losses, and disconnect uncertainty, with per-active-source stats. Chunk counts are not printf-call counts.
- OutputHub source attribution is bounded; overflow retains aggregate/unattributed counts. Allocation-failure loss diagnostics allocate nothing.
- Sequence, raw-stream, and file-delivery gaps are marked, but diagnostic records may themselves drop; counters are independent evidence.
- Host watchdog resends nonzero reject totals every second using reserved capacity, even without new application logs. Disconnection/persistent congestion still limit delivery.
- Unfenced disconnect leaves the final in-flight count unknown. Stats cover the current run; exited sources retain totals/available diagnostics without an unbounded source list.

- 時間：結構化紀錄有 producer wall clock 微秒數；全部紀錄有接收 wall clock 與 monotonic 時間。
  顯示本地時間及 UTC offset，數值单位不代表 OS 有相等的實際精度。
- printf 無 producer 時間或 Log level；標成 `produced_us=unknown`／`RAW`，stdout、stderr 分開。
  OutputHub 的 wall clock 是收集原始 chunk 時取得。跨 chunk 行使用最後交付它的事件時間。
- 啟動名稱核准前使用 `starting:MID`；名稱變更前的 fragment 以舊名交付，不改寫歷史身份。
- 中央序號代表收集順序；不按跨程序 timestamp 排序。
- 控制字元、換行、反斜線轉義；保留其他原始 bytes，不猜測 stdout 的編碼。
  長 printf 行以 `[fragment]` 分段；尾端／改名 fragment 用 `[tail]`。UTF-8 scalar 不因正常長行分段被拆開。
- `log status` 提供 disk queue drops、OutputHub lost chunks／bytes、transport drops、Host rejects、
  file losses 與 disconnect uncertainty。活躍來源另有各自的排隊／原始輸出損失統計。
  原始輸出計數不是 printf 呼叫次數。
- OutputHub 額外提供有界 source attribution：超出可歸屬的來源格數仍保留 aggregate／unattributed 計數；
  allocation failure 的 gap 不會因為建立診斷而再次配置。
- 序號缺口、原始 stream gap 與檔案交付 gap 會標記；診斷紀錄本身仍可能被丟棄。
  counters 獨立保存，不以「成功排入一則 gap Log」作為唯一證據。
- Host watchdog 每秒重送拒絕累計值（有拒絕時），使用預留傳送容量；接收壅塞解除後可補查，
  不依賴下一筆應用 Log。統計同樣不能突破斷線或持續壅塞的交付限制。
- 沒有正常 fence 的斷線，最後在途紀錄數未知；不假裝知道丟失了幾筆。
  stats 僅描述本次執行，來源退出後保留總計及可保存的診斷，不無限累积來源清單。

<a id="檔案與容量"></a>

## Files and capacity / 檔案與容量

Default: `<Vinc executable directory>/vinc_data/<VINC_NAME>/logs/YYYY-MM-DD/vinc-00000000000000000001.log`. Central startup initializes storage; Module authors manage neither spdlog nor files.

預設路徑：`<Vinc 執行檔目錄>/vinc_data/<VINC_NAME>/logs/YYYY-MM-DD/vinc-00000000000000000001.log`。
由中央啟動自動初始化；Module 使用者不操作 spdlog 或檔案。

| Setting | Default | Meaning |
| --- | --- | --- |
| config::log_file_bytes / FileOptions::max_file_bytes | 16 MiB | Per-file limit |
| config::log_total_bytes / FileOptions::max_total_bytes | 128 MiB | All managed date directories combined |

| 設定 | 預設 | 含義 |
| --- | --- | --- |
| `config::log_file_bytes`／`FileOptions::max_file_bytes` | 16 MiB | 每個檔案的上限 |
| `config::log_total_bytes`／`FileOptions::max_total_bytes` | 128 MiB | 所有受管理日期目錄的檔案總上限 |

- Limits must be positive, total>=file. Accounting uses final encoded text plus one LF, not filesystem allocation units/directories/metadata. A record exceeding file capacity fails rather than endlessly rotating.
- Receive date chooses the directory; file sequence continues across dates. Clock rollback still evicts oldest closed files by creation sequence.
- Startup/reopen scans existing managed names into the total, creates rather than overwrites, and makes budget before writing. When total==file, the active file may close and be evicted. Only empty managed date directories are removed, never recursive trees.
- Operate only regular files matching managed names/dates. Reject symlink ancestors and do not follow date-directory symlinks. Concurrent external changes are unsupported; this is not a same-account security sandbox.
- Delete/rotate/write/flush failure disables file output and exposes errors via log status/Shell; other services continue. Resolve disk issues then explicitly log reopen. Lost records are not replayed.
- The disk worker alone owns a basic sink, without spdlog registry, async pool, or flush thread. Flush normally at most once/second; Error flushes when the worker processes it. No fsync/power-loss guarantee.
- Shutdown: Module hooks/destruction → Host FIFO-fence attempt (<=1 s) → OS exit → OutputHub tail drain → collector drain → disk flush. Fence proves collection of preceding non-dropped records, not file persistence. Final failures after Shell exit use numeric stderr diagnostics without recursive logging; fatal shared reports remain separate.

- 值須大於 0，total 不小於 file。按最後實際編碼文字＋一個 LF 計算邏輯檔案 bytes，
  不包含 filesystem allocation unit、目錄或 metadata。超過單檔容量的一筆回報錯誤，不反覆輪替。
- 接收日期決定日期目錄；序號跨日遞增，時鐘往回跳亦依建立序號清掉最舊的封閉檔案。
- 啟動／reopen 掃描既有受管理檔名，納入總量，建立新檔而不覆寫；寫入前先騰出預算。
  total==file 時允許關閉並淘汰原本 active file。空的受管理日期目錄才移除，不遞迴刪目錄。
- 僅操作指定的日期／檔名規則與 regular files；拒絕 root 的 symlink ancestors，不跟隨日期目錄 symlink。
  對 Log 目錄進行並行外部修改不受支援；這不是抵抗同一 OS 使用者惡意替換檔案的安全沙箱。
- 刪除、輪替、寫入或 flush 失敗時停用 file output，`log status`／Shell 診斷可查錯誤。
  其餘框架服務繼續；解決磁碟問題後明確 `log reopen`。已丟失紀錄不重播。
- disk worker 單獨持有 basic file sink，不使用 spdlog registry、async pool 或額外 flush thread。
  一般最多每秒 flush；Error 在 disk worker 處理該筆後 flush。未承諾 fsync 或斷電保存。
- 正常關閉：Module hooks／解構完成 → Host 嘗試 FIFO fence（最多 1 秒）→ Module OS exit →
  OutputHub tail drain → collector drain → disk queue flush。fence 表示中央收集到此前未丟失的紀錄，
  不表示已寫入檔案。Shell 已關閉後的最後失敗，中央以 stderr 留下數值診斷，不遞迴寫入 Log。
  Fatal shared report 始終獨立。

Budgets/intervals live in config; this work did not migrate existing settings into Param.

所有新預算／間隔放在 config；本輪沒有把既有設定遷入 Param。

<a id="shell"></a>

## Shell / Shell 操作

```text
log status
log level camera debug
log level 42 warn
log level all info
log reopen
log show on
log show debug
log show module camera
log show module all
log show off
```

log level affects currently matching names/MIDs; all applies only to existing sources. log show controls display for the current local/Socket Session, defaults off, and changes neither persistence nor source level. Display batches are at most 16 records/64 KiB with gap reports. output on/off independently controls raw Module output. Tcl supports status/level/reopen, while show needs an interactive Session. Command results, Tcl puts, and user input are not automatically logged.

`log level` 改目前符合名稱／MID 的 Module 來源等級；`all` 僅適用當時已存在的來源。
`log show` 是目前本地／Socket Shell session 的顯示設定，預設關閉，不改保存或來源等級。
顯示每次最多 16 筆／64 KiB，落後會顯示 gap。`output on/off` 仍獨立控制原始 Module 輸出。
Tcl 可使用中央 status／level／reopen 指令，show 需要互動 Session。
Shell 命令結果、Tcl puts 與輸入文字不自動納入 Log。

<a id="target-與第三方"></a>

## Targets and dependencies / Target 與第三方

- vinc::log: Module API and fmt, dependent on Common, without Socket/central ownership.
- vinc::log_host: Host transport, explicitly encoded protocol v1, private Control dependency.
- vinc::log_manager: central collection/files/diagnostics/display history, private spdlog.
- vinc_log_errors: static catalog; Common adds only ErrorDomain::Log=11.
- fmt 12.2.0 and spdlog 1.17.0 are pinned unmodified submodules under library/. spdlog shares external fmt. Wrapper-local options do not FORCE parent cache. SPDLOG_NO_EXCEPTIONS is not enabled; the file backend translates third-party I/O exceptions into Result.

- `vinc::log`：Module API、fmt，依賴 Common；沒有 Socket 或中央所有權。
- `vinc::log_host`：Host 專用傳輸、明確編碼 Log protocol v1，私有依賴 Control。
- `vinc::log_manager`：中央收集、保存、診斷與顯示 history；spdlog 私有相依。
- `vinc_log_errors`：靜態 error catalog；Common 只新增 `ErrorDomain::Log = 11`。
- fmt 12.2.0、spdlog 1.17.0 由 `library/` 的原樣 Git submodule 固定完整 revision。
  spdlog 使用同一份 external fmt。選項為 wrapper 目錄區域變數，不 FORCE parent cache；
  不啟用 `SPDLOG_NO_EXCEPTIONS`，第三方 I/O exception 在 file backend 轉 Result。

<a id="驗證與限制"></a>

## Recorded validation and limits / 驗證與限制

Tests cover formatting/filtering/limits, protocol versions/truncation, day changes/clock rollback, reopen, equal total/file limits, special-file rejection, cross-process raw/structured/Debug/shutdown-tail output, control survival after floods, file failure/reopen, and remote commands. Control tests cover DropNewest/reserves; SDK tests check parent-option isolation and no public spdlog definitions.

新增測試：格式化與過濾、上限、protocol 版本／截斷、跨日與回撥、重開、總容量與單檔相等、
非檔案替換拒絕、跨程序原始／結構化／Debug／停止尾端輸出、洪水後命令存活、
file failure／reopen 與 Socket Shell 指令。Control 測試加入 DropNewest 與保留傳送容量；
SDK 檢查 parent 設定隔離及公開標頭不洩漏 spdlog definitions。

Recorded 2026-10-09 on Windows x64/MSVC: Debug/Release builds and full **67/67** CTests each passed. A paused-receiver transport fixture verified Backpressure and reject-counter recovery after reception resumed with no new application logs. No native Linux/macOS validation in that round.

2026-10-09 在 Windows x64／MSVC 完成 Debug 與 Release 建置，兩種組態的完整 CTest
皆為 67／67 通過。另以暫停接收的傳輸測試確認 Backpressure，以及恢復接收後在沒有新
應用 Log 的情況下仍能補送拒絕統計。後續 Windows／macOS 結果與 Linux 報告範圍見[平台驗證報告](../../../README.md#validation-and-limits)。

<a id="待處理事項"></a>

### Outstanding issues / 待處理事項

- OS limitation: synchronous filesystem I/O can block indefinitely, preventing bounded writer join. File work does not hold the central Log mutex, so collector/Module control do not wait on that lock. No detach-based abandoned-worker fallback exists.
- Native Linux/macOS builds and behavior remain to be validated; Windows results do not prove all platforms.
- Crashes, unflushed CRT output, unfenced disconnects, or disk failures can lose records. Log is not a crash-dump tool.

- 已知 OS 能力限制：同步檔案 I/O 若被 OS／filesystem 長期卡住，writer join 無法保證有限時間；
  collector／Module control 不因檔案 I/O 持中央 Log mutex。沒有以 detach 遺留背景資源的替代路徑。
- 平台待驗證：Linux／macOS 仍須實機建置及行為驗證，Windows 結果不代表三平台已通過。
- 策略限制：程序 crash、CRT 未 flush、無 fence 斷線或磁碟故障可能遺失 Log；這不是 crash dump 工具。
