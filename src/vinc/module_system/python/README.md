# Python Module

Python Module 由中央 Shell 啟停，每個實例使用一個系統 Python 程序。
中央不連結或嵌入 Python，不附帶直譯器，也不自動安裝 Python／pip 套件。
使用者程式透過 pybind11 原生擴充使用同一套 C++ Runtime；沒有另寫 Socket／共享記憶體協定。

## 建置與部署

```sh
cmake -S src -B build/my-build -DVINC_BUILD_PYTHON=ON -DPython_EXECUTABLE=/absolute/path/to/python3
cmake --build build/my-build
```

沿用所選建置目錄的編譯器與組態。`VINC_BUILD_PYTHON` 預設 OFF，讓只使用 C++ 的建置不要求 Python 開發檔。
開啟後需要 CPython 3.9 以上及其 Development.Module 檔案；這是相依最低要求，不代表所有版本已通過驗證。
第一版限定 64-bit、含 GIL 的 CPython；不支援 free-threaded Python、subinterpreter 或 PyPy。
Python 2 不支援。

`vinc_python_native` 與它連入的 C++ Runtime 使用同一工具鏈／標準庫／組態／CRT。
一般 C++ Plugin 的原有 ABI 契約不變。pybind11 使用 CPython C API 管理 Python 物件；
不讓 CPython 釋放由 C++ new 建立的 Module 物件。
Windows 的 MSVC 擴充不能用 MinGW Python 建置。Python minor 版本、平台 ABI、位元數與 GIL 組態需符合建置資訊；啟動不相容時明確拒絕。

中央建置會部署以下檔案：

```text
Vinc.exe / Vinc
python/vinc/
    __init__.py
    bootstrap.py
    _build.py
    _native.<平台與Python版本後綴>.pyd / .so
licenses/pybind11/LICENSE
data/module/<ModuleType>/<ModuleType>.py
```

`data/module` 是使用者程式的位置，與 Param／Log 的 `vinc_data` 不同。
只修改既有 Python Module 程式不需編譯中央。`vinc.messages` 由 `vinc_messages` 的
編譯期型別清單建立；新增 `.msg` 後須重新生成並建置所有使用它的程式及 Python 擴充。
不提供 runtime `.msg` 解析器或動態訊息型別。
`Vinc --python /absolute/path/to/python` 可指定 Python 或 venv，CLI 亦可接受相對路徑並在中央啟動時轉成絕對路徑。
省略時，在啟動 Module 當下依 PATH 順序尋找 Windows `python.exe` 或 POSIX `python3`。
不使用建置時選定的 Python 位置作為部署端的隱含預設，不在第一個 Python 不相容時偷偷換另一個環境。

## 掃描與 ModuleType

- 中央在保留 Shell 命令、登記編譯期 catalog 後，掃描執行檔旁的 `data/module` 一次。
- 只處理一層實體子目錄，依目錄名排序；不遞迴掃描、不跟隨目錄 symlink。
- 每個子目錄必須有同名 `.py`，名稱遵守 Module 命名規則。掃描不 import 或執行 Python。
- 無此根目錄是正常空集合。讀取失敗、名稱不合法、缺入口、重複種類只回報問題；中央繼續運作。
- 不覆蓋 C++ 或既有 ModuleType。新增目錄需重新啟動中央；尚未提供熱掃描。
- 預設 `on_name()` 消耗 `--name <實例名稱>`，不自動加尾碼；使用者可覆寫命名 hook。
- 種類與實例不能同名；多實例各自有唯一名稱與中央頒發的 MID。

## 使用介面

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

FrameworkLoop 由框架執行 `context.pumpLoop()`，不能覆寫 main。Manual 必須實作 main；
可自行呼叫 pumpOnce／pumpLoop 或建立自己的 pump thread。
生命週期 hooks 的成功回傳是 None；失敗使用 Python exception。Shell hook 明確回 int32，非零代表命令失敗，不是 Module 故障。
`module_autoComplete(args)` 回傳字串列表。`on_name(args)` 回傳實例名稱並可消耗 mutable args；
其他 hooks 的 args 是本地列表。Python 的 args 不包含 Shell 前面的種類／實例名稱。

