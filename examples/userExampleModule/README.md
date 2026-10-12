# userExampleModule：C++／Python 使用體驗對照

這是一組可執行的 Module SDK 範例。從繼承 `BaseModule` 開始，透過自己的 `context`
使用 Bus／Param／Log；不需要自己建立 Socket、heartbeat 或共享記憶體。
本範例涵蓋一般 Module 作者的介面；OSAL 的獨立工具、中央 Manager、內部 detail 接線不在此範圍。
API 契約以各元件 README 為準，不以範例取代契約。

## 建議閱讀順序

1. [C++ 主 Module](cpp/user_example_module.cpp)／[Python 主 Module](python/userExamplePython/userExamplePython.py)：
   看 `on_start`、`publish`、訂閱 callback、Param callback、`module_interface`、`on_stop`。
2. [C++ 訊息操作](cpp/message_examples.cpp)／Python 的 `demonstrate_storage`：看容器與錯誤檢查的使用成本。
3. [C++ Manual](cpp/manual_module.cpp)／[Python Manual](python/userManualPython/userManualPython.py)：
   比較自行呼叫 `pumpOnce` 和交給 `pumpLoop`。
4. 共用訊息 [UserExampleFrame.msg](../../vinc_messages/messages/UserExampleFrame.msg)、
   [UserExampleItem.msg](../../vinc_messages/messages/UserExampleItem.msg)：純量、字串、bool 陣列、訊息陣列與巢狀陣列。

每個例子明確檢查錯誤，沒有用巨集或「失敗一律 throw」的 helper 隱藏 C++ Result 操作。
C++ 啟動失敗包成 ModuleSystem 原因鏈；互動命令失敗印出診斷並回狀態 1，保持 Module 運作。
Python 互動命令捕捉預期的 SDK／輸入例外；未處理的 callback 例外仍依框架政策使該 Module 失敗。

## 建置與部署

現有建置開啟 `VINC_BUILD_EXAMPLES` 與 `VINC_BUILD_PYTHON` 後：

```sh
cmake --build build/windows-msvc-debug --target user_examples_python
```

這個明確執行的 target 建置中央及 C++ Plugin，再複製兩個 Python Module 到執行檔旁：

```text
data/module/userExamplePython/userExamplePython.py
data/module/userManualPython/userManualPython.py
```

**再次執行此部署 target 會覆寫這兩個 Python 範例檔案。** 一般 `shell` 建置不覆寫它們。
若要保留自己的修改，另取 ModuleType／目錄／檔名；中央啟動時才掃描一次。
Python 主範例全部在單一入口檔，不需要另外配置 import 路徑。

新建置目錄的設定範例（在適用的編譯器環境執行）：

```sh
cmake -S src -B build/my-build -DVINC_BUILD_EXAMPLES=ON -DVINC_BUILD_PYTHON=ON -DPython_EXECUTABLE=/absolute/path/to/python3
cmake --build build/my-build --target user_examples_python
```

Python ABI 必須符合建置使用的 CPython；Windows MSVC 使用官方 Windows CPython，不能混用 MinGW Python。
省略執行時 `--python` 會用系統 PATH，不會自動選建置時的直譯器。
本機已使用的直譯器可這樣啟動：

```powershell
.\build\windows-msvc-debug\Vinc.exe --python C:/Users/acers/AppData/Local/Programs/Python/Python313/python.exe
```

只有 C++ 時不必啟用 Python：建置 `shell` 後，`userExampleModule`／`userManualModule` 已在中央 catalog。
這兩個是 Plugin 動態庫，由中央啟動獨立 ModuleHost，不能直接當 executable 執行。
新增 `.msg` 仍須重新生成並建置；本範例沒有引入動態 schema 載入。

## 實際操作

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

預期看到 `READY`、`STORAGE_OK`、`CALLBACK` 與 `MESSAGE`。`publisher=` 是中央核准的 Module 名稱，
`publisher_id=` 是 Publisher ID，**不是 MID**；MID 用 `get_mid()` 取得。
timestamp 為同機 monotonic nanoseconds，不是日期時間。`get_last` 不推進 callback 游標，可重複取得同筆。
尚未有發布資料時，`last` 回錯誤；Module 仍活著。

每個 Publisher 配置 8 slots、每筆 64 KiB；這些是本範例的容量選擇，未改框架 config。
callback 已取得本機訊息所有權，`retained_`／`retained` 只保留最後一筆，避免示範無限累積。
收送不保證每次 callback 都看得到被覆蓋的資料；`status` 顯示遺失與碰撞統計。

### 參數與權限

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

