# SQLite

固定版本 **3.54.0**，使用官方發行的 amalgamation；上游檔案原樣保存。
來源、完整 `SQLITE_SOURCE_ID` 與發行壓縮檔 SHA3-256 見 [revision.txt](revision.txt)。
CMake 另外核對三個原始碼檔案的 SHA-256，一般建置不連網或追蹤最新版。

- 官方來源：https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip
- 發行內容是 SQLite 官方從來源生成的 C 檔，故在主儲存庫保存固定檔案，而非自行生成或修改上游。
- SQLite 核心屬 public domain；原始碼開頭的授權／blessing 保留。參見 https://www.sqlite.org/copyright.html 。
- wrapper target `vinc::sqlite` 只由 `vinc::bag` 私有連結。沒有全域 cache、編譯定義或 include 修改。
- 固定 static、PIC、hidden symbols、threadsafe。關閉動態 extension 載入及 double-quoted string literals。
- Windows CRT 跟隨主專案設定。Linux 連結 Threads／dl／m；三平台行為須各自實測。
- 更新需重新核對官方來源、SOURCE_ID、壓縮檔及各檔案雜湊，並重跑 Bag 測試。
