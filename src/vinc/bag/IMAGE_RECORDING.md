# Generic image recording contract / 通用映像錄製開工契約

Date / 日期：2026-11-17

**Status: stages 1–3 implemented on 2026-11-18; validation is recorded per stage below.
Stage 4 Windows/WSL acceptance is complete; native macOS remains pending.**
The owner approved structural validation without concrete field-schema validation,
and recording valid payloads even when the Recorder has no corresponding C++ type.
This document does not authorize implementation; each stage follows the project
[authorization rules](../../../AGENTS.md).

**狀態：第一至三階段已實作；第四階段 Windows／WSL 驗收完成，Mac 待原生驗證。**
使用者已同意下方兩項行為取捨。已實作的 Storage、Bus、Context 與 Recorder 契約由各自 README 維護；
後續乾淨 SDK／搬移與成本驗收仍依分階段授權執行。現況見 [Bag README](README.md)。

## 1. Problem and evidence / 問題與證據

修正前 `module/recorder_module.cpp` 引入 `vinc_messages/message_types.hpp`，透過
`visit_types()` 為具體型別編譯 `subscribe<T>()`、callback、`Subscriber<T>` 與
Snapshot Encoder。執行時只錄一個 Topic，也不會減少這份建置清單。
相同型別的多個 Topic 不等於多份模板實例；Topic 數與具體型別數須分開統計。

2026-11-17 MMDD Manager 回覆的既有實驗：Open-VINC
`04fe4ee69e1d8c83fe39dcb27afde879f053b365`，Windows x64、MSVC 工具目錄
14.51.36231、Release `/O2 /Ob2 /DNDEBUG /EHsc /MD /utf-8 /bigobj`，
單一工作直接編譯 Recorder TU；生成清單有 672 個 visitor 型別入口（包含 record，
不等於 672 個實際 Topic）。該 `cl.exe` 峰值 Working Set 為 2,316,918,784 bytes，
峰值 Commit 為 2,317,971,456 bytes，程序耗時 89.9005 秒。

這是 **Manager 提供的編譯程序量測**，本輪未重跑或匯入原始證據。
不是 Recorder 執行期 RAM，也不是完整平行 build 的峰值。
尚無同條件 typed／通用 Recorder A/B，不能將全部用量歸因於模板，亦不承諾節省比例。
第一階段須保存可重建的基線與實際量測方法，不把上述外部結果當成本輪驗收。

## 2. Approved behavior / 已確認行為

1. **Recorder 驗證通用映像結構與 schema hash，不核對具體 C++ 欄位定義。**
   必須檢查格式版本、長度、offset、descriptor／payload 邊界、子節點所有權與
   可達性，並確認單一訊息根節點及其 hash 與本次訂閱相符。
   不以 hash 相符宣稱欄位語意正確或來源可信。
2. **未知 C++ 型別也錄製合法正文。** 查不到型別名稱時保留 Topic／hash，顯示
   `unknown type`；錄製能力與本機解碼能力分開。這不代表新增 Player、未知型別播放
   或動態訊息載入。

結構合法且 hash 相符、但欄位不符合對應 schema 的映像可能被保存。
日後認識該 schema 的 typed 解碼端仍須拒絕不相符內容。
一般 `subscribe<T>()` 的完整 schema 驗證不得因此放寬。

## 3. Ownership and boundaries / 所有權與元件邊界

```text
Publisher RingBuffer
  -> Bus 有界本機複製，隨即釋放共享 Reader 保護
  -> Message Storage 接管副本、驗證通用映像
  -> Context pump callback 交付映像與 metadata
  -> Recorder 有界 queue
  -> 既有 worker 整理 canonical Snapshot，寫入 SQLite
```

| Owner / 元件 | Contract / 責任 |
| --- | --- |
| Message Storage | 擁有格式、驗證、接管 buffer 與 canonical 整理；重用既有實作，不另造序列化格式。 |
| Message Bus | 非模板映像訂閱共用註冊、拓樸、選擇來源、讀取保護、游標、批次邊界、統計與退訂；不複製一套 RingBuffer 演算法。 |
| Module System / Context | 指定 `detail` 介面把通用 callback 接到既有 pump／通知／停止流程；Host 仍單一擁有程序級控制服務。 |
| Recorder | 接收所有權、計帳、入列、持久化與報告；不操作 RingBuffer 內部布局。 |
| Ribosome / SDK | 移除 Recorder 對全部訊息 class 與 typed visitor 的相依；保留所需的輕量 hash／名稱查詢資料。 |

Recorder 仍是受監督的 System Module。此 API 是框架指定的內部整合能力，
不放進一般使用者的 Context 公開介面或 umbrella header。
不新增 Topic 讀取權限或 System Module 授權協定；`detail` 標頭不是安全沙箱。
正常中央身分、端點持有人及資源檢查仍適用，不能自行建另一個 Context 或 Socket。

交付物必須獨立持有本機不可變映像，以及 framework 填入的 Topic、schema hash、
Publisher 名稱／ID、Topic count、原始 UTC／單調發布時間。
Publisher ID 不得誤當 MID。Recorder 另保留接收單調時間。
metadata 不混入 payload，時間戳不重採樣代替原始值。

callback 可 move 交付物進 queue；下一次接收、來源退場或退訂不能覆寫其內容。
交付物不能持有共享 Reader lease；驗證、canonical 整理、callback 與磁碟 I/O
都在共享保護解除後執行。Bus 複製後以移交所有權避免再做一次無必要的整筆複製。
canonical 整理仍可能需要配置與複製，這不是端到端 zero-copy 承諾。

