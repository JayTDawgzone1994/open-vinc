<a id="userexamplemodulecpython-使用體驗對照"></a>

# userExampleModule: C++/Python API comparison / userExampleModule：C++／Python 使用體驗對照

Runnable Module SDK examples inherit BaseModule and use their Context for Bus/Param/Log, not self-managed Socket/heartbeat/shared regions. Scope is normal authors, excluding standalone OSAL/central Manager/detail integration. Component contracts, not examples, define semantics.

這是一組可執行的 Module SDK 範例。從繼承 `BaseModule` 開始，透過自己的 `context`
使用 Bus／Param／Log；不需要自己建立 Socket、heartbeat 或共享記憶體。
本範例涵蓋一般 Module 作者的介面；OSAL 的獨立工具、中央 Manager、內部 detail 接線不在此範圍。
API 契約以各元件 README 為準，不以範例取代契約。

<a id="建議閱讀順序"></a>

## Suggested reading order / 建議閱讀順序

**English**

1. [C++ main Module](cpp/user_example_module.cpp) / [Python](python/userExamplePython/userExamplePython.py): startup, publish, subscription/Param callbacks, Shell hooks, shutdown.
2. [C++ storage examples](cpp/message_examples.cpp) / Python demonstrate_storage: container/error-checking cost.
3. [C++ Manual](cpp/manual_module.cpp) / [Python Manual](python/userManualPython/userManualPython.py): pumpOnce versus pumpLoop.
4. [Frame](../../vinc_messages/messages/UserExampleFrame.msg) / [Item](../../vinc_messages/messages/UserExampleItem.msg): scalars, strings, bool/record/nested arrays.

**繁體中文**

1. [C++ 主 Module](cpp/user_example_module.cpp)／[Python 主 Module](python/userExamplePython/userExamplePython.py)：
   看 `on_start`、`publish`、訂閱 callback、Param callback、`module_interface`、`on_stop`。
2. [C++ 訊息操作](cpp/message_examples.cpp)／Python 的 `demonstrate_storage`：看容器與錯誤檢查的使用成本。
3. [C++ Manual](cpp/manual_module.cpp)／[Python Manual](python/userManualPython/userManualPython.py)：
   比較自行呼叫 `pumpOnce` 和交給 `pumpLoop`。
4. 共用訊息 [UserExampleFrame.msg](../../vinc_messages/messages/UserExampleFrame.msg)、
   [UserExampleItem.msg](../../vinc_messages/messages/UserExampleItem.msg)：純量、字串、bool 陣列、訊息陣列與巢狀陣列。

Examples explicitly check Results, without macro/throw-on-every-failure helpers. C++ startup wraps ModuleSystem causes; interactive failure prints diagnostics/status 1 and stays running. Python commands catch expected SDK/input errors; escaping callback exceptions still fail that Module.

每個例子明確檢查錯誤，沒有用巨集或「失敗一律 throw」的 helper 隱藏 C++ Result 操作。
C++ 啟動失敗包成 ModuleSystem 原因鏈；互動命令失敗印出診斷並回狀態 1，保持 Module 運作。
Python 互動命令捕捉預期的 SDK／輸入例外；未處理的 callback 例外仍依框架政策使該 Module 失敗。

<a id="建置與部署"></a>

## Build and deploy / 建置與部署

Select userExampleModule/userManualModule and enable Python in App Kconfig:

在 App Kconfig 選入 userExampleModule／userManualModule，並在 App Kconfig 設定 `CONFIG_VINC_APP_PYTHON=y` 後：

```sh
cmake --build build/windows-msvc-debug --target user_examples_python
```

This explicit target builds central/C++ Plugins and copies two Python Modules beside Vinc:

這個明確執行的 target 建置中央及 C++ Plugin，再複製兩個 Python Module 到執行檔旁：

```text
data/module/userExamplePython/userExamplePython.py
data/module/userManualPython/userManualPython.py
```

