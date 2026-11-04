# File Log parameters / 檔案 Log 參數

## Ownership and application / 所有權與套用

| Param | Type | Default | Permission / restart hint |
| --- | --- | --- | --- |
| `vinc.log_file_enabled` | bool | true | Limited / App |
| `vinc.log_file_bytes` | uint64 | 16777216 (16 MiB) | Limited / App |
| `vinc.log_total_bytes` | uint64 | 134217728 (128 MiB) | Limited / App |

Central owns all three. Load persistence, register declarations, copy and validate
one selected Param snapshot, then configure file output before starting Modules.
The Module System integration owns `LogSettings`; Log receives `FileOptions` and
does not depend on Param. Config constants remain registration/standalone defaults.
There is no new command-line override, worker, validator registry or control protocol.

三項均由中央維護。先載入保存資料、註冊、取得副本並驗證，再於 Module 啟動前設定
檔案輸出。Module System 整合層的 LogSettings 負責 Param；Log 元件只接收 FileOptions，
不反向依賴 Param。config 保留首次註冊／獨立使用的預設。不新增 CLI、執行緒、
驗證器 registry 或控制協定。

Changes affect desired settings only. `param save` persists them; the next App
startup adopts them. The running writer retains a separate applied snapshot.
`log reopen` retries that applied snapshot after file failure, never the latest
Param values. Setting true during a disabled run cannot bypass restart via reopen.

修改只改期望值，save 後下次 App 啟動套用。本次 writer 保存獨立的已套用快照；
log reopen 只重試本次快照，不讀新 Param。停用期間把參數設為 true，也不能透過
reopen 繞過重啟。

```text
param set vinc.log_file_enabled false
param save
```

Restart the App, then inspect `log status` / 重新啟動 App 後，以 `log status` 確認寫檔已停用。

To raise capacity, set total first, then file, and save. For example, 256 MiB
total and 32 MiB per file / 提高容量時先總量、後單檔，最後保存，例如總量 256 MiB、單檔 32 MiB：

```text
param set vinc.log_total_bytes 268435456
param set vinc.log_file_bytes 33554432
param save
```

## Validation and correction / 驗證與修正

Capacities must satisfy `file_bytes > 0` and `total_bytes >= file_bytes`, even
when files are disabled. A bad pair restores both capacities to the last validated
pair; before any valid read, use code defaults. The last validated pair is not
necessarily the currently applied pair. The enable flag is not changed by a
capacity repair. Typed parsing rejects wrong types, negatives and integer overflow.
Increase total before file; decrease file before total. A tiny positive file limit
may still reject an encoded record as TooLarge under the existing file contract.

即使關閉寫檔仍須驗證容量：單檔 > 0，總量 ≥ 單檔。非法組合把兩項一起恢復為
最後合法組合；首次未有合法值則用程式預設。「最後合法」不等於本次正在使用的值，
容量修正不改開關。型別檢查拒絕錯誤型別、負數、整數溢位。放大時先改總量，
縮小時先改單檔。極小但合法的單檔上限，仍可能依原契約拒絕較大的編碼紀錄。

All current central Shell/local/remote/Tcl Param writes share the owner refresh
point after a successful relevant set. Future central writers must also invoke
owner validation. Corrections are ordinary writes, not atomic pair transactions
or CAS; concurrent edits may race and a partial repair failure is reported.
They do not implicitly save. `param save` is not a validation/callback barrier.
File permission or disk failures do not justify rewriting valid parameters.

目前本機／遠端 Shell 與 Tcl 的相關 set 成功後共用消費端驗證；後續新增中央寫入者
也須接上。修正是普通寫入，不是成對交易或 CAS，可能與並行修改競爭；部分修正失敗
必須回報。不隱含 save，save 也不是驗證／callback 屏障。磁碟或檔案權限故障不改合法設定。

## Disabled, unavailable and failed / 停用、不可取得及故障

Disabled startup never creates/scans/rotates/removes Log files or queues records
for future writing. It preserves existing files. Context Log and raw capture still
run; bounded display history remains available, with its existing loss semantics.
Intentional omission does not increment disk queue drops or file_lost. Real
transport/source/raw-capture loss counters continue. There is no later replay.
The existing writer sleeps without a periodic flush deadline while disabled and
is not signalled for each display-only record; shutdown still wakes and joins it.

停用啟動不建立／掃描／輪替／刪除 Log，不排隊等日後補寫，也不動既有檔案。
Context Log、原始擷取與有界顯示歷史保留；顯示本身仍可能落後。刻意不寫檔不計為
磁碟佇列丟棄或 file_lost；真正的傳輸、來源與擷取損失仍計數。不重播。
既有 writer 停用時不週期 flush，不因僅供顯示的紀錄喚醒；關閉仍喚醒並 join。

Before configuration, records can enter display history but are not retained for
future file writing. StartupReport/stderr supplies diagnostics without requiring
a file. If registration/read/correction cannot establish trustworthy settings,
files stay unavailable, report the cause, reject reopen, and let central continue.
This differs from ordinary Param persistence failure when valid live defaults
remain available: those defaults may still be adopted.

