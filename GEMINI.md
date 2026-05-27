# OpenVinc 開發鐵律 (OpenVinc Development Workflow)

當執行 `openvinc` 專案的開發任務時，必須嚴格遵守以下標準作業流程：

## 1. 開發前置作業 (Sync & Rebase)
- 確保本機的 `develop` 分支為最新狀態 (`git pull origin develop`)。
- 於開發分支開始工作前，**必須**先執行 rebase (`git rebase develop`)，確保程式碼基底與最新的 develop 同步。

## 2. 程式碼標準與架構規範 (Code Standard & Architecture)
- **C++20 嚴謹標準**：必須使用最嚴謹的 C++20 標準寫法來撰寫程式碼，確保現代化、安全性與高效能。
- **不破壞系統架構**：開發時必須遵守現有的系統架構標準。絕對不可為了一時方便而破壞原本的架構設計。
- **架構變更需獨立處理**：若開發過程中發現必須更改系統架構，**不可**在目前的開發任務中一併處理。必須提出額外、獨立的 Request (Issue/Task) 來專門處理架構變更。

## 3. 開發與驗證 (Develop & Iterate)
- 開發期間必須反覆測試，確保所寫的功能邏輯正確且符合需求。

## 4. 建置與最終確認 (Build & Final Check)
- 開發完成後，必須執行專案建置 (Build)，確保編譯/打包過程完全通過且無錯誤。
- 建置成功後，需進行最終驗證，確認開發的目標功能在建置版本中運作完全正常。

## 5. 提交與合併請求 (Commit & PR/MR)
- 撰寫高品質、描述清晰的 Commit Message。
- 將分支推送到遠端儲存庫，並建立一個 Merge Request (或 Pull Request) 請求合併回 `develop` 分支。