**Rerunning the deployment target overwrites those two Python examples.** Ordinary shell builds do not. Preserve edits under different type/directory/filenames; discovery runs only on central startup. Each Python example has one entry file without extra import-path configuration.

**再次執行此部署 target 會覆寫這兩個 Python 範例檔案。** 一般 `Vinc` 建置不覆寫它們。
若要保留自己的修改，另取 ModuleType／目錄／檔名；中央啟動時才掃描一次。
Python 主範例全部在單一入口檔，不需要另外配置 import 路徑。

New-build configuration in an appropriate compiler environment:

新建置目錄的設定範例（在適用的編譯器環境執行）：

```sh
# Enable CONFIG_VINC_APP_PYTHON=y and choose System/Custom in apps/vinc/vinc.conf first.
cmake -S src -B build/my-build
cmake --build build/my-build --target user_examples_python
```

Python ABI must match build CPython; Windows MSVC needs official Windows CPython, not MinGW Python. Without runtime --python, PATH wins, not the build interpreter. The following uses a placeholder: replace it with your compatible CPython executable and build directory.

Python ABI 必須符合建置使用的 CPython；Windows MSVC 使用官方 Windows CPython，不能混用 MinGW Python。
省略執行時 `--python` 會用系統 PATH，不會自動選建置時的直譯器。
下例使用通用佔位路徑，請換成自己的建置目錄與相容 CPython 執行檔：

```powershell
.\build\windows-msvc-debug\apps\vinc\Vinc\Debug\Vinc.exe --python C:/path/to/python.exe
```

C++-only use needs no Python. Building shell registers userExampleModule/userManualModule as Plugin libraries launched in separate Hosts, not executables. Added .msg types still require regeneration/rebuild; no dynamic schemas.

只有 C++ 時不必啟用 Python：建置 `shell` 後，`userExampleModule`／`userManualModule` 已在中央 catalog。
這兩個是 Plugin 動態庫，由中央啟動獨立 ModuleHost，不能直接當 executable 執行。
新增 `.msg` 仍須重新生成並建置；本範例沒有引入動態 schema 載入。

<a id="實際操作"></a>

## Run the examples / 實際操作

Enter in **Vinc Shell**, not PowerShell. Start two instances on one topic:

以下輸入到 **Vinc Shell**，不是 PowerShell。先啟動兩個實例，共用同一 topic：

```text
userExampleModule --name native --topic userExample/frames
userExamplePython --name peer --topic userExample/frames
native help
peer help
native storage
peer storage
native push 101
peer last
peer loan 202
native last
native status
peer status
native topics
```

Expect READY/STORAGE_OK/CALLBACK/MESSAGE. publisher is the approved Module name; publisher_id is Publisher identity, **not MID** (get_mid). Timestamp is Unix nanoseconds; format_utc works in C++/Python. A separate getter returns same-host monotonic time. get_last does not move callback cursors and may repeat a sample; no publication returns an error while the Module remains alive.

預期看到 `READY`、`STORAGE_OK`、`CALLBACK` 與 `MESSAGE`。`publisher=` 是中央核准的 Module 名稱，
`publisher_id=` 是 Publisher ID，**不是 MID**；MID 用 `get_mid()` 取得。
timestamp 為 Unix nanoseconds，可用 `vinc::format_utc()`／Python `vinc.format_utc()` 顯示 UTC。
`get_message_monotonic_timestamp()` 另提供同主機單調發布時間。`get_last` 不推進 callback 游標，可重複取得同筆。
尚未有發布資料時，`last` 回錯誤；Module 仍活著。

Each example Publisher uses 8 slots/64 KiB messages, not changed framework defaults. Callbacks own local messages; retained stores only the latest rather than grow indefinitely. Overwritten data can be missed; status reports loss/contention.

每個 Publisher 配置 8 slots、每筆 64 KiB；這些是本範例的容量選擇，未改框架 config。
callback 已取得本機訊息所有權，`retained_`／`retained` 只保留最後一筆，避免示範無限累積。
收送不保證每次 callback 都看得到被覆蓋的資料；`status` 顯示遺失與碰撞統計。