初始化設定前只保留有界顯示歷史，不累積待寫檔紀錄；StartupReport／stderr 不依賴
檔案即可回報。註冊／讀取／修正失敗而無法取得可信設定時，檔案維持 unavailable、
拒絕 reopen、中央繼續。若僅保存 I/O 失敗但 Param 仍提供合法 live 預設，則可正常採用。

`log status`: unconfigured = no decision yet; disabled = startup policy off;
unavailable = settings could not be obtained; enabled/failed = startup permits
files and the sink is healthy/faulted. It shows applied capacities, not promises
that recent Param edits are active. Startup marks intentional disabling as healthy.
`bytes=0` while disabled means no file accounting was performed, not that existing
files were deleted or the directory is empty.
When files are not available it prints error details instead of directing users to
nonexistent Log files. File I/O failure still disables file output without stopping
the framework; explicit reopen remains the recovery mechanism for enabled policy.

status 區分 unconfigured、disabled、unavailable、enabled／failed，並顯示已套用容量。
停用時 bytes=0 表示沒有掃描計算檔案，並非既有檔案被刪除或目錄為空。
啟動摘要不把設定停用當故障；檔案不可用時直接呈現錯誤，不指向不存在的 Log。
檔案 I/O 故障仍只停用寫檔；本次允許寫檔時可用 reopen 恢復，中央不因此終止。

## Validation / 驗證

2026-11-04, Release, topic `codex/log-file-params` (local work tree). These are
targeted regressions, not a full-project/Debug/three-platform acceptance claim.

2026-11-04，Release，分支 `codex/log-file-params` 的本機工作樹。以下為針對性回歸，
未宣稱本輪跑過全專案、Debug 或三平台驗收。

| Environment / 環境 | Source tests / 來源測試 | Installed SDK |
| --- | --- | --- |
| Windows 11 x64 build 26200, MSVC 19.51.36257, CMake 4.3.2, CPython 3.13.12 | 21/21 | Consumer 1/1; application rerun 1/1 / consumer 與 App 重跑各通過 |
| WSL2 Ubuntu 24.04 x64, GCC 13.3.0, glibc 2.39, CMake 3.28.3, CPython 3.12.3 | 16/16 service tests + 2/2 App tests | 2/2 consumer/application |
| Native macOS / 原生 macOS | Pending / 待驗 | Pending / 待驗 |

Coverage / 覆蓋範圍：

- `log.settings`: Limited/App declarations, typed rejection, last validated pair,
  desired versus applied capacities, save/restart, no implicit save on repair,
  disabled display/counters/files, unavailable configuration and reopen rejection.
- `log.process`: real Plugin structured/raw output, command/lifecycle survival,
  file failure/reopen, queue floods, shutdown tails and disabled collection.
- `app.entry` and `sdk.installed_application`: three real App runs (save off,
  disabled run saving on, restarted enabled run), Log file inventory/hash/mtime
  preservation while disabled, startup messages and no reopen bypass.
- Shell/Tcl/Param, System Module startup and Python service regressions. SDK
  consumer/application also retain existing relocation, custom message, UI,
  Publisher, Recorder and package-inventory checks.

涵蓋宣告、型別、最後合法容量、期望值／套用值分離、保存重啟、修正不自動保存、
停用與不可取得狀態、真實 Plugin 收送／退出，以及安裝後三次 App 啟停。
停用期間比對 Log 檔案清單、SHA-256 與修改時間；並重跑既有 Shell／Tcl／Param、
System Module、Python 服務與 SDK 搬移／自訂訊息／UI／Recorder 驗收。

Windows' first installed-application run reached the old 360-second suite timeout
after all three Log flows had exited normally, during final CMake rejection checks.
The suite budget is now 600 seconds for the extra App launches and repeated clean
deployments; individual operation limits remain unchanged. This changes only test
budgeting, not production deadlines. The clean application rerun passed in
324.87 seconds; retain the original timeout evidence rather than erase it.

Windows 首次 installed application 在三次 Log 流程正常退出後，於最後 CMake 拒絕案例
遇到舊的 360 秒整套期限。因新增 App 啟動及多次乾淨部署，整套上限調為 600 秒，
單一步驟期限不變；不改正式程式 timeout。乾淨 App 重跑以 324.87 秒通過，保留原始逾時證據。

Local evidence is under `build/log-params/`: build logs, Windows/WSL CTest logs
and XML, the initial Windows timeout/flow captures, and the Linux source/process
audit. Linux audit compared 724 non-Markdown tracked/new source files with no
differences and found no remaining task processes. The only later test change was
the larger overall SDK deadline, which Linux had passed under the stricter limit.
These ignored artifacts are not shipped in the SDK.

本機證據位於 `build/log-params/`，不隨 SDK 交付。Linux 核對 724 個非 Markdown
來源檔案一致，無殘留測試程序；其後僅同步 SDK 整套期限調整，Linux 原本已在較嚴格
期限內通過。macOS 須在同一分支重跑上述來源與乾淨 SDK 案例，舊 Log／Param 證據不能替代。