Context 提供 `living()`、`pumpOnce()`、`pumpLoop()`、Message Bus、Param 與 Log；
BaseModule 提供 `get_mid()`、`get_module_name()`。
Context 無公開建構子，來自框架建立的 BaseModule；成功 import 不會建立控制連線或取得身分。
初始化／命名時 Context 尚未接上服務，pump 會回報 ResourcesUnavailable。
建構與命名 hook 不得啟動背景工作；待 on_start 時身分及服務才已完成接線。
停止時 living 為 false；pump 可返回 stopped。Runtime 銷毀時撤銷代理，之後呼叫會拋 VincError，不能使用已釋放的原生 pointer。

## 錯誤、執行緒與退出

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

## 接線與責任

`ModuleSource` 是 Module System 私有建立介面；native_source.cpp 保留 Plugin create／destroy 配對，
PythonSource 提供 Python 物件適配。共同 Runtime 先準備認證、fatal bridge、Bus／Param／Log 接線及 watchdog，
再載入使用者程式。Module 銷毀後停止服務，最後 clear source，仍早於 fatal bridge 失效。
Python 擴充包含唯一 Runtime owner；不另啟動一個 ModuleHost 幫 Python 轉送。
Python 初次接入時未改變 Native Plugin 入口；目前雙時間 metadata 使用 Plugin entry 7／bootstrap v12。
Python Source 以無參數 create 建立 wrapper，不從中央取得預設實例名稱；on_name 契約不變。

## 驗證與後續

2026-10-12：Windows x64／MSVC／官方 CPython 3.13.12：

- Debug／Release 完整建置成功；全套各 72 項 CTest 均已通過，包含修正測試設定後的失敗項目重跑。
  會自行建置的 Common／SDK 測試需在 MSVC 開發環境執行；新增重啟測試使用既有政策允許的延遲值。
- 最後的 Module／Context／Plugin／Python 重點回歸兩種組態各 6/6 通過。
- 實測掃描不執行程式、重複名稱拒絕、FrameworkLoop／Manual／自行建立 pump thread、Shell 狀態碼及補全。
- 實測 SDK 錯誤捕捉、未處理例外的原因鏈、重啟上限與新 MID、Context 撤銷、卡住的 hook／import 回收、
  中央斷線及停止 heartbeat 後的子程序退出。
- 真正的中央 Shell 驗證中文部署路徑、不同工作目錄、PATH／指定 Python、ABI 不相容拒絕、
  原始輸出與 traceback；外部 C++ SDK 消費端仍可不啟用 Python 建置。

本機 PATH 的第一個 Python 是 MinGW 3.14，與這次 MSVC／CPython 3.13 擴充不相容。
本機試用可明確指定 `--python C:/Users/acers/AppData/Local/Programs/Python/Python313/python.exe`；
框架不修改系統 PATH，也不自動改用另一個 Python。Linux／macOS 未實機驗證。

## 訊息與 Message Bus

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

## Param 與 Log

```python
from vinc.param import Type, Permission  # 或使用 vinc.param.Type
```

`param`、`log`、`messages`、`message_bus` 同時支援 package import 和 `vinc.*` 屬性存取。

```python
self.context.param.register_param("gain", vinc.param.Type.Double, 1.25)
self.context.param.register_param("other.gain", vinc.param.Type.Double)
self.context.param.on_change(self.parameters_changed)
value = self.context.param.update("gain", vinc.param.Type.Double)
```

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

## 第二輪驗證（2026-10-12）

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

## 待處理事項

- 2026-10-12 範例 review 確認：`tests/services_module.py` 的 received_message 使用
  `publisher_instance() != self.get_mid()` 作斷言。Publisher ID 與 Module MID 是不同識別空間，
  數值可能恰好相同，不能以不相等當正確性條件。目前測試配置通過不代表此假設有效；
  建議後續改為驗證 Publisher 身分與已知發布者對應，不比較跨種類數值。
  本輪修正示範的 `publisher_id` 顯示名稱，新範例測試不採此假設；既有測試留待修正。