`BaseMessage::serialize()` 已存在，但前置的 typed 訂閱仍需建立具體物件；
只更換 Encoder 呼叫不能達成此契約。`Snapshot::copy_from()` 已有通用驗證，
但它會複製輸入，也不等於完成 canonical 整理。擴充指定的內部接管／整理能力，
不可直接拿未整理的共享映像冒充目前 Bag 的 canonical BLOB。

## 4. Scheduling, limits and failures / 排程、容量與錯誤

- Recorder 維持 Manual `pumpOnce()` 加 `waitForWork()` 及既有寫入 worker，
  不新增背景執行緒。通用訂閱沿用 NewOnly、pump 開始時的有界批次及既有通知。
- typed／通用接線共用 Context 的同 Topic 序列、跨 Topic 排程與停止收尾規則；
  不讓同一份 queue 狀態因新增接線而意外並行存取。
- 保留現有多 Publisher 選擇順序、退場補查、無資料繼續其他工作、`close()` ACK
  與端點回收契約，不在缺資料來源上阻塞 pump 等待新資料。
- 單筆接收上限在 payload 配置前檢查。驗證與 canonical 整理沿用有界布局及
  配置錯誤處理；外來損壞資料與資源不足回 `Result`，不是 contract termination。
- callback 不等待磁碟。queue 維持 byte／筆數上限與 DropNewest；worker 取出的批次
  在釋放前仍計帳。canonical 整理在 queue mutex 外進行。
- 保留 `bus_lost`、`queue_dropped`、oversized、received、written 的可辨識統計，
  不把錄不到、未知名稱或失敗批次算成成功寫入。
- queue 上限不是 RSS 上限；接收暫存、驗證索引、當前 Snapshot、SQLite cache 與
  OS cache 另計。記錄新路徑的計帳口徑，不能透過少算暫存宣稱 RAM 已受完全限制。
- 維持主資料庫容量、交易批次、排空與 incomplete 契約，不新增保證不漏資料或匹配 Hz。
- 不順帶改故障政策：現行 pump 接收失敗使 Recorder Module 回報失敗；
  queue／worker 中的錄製失敗依既有錄製停止流程處理，中央與其他 Module 保持隔離。
  跨層錯誤保留 operation、cause chain 與原生診斷。

## 5. Metadata and compatibility / Metadata 與相容性

保留可查詢的型別名稱，但不為查名字引入全部 class header 或 `visit_types()`。
名稱表只存 hash／名稱，不提供解碼、runtime schema registry 或型別工廠。
無名稱映射不妨礙錄製，亦不可由 Topic 名稱猜型別。

修正前 Bag `supported` 與 `info` 把型別認識程度和錄製能力連在一起。
第三階段必須明確分開這些含義，同步錯誤輸出、統計、測試與 README；
不得在未標明格式差異下把舊欄位重新解釋。
訊息映像格式優先保留現行版本；若持久化 metadata 語意／布局變更，調整 Bag
格式版本與明確拒絕規則。舊 Bag 支援政策遵循既有契約，不自行加入轉檔功能。

若 Context／Host／Plugin 接線改變 ABI，更新相應版本並拒絕不相容產物，
source 與 installed SDK 的連接元件一起重建。不得只因改動是 `detail` 就略過 ABI。
不預先要求 Bus wire 改版；依實際封包變更判定，仍保留訂閱 hash 一致性。

## 6. Stages and acceptance / 開工步驟與驗收

### Stage 1: Storage and baseline / 映像能力與基線

- 保存修改前的環境、訊息集合、編譯指令及 Recorder TU／整體建置成本。
- 重用通用 validator，補擁有副本的接管、根節點／hash 驗證及 canonical 整理。
- 測試純量、字串、空訊息、巢狀 record／array、預留容量；與 typed Snapshot
  比對 canonical bytes，並由 typed 解碼端核對欄位內容。
- 覆蓋截斷、壞 offset、溢位、非法節點關係、錯 root／hash、版本不符與配置失敗。
- 加入結構合法但具體欄位不符的受控案例：通用路徑可保存，typed 路徑仍拒絕。
- 檢查 move／失敗後所有權、保留容量與空白區域的 canonical 處理，避免保存未使用內容。

### Stage 2: Bus and Context / 通用訂閱與 pump

- 將既有 protected copy 與具體型別建構分離，typed／映像路徑共用收發核心。
- 接上有所有權的訂閱 handle、通知與 Context callback；沿用 `Result` 與 close ACK。
- 受控測試多 Publisher、NewOnly／批次邊界、順序、覆寫／oversized、通知、缺失 mapping
  補查、退訂／重新訂閱、停止及清理；驗證獨立副本在下一次 pump 後仍有效。
- 驗證 canonical／queue／callback 不持有共享 lease；一般 typed C++ 與 Python 接收
  不放寬 schema 檢查，Context 排程和收尾回歸通過。

### Stage 3: Recorder and packaging / Recorder 與建置交付

- 移除 Recorder 的 typed visitor、`Subscriber<T>` 接線與逐型別 Encoder。
- queue 接收映像與 metadata，worker 使用通用 canonical 整理；保留容量／錯誤政策。
- 產生輕量名稱查詢資料，處理 unknown type、錄製／解碼狀態與 Bag 版本決策。
- 更新 source／installed SDK 的 target 相依、交付檔、範例及文件，避免透過間接
  include 仍把全部訊息 class 編回 Recorder。