<a id="參數與權限"></a>

### Parameters and permissions / 參數與權限

```text
native params
peer params
native set peer.gain 2.5
peer set native.gain 3.5
native params
native set locked_gain 2.0
param set native.locked_gain 4.0
param set native.label reviewed
param get native.gain
param save
```

- gain is Double/Public, writable by every Module; locked_gain is Limited, writable by its owner and central.
- label is String/Limited with a Module restart hint; `status` shows the startup-applied `startup_label`. Example set accepts doubles only; central param set parses the registered type.
- Successful set acknowledges commit; subsequent pumps refresh caches/callbacks independently across instances.
- peer.gain demonstrates full-name watching; absent peer means absent map entry/missing watch.
- Explicit param save persists central values; exit does not save and examples do not save secretly.
- C++ callbacks borrow maps only during invocation; Python receives independent read-only snapshots.

- `gain` 是 Double／Public，其他 Module 也能改；`locked_gain` 是 Double／Limited，只有所屬 Module 與中央可改。
- `label` 是 String／Limited，宣告需重啟 Module；`status` 顯示啟動時套用的 `startup_label`。`set` 是本範例只接受 double 的 Module 命令，中央 `param set` 依登記型別解析。
- 成功 set 表示中央提交；快取在後續 pump 更新。`params`／callback 稍後才看見新值，不要求兩個實例同時更新。
- `peer.gain` 示範完整名稱監聽；沒有 peer 時 map 中沒有這筆，`watches` 顯示 missing。
- `param save` 會保存此中央的參數；退出不自動保存 live 調整。範例不偷偷呼叫 save。
- C++ callback 借用 map，不能保留其 reference；Python callback 收到獨立唯讀快照。

<a id="restart-hints"></a>

### Restart-hint example / 重啟標記範例

Both implementations declare `label` at registration, keeping the default Limited
permission. The C++ source uses `RegisterOptions`; Python uses a keyword:
兩種實作都在註冊時宣告 `label`，省略權限即為 Limited；C++ 使用 options，Python 使用關鍵字：

```cpp
auto label = context.param.register_param<std::string>(
    "label", "example", {.restart_hint = vinc::param::RestartHint::Module});
if (!label) {
    return label;
}
```

```python
self.context.param.register_param("label", vinc.param.Type.String, "example",
                                 restart_hint=vinc.param.RestartHint.Module)
```

The Module reads `label` once with `read_current()` during startup, then retains
it in `startup_label`. Param callbacks still receive updated metadata/values but
do not apply this field. Gain remains the example of live callback application.
The framework does not enforce this timing; the Module author owns it.
Module 啟動時用 `read_current()` 讀取 label 並保存在 startup_label；callback 仍收到新值，
但不套用此欄位。gain 則繼續示範即時 callback 更新。何時套用由作者負責，不由標記強制。

```text
param get native.label
param set native.label reviewed
native status
```

Get displays `[restart: Module 'native']`; set confirms commit and reminds the
operator to restart that Module. Status still shows `startup_label=example` on
its first lifetime. Explicitly stop the MID reported by status using `debug stop
<MID>`, then start `userExampleModule --name native` again: startup_label becomes
reviewed. Use the corresponding `peer`/`userExamplePython` commands for Python.

get 顯示 `[restart: Module 'native']`；set 確認提交並提醒重啟該 Module，但第一次啟動的
status 仍為 `startup_label=example`。以 status 顯示的 MID 執行 `debug stop <MID>`，
再執行 `userExampleModule --name native`，才會套用 reviewed。Python 用 peer 與
userExamplePython 做相同操作。MID 每次啟動不同，勿照抄舊編號。

