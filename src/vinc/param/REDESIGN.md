# Param contract guide / Param 契約導覽

The redesign is implemented. The [Param README](README.md) owns the current contract;
this page is a reading guide, not another specification or implementation checklist.
重設計已完成；正式契約只由 README 維護，避免重複版本與過時的開工步驟。

| Topic / 主題 | Contract / 契約 |
| --- | --- |
| Ownership and permissions / 所有權與權限 | [Limited／Public](README.md#命名型別與權限) |
| Startup and correction / 啟動與非法值修正 | [固定預設、args 與 Module 審核責任](README.md#startup-and-correction) |
| Current values / 讀取現值 | [read_current，不更新 pump 快取](README.md#current-reads) |
| Notifications / 通知 | [快取與合併 callback](README.md#pump快取與-callback) |
| Persistence / 保存 | [TOML 與明確 save](README.md#toml-與保存) |
| Restart hints / 重啟提醒 | [RegisterOptions](README.md#restart-hints) |

System Modules use the same Context and permission rules as other Modules.
No expected-version correction API or per-change callback delivery is implied.
System-setting migrations are documented by their owners: Publisher defaults, network
settings and file logging are separate integrations, not exceptions to Param ownership.

Validation: [Windows／WSL redesign acceptance](README.md#param-redesign-stage-four),
[macOS Param source and SDK acceptance](../sdk/MACOS_PARAM_VALIDATION.md).