- 以 Recorder 未包含的訊息 class 錄下正文，再由擁有 schema 的測試程式完整解碼。
- 驗證選定 Topic／全錄、晚出現來源、重啟、queue 滿載、磁碟／容量失敗及正常排空。

### Stage 4: Integration and cost / 整合與成本驗收

- Windows／WSL Debug、Release 的 Storage、Bus、Context 與 Bag 相關回歸；保留
  原始失敗、重跑及原因，紀錄實際 OS／CPU／工具鏈／標準庫／相依版本。
- 乾淨 installed SDK、外部自訂訊息、多 App 及搬移部署，隱藏原 build／SDK 路徑，
  確認錄製、metadata、內容／順序、finish 與程序回收。
- 同一份訊息集合、工具鏈、組態、編譯選項與 jobs 做 typed／通用 A/B。
  分開量 Recorder TU 編譯峰值與耗時、完整 build、產物大小及增量重建。
- runtime 另量 CPU、RAM、吞吐、漏收與排空成本；相同 Topic／型別／訊息尺寸／速率、
  RingBuffer／queue 配置，不以改小負載或改大 budget 代替改善。
- 目標是 Recorder 不再因每個具體型別實例化訂閱模板；名稱資料表仍可隨型別數成長。
  不承諾整個 App 建置成本與型別數無關，也不預填節省比例或最低效能提升。
- 原生 macOS source 與乾淨 installed SDK／搬移驗收加入根 TODO 及交付清單。
  既有 typed Recorder 的 Mac 通過紀錄不能代替本輪通用路徑驗收。

| Stage / 階段 | Status / 狀態 | Evidence / 證據 |
| --- | --- | --- |
| 1 | 已完成 Storage 能力與本機基線 | 見下方 2026-11-18 驗收；Mac 未驗 |
| 2 | Bus／Context 通用訂閱已實作 | 見下方第二階段驗收；Mac 未驗 |
| 3 | Recorder／名稱表／Bag v3 已完成 | 見下方第三階段驗收；Mac 未驗 |
| 4 | Windows／WSL SDK／搬移／成本 A/B 完成 | 見下方第四階段紀錄；Mac 未驗 |

<a id="stage-1-validation"></a>

## Stage 1 validation / 第一階段驗收（2026-11-18）

