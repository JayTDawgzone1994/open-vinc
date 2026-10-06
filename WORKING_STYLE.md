# Working Style

本文件保存設計哲學、審美與方案取捨偏好。工作流程、授權及必須遵守的共同契約由
[AGENTS.md](AGENTS.md) 維護；正式術語由 [NAMING.md](NAMING.md) 維護。

## 1. 使用者重視什麼

- 架構一致性高於短期開發速度。
- 可讀性與責任邊界高於炫技。
- 不為 zero-copy、lock-free 等技術名詞犧牲通用性。
- 真實 workload 與產品落地優先於漂亮 benchmark。
- framework 應暴露壓力與瓶頸，不默默掩蓋。
- 命名要讓第一次看到的人大致猜對用途，避免過度隱喻。
- component locality 很重要，進入元件目錄應能看到其 API、實作、測試與建置。
- 能在 build time 解決的問題，不要無故拖到 runtime。

## 2. 設計討論方式

- 不只回答「能不能做」，要指出 trade-off。
- 若有多種方案，優先比較責任邊界、維護成本、使用者直覺、runtime 成本。
- 遇到 framework API，先區分 programmer error 與 runtime failure，再辨認容量不足、資源取得失敗等執行期原因。回報方式與例外邊界依 [AGENTS.md](AGENTS.md) 的錯誤契約，不因此擴大例外處理範圍。
- 不因為某做法「常見」就默認採用，要說明它是否符合 Open-VINC 的哲學。
- 若發現使用者方案其實更簡潔，不要為了顯得專業而硬加 abstraction。

## 3. 對 AI 產出要求

- 不接受「能編譯就算完成」。
- 主動檢查命名、重複邏輯、隱性相依、錯誤路徑、生命週期與所有權。
- 偏好清楚展開操作順序的寫法；具體格式依 [AGENTS 程式碼可讀性規則](AGENTS.md#程式碼可讀性)。
- 不偷偷增加 exception、allocation、background thread、runtime registry。
- 新的抽象、target、目錄或 helper 應能說明它解決的具體問題，以及增加的維護成本。
- 可讀性重構應有具體收益，避免只因個人審美就大幅改寫功能正確的 code；變更範圍依 AGENTS 的授權流程確認。
