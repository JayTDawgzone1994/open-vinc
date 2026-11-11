# Publisher defaults / Publisher 預設容量

## Parameters / 參數

Central registers these after persistence load and before Module startup, using
ordinary Param, `Limited` permission and `RestartHint::None`.
中央在載入參數檔後、啟動 Module 前註冊，使用一般 Param、Limited 權限與 None 重啟標記。

| Name / 名稱 | Type / 型別 | Code default / 程式預設 |
| --- | --- | --- |
| `vinc.pub_default_slots` | `uint32` / `std::uint32_t` | 2 |
| `vinc.pub_default_message_bytes` | `uint64` / `std::uint64_t` | 1,048,576 bytes |

Bytes cover the complete encoded message image, including metadata/alignment.
Ring protection metadata and the Topic counter additionally consume Bus budget.
`config.hpp` retains registration and standalone defaults.
bytes 包含完整訊息映像與其 metadata／對齊；Ring 保護資訊與 Topic 計數器另占 Bus
預算。config 常數保留作首次註冊及獨立 Bus 預設，不直接填入 PublisherOptions。

## API and timing / API 與時點

```cpp
auto publisher = context.advertise<Frame>("camera/frames");
auto partial = context.advertise<Frame>("camera/preview", {.slots = 8});
auto fixed = context.advertise<Frame>("camera/raw", {
    .slots = 8, .max_message_bytes = 4 * 1024 * 1024
});
// Check all Results before using the Publishers.
```

- Options fields are empty `std::optional`; Python uses `None`, accepts integers,
  and can reset to `None`. Explicit zero is invalid. Requested 1 slot still means
  2 effective slots, with both statistics preserved.
- Only omitted fields consult defaults, on the existing Bus worker. Framework
  copies both entries in one Param worker action, then validates the complete
  candidate pair. Fully explicit options bypass this source. Malformed fields
  fail before any default read/repair.
- `param set` acknowledges commit, not application. The next advertisement
  needing defaults reads a snapshot without waiting for Module pump. Existing
  rings never resize/relocate. No lookup is added to push/commit, heartbeat or
  pump; no periodic validator or thread is added. Standalone Bus uses fixed defaults.

- 選項欄位為未指定的 optional，Python 對應 None，可指定整數或改回 None。明確 0
  仍非法；要求 1 筆仍實際配置 2 筆，統計保留兩者。
- 省略欄位才由既有 Bus worker 讀預設，一次複製兩個 Param Entry 並驗證整組。
  完整指定不讀來源，畸形欄位先拒絕，不觸發參數讀取或修正。
- set 只承諾提交；下次需要預設的建立操作才讀快照，不等待 Module pump。既有 Ring
  不重設大小或搬移；發布／heartbeat／pump 不增查詢、定期審核或執行緒。獨立 Bus 採固定預設。

## Validation and correction / 驗證與修正

Keep one effective pair, initially the code defaults. Requested slots must be
1..65,536; image bytes must be at least 64 and safely alignable. One ring including
metadata and its Topic counter must fit an empty configured Bus budget. The
layout minimum does not promise every schema fits. A valid candidate replaces
the pair. An invalid candidate replaces neither field: diagnose it, then use
ordinary central set to restore differing fields to the last effective pair,
not always 2/1 MiB.

保留一組有效值，初始為程式預設。要求筆數為 1～65,536，映像至少 64 bytes 且對齊
不溢位；單一 Ring（含 metadata）及 Topic 計數器須能放入空 Bus 預算。布局下限不
保證所有 schema 都放得下。整組合法才套用；任一欄或組合非法則整組不套用，回報原值，
以一般中央 set 將不同欄位修回最後有效值，不是一律重設 2／1 MiB。

- Read/registration failures propagate Result, without silent fallback. Correction
  failure keeps the effective pair and reports failure, without claiming repair.
  Ordinary set may overwrite a concurrent edit; no CAS or extra queue is added.
- Current allocation pressure and OS failures are registration failures, not bad
  settings. Explicit overrides may also make a request exceed budget. None of
  these failures changes defaults. A tiny Bus unable to fit even the initial pair
  requires explicit options; do not invent smaller defaults or an unusable repair.
- Live edits/repairs do not save. `param save` keeps existing snapshot semantics,
  without a validation gate. Invalid saved values may load and be repaired again.

- 讀取／註冊失敗回 Result，不默默取預設。修正失敗仍保留有效值並回報，不宣稱已修好。
  一般 set 可能覆蓋並行新修改，沿用既有契約，不加 CAS 或佇列。
- 當下預算已被占用、OS 配置失敗或明確覆寫組合超限，只讓該次建立失敗，不改預設。
  特別縮小的 Bus 若連初始預設也放不下，須明確指定，不自動縮小或寫回無法使用的值。
- 執行期修改／修正不自動保存；save 不增加審核門檻，非法 saved value 可能在重啟後再次修正。

## Ownership and compatibility / 所有權與相容性

