# Param

Param 是中央維護的具型別參數服務。Module 使用繼承的 `context.param`，由中央核准的
名稱與 MID 決定身分，不自行建立 Context、共享區或持鎖物件。參數允許延遲與版本合併，
不取代 Message Bus，也不保證多個 Module 同時套用更新。

**本輪沒有將既有 `config.hpp` 設定改接 Param。** Socket Shell 的 IP、port、timeout
仍依原有 CLI／config 流程；`vinc` 命名空間只提供框架後續接線能力。

## Module API

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

| API | 行為 |
| --- | --- |
| `register_param<T>(name, default, permission)` | 登記自己的簡稱、預設值與權限；成功後自動監聽 |
| `register_param<T>(name)` | 登記監聽；完整名稱可指向其他 Module，簡稱指自己 |
| `set<T>(name, value)`／`set_value(name, Value)` | 向中央請求修改，型別必須完全相符；不隱含 save |
| `update(name, destination)` | 只讀本地快取並複製至變數；不存在／型別不同回 Result，保留原變數 |
| `on_change(callback)` | 設定一個整體快取 callback；空 function 停用 callback |
| `watches()` | 本地監聽清單，包含預期型別、目前是否存在及型別是否相容 |

所有可能正常失敗的操作回 `vinc::Result`；`watches()` 是本地查詢，不包 Result。
`Params` 不可自行建構、複製或替換；標頭入口為 `<vinc/param/params.hpp>`、
`<vinc/param/types.hpp>` 與 `<vinc/param/error.hpp>`，亦由 SDK 整合標頭帶入。

### 命名、型別與權限

- 完整名稱為 `module.param`，恰一個 `.`；簡稱依實際核准的 Module 名稱補全。
  Module／參數段限 ASCII 字母、數字、`_`、`-`，不接受 `/`、空白或中文名稱。
  完整名稱（含 Module、點）上限 256 bytes；`vinc` 保留給中央。
- 支援 bool、int8/uint8、int16/uint16、int32/uint32、int64/uint64、float、double、string。
  C++ 使用對應 `std::int*_t`／`std::uint*_t`，平台原生型別若為其別名，遵循相同型別。
  無陣列或自訂物件。字串限 256 bytes 且為有效 UTF-8；字串內容及檔案路徑可含中文。
- 預設 `Permission::FrameworkOnly`，只有中央命令能修改，所屬 Module 也不例外。
  `Permission::Open` 允許**所有 Module**修改；不保證應用在彼此反覆修改下仍正確。
- 中央由認證 Session 導出 MID／名稱。Module 不傳入自選 owner；其他 Module 的完整
  名稱只能監聽或依 Open 權限寫入，不能冒名註冊其 schema/default。
- 同一 MID 對同一參數重複登記回 `AlreadyRegistered`；新 MID 的同名 Module 可重新
  登記並更新 default／type／permission。停止不刪參數，也不撤銷既有 Open 設定。
- 監聽尚不存在的參數成功，快取 map 不建立假值。重新取得快照後自然出現。
  監聽型別與後來的實際型別不同時，map 保留實際 Entry，`get<T>`／`update` 回
  `TypeMismatch`，`watches().compatible` 為 false；不把值強制轉型。
- 登記先保留本地監聽，再送出中央請求；遠端失敗時保留這份監聽，避免逾時但已提交的
  參數失去追蹤。不存在的值仍不出現在快取 map；重試監聽會重新送出通知訂閱。

### pump、快取與 callback

中央提交、Module 快取更新與檔案保存是不同事件。`set`／登記成功表示中央提交完成，
不表示本地快取立即變動；使用者在下次成功 pump 後才能從 update／callback 取得新值。
只有 pump 更新快取。`10 → 20 → 30` 可合併成一次交付 30。

每輪 pump 先擷取 Bus 批次界線，再更新 Param 快取與派送 Param callback，接著處理
Shell hooks／Bus callback；沒有新增使用者 timer API。FrameworkLoop 使用既有主執行緒，
Manual 由使用者決定 pump 執行緒。同一 Context 仍只允許一個 active pump。

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

