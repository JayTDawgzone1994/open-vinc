# Param Redesign Implementation Record / Param 重設計實作紀錄

Later work migrates only the two [Publisher defaults](../message_bus/PUBLISHER_DEFAULTS.md).
The exclusions below describe this earlier redesign, not the later migration.
後續另輪僅搬移上述兩項 Publisher 預設；下文排除範圍保留原輪次的事實，不否定後續接線。

This record predates the subsequent [restart-hint work](README.md#restart-hints),
which advances wire/Plugin versions and has its own acceptance stages.
本紀錄早於後續重啟標記工作；新版 wire／Plugin 與其驗收階段見上方連結。

## Status / 狀態

Stages 1–4 are implemented and validated on Windows/WSL, including clean installed-SDK
acceptance. Subsequent native Mac source and Debug/Release clean-SDK acceptance,
including the later restart-hint ABI, are recorded in the [Mac report](../sdk/MACOS_PARAM_VALIDATION.md).
第一至四階段已實作並通過 Windows／WSL 驗收；後續 Mac 來源及兩組乾淨 SDK 已完成，
包含後來的重啟標記 ABI，範圍見上述報告。四階段均已取得使用者開工授權。

The confirmed behavior is now maintained in the [Param README](README.md).
This file records scope, decisions and completion evidence; it does not maintain a
second API specification. Do not infer authorization for future work from this record.
已確認契約統一由 Param README 維護；本文件只保留範圍、決策索引及階段證據，
不重複維護 API 規格，也不授權後續新工作。

## Confirmed scope / 已確認範圍

- Limited/Public ownership and central Session identity checks:
  [names and permissions](README.md#命名型別與權限).
- Ordinary set, bounded central admission, coalesced delivery and accepted correction
  races: [startup/correction](README.md#startup-and-correction),
  [pump](README.md#pump快取與-callback), [errors/costs](README.md#錯誤與成本).
- Independent current reads without changing pump cache:
  [read_current](README.md#current-reads).
- Fixed defaults, explicit args overrides, owner business validation and explicit save:
  [startup](README.md#startup-and-correction), [persistence](README.md#toml-與保存),
  [Shell/Tcl](README.md#shell).
- Wire/shared-image v2 and Plugin entry 12; TOML remains format 1:
  [integration and compatibility](README.md#接線同步與協定).

本輪保留一般 set 與合併 callback，不新增 expected-version／correct API、隱含快取版號、
逐筆交付佇列、業務審核 registry 或背景執行緒。System Module 與一般 Module 使用相同
Context／權限；具體成本與生命週期限制見上述正式契約。

**Excluded:** migration of the nine system settings, changes to config.hpp values,
UI/Bus/Log setting wiring, and global App startup reordering. These remain separate work.
**不包含：** 九項系統設定搬移、config.hpp 數值修改、UI／Bus／Log 設定接線及整個 App
啟動順序重排；這些仍須另輪設計與授權。

## Implementation and evidence / 實作與證據

| Stage / 階段 | Delivered / 成果 | Validation / 驗收 |
| --- | --- | --- |
| 1. Permissions and identity / 權限與身分 | Limited/Public、中央授權、Session/MID 檢查、協定相容性 | [Stage 1](README.md#param-redesign-stage-one): Windows／WSL 通過 |
| 2. Current reads / 啟動讀取 | C++ read_current、Host 並行使用權、Python binding；快取語意不變 | [Stage 2](README.md#param-redesign-stage-two): Windows／WSL 通過 |
| 3. Startup and persistence / 啟動與保存 | C++／Python 範例、固定 default／args／live／saved 分離、修正流程、佇列滿測試 | [Stage 3](README.md#param-redesign-stage-three): Windows／WSL 通過 |
| 4. SDK integration / SDK 整合 | 乾淨 SDK、搬移後 C++／Python 消費端、文件收斂與 Mac 清單 | [Stage 4](README.md#param-redesign-stage-four): Windows／WSL 各三套通過；後續 [Mac Debug／Release 各三套通過](../sdk/MACOS_PARAM_VALIDATION.md) |

Stage boundaries are work/validation boundaries, not a requirement that every intermediate
commit be independently deliverable. Stages 2–3 did not add another wire/Plugin layout change;
central persistence/queue algorithms were retained and tested rather than rewritten.
階段是工作／驗收邊界，不要求每個中間 commit 可獨立交付。第二至三階段沒有再改
wire／Plugin 布局；中央保存與佇列演算法保留，以行為測試確認契約。

## Acceptance follow-up / 後續驗收

The original Mac checklist was completed by source regression and fresh Debug/Release installed
SDK acceptance. The [Mac report](../sdk/MACOS_PARAM_VALIDATION.md) owns scope and evidence.
Earlier platform baselines remain valid for their recorded revisions; this does not claim another
full-project run or authorize the excluded system-setting migration.

原 Mac 清單已有來源回歸及兩組乾淨 SDK 證據，方法與限制統一由上述報告維護。
根目錄 TODO 不再保留此已完成項目；未擴張為本輪重跑全套或系統設定搬移的授權。