Bus accepts a designated source without a Param dependency. Module System owns
the consumer and Param; after startup only the Bus worker reads. Normal teardown
and partial-create rollback join Bus before destroying its consumer/Param.
Param never waits back on Bus; supervision does not perform the synchronous read.
Bus 不依賴 Param，由 Module System 接線及持有消費端／Param；啟動後只由 Bus worker
讀取。正常收尾及建立失敗的回滾均先等待 Bus 結束，才銷毀消費端／Param；Param 不反向
等待 Bus，監督執行緒不執行同步讀取。

Rebuild Host, Plugins, SDK and Python together: PublisherOptions ABI changed,
Plugin entry is **14**, Bus control is **v4**. Advertise uses u32 presence bits
(bit 0 slots, bit 1 bytes), followed by only present dimensions in that order.
Success prefixes Descriptor/routes with resolved requested slots u32. Reject old
operations, unknown flags, truncation and trailing bytes. Ring/message layouts,
Param wire v3 and TOML format 1 remain unchanged.
Host／Plugin／SDK／Python 須一起重建：Plugin entry **14**、Bus **v4**。Advertise
以 u32 bit 0／1 表示 slots／bytes 存在，依序只傳已指定欄位；成功回覆在 Descriptor／
routes 前帶解析後要求筆數 u32。拒絕舊操作、未知旗標、截斷及尾隨資料；Ring／訊息布局、
Param wire v3 與 TOML format 1 不變。

## Validation / 驗證

New: `module.publisher_defaults`, `message_bus.defaults_process`,
`sdk.installed_publisher_defaults` (a real custom-message App independent of UI
HTTP availability). Extended: Bus cleanup, Python services and installed C++/Python App fixtures.
新增上述三套測試，擴充 Bus cleanup、Python services、安裝式 C++／Python App。

### Recorded acceptance (2026-11-04) / 驗收紀錄

| Environment / 環境 | Result / 結果 |
| --- | --- |
| Windows 11 x64 build 26200; MSVC 19.51.36257.0, toolset/STL 14.51.36231; CMake 4.3.2; Ninja 1.13.2; CPython 3.13.12 for build/config tools; Release | Native build and public-header smoke passed. Targeted **16/16** passed; final native regression **70/70** passed, excluding Python and blocked `ui.shell`. / 原生建置、公開標頭及選定回歸通過。 |
| Windows fresh installed prefixes / 全新安裝目錄 | `sdk.installed_consumer` and `sdk.installed_publisher_defaults` **2/2** passed again after the final guard (124.03 s), with moved SDK/App and hidden SDK/consumer build at execution. / 最後檢查後重跑，SDK 搬移、消費端自訂訊息、容量修改／修正、原 Publisher 不變與退出通過。 |
| WSL2 Ubuntu 24.04 x86_64; kernel 6.18.54.1-microsoft-standard-WSL2, GCC/libstdc++ 13.3.0, glibc 2.39; CMake 3.28.3; Ninja 1.11.1; CPython 3.12.3; Release | Full default build including Python passed. Targeted **20/20**, final selected regression **72/72** passed, including C++/Python, UI, Bag, RPC, Param and source SDK consumer. / 含 Python 完整預設建置及所列回歸通過。 |
| WSL fresh installed prefixes / 全新安裝目錄 | Consumer/application **2/2** passed (180.59 s); dedicated Publisher App **1/1** passed again after the final guard (42.41 s); maintained `installed_python.cmake` passed separately (321.58 s), including custom message Bus/RPC/Param, schema changes, relocation, ABI rejection, repair and cleanup. / 三種既有安裝式流程及新增專用案例通過；最後檢查後重跑專用案例。 |
| Native macOS Debug/Release / 原生 Mac | Pending on the same branch, including native/Python and clean installed SDK. Earlier entry-13 evidence does not cover this entry-14 change. / 待驗，舊版證據不能替代。 |

Final review restored rejection of an explicitly oversized byte field before any
default-source read/repair. After rebuilding that change, Windows targeted
**16/16** and WSL targeted **20/20** passed again. The broader 70/72 selections
above preceded this final early-rejection guard; their unchanged-path evidence is
retained separately rather than described as a full rerun of the last revision.

最後 review 補回「明確指定 bytes 已超過總預算，先拒絕再讀預設」的檢查；重建後
Windows 定點 **16/16**、WSL 定點 **20/20** 再次通過。上述 70／72 套較廣回歸在此
最後檢查前執行，保留未受影響路徑的證據，不宣稱最後版本全部重新跑過。

The targeted and regression selections overlap; do not add their counts as unique
tests. Installed consumer includes four nested native tests. WSL's Python script
was invoked explicitly because the optional installed-Python CTest was not
registered in this producer; it is not part of the 2/2 CTest count. These are
Release selections, not complete-project CTest, native Mac, bare-metal Linux or
Ubuntu 22.04 deployment acceptance. Windows Debug and Windows Python runtime were
not accepted in this round.

