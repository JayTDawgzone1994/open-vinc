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

In a configured build with EXAMPLES/PYTHON enabled:

現有建置開啟 `VINC_BUILD_EXAMPLES` 與 `VINC_BUILD_PYTHON` 後：

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

**再次執行此部署 target 會覆寫這兩個 Python 範例檔案。** 一般 `shell` 建置不覆寫它們。
若要保留自己的修改，另取 ModuleType／目錄／檔名；中央啟動時才掃描一次。
Python 主範例全部在單一入口檔，不需要另外配置 import 路徑。

New-build configuration in an appropriate compiler environment:

新建置目錄的設定範例（在適用的編譯器環境執行）：

```sh
cmake -S src -B build/my-build -DVINC_BUILD_EXAMPLES=ON -DVINC_BUILD_PYTHON=ON -DPython_EXECUTABLE=/absolute/path/to/python3
cmake --build build/my-build --target user_examples_python
```

Python ABI must match build CPython; Windows MSVC needs official Windows CPython, not MinGW Python. Without runtime --python, PATH wins, not the build interpreter. The following uses a placeholder: replace it with your compatible CPython executable and build directory.

Python ABI 必須符合建置使用的 CPython；Windows MSVC 使用官方 Windows CPython，不能混用 MinGW Python。
省略執行時 `--python` 會用系統 PATH，不會自動選建置時的直譯器。
下例使用通用佔位路徑，請換成自己的建置目錄與相容 CPython 執行檔：

```powershell
.\build\windows-msvc-debug\Vinc.exe --python C:/path/to/python.exe
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

- gain is Double/Open, writable by every Module; locked_gain is FrameworkOnly even for its owner.
- label is String/FrameworkOnly. Example set accepts doubles only; central param set parses the registered type.
- Successful set acknowledges commit; subsequent pumps refresh caches/callbacks independently across instances.
- peer.gain demonstrates full-name watching; absent peer means absent map entry/missing watch.
- Explicit param save persists central values; exit does not save and examples do not save secretly.
- C++ callbacks borrow maps only during invocation; Python receives independent read-only snapshots.

- `gain` 是 Double／Open，其他 Module 也能改；`locked_gain` 是 Double／FrameworkOnly，Module 自己也不能改。
- `label` 是 String／FrameworkOnly。`set` 是本範例只接受 double 的 Module 命令，中央 `param set` 依登記型別解析。
- 成功 set 表示中央提交；快取在後續 pump 更新。`params`／callback 稍後才看見新值，不要求兩個實例同時更新。
- `peer.gain` 示範完整名稱監聽；沒有 peer 時 map 中沒有這筆，`watches` 顯示 missing。
- `param save` 會保存此中央的參數；退出不自動保存 live 調整。範例不偷偷呼叫 save。
- C++ callback 借用 map，不能保留其 reference；Python callback 收到獨立唯讀快照。

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