## TOML 與保存

中央 Vinc 啟動後，在啟動 Module／部署腳本前讀取：

```text
<Vinc executable directory>/vinc_data/<VINC_NAME>/params.toml
```

路徑相對執行檔位置，不依啟動時 current directory。不同 VINC_NAME 使用不同目錄。
正常可寫環境首次執行即建立空檔，不由 build 產生。框架自訂組裝使用私有
`ManagerAccess::configure_params` 接入根目錄；它必須在 Module 啟動與參數登記前呼叫。

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

完整名稱是帶引號的一個 key，不是 TOML 巢狀的 Module table。
`uint64` 用十進位字串表示以保留完整範圍；其他整數用 TOML integer，float／double
用浮點數，字串用 TOML string。型別欄位必須明寫，不從數字外觀猜測。
浮點保留 round-trip 精度；NaN／inf 可表示，不承諾保存 NaN payload bits。

中央區分三種值：註冊 default、檔案 saved value、執行期 live value。

1. 新參數：default 與 saved value 都用登記值，補進檔案。
2. 同型別 default 改變：原 saved value 等於舊 default 才跟隨新 default，否則保留自訂值；
   本次中央仍存在的 live value 亦獨立依同樣規則處理，不把未保存 live value 寫入檔案。
3. 型別改變：只將該參數 default／saved／live 重設為新 default，發出診斷。
4. 舊參數本輪沒登記／Module 未啟動：繼續保留，包含下次 save。
5. `param save` 才把當下所有 live value 更新成 saved value。退出不自動 save。

因此「新增另一參數」或「更新 default」的自動檔案維護，不會順便保存其他未 save 的調整。
使用者可直接改檔案的 value，重新啟動後載入；執行中不監看檔案，也不自動 reload。
需要刪除歷史參數時，停止中央後編輯檔案；仍被 Module 登記的參數下次會補回。

### 檔案失敗

- TOML 語法、格式版本或結構／型別值不合法：整份不採用，刪除後建立乾淨檔案，提示原因。
  Module 註冊時才有 defaults。一般的「Module default/type 更新」依上述個別條目規則。
- 權限不足、一般讀寫／替換失敗：提示並停用本次 file I/O，不以 I/O 錯誤推論檔案損壞，
  不刪原檔。live 服務仍運作；`param status` 可查原因，重新啟動後才重新嘗試載入。
- 檔案超過讀取容量：保留原檔並停用 I/O，回報容量錯誤。
- 同目錄 `.tmp` 完整寫入／flush／close 後 rename 取代，不先刪目的檔。
  失敗保留上一次完整檔；下次寫入可覆寫殘留 temp。**不宣稱斷電持久交易保證**。
- 自動保存失敗不否定已成功的共享提交。中央保留有界的最新診斷槽，本地與 Socket Shell
  各依游標提示；多筆快速發生的診斷可合併，`param status` 留最後檔案錯誤／型別更新提示。

## Shell

本地、Socket Shell 及 Tcl 共用相同 router：

```tcl
param list
param get camera.gain
param set camera.gain 3
param save
param status
```

`param` 為保留命令，Module 不能使用該名稱。set 依登記型別解析，失敗不修改值；中央可改
FrameworkOnly。字串含空白時使用所處 Shell／Tcl 的引號規則；get/list 將控制字元跳脫顯示。
大型 list 仍受 Socket Shell 既有回覆容量限制，可改查個別 get；不悄悄截斷成成功回覆。
Socket Shell 尚未有登入／加密；這些權限是框架 API 邊界，不是對同帳號惡意 native code 的沙箱。

## 接線、同步與協定

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
  筆數，以及每筆 name、type/value/default、permission、entry version、steady-clock timestamp。
  整數使用 Control codec 明確編碼；不直接傳 native struct。
- 0 是初始空映像 revision；有效發布版本不回繞，UINT64_MAX 保留為未有快取的 sentinel。
  timestamp 是同主機 steady-clock 奈秒，不是可跨重啟排序的日曆時間。