實作入口與錯誤／所有權契約由 [Storage README](../components/message_storage/README.md#owned-record-images)
維護：`FrameworkAccess::adopt_image()` 與 `canonical_image()`。
接管共用原 validator；typed 與通用 canonical 共用 `make_image()`／renderer。
沒有改 Bus、Context、Recorder、Ribosome、SQLite 格式或 Plugin ABI。
Recorder 的 `visit_types()` 仍存在，**不能宣稱本輪已解決 Recorder 編譯 RAM 成本**。

來源基準為 `bdde615a5182741e0f6e30b071deef2b8ed1c068` 加本階段未提交變更。
機器為 AMD Ryzen 7 5700X3D；測試只針對 Storage，不是全專案驗收。

| Environment / 環境 | 最終內容驗收 |
| --- | --- |
| Windows 11 x64 build 26200，MSVC 19.51.36257／工具目錄 14.51.36231，MSVC STL，SDK 10.0.26100.0，CMake 4.3.2，Ninja | Debug `message.errors/storage/generated/access` **4/4，2.37 s**；Release 同組 **4/4，1.34 s**。 |
| WSL Ubuntu 24.04.5 x64，kernel 6.18.58.1，GCC/libstdc++ 13.3.0，CMake 3.28.3，Ninja | Debug 同組 **4/4，0.67 s**；新建 Release 樹同組 **4/4，0.17 s**。 |
| macOS 原生 | 未驗；已加入根 TODO，不沿用舊 typed Recorder 的 Mac 驗收作本輪證據。 |

`message.access` 新案例逐 byte 對照 typed／通用 canonical，涵蓋非零有號／無號純量、
bool／double、空 record、字串、巢狀 record／array、預留容量、來源釋放後的副本、
同一 Snapshot 並行 canonical、輸入／輸出上限、錯根／hash、未知 hash、截斷／版本、
offset／capacity 溢位、子節點重用與非零閒置區拒絕。另驗結構合法的 uint32→float
改寫可通用保存，但 typed 解碼維持 TypeMismatch。跨程序 typed 回歸保持通過。

Windows Release 配置失敗掃描新增接管 **9 處**、canonical **30 處**；WSL Debug／Release
分別是 **8／26 處**。逐次回 ResourceExhausted，canonical 原輸入不變並可重試。
Windows Debug 沿用既有規則，
略過會觸發 MSVC noexcept iterator-proxy 配置終止的掃描；未關閉 iterator checks。
這不是涵蓋所有配置／所有負載的無洩漏證明。

原始失敗保留：首次 Windows sandbox 下兩個跨程序測試因原生資源權限失敗，
完整權限原設定重跑通過；新測試最初誤認非零閒置區可被清洗，實際 validator 正確拒絕，
修正測試預期，沒有放寬正式格式。新 WSL Release 配置先因 PATH 無 Node 失敗，
使用本機既有 Node v24.19.0 路徑後配置／建置通過，未安裝工具。
GCC 仍輸出既有 `Access::image` aggregate missing-initializer 警告；本輪未修該無關介面。

### Typed Recorder baseline / typed Recorder 成本基線

本機清單是 **8 個 visitor 型別入口**，不是 MMDD 的 672 個型別。
量測使用尚未改接線的 `recorder_module.cpp`，Storage 第一階段函式已存在；
不是整個修改前 HEAD 的 clean checkout。後續比較必須維持相同訊息集合／工具鏈／組態。

| 觀測 | 結果與口徑 |
| --- | --- |
| Release Recorder TU，直接執行 Ninja compdb 的 `cl.exe` 指令，輸出另存 | 5.3397 s；peak Working Set **309,649,408 bytes**；peak Commit **306,122,752 bytes**；object **2,715,330 bytes**。 |
| 新目錄完整預設 Release App 建置，jobs=4、BUILD_TESTING=OFF | 配置＋建置 **174.0975 s**，167 個 Ninja 工作，成功。包含 UI、Tcl 與官方 typed Recorder；不是執行期測試。 |

編譯 RAM 使用 GetProcessMemoryInfo 的程序生命週期峰值，程序結束後仍保留 handle
讀取 counters；不是全系統 RAM、不是多程序峰值相加，也不是 Recorder runtime RAM。
整體建置期間另有功能測試建置，時間只作可重建的初始觀測，**不作受控效能提升對照**。
第四階段需在固定負載下重做雙版本 A/B；目前沒有節省比例或大型 MMDD 工作負載的結論。

本機證據（生成物，不上 Git）：`build/recorder-image-stage1/` 的 `compile-recipe.json`、
`compile-result.json`、`compiler.log`、`measure_compile.py`、`app-baseline.log` 與
`app-baseline-result.json`；基線保留原命令、工具鏈及輸入 hashes。
功能測試在 `build/image-stage1-final-*.log`，最初失敗保留 `image-stage1-tests-release.log`、
`image-stage1-tests-release-elevated.log` 與 `image-stage1-wsl-configure-release.log`。

重跑功能驗收（先依本機工具鏈重建同一來源）：

```sh
cmake --build <build-dir> --target message_access_tests message_storage_tests generated_message_tests message_storage_error_tests --parallel 4
ctest --test-dir <build-dir> -C <Debug-or-Release> -R "^message\." --output-on-failure
```

Windows 原生測試需可建立子程序及共享記憶體；WSL 使用 Linux 工具鏈／Node 路徑。
不要只靠 CTest 計數，保留 executable 非空與實際案例完成輸出。

<a id="stage-2-validation"></a>

## Stage 2 validation / 第二階段驗收（2026-11-18）

**Bus and Context integration is implemented; Recorder remains typed.**
The Bus owns `detail::ImageSubscriber` / `ReceivedImage`; Module System owns
`detail::ContextImageAccess`. Their API and lifecycle contracts are maintained in
the [Bus README](../message_bus/README.md#framework-image-subscriptions)
and [Module System README](../module_system/README.md#recorder-接線2026-10-12).
No new transport, thread or runtime schema registry was added. Plugin entry is
**23** because the shared Context subscription dispatch layout changed; Bus v6,
bootstrap and message image v2 are unchanged. All connected binaries must rebuild.

**第二階段已完成；Recorder 尚未替換。** typed／通用讀取共用同一份來源選擇、保護複製、
游標與退場處理，差異只在複製後的解碼。Context 的兩種 pump 都透過同一訂閱紀錄派送，
沒有另建 queue／worker 或繞過 Host。Plugin entry 23 拒絕舊入口版本；
不能把 detail 當成不需要重建的 ABI 邊界。

本輪仍為 `bdde615a5182741e0f6e30b071deef2b8ed1c068` 上的第一、二階段未提交變更。
Windows 11 x64 build 26200／Ryzen 7 5700X3D，MSVC 19.51.36257、工具目錄
14.51.36231／MSVC STL、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja 1.13.2、
CPython 3.13.12。WSL Ubuntu 24.04.5 x64、kernel 6.18.58.1、GCC／libstdc++ 13.3.0、
CMake 3.28.3、CPython 3.12.3；使用既有 Node 24.19.0，未安裝新工具。

| 實測範圍 | 結果 |
| --- | --- |
| Windows Debug：images、context、parallel、parallel_images、parallel_stop、parallel_image_stop | **6/6**，85.42 s；追加原生通知案例後 `message_bus.images` **1/1**，0.38 s |
| WSL Debug：相同六組 | **6/6**，14.86 s；追加原生通知案例後 **1/1**，0.17 s |
| Windows Release：Storage 四組、Bus 錯誤／ring／module／cleanup／control_plane／通知／images，Context／wait／shutdown／parallel | **33/33**，95.23 s；追加原生通知後 images **1/1**，0.18 s |
| WSL Release：上述範圍加兩個 POSIX EMFILE 案例 | 首輪 **33 passed、2 Not Run**，53.27 s，原因是選定的兩個 EMFILE 案例尚未建置；補建後 EMFILE 兩組＋最終 images **3/3**，0.48 s。合計 35 個選定案例均有通過證據，並非首輪 35/35 |
| Windows Python Debug：python、python_messages、python_services、python_parallel | **4/4**，42.03 s；binding 及 Host／Plugin 依本輪來源重建 |
| WSL Python Debug：相同四組，獨立啟用 Python 的來源 App | **4/4**，15.14 s |

測試內容：

- `message_bus.images` 以受控 wire bytes 驗證真正 Bus reader：hash 錯誤與畸形
  descriptor 拒絕並保留 Storage cause；失敗重讀不前進游標；未知 hash 可交付。
  同寬 uint64→double 改寫可通過結構驗證，但 typed Frame 仍回 TypeMismatch。
- 比對 typed／image 的 Publisher ID／名稱、Topic count、UTC 與單調時間。
  新映像、壞映像、來源退場後原本保存的 Snapshot 仍可解碼／canonical。
  直接檢查 Reader token 已歸零；持有副本期間反覆覆寫 Ring 不會等待該副本。
- 通用訂閱向中央啟用路由，真實 Host dispatcher 套用路由與回覆 ACK，
  排空安裝提示後發布訊息，直接用 OSAL NotificationReceiver 等到新通知並讀到正文。
  此測試沒有 Context 的定時 fallback，因此不是用「最後有收到」推測通知正常。
- lazy-open 缺失來源的中央補查、真退場與仍登記但映像缺失、等待補查期間
  繼續派送另一 Publisher，均以通用路徑重跑；保留原 typed 案例。
- 真實 Plugin 的 image-batch／new-only／loss／oversized／cancel／error
  驗證有限批次、多 Publisher 合併、latest 不吃游標、獨立副本、close ACK 後
  重新訂閱、自行 close 與例外分類。Handle 的 move／銷毀仍遵守既有同步責任。
- `module.parallel_images` 混合兩個通用訂閱與一個 typed 訂閱；有期限 rendezvous
  要求不同 Topic 真正並行，同 Topic 不重疊，Param／Shell／RPC 控制路徑仍可推進，
  callback 內遞迴 pump／wait 與同步 RPC 防護仍成立。
- `module.parallel_image_stop` 重跑 Manual return、callback 內 close／替換、
  callback 拋例外與故意卡住；確認排空早於 on_stop、真實程序退出與收尾分類。
- Python 沒有新增映像 API；原 typed 訊息、Context 服務及多執行緒生命週期仍通過。

原始失敗未刪除：首輪新測試有一處把 ReceivedImage 當成 typed `data()` 使用，
已修 fixture；Windows `module_parallel_helper` 既有編譯 PDB 兩次 C1033 無法開啟，
保留舊 PDB 並只重建該中間檔後通過，未改 runtime／編譯選項。
WSL 隔離 Python 測試入口漏設來源根路徑，補正測試專案後配置通過。
WSL Release 的兩個 Not Run 如表記錄，沒有排除測試或放寬期限。
上述不是已證實的正式收發／生命週期故障；PDB 原因未進一步判定。

本機原始證據（不入 Git）：`build/image-stage2-*.log`，包含 `tests-debug`、
`tests-release`、`wsl-tests-debug`、`wsl-tests-release`、`wsl-tests-release-followup`、
`notification-tests-*`／`notification-wsl-tests-debug`、`python-tests-debug`、
`python-wsl-tests`；各名稱均以前綴 `image-stage2-` 開始。
首輪建置錯誤、retry 與最後結果分檔保存。WSL Release 兩個 Bus 程式已確認為非空
原生產物，並檢查實際案例完成輸出。

核心新增案例重跑方式（先重建對應 targets）：

```sh
cmake --build <build-dir> --target module_context_tests module_parallel_tests module_parallel_stop_tests message_bus_cleanup_tests --parallel 4
ctest --test-dir <build-dir> -C <Debug-or-Release> -R "^(module\.(context|parallel|parallel_images|parallel_stop|parallel_image_stop)|message_bus\.images)$" --output-on-failure
```

這是指定範圍的來源驗收，不是全專案 CTest 或效能結論。
新路徑的乾淨 installed SDK／搬移與成本 A/B 留第四階段；Python Release 本階段未重跑。
macOS 未執行，根 TODO 已列同版重建、上述測試與後續 Bag／SDK 交付項目。
Recorder 仍使用 typed visitor，尚不能宣稱已降低 Recorder 編譯 RAM。

## 7. Scope limits / 範圍限制

不實作 Player、動態載入訊息 class、schema 直譯器、通用欄位編輯器、公開 raw
訂閱 API、UI 改版或 Python binding 重設計。不增加依賴、執行緒、權限平台或
另一套收發協定來解決此次模板成本。若實作發現無法在此邊界內維持正確性，
先提出證據與必要取捨，不以通過編譯為理由悄悄放寬契約。

<a id="stage-3-validation"></a>

## Stage 3 validation / 第三階段驗收（2026-11-18）

Recorder 已移除 `visit_types()`、`Subscriber<T>`、逐型別 Encoder，改用第二階段的
`ContextImageAccess`，直接把不可變 `ImageDelivery` 移進有界 queue。worker 於 mutex 外
呼叫 Storage canonical helper；in-flight 批次仍持有 byte／筆數 credit 至提交並釋放。
Byte credit 按 payload vector capacity，單筆上限按 image length；其他記憶體成本仍另計。
Bag 對 Bus 的 target 相依是 `message_bus_client`，未引入中央控制服務所有權。

Ribosome 額外產生只含標準函式庫的 `message_names.hpp`；保留 typed visitor 給其他用途。
來源及 SDK 共用生成 recipe，SDK generator 也已建置／直接執行生成器測試。
Windows／WSL Release 的 SDK export 配置通過；**尚不是乾淨 installed SDK 或搬移驗收**。

Bag 改為 **v3**，`topics.type_known` 只記錄當時是否查到名稱，未知型別合法正文仍錄製。
舊 v2 明確回 InvalidFormat，不轉換／刪檔；訊息 image v2、Bus v6、Plugin entry 23 不再改版。
Bag API、generator、Recorder adapter 必須一起重建。`info` 名稱知識不等於已解碼內容。

本輪仍為 `bdde615` 加同一 topic branch 的未提交三階段內容，環境同上：
Windows 11 x64 build 26200／Ryzen 7 5700X3D／MSVC 19.51.36257、工具目錄 14.51.36231、
MSVC STL、Windows SDK 10.0.26100.0、CMake 4.3.2／Ninja 1.13.2；
WSL Ubuntu 24.04.5／kernel 6.18.58.1／GCC、libstdc++ 13.3.0／CMake 3.28.3。
SQLite 沿用 repository 的 vendored library，沒有更新第三方套件。

| Platform / 組態 | 六項組合 | 最後測試同步／命名補強後 |
| --- | --- | --- |
| Windows Debug | 6/6，26.35 s | `bag.storage/module` 2/2，23.63 s |
| Windows Release | 6/6，22.96 s | 同組 2/2，20.44 s |
| WSL Debug | 6/6，19.66 s | 同組 2/2，18.54 s |
| WSL Release | 6/6，16.95 s | 同組 2/2，16.78 s |
| macOS 原生 | 未驗 | 已列根 TODO 的同版測試清單 |

六項為 `bag.storage/module/loop` 及 `ribosome.schema/generated/names`，不是全專案測試。
最後同步補強是等 Publisher 看見真實 Subscriber，再發布唯一必須錄到的未知訊息；
metadata 落盤不代表訂閱 ACK 已完成。測試用 worker gate 也改用準確名稱。
既有通過證據保留；最終改動的兩個測試已在四組態重跑。

覆蓋：

- 官方 Recorder 的名稱表沒有 `Frame`，仍實際錄下其正文；外部測試用 Frame schema
  解碼 sequence、全部 payload bytes、巢狀 record／string，並確認只錄一次。
  known／unknown 顯示、同名不同 schema 診斷與 `type_known` metadata 分開核對。
- 全錄／指定 Topic、晚出現 Publisher、多來源、來源重啟保留新 ID；512 筆 burst
  順序／正文／統計、oversized 後合法訊息繼續、原始雙時鐘與接收時間。
- queue 筆數、byte 容量分別受控滿載；已 dequeue 的 worker 批次仍占額度；
  payload 長度過大算 oversized，配置 capacity 超過 queue 預算算 queue_dropped。
  拒絕後能接續合法輸入，finish 排空，完成後拒絕新輸入。
- SQLite 寫入競爭失敗、主 DB 容量、整批 rollback、故障後同 Module 新錄製、
  crash journal、正常停止與 incomplete 狀態。1–128 MiB storage round-trip 保留。
- 名稱表單獨編譯的 target 沒有 Storage include 路徑；名稱／hash 對應與保留檔名
  碰撞拒絕通過。兩平台 Ninja 的 Recorder 真實 include 清單中，生成訊息標頭
  **只有 `message_names.hpp`**，沒有 class headers／all_messages／message_types。

原始輸出留在 ignored `build/`：

- `image-stage3-final-{build,tests}-{debug,release}.log`（Windows）；
  `image-stage3-final-wsl-{build,tests}-{debug,release}.log`（WSL）。
- 最後兩項重跑：`image-stage3-{build,tests}-{debug,release}-ack.log`、
  `image-stage3-wsl-{build,tests}-{debug,release}-ack.log`。
- SDK generator：`image-stage3-sdk-generator-{windows,wsl}.log`；
  export 配置：Windows final Release build log、`image-stage3-sdk-wsl-configure-release.log`。
- include 清單：`image-stage3-recorder-deps.log`、`image-stage3-recorder-wsl-deps.log`。

首輪與過程紀錄未覆寫：Windows 首輪六項 6/6、62.42 s，WSL 6/6、19.12 s；
追加 queue 精確計帳後單測分別 1/1、9.77 s 與 1/1、8.57 s。
WSL 最初使用原 Module-only Debug tree，因 `VINC_BUILD_APPS=OFF` 沒有 Bag targets 而
建置入口失敗（`image-stage3-wsl-build-debug.log`）；啟用該隔離測試樹 App 後才開始驗收。
這不是 Runtime 測試失敗，也沒有用空測試當通過。Windows Debug 首輪耗時較長，
沒有當作受控效能比較或推論根因。

重跑方式：以對應工具鏈建置 `bag_storage_tests bag_module_tests bag_loop_tests
ribosome_generated_tests ribosome_names_tests`，再執行：

```sh
ctest --test-dir <build> -C <Debug-or-Release> \
  -R '^(bag\.(storage|module|loop)|ribosome\.(schema|generated|names))$' --output-on-failure
```

SDK recipe 檢查另以 `VINC_INSTALL_SDK=ON` 配置並建置 `vinc_ribosome_sdk`，使用其產物
執行 `ribosome/tests/schema_tests.cmake`（指定 GENERATOR、WORK、TEMPLATE）。
本輪沒有做整體 Python、完整 CTest、實體斷電或新效能 A/B；第二階段證據維持原範圍。
乾淨 SDK／外部自訂訊息／搬移、多 App 與編譯／runtime 成本對照仍為第四階段。

<a id="stage-4-validation"></a>

## Stage 4 validation / 第四階段驗收（2026-11-18）

Windows and WSL functional acceptance passed on the stage 1–3 working tree.
The following records distinguish compilation cost from Recorder process usage;
they do not establish a saturated throughput guarantee or validate MMDD's workload.
Native macOS remains pending, with the handoff below and in the root TODO.

### Environment and integration / 環境與整合

- 同一台 Ryzen 7 5700X3D：Windows 11 build 26200，MSVC 19.51.36257／工具目錄
  14.51.36231／對應 MSVC STL、Windows SDK 10.0.26100.0、CMake 4.3.2、Ninja
  1.13.2、測試腳本 CPython 3.13.12。
- WSL2 Ubuntu 24.04.5、kernel 6.18.58.1、GCC／libstdc++ 13.3.0、glibc 2.39、
  CMake 3.28.3、CPython 3.12.3。SQLite 3.54.0、fmt 12.2.0、spdlog 1.17.0；
  採用專案固定相依，沒有為本輪更新套件。
- Plugin entry 23、Bus v6、Bag v3、Storage image v2。Windows／WSL 各組 source
  回歸涵蓋 Storage、Bus errors/control-plane/images、Context/parallel/stop、Bag 與
  Ribosome；這是相關元件驗收，不是完整專案 CTest 或本輪 Python binding 全套驗收。

| Platform | Configuration | Source regression | Clean SDK + relocated two-App recording |
| --- | --- | --- | --- |
| Windows | Debug | 39/39，145.00 s | 通過 |
| Windows | Release | 39/39，110.56 s | 通過 |
| WSL | Debug | 41/41，79.68 s | 通過 |
| WSL | Release | 41/41，71.91 s | 通過 |

每個組態由全新 producer tree 建置 `VINC_INSTALL_SDK=ON`、`VINC_BUILD_APPS=OFF`、
`BUILD_TESTING=OFF` 的 SDK，安裝後先搬移 SDK，再由 `find_package(vinc)` 建置兩個
外部 App 與自訂 `RecorderSample`。安裝／搬移 App 後，原 consumer、SDK 與 producer
目錄均改名隱藏，由無關 cwd 同時啟動兩個不同 instance 的 App；驗完還原 producer。
`HIDE_PRODUCER` 只接受 producer 之外的新驗收目錄，不刪除任何舊 SDK 或 App。

每個 App 各 64 筆，核對 Bag v3、received/written=64、零 drop/loss/oversized、
completed=1、Topic／型別名稱、publisher name/ID、sequence、兩種原始時間與接收時間。
用搬出的獨立 typed decoder 逐筆檢查序號、字串及每個 array byte；兩個 App 都必須確認
`All module processes exited`。UI 共用預設 HTTP port 的衝突不作 Recorder 失敗；本案例
不宣稱兩個 UI 可以共用同一監聽端點。Python 只執行驗收工具，App 未啟用 Python Module。

### Cost method / 成本方法

重跑入口見 [image_cost README](tests/image_cost/README.md)。固定舊 Bag／adapter 來源
`bdde615a5182741e0f6e30b071deef2b8ed1c068`，對照本輪 generic Bag／adapter；兩者都連入
**同一份本輪 Release SDK**。這是隔離 Recorder 改動，不是整個舊版框架對新版框架。
來源 SHA-256、編譯命令與結果保留在各自 work 的 manifest／recipe／results JSON。

共同生成 137 個 class：128 個額外未發布型別、一個錄製型別及 SDK 的八個內建型別。
兩組相同 Release flags、單一 build job；Windows 全部 fixture targets 均使用 `/bigobj`。
平台依序量測，沒有同時跑其他本輪編譯／負載。單檔交錯量三次，完整外部 App build
與 comment-only 訊息修改增量各一次。完整 build 不包含已預編譯 SDK 的成本，增量
包含其他接線的重建，不能當成純 Recorder 編譯。

| Platform | Recorder | TU seconds (median of 3) | Compiler peak RSS MiB (median of 3) | Clean external App seconds (1 run) | Comment-only incremental seconds (1 run) |
| --- | --- | ---: | ---: | ---: | ---: |
| Windows | typed | 20.788 | 622.77 | 99.391 | 72.502 |
| Windows | generic | 3.587 | 268.88 | 84.925 | 54.072 |
| WSL | typed | 59.956 | 2,004.20 | 161.850 | 153.447 |
| WSL | generic | 3.322 | 325.66 | 103.646 | 98.037 |

Windows peak 指 `cl.exe` working set；Linux 指 GNU time 回報該 compiler invocation
的最大 RSS，均不是完整 build 峰值。各平台只比較自己同條件的兩組。
Recorder binary 未額外 strip：Windows 2,995,200 → 1,493,504 bytes；WSL
6,217,208 → 2,605,976 bytes。完整初始部署目錄（含資源與 ownership manifests，
不含測試錄製資料）：Windows 16,037,836 → 14,536,140 bytes；WSL
25,084,662 → 21,473,430 bytes。UI 仍有 typed 編譯成本，不外推全 App 峰值 RAM。
這些數字不取代第一階段基線，也不外推為 MMDD 672 visitor 入口的節省比例。

Runtime 每平台兩種尺寸各三組 A/B，每次一個 Topic、1,000 筆正文，array payload
1 KiB／64 KiB，1,024 slots、單筆 131,072 bytes、queue 64 MiB／4,096 筆，batch
16 MiB／256 筆。發布十組各 100 筆、每筆 sleep 2ms，避免把測試塞成超過 Shell 期限
的單一命令；這是受控 paced load，不是最大吞吐壓測。
紀錄 Recorder 程序 CPU、20ms 採樣 RSS、完成率、最後發布後排空及 finish。
RSS 含共享映射，不是獨占實體記憶體；1 秒 idle 取樣為零也不表示零成本。
每次要求零 Bus loss／queue drop、所有順序／publisher metadata 正確，並逐筆比較
typed/generic canonical payload bytes，最後正常回收所有子程序。

以下均為三次中位數，箭頭為 typed → generic；CPU 是錄完 1,000 筆期間累積的
程序 CPU 秒數，不是每筆延遲。Drain 是最後一組發布命令完成至確認 SQLite 計數達標，
包含既有 commit/flush 與測試輪詢，不能當成 callback latency。

| Platform / payload | CPU seconds | Sampled RSS MiB | Observed records/s | Drain milliseconds |
| --- | ---: | ---: | ---: | ---: |
| Windows / 1 KiB | 0.281 → 0.641 | 17.54 → 17.23 | 62.77 → 62.73 | 1.6 → 1.8 |
| Windows / 64 KiB | 0.547 → 0.578 | 87.15 → 86.81 | 62.38 → 62.43 | 77.5 → 1.6 |
| WSL / 1 KiB | 0.100 → 0.090 | 15.27 → 13.86 | 439.82 → 422.40 | 59.7 → 154.9 |
| WSL / 64 KiB | 0.230 → 0.220 | 88.80 → 86.95 | 408.36 → 419.89 | 34.1 → 154.6 |

共 24 次錄製、24,000 筆正文，所有 A/B payload digest 相同，順序／publisher metadata
符合預期、零 loss/drop，finish 與正常回收均通過。各組 finish 中位數約 29–32ms。
Windows 發布端 sleep 在此環境約 16ms，因此約 63 records/s 不是 Recorder 吞吐上限；
也不能拿這個速率直接與 WSL 約 400 多筆/s 比較。
Windows 1 KiB CPU 中位數較高，但 typed 0.266–0.969 s、generic 0.625–0.875 s
範圍有重疊；WSL 沒有相同方向。三次樣本不能證明普遍退化或改善，亦未定位原因。
已在 Bag README 留後續真實負載／CPU profiling 建議；不能因編譯下降宣稱 runtime 更省。
完整樣本與來源 hash 收在 [compact results](tests/image_cost/results-2026-11-18.json)。

### Initial failures and scope / 初次失敗與範圍

- Windows SDK 首次 consumer compiler check 缺少 `rc.exe`／`mt.exe` PATH；補齊
  相同 Windows SDK 工具環境後以新目錄重跑，正式工具鏈政策未更動。
- 成本 fixture 首次 typed Recorder 觸發 C1128；補 `/bigobj` 後，又發現原 UI 的
  `topic_monitor.cpp` 也達 section 上限。兩組 fixture 統一旗標後重跑；正式 UI
  未修改，其獨立待辦記在 [UI README](../ui/README.md#large-typed-catalogs)。
- Runtime 探針第一次把 Tcl 大括號引用用在 Raw Shell，record 沒有成功；第二次
  將正常的 `OK` 回覆誤認為未 ready。兩次都在發送資料前停止並正常回收；修正測試
  引用／回覆判定後重新使用獨立輸出路徑。沒有放寬資料斷言或改 Runtime 來通過測試。
- WSL 成本 fixture 首次自建 `cost_bag` 未開 PIC，連入 `.so` 時被 linker 拒絕。
  兩組 fixture 統一 `POSITION_INDEPENDENT_CODE=ON`，由新的成本目錄重跑；
  正式 SDK 原有靜態庫／Plugin 接線沒有這個缺漏，乾淨 SDK 驗收已先通過。

### Evidence and macOS handoff / 證據與 Mac 交付

Local generated artifacts (not committed):

- Windows `build/image-stage4-{win,accept-win,sdk-windows}-*`；成功 SDK 驗收目錄為
  `image-stage4-accept-win-{debug,release}-r2`；成本 `image-stage4-cost-win-r4`，
  runtime 成功紀錄 `runtime-driver-r3.log`，前兩次失敗與早期 cost 目錄均保留。
- WSL producer `/home/vinc-test/image-stage4-sdk-{debug,release}`；驗收目錄
  `image-stage4-accept-{debug,release}`；成本 `image-stage4-cost-r3`（r2 保留 PIC 失敗）。
  source／SDK 建置與驗收摘要同步於 Windows `build/image-stage4-*wsl*` logs。
  成本 JSON／log／GNU time 複本在 `build/image-stage4-cost-wsl-evidence`；大型 SQLite
  與 object 仍留 WSL，沒有上傳任何外部服務。

Mac 必須以同版 native Debug／Release 重建，重跑上述元件與新 SDK fixture；
每個組態各用全新 producer／prefix，隱藏 producer、consumer、SDK 後同時錄製兩個
App，驗正文、順序、metadata、finish 及真實回收。確認名稱表的 installed generator
輸出一致、Recorder 依賴清單不帶具體 class，並重跑 Python typed 回歸。
成本比較沿用相同 137 class／queue／ring／尺寸／筆數，先補 Mac 原生 CPU／RSS
採樣（目前工具明確拒絕 Darwin，不能拿 Linux `/proc` 結果代用），記錄三次樣本、
工具鏈／標準庫／架構與初次失敗。不以既有 typed Recorder 或其他平台通過取代。