定點與回歸有重疊，不相加成獨立測試數；installed consumer 內含四套原生測試。
WSL 的可選 installed-Python CTest 未登記，因此另行執行維護中的腳本，不混入 2/2。
本輪為 Release 選定範圍，不是全專案 CTest、Mac、裸機 Linux 或 Ubuntu 22.04 部署
驗收；Windows Debug 與 Windows Python 執行期本輪未驗收。

**Subsequent Python acceptance (2026-11-04):** after the separately authorized
[static-import policy correction](../module_system/python/README.md#msvc-1951-static-import-review),
the same Windows Release toolchain passed Python service tests and a clean
installed-Python consumer, including both Publisher defaults. The original blocked
run below remains historical evidence; Windows Debug and default-port UI remain
outside that follow-up acceptance.
**後續 Python 補驗：** 獨立授權修正靜態匯入政策後，同一 Windows Release 工具鏈
已通過 Python 服務與乾淨 installed Python consumer，包含兩項 Publisher 預設。
下方阻擋為原始紀錄；此補驗不涵蓋 Windows Debug 或預設端口 UI。

**Retained failures and corrections:**

- Initial fixtures missed one v4 success prefix, used the wrong Plugin type name,
  or shared independent request counters on one authenticated owner. Corrected
  the fixtures without weakening protocol/replay checks; the final tests pass.
- The new exhaustion case initially budgeted 1 KiB for a nested Frame that needs
  **1040 bytes** on MSVC. It now verifies that its sample fits and can publish
  before testing exhaustion, uses a 4 KiB slot, and still budgets exactly **one**
  ring. The second Module fails with the genuine Bus ResourceExhausted cause;
  Param stays unchanged and the first Publisher still works on both platforms.
- Running Windows console tests through the build worker inherited Ctrl+C-ignore
  process-group state. A dedicated hidden UTF-8 console with Ctrl+C enabled passed
  both original tests and the final regression; production Shell/OSAL were unchanged.
- Windows Python hit the existing [19.51 import guard](../module_system/python/README.md#msvc-1951-static-import-review).
  The policy was not relaxed and no compiler was installed. This is a build
  blocker, not evidence of a Param/CRT crash.
- Windows independently rejects loopback 8080 bind with **10013**. `ui.shell`
  failed; the final 70-test selection excludes it. The full installed UI HTTP App
  flow was not accepted on Windows. See [UI limits](../ui/README.md#local-windows-port-validation).
  The dedicated Publisher App does not assert UI availability; its pass does not
  erase the failed UI check; Windows Python is covered by the separate follow-up above.

**原始失敗與修正保留：** 初次測試漏同步 v4 回覆欄位、Plugin 類型名稱錯誤、同一認證
owner 使用兩組請求計數器，均修正 fixture 後通過，未放寬協定／重播檢查。耗盡案例
原給 1 KiB，但 Windows 訊息需 1040 bytes；現先驗證訊息可放入並能發布，再以 4 KiB
槽位、總預算恰一個 Ring 製造真正耗盡。兩平台均驗到第二個 Module 的 ResourceExhausted、
參數不變、第一個 Publisher 仍可發布。Console 原因為驅動繼承 Ctrl+C-ignore，隔離 console
重跑通過，正式碼未改。Windows 19.51 Python 防護由上述後續任務修正；8080／10013 仍待處理；
原 UI 失敗不以其他測試通過代替，專用 SDK App 不檢查 UI 可用性。

**Audit/evidence:** 717 non-Markdown repository inputs matched the compiled Linux
snapshot; final Windows and Linux task-process scans were empty. Ignored evidence
is under `build/publisher-param/`: original failures, final build/test logs, JUnit,
result JSON, `linux-final-audit.json`, `windows-final-processes.json`, and per-SDK
fixture `run.log`/stage logs. No benchmark or runtime-policy changes were made.

**稽核／證據：** 717 個非 Markdown 輸入與實際 Linux 快照一致，兩平台未遺留本輪程序。
原始失敗、最終 logs／JUnit／結果 JSON 及 SDK 分階段紀錄保留於上述 Git 忽略路徑。
本輪未改效能量測或 Runtime 政策。

## macOS 來源補驗（2026-11-11）

目前 entry-19／Bus v4 的原生 Debug／Release 已補驗 `module.publisher_defaults`、`message_bus.defaults_process` 及 Python services，涵蓋兩項預設、部分／完整覆寫、最後合法值、既有 Publisher、真實預算耗盡及 Python None／整數。
環境、實際案例、首輪失敗與最終結果統一見
[第二階段主報告](../module_system/MACOS_SOURCE_FUNCTIONAL_VALIDATION.md)。
上方歷史輪次的來源待驗狀態由本輪取代；後續[第三階段](../sdk/MACOS_SDK_DEPLOYMENT_VALIDATION.md)
已補新 Debug／Release SDK-only 交付及搬移 App；來源功能與交付整合的案例範圍分別記錄，未做 benchmark。