- `gain` 是 Double／Open，其他 Module 也能改；`locked_gain` 是 Double／FrameworkOnly，Module 自己也不能改。
- `label` 是 String／FrameworkOnly。`set` 是本範例只接受 double 的 Module 命令，中央 `param set` 依登記型別解析。
- 成功 set 表示中央提交；快取在後續 pump 更新。`params`／callback 稍後才看見新值，不要求兩個實例同時更新。
- `peer.gain` 示範完整名稱監聽；沒有 peer 時 map 中沒有這筆，`watches` 顯示 missing。
- `param save` 會保存此中央的參數；退出不自動保存 live 調整。範例不偷偷呼叫 save。
- C++ callback 借用 map，不能保留其 reference；Python callback 收到獨立唯讀快照。

### 可恢復失敗與資源生命週期

```text
native capacity
peer capacity
native reopen
peer reopen
native push 303
peer last
```

`capacity` 先建立正常的本地大訊息，再示範 push 因 Publisher 容量不足被拒絕。
`storage` 使用 checked `at()` 示範越界回報；不執行會終止 Module 的 C++ `[]` 越界。
`reopen` 明確關閉並重新建立 Pub／Sub，Publisher ID 與統計會更新。
`loan` 正常路徑直接填共享 slot 並 commit；成功後所有借用 view 失效。
若填寫／commit 失敗，本範例明確關閉該 Publisher，需 `reopen` 後再借用。
只丟棄未提交 loan 不會自動取消它；範例沒有假造 cancel API。

### Log 與原始輸出

```text
native logs
peer logs
log level all debug
native logs
peer logs
log status
```

Log 等級不是故障政策；寫 Error 不會讓 Module 終止。Debug 預設可能被來源過濾。
C++ 使用 fmt、Python 使用 f-string；記錄失敗時回 raw 輸出，不遞迴寫 log。
`printf`／`print` 由既有輸出擷取保存，沒有結構化 Log level。
來源等級套用有延遲，執行 `log level` 後不保證緊接的第一筆已用新等級。

### Manual 與停止

```text
userManualModule
manualCpp status
manualCpp finish
userManualPython --name manualPy
manualPy status
manualPy finish
```

`finish` 讓自行管理的迴圈正常返回。等它退出後可再啟動：

```text
userManualModule loop
userManualPython --name manualPy loop
manualCpp status
manualPy status
exit
```

`loop` 模式在自己的 main 呼叫 `pumpLoop`，由中央停止要求喚醒退出；沒有另開 pump thread。
`finish` 只適用逐批 pump 的分支；整個中央可用 `exit`／Ctrl+C 關閉，單一實例可用既有 `debug stop <MID>`。
不要在 FrameworkLoop 的 callback／Shell hook 中遞迴呼叫 pump；同一 Context 只允許一個 active pump。

## API 對照索引

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
| 路由 metadata 五個 getter | describe | describe |
| Param 註冊／預設／權限、外部監聽 | register_parameters | register_parameters；明確指定 param.Type |
| Param callback／Entry.get／update／watches | register_parameters、execute(params) | parameters_changed、execute(params) |
| Param set／set_value、文字轉值 | execute(set) | param.set(Type, value)；沒有另設 set_value |
| Log enabled／write／format／error／warn／info／debug | execute(logs) | demonstrate_logs；f-string 先格式化 |
| Result、Error.is、wrap_error、error_to_string | hooks、start_failure、storage、capacity | VincError.error、cause chain、標準 Python 例外 |

所有固定寬度 Param 型別都用同一註冊方法；本例選 double 與 string 展示權限與型別差異。
支援型別清單見 [Param](../../src/vinc/param/README.md)。
獨立 `MessageArray<任意本地型別>`、Snapshot 的底層讀取 view 與 BaseMessage 文字診斷等延伸用法見
[Storage](../../src/vinc/components/message_storage/README.md)／[Ribosome](../../src/vinc/ribosome/README.md)，
不代表 Python 開放任意類別作為可傳輸訊息。

## 驗證

```sh
ctest --test-dir build/windows-msvc-debug -R "^examples.user_module$" --output-on-failure
```

測試在暫存部署目錄啟動真實中央，保存檔案不碰日常部署的 `vinc_data`。
檢查兩語言各自的容器操作、雙向 push／loan callback、發布者 metadata、參數更新／權限拒絕、
容量拒絕、重新開啟 handles、Manual 正常完成和中央退出。
測試內的短等待只讓背景流程有機會運作，不是框架的延遲承諾。
不同組態的中央測試應依序執行，避免同名 Vinc 的單實例鎖互相衝突。

2026-10-12：Windows x64／MSVC／官方 CPython 3.13.12，Debug 與 Release 的中央／範例／Python
擴充建置成功。兩種組態各通過 6/6 項重點 CTest（本範例、四個 Python 測試、Ribosome schema）。
Release 的 `user_examples_python` 部署 target 亦執行成功；Debug 同步複製相同範例檔案。
本輪未重跑全專案測試；Linux／macOS 未實機驗證。