- Control Domain=4、Param wire version=1；Attach=0x101、Register=0x102、Set=0x103、
  Changed=0x104、Watch=0x105。請求／回覆 ID 及 operation 配對，拒絕重複／未知 ID、
  畸形版本、截斷、尾端資料、非法型別／權限。原因鏈沿用明確欄位的 Result envelope。
- 中央在排入工作佇列前限制請求大小；上限依固定欄位與名稱／字串上限推導，預設 532 bytes。
  不讓單一畸形請求占用控制通道允許的最大 payload 容量。
- Changed 僅包含 revision 提示；丟失提示由共享版本補查恢復。未知錯誤代碼保留數值。
- 新增 Context 欄位後 **Plugin entry version=4、bootstrap v9**；中央、Host、Plugin、SDK
  須一起重建，仍不承諾跨工具鏈／CRT／Debug-Release ABI。
- 上層只在 OS 確認 Module 退出後回收其鎖；heartbeat timeout 不足以回收。中央 crash
  使整世代失效，新中央依 [INSTANCE](../module_system/INSTANCE.md) 建新區，不重設舊映射。

### 錯誤與成本

錯誤分類為 `ErrorDomain::Param`，使用 common Result、固定原因鏈與靜態 catalog。
最外層 operation 對應呼叫端使用的 API；同元件 helper 有不同操作語意時保留於原因鏈。
容量不足、型別不符、權限不足、缺少值均為 Result，不引入 framework exception。
STL／配置器例外維持既有邊界；中央 worker 收斂配置／標準例外，使用者 callback 另依 Module 故障政策。

送出修改後的等待逾時／取消回 `TimeoutOutcomeUnknown`／`CancelledOutcomeUnknown`，
不保證遠端未執行，不自動重送；查詢等待可回一般 Timeout／Cancelled。
worker 保存的工作只包含框架擁有的值／Session，不保存呼叫端的 stack reference。

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

共享區固定兩槽，不會為每個參數建立 OS 資源；占用中央 `module_lock_limit` 中一把 mutex。
每個開始讀取的 Host 有 1 MiB 複製 buffer 與本地 map；中央持有 live／saved 狀態，提交時
另配置候選。更新會重編碼全映像，泵送也會檢查全域 revision；第一版偏重簡單、穩定與可檢查性，
未宣稱高頻更新或無配置。只對使用中的監聽集合保留 Context 快取。

## 驗證與待處理事項

- 已增加 `param.values`、`param.protocol`、`param.process`：型別／容量、TOML 往返與失敗保留、
  版本／截斷／原因鏈／重放、A/B 提交、停止取消、真實 Host/Plugin、權限、缺少參數、
  合併 callback、Module 停止／中央重啟、type/default 更新、Tcl 與真實 TCP Shell。
- Windows x64／MSVC 19.51：Debug 全套 CTest **64/64 通過**（224.37 秒）；
  最後補強請求容量與外層 operation 後，重新建置並重跑 Param **3/3 通過**。
  Release 最終完整建置及全套 CTest **64/64 通過**（189.44 秒）。
  Linux／macOS 尚待實機建置及行為驗證。
- 已確認限制：作業系統／檔案系統 I/O 卡住時，C++ 同步磁碟工作無法安全強制取消；
  呼叫者等待有界且不占 heartbeat，但 join worker 的關閉仍可能等候原生 I/O 返回。
  未將 worker 另放隔離程序，也沒有以無證據的硬期限宣稱處理此 OS 限制。
- 已確認限制：檔案重寫不保留手工註解／排版；無執行期 reload、參數刪除 API、保存歷史版本
  或跨 Module 同步套用交易。後續擴充須另行設計與授權。
- 已確認限制：FrameworkLoop 的 pump 若回可恢復錯誤，沿用 `main()` Result 故障政策；
  Manual 可自行處理 Result 再繼續。callback 例外已鎖定故障，不能以忽略 Result 規避。
