# Param Redesign Implementation Record / Param 重設計實作紀錄

This record predates the subsequent [restart-hint work](README.md#restart-hints),
which advances wire/Plugin versions and has its own acceptance stages.
本紀錄早於後續重啟標記工作；新版 wire／Plugin 與其驗收階段見上方連結。

## Status / 狀態

Stages 1–4 are implemented and validated on Windows/WSL, including clean installed-SDK
acceptance. Native macOS acceptance remains pending.
第一至四階段已實作並通過 Windows／WSL 驗收，包含乾淨 installed SDK 整合；
Mac 原生驗收待補。四階段均已取得使用者開工授權。

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
| 4. SDK integration / SDK 整合 | 乾淨 SDK、搬移後 C++／Python 消費端、文件收斂與 Mac 清單 | [Stage 4](README.md#param-redesign-stage-four): Windows／WSL 各三套通過；Mac 待驗 |

Stage boundaries are work/validation boundaries, not a requirement that every intermediate
commit be independently deliverable. Stages 2–3 did not add another wire/Plugin layout change;
central persistence/queue algorithms were retained and tested rather than rewritten.
階段是工作／驗收邊界，不要求每個中間 commit 可獨立交付。第二至三階段沒有再改
wire／Plugin 布局；中央保存與佇列演算法保留，以行為測試確認契約。

## Remaining acceptance / 剩餘驗收

Native macOS must validate the same topic commit. Follow the source, example and clean SDK
checklist in the [stage 4 record](README.md#param-redesign-stage-four), also indexed by
[root TODO](../../../TODO.md). Existing Mac baselines are retained but do not certify this change.
Mac 必須對同一 topic commit 做原生驗收；步驟與紀錄統一維護於上述連結，根目錄 TODO
保留入口。既有 Mac 基線不撤銷，也不拿來代替本次重設計驗收。