Module restart retains the central live value. App restart drops unsaved edits:
use `param save` explicitly when the change must survive it. Restart hints are
not saved in TOML; each Module registers them again. `RestartHint::App` is also
available for author-declared whole-App requirements; these examples do not
invent an App-wide dependency merely to demonstrate that annotation.
Module 重啟沿用中央 live value；App 重啟會丟失未保存修改，需要跨 App 重啟保留時才明確
`param save`。TOML 不保存標記，Module 下次註冊重新宣告。若需求是整個 App，作者可選
`RestartHint::App`；本範例無此相依，不為展示標記而虛構 App 重啟需求。

The smoke test runs both actual implementations, verifies unchanged active values/MIDs after a write, no implicit save, loss of
unsaved edits after central restart, and startup application after explicit save
and App restart. See [Param](../../src/vinc/param/README.md#restart-hints)
for the shared contract and platform evidence.
自動 smoke 實跑兩種語言，驗證寫入後有效值／MID 不變、不隱含保存、
中央重啟丟失未保存修改，以及明確保存並重啟 App 後才於啟動時套用新值。共用契約與驗收證據見上述 Param 文件。

<a id="startup-parameters"></a>

### Startup values and correction / 啟動值與修正

```text
userExampleModule --name native --topic userExample/frames --gain 2.0
userExamplePython --name peer --topic userExample/frames --gain 3.0
```

Both Modules parse their own `--topic`/`--gain` flags. Gain must be finite and in
0..10; duplicate/unknown/incomplete flags or invalid startup gains reject startup.
The registered default stays 1.0. Each Module reads the committed Entry before
its first pump, applies an explicit override through `set()`, validates it, and
only then opens Bus resources. `INITIAL` reports the effective gain and fixed
default. A later start without `--gain` uses current state; after central restart
it uses the saved value. Nothing here automatically saves.

兩個 Module 自己解析 `--topic`／`--gain`。gain 必須有限且介於 0..10；重複、未知、
不完整旗標及非法啟動 gain 拒絕啟動。註冊 default 固定為 1.0；第一次 pump 前讀目前
Entry，透過 set 提交明確覆寫，驗證後才建立 Bus 資源。`INITIAL` 顯示有效 gain 與固定
default。不帶 `--gain` 的後續啟動沿用中央目前值；中央重啟後改用已保存值，不隱含 save。

For a correctly typed but unsuitable value loaded from TOML or delivered by a
callback, the example reports `CORRECT` and writes 1.0 using ordinary set. This
can overwrite a concurrent edit; it is not a conditional transaction. Startup
correction failure rejects startup; callback correction failure reports the
error and retains the last effective gain. `param save` does not wait for this
validation. Save again to persist a correction if the prior value was saved.

TOML 或 callback 給的值若型別正確但不適用，範例輸出 `CORRECT` 並用普通 set 寫回 1.0。
這可能覆蓋並行修改，不是條件交易。啟動修正失敗就拒絕啟動；callback 修正失敗則回報並
保留最後有效 gain。param save 不等待這項審核；若舊值已保存，需再次 save 才保存修正。
完整契約見 [Param](../../src/vinc/param/README.md#startup-and-correction)。

<a id="可恢復失敗與資源生命週期"></a>

### Recoverable failures and resource lifetime / 可恢復失敗與資源生命週期

```text
native capacity
peer capacity
native reopen
peer reopen
native push 303
peer last
```

capacity creates a valid large local message then demonstrates bounded Publisher rejection. storage uses checked at for recoverable bounds errors, not fatal C++ []. reopen explicitly closes/recreates Pub/Sub, with new IDs/stats. loan fills shared storage/commits; all views expire on success. Failed fill/commit explicitly closes that Publisher and requires reopen. Dropping an uncommitted loan does not cancel it; no invented cancel API.

`capacity` 先建立正常的本地大訊息，再示範 push 因 Publisher 容量不足被拒絕。
`storage` 使用 checked `at()` 示範越界回報；不執行會終止 Module 的 C++ `[]` 越界。
`reopen` 明確關閉並重新建立 Pub／Sub，Publisher ID 與統計會更新。
`loan` 正常路徑直接填共享 slot 並 commit；成功後所有借用 view 失效。
若填寫／commit 失敗，本範例明確關閉該 Publisher，需 `reopen` 後再借用。
只丟棄未提交 loan 不會自動取消它；範例沒有假造 cancel API。

<a id="log-與原始輸出"></a>

### Log and raw output / Log 與原始輸出

```text
native logs
peer logs
log level all debug
native logs
peer logs
log status
```

Severity is not failure policy: Error does not terminate a Module; Debug may filter. C++ fmt/Python f-strings format messages; Log failure falls back to raw output, never recursive Log. printf/print are captured without structured severity. Source-level changes arrive asynchronously, so the next record is not guaranteed to use the new setting.

Log 等級不是故障政策；寫 Error 不會讓 Module 終止。Debug 預設可能被來源過濾。
C++ 使用 fmt、Python 使用 f-string；記錄失敗時回 raw 輸出，不遞迴寫 log。
`printf`／`print` 由既有輸出擷取保存，沒有結構化 Log level。
來源等級套用有延遲，執行 `log level` 後不保證緊接的第一筆已用新等級。

<a id="manual-與停止"></a>

### Manual mode and stop / Manual 與停止

```text
userManualModule
manualCpp status
manualCpp finish
userManualPython --name manualPy
manualPy status
manualPy finish
```

finish returns normally from the user loop; after exit it may restart:

`finish` 讓自行管理的迴圈正常返回。等它退出後可再啟動：

```text
userManualModule loop
userManualPython --name manualPy loop
manualCpp status
manualPy status
exit
```

Loop mode calls pumpLoop in its own main, awakened by central stop without another pump thread. finish applies only to the batch-pump branch. exit/Ctrl+C stops central; debug stop <MID> stops one instance. Never recursively pump from FrameworkLoop callbacks/hooks; only one active pump per Context.

`loop` 模式在自己的 main 呼叫 `pumpLoop`，由中央停止要求喚醒退出；沒有另開 pump thread。
`finish` 只適用逐批 pump 的分支；整個中央可用 `exit`／Ctrl+C 關閉，單一實例可用既有 `debug stop <MID>`。
不要在 FrameworkLoop 的 callback／Shell hook 中遞迴呼叫 pump；同一 Context 只允許一個 active pump。

<a id="api-對照索引"></a>

## API comparison index / API 對照索引

| API/operation | C++ location | Python location/difference |
| --- | --- | --- |
| BaseModule/hooks/name/MID | public hooks | hooks; default naming consumes --name |
| Shell/status/completion | module_interface/module_autoComplete | same hooks; no remote completion UI |
| Run modes/living/pumps/stats | main/manual_module.cpp | main/userManualPython.py |
| advertise/subscribe/options | open_publisher/open_subscribers | type passed as argument |
| push/get_next/commit | publish | publish; native bridge handles GIL waits |
| get_last/owned callbacks/close/stats | execute/open_subscribers/close_bus | execute/received/close_bus |
| list_topics paging | execute(topics) | execute(topics) |
| factories/fields/nested arrays | message_examples fill | fill |
| size/capacity/empty/iteration | demonstrate_storage | len/capacity/len==0/list |
| resize/reserve/clear/shrink/push/emplace/pop | fill/demonstrate_storage | same except no emplace_back |
| []/at/set/assign/proxies | demonstrate_storage | checked Python indices/copy.copy view |
| vector import/export | copy_from/to_vector | assign(iterable)/list |
| MessageString bytes/copy | assign/str | assign/to_bytes/strict UTF-8 decode |
| message copy/assign/serialize/deserialize | demonstrate_storage | demonstrate_storage |
| usage/compact/snapshot/from_bytes | demonstrate_storage | usage/compact/serialize; no Snapshot view |
| names/hash/constants | msg_name_str/type_hash/protocol | compiled type/schema_hash/constants |
| six routing metadata getters including dual times | describe | describe |
| Param registration/defaults/permission/watch | register_parameters | explicit param.Type |
| Param callback/Entry/update/watches | registration/execute(params) | parameters_changed/execute(params) |
| Param set/set_value | execute(set) | typed set, no separate set_value |
| Param current read/startup override/business correction | initialize_gain/apply_gain | initialize_gain/apply_gain |
| Log enabled/write/format/severity | execute(logs) | demonstrate_logs; eager f-string |
| Result/Error/wrap/text | hooks/start_failure/storage/capacity | VincError.error/causes/standard exceptions |

| API／操作 | C++ 位置 | Python 位置／差異 |
| --- | --- | --- |
| BaseModule、on_name、on_start、on_stop、名稱／MID | 主檔 public hooks | Module hooks；預設 on_name 要求 --name |
| Shell 命令、狀態碼、自動完成 hook | module_interface、module_autoComplete | 同名方法；遠端 Shell 沒有補全 UI |
| FrameworkLoop／Manual、living、pumpOnce、pumpLoop、PumpStats | 主檔／manual_module.cpp | 主檔／userManualPython.py |
| advertise、PublisherOptions、subscribe 兩個 overload、SubscriberOptions | open_publisher、open_subscribers | 同名方法，型別作參數傳入 |
| push、get_next、commit | publish | publish；原生等待由 bridge 處理 GIL |
| get_last、callback 持有訊息、close、Pub/Sub stats | execute、open_subscribers、close_bus | execute、received、close_bus |
| list_topics 分頁 | execute(topics) | execute(topics) |
| make_message、data、純量、字串、bool[]、訊息陣列、巢狀陣列 | message_examples.cpp fill | fill |
| size／capacity／empty／iteration | demonstrate_storage | len／capacity 屬性／len == 0／list |
| resize／reserve／clear／shrink_to_fit／push_back／emplace_back／pop_back | fill、demonstrate_storage | 同名方法；沒有 emplace_back |
| []、at、set、assign、proxy copy／借用 | demonstrate_storage | [] 有 Python 索引檢查；copy.copy(record view) |
| std::vector 匯入／匯出 | copy_from、to_vector | assign(iterable)、list(array) |
| MessageString bytes、字串複製 | assign、str | assign、to_bytes、decode（strict UTF-8） |
| 訊息 copy／assign、serialize／deserialize | demonstrate_storage | demonstrate_storage |
| usage、compact、snapshot、from_bytes | demonstrate_storage | usage／compact／serialize；不提供 Snapshot view |
| 訊息名稱／hash／常數 | msg_name_str、type_hash、protocol | 編譯型別、schema_hash、constants |
| 路由 metadata 六個 getter（含雙時間） | describe | describe |
| Param 註冊／預設／權限、外部監聽 | register_parameters | register_parameters；明確指定 param.Type |
| Param callback／Entry.get／update／watches | register_parameters、execute(params) | parameters_changed、execute(params) |
| Param set／set_value、文字轉值 | execute(set) | param.set(Type, value)；沒有另設 set_value |
| Param 目前值／啟動覆寫／業務修正 | initialize_gain／apply_gain | initialize_gain／apply_gain |
| Log enabled／write／format／error／warn／info／debug | execute(logs) | demonstrate_logs；f-string 先格式化 |
| Result、Error.is、wrap_error、error_to_string | hooks、start_failure、storage、capacity | VincError.error、cause chain、標準 Python 例外 |

All fixed-width Param types share registration; examples choose double/string to show permissions/types. Supported types: [Param](../../src/vinc/param/README.md). Independent arbitrary local MessageArray, low-level Snapshot views, and BaseMessage diagnostics belong to [Storage](../../src/vinc/components/message_storage/README.md)/[Ribosome](../../src/vinc/ribosome/README.md); they do not make arbitrary Python classes transferable. RPC has a separate [example](../rpc/README.md).

所有固定寬度 Param 型別都用同一註冊方法；本例選 double 與 string 展示權限與型別差異。
支援型別清單見 [Param](../../src/vinc/param/README.md)。
獨立 `MessageArray<任意本地型別>`、Snapshot 的底層讀取 view 與 BaseMessage 文字診斷等延伸用法見
[Storage](../../src/vinc/components/message_storage/README.md)／[Ribosome](../../src/vinc/ribosome/README.md)，
不代表 Python 開放任意類別作為可傳輸訊息。
RPC 另見 [獨立範例](../rpc/README.md)。

<a id="驗證"></a>

## Recorded validation / 驗證

```sh
ctest --test-dir build/windows-msvc-debug -R "^examples.user_module$" --output-on-failure
```

Tests launch real central in temporary deployment, not daily vinc_data. They check both languages, bidirectional push/loan callbacks/metadata, Param changes/permission refusal, capacity, reopened handles, Manual completion, central exit. Short sleeps only schedule background work, not latency promises. Serialize same-name central tests across configurations to avoid exclusivity collisions.

測試在暫存部署目錄啟動真實中央，保存檔案不碰日常部署的 `vinc_data`。
檢查兩語言各自的容器操作、雙向 push／loan callback、發布者 metadata、參數更新／權限拒絕、
容量拒絕、重新開啟 handles、Manual 正常完成和中央退出。
測試內的短等待只讓背景流程有機會運作，不是框架的延遲承諾。
不同組態的中央測試應依序執行，避免同名 Vinc 的單實例鎖互相衝突。

Recorded 2026-10-12 Windows x64/MSVC/official CPython 3.13.12: both central/examples/native builds and **6/6** focused suites (example, four Python, Ribosome schema) passed. Release explicit Python deploy target succeeded; Debug copied identical examples. No full-project rerun or native Linux/macOS test that round.

2026-10-12：Windows x64／MSVC／官方 CPython 3.13.12，Debug 與 Release 的中央／範例／Python
擴充建置成功。兩種組態各通過 6/6 項重點 CTest（本範例、四個 Python 測試、Ribosome schema）。
Release 的 `user_examples_python` 部署 target 亦執行成功；Debug 同步複製相同範例檔案。
本輪未重跑全專案測試；Linux／macOS 未實機驗證。

<a id="macos-deployment-fixture-gap"></a>

## Deployment fixture repair / 範例部署測試修復（2026-11-03）

The temporary deployment now includes `modules/`, `licenses/`, and the bundled
App's `vinc_data/open-vinc/ui/` tree. It preserves relative Plugin/resource paths
without copying runtime logs or parameters. The test checks `ui/app/index.html`
and both official Modules reaching Ready, in addition to all original C++/Python
example assertions. Production Runtime, catalog and deployment contracts are unchanged.
Native macOS Debug and Release focused CTest runs both passed; Windows/Linux
execution of this fixture revision remains pending.

**已修復並完成 macOS 定點驗證；Windows／Linux 原生重驗待補。**
`tests/smoke.py` 現在補複製 `modules/`、`licenses/` 與 `vinc_data/open-vinc/ui/`，
保留 bundled App `open-vinc` 的相對目錄。只取 UI 資源，不搬入執行紀錄或參數資料。
新增 UI 入口 `ui/app/index.html` 存在與 Web UI／Bag Recorder Ready 斷言，
原 C++／Python 雙向訊息、容器、參數權限、Manual 模式與正常收尾斷言均保留。
三平台使用同一份腳本，沒有修改正式 Runtime、catalog 或 App 部署機制。

| 原生定點驗證 | 結果 | CTest 時間 |
| --- | --- | --- |
| macOS Debug `examples.user_module` | 1/1 通過 | 9.06 秒 |
| macOS Release `examples.user_module` | 1/1 通過 | 8.02 秒 |

環境沿用前輪：macOS 26.3.1(a)／Apple M1 Pro arm64、Apple Clang 21／libc++、
SDK 26.5、CMake 4.4.4、CPython 3.12.14。基底 `a19c0d4` 加既有退出修復及 Plugin 測試修復；
本輪唯一非文件差異是 `smoke.py`。重用前輪 Debug／Release binaries，前後 SHA-256
核對一致，不把這次描述成重新建置。兩組循序執行：

```sh
ctest --test-dir <Debug-or-Release-build> -R '^examples[.]user_module$' --verbose --output-junit <result.xml> --parallel 1
```

前輪完整 CTest 各 153/154、此範例失敗的紀錄保持不變；這輪只定點補驗該項，
**不是修復後再跑一次完整 154 項**。先前 `627c409` 的原始各 125/126 及外部部署對照
仍見 [SDK 原始報告](../../src/vinc/sdk/MACOS_VALIDATION.md)，退出修復第三步對照見
[退出修復驗收](../../src/vinc/osal/MACOS_EXIT_VALIDATION.md#example-deployment--範例部署)。

本輪初次新增入口檢查曾誤用 `ui/index.html`，在啟動 App 前即被斷言擋下；
依 [UI 部署契約](../../src/vinc/ui/README.md#部署) 修正為 `ui/app/index.html` 後，
兩組最終通過。初次失敗與最終結果分別保存，不覆寫歷史證據。

原始 logs、JUnit、命令、環境與 binary/source hashes 留在 Git 外：
`/private/tmp/open-vinc-example-deployment-20261103`。
交付壓縮包：`/Users/jay/Downloads/open-vinc-example-deployment-20261103/evidence.tar.gz`。
證據包 SHA-256：`7fb4d3849fa8e71ea2e442a2cc33dbd0570ab4dcc8e8445ae73a95abd6395c16`。
受測 `tests/smoke.py` SHA-256：`0854d380d6f11205f8c525506f018fd93c684cbaf967753deb634a5ed1830012`。
兩組原始輸出皆包含以下通過／回收訊息；完整 stdout 與 JUnit 保存在上述證據包：

```text
[OK] Web UI ready - browser workspace initialized (MID=1)
[OK] Bag Recorder ready - idle, waiting for recording commands (MID=2)
All module processes exited; Message Bus cleanup complete.
C++ / Python user examples passed
```

來源核對、34 個 App／Module／Python 檔案的 hash 比對與最後程序篩選均通過。
[本輪驗收總結](../../src/vinc/osal/MACOS_EXIT_VALIDATION.md#current-acceptance-status)
連結退出修復、Plugin 測試修復與此定點補驗，避免混淆各次結果。
本輪無 benchmark、commit 或 push；未新增 Windows／Linux 原生驗證證據。

### Parallel Param validation / develop 的 Param 驗證紀錄

2026-11-03 Param stage-one validation reproduced the same omission on Windows
x64 (MSVC 19.44.35229, Release, CPython 3.13.12). A separate test App explicitly
selected both example ModuleTypes. With `modules/` and its UI assets copied in
an external fixture variant, all updated C++/Python smoke assertions passed,
including owner Limited writes, foreign Limited rejection and Public writes.
At stage 1, the repository fixture's deployment omission remained unfixed; that result does
not claim an unmodified `examples.user_module` pass.

2026-11-03：Windows 隔離 App 已選入兩個 C++ 範例 ModuleType；外部測試副本補齊
`modules/` 與 UI 資源後，更新過的所有 C++／Python 斷言通過，包含自己可改 Limited、
其他 Module 不可改 Limited 及跨 Module 修改 Public。第一階段當時尚未修正原 fixture 漏檔，
不能將本次結果當作未修改的 `examples.user_module` 已通過。

整合 develop `f16da98` 後保留其完整 Param／重啟標記 workflow，並合併本輪授權檔案與 UI／Bag 檢查。
上方 9.06／8.02 秒是合併前的歷史結果；整合後驗證另見
[發布前驗證](../../src/vinc/osal/MACOS_EXIT_VALIDATION.md#publication-validation)。
