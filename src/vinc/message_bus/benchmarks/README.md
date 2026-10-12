# Message Bus 延遲 benchmark

這是 opt-in 量測工具，不加入一般 CTest，也不更動框架預設設定。
一個測試中央透過 ModuleManager 啟動兩個 ModuleHost，載入同一個 Plugin，
分別作為 Publisher 與 Subscriber。使用真正的 Context、Ribosome 訊息、共享記憶體與 callback。

## 建置與執行

在 MSVC x64 developer environment 中，從儲存庫根目錄執行：

```powershell
cmake -S src -B build/windows-msvc-release -DVINC_BUILD_BUS_BENCHMARK=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows-msvc-release --target bus_benchmark
python src/vinc/message_bus/benchmarks/run.py --exe build/windows-msvc-release/vinc/message_bus/benchmarks/bus_benchmark.exe --plugin build/windows-msvc-release/vinc/message_bus/benchmarks/bus_benchmark_plugin.dll --output build/bus-benchmark-results
```

Python 僅使用標準函式庫，不需要 psutil。輸出目錄必須尚不存在，避免覆蓋舊結果。
可用 `--sizes 1 128`、`--modes push`、`--samples 1000` 縮小測試。
冒煙測試可用 `--sizes 1 --samples 16`，少量樣本不可用來估計穩定 P99。
Windows 執行檔使用 wmain 與 OSAL UTF-8 轉換；Module 參數、報告路徑都明確使用 UTF-8。

分析器驗證：

```powershell
python src/vinc/message_bus/benchmarks/test_analysis.py
```

## 範圍與容量政策

- 資料陣列大小為 1、2、4、8、16、32、64、128 **MiB**，1 MiB = 1,048,576 bytes。
  訊息另有 sequence 與映像 metadata。完整映像大小及 Publisher slot 上限分開記錄。
- 每次只有一組 Publisher／Subscriber；前一組完成 shutdown、確認程序退出與回收後才開始下一組。
- 測試中央的共享配置預算是 **4 GiB**，包含 RingBuffer control、對齊 payload 與 topic counter。
  不包含 OS page rounding、程序本機副本、Plugin／Host 及其他框架資源。
- 從 8 slots 開始。校準的容量上限額外保留 1 MiB control 餘裕；真正准入仍交給 BusManager，
  事後再核對 `PublisherStats::shared_bytes + 8 <= 4 GiB`，不把估算當實際配置。
- 先試跑 128 筆，正式預設 1,000 筆；兩者均暖機至少 64 筆且不少於 ring 兩輪。
  暖機後停 1 秒讓讀者消化資料，正式測量期間容量與間隔保持不變。
- 有讀寫碰撞或任何遺失時先增加容量；到上限才降低頻率。每組最多 12 次校準。
  若 Publisher 的 P99「準備＋送出呼叫」成本大於設定間隔，也會先降低頻率再試跑。
  無法滿足條件則報錯並保留證據，不宣稱該組完成。
- 名義起始頻率 100 Hz；Windows 一般 sleep／pump 等待可能被 OS 延後。
  同時記錄要求與實際頻率，不改 timer resolution、執行緒優先權、affinity 或 pump 設定。
  發送落後不突發追趕。校準不以 sleep 的排程誤差判定處理超載。
- 正式結果若碰撞／遺失，保留該次資料並重新校準；summary 只收錄合格正式組，
  所有成功執行的試跑及未合格正式組另保留於 `attempts.csv`。

這輪是**有節奏、低碰撞的延遲測量**，報告吞吐量為該負載下實際收到的資料率，
不是飽和吞吐量或最高效能。不同大小／路徑可能採不同頻率，不能忽略負載條件直接比較。

## 兩條路徑與時間邊界

`BenchmarkPayload` 使用一個 uint64 sequence 與 uint8 陣列。
本機 `push` 路徑重用已配置、初始化的訊息，每筆更新 sequence。
`loan` 路徑每筆呼叫 `get_next()`、resize／清零共享陣列、設定邊界標記，再 commit。
目前公開 MessageArray 沒有可寫 `data()`，benchmark 不繞進私有布局取得 raw pointer。
這是實際 SDK 操作成本；不宣稱整條 loan 路徑沒有配置、初始化或內部資料搬移。

每筆時間使用 OSAL 同主機單調 nanoseconds，不使用 wall clock，也不直接比較不同程序的
`steady_clock::time_point`。記錄存入預留的小型 vector，完成後才寫 CSV；不逐筆印到 OutputHub。

| 指標 | 邊界 |
| --- | --- |
| latency | push／commit 呼叫前 → 接收端 callback 第一個時間戳 |
| send_call | push／commit 呼叫前 → 返回後 |
| prepare | 更新本地 sequence，或借用＋resize＋填寫 → push／commit 呼叫前 |
| full_path | prepare 開始 → callback 進入 |

latency 包含框架提交、pump 等待、接收複製與驗證。loan 的 get_next 等待歸在 prepare，
因此必須一起看 full_path／碰撞統計，不能僅用 commit latency 宣稱整條路徑快。
callback 只讀取 sequence、驗證資料長度與頭尾標記、記錄到預留 vector，隨後釋放本機訊息。
callback 進入後的驗證／析構不計入該筆 latency，但仍會影響下一筆與持續吞吐量。
不在應用 callback 再完整掃描 128 MiB；框架映像驗證照常執行，頭尾標記不是全內容雜湊。

## 統計與限制

- P50／P90／P99 使用 nearest rank `ceil(p × n)`，只統計成功收到的正式樣本。
  預設 1,000 筆的上端 1% 約 10 筆，是初次基準，不是穩定尾延遲或最差情況保證。
  另列出前／後四分之一樣本的 P50，供檢查排隊延遲是否持續累積；沒有碰撞不代表負載可無限維持。
- 完整比對應收／實收序號，包含尾端沒收到的資料；不只相信 Bus 的覆蓋計數。
  重複、未知序號、負延遲或邊界標記失敗皆視為 integrity failure，不靠降頻掩蓋。
- 碰撞、等待、覆蓋遺失的計數包含暖機，以確保整個案例都符合低干擾條件。
  延遲百分位排除暖機。Subscriber 最後最多等待 5 秒追上，Publisher 在收尾前維持存活。
- 收到的 MiB/s 以正式第一筆送出開始到最後送出返回／callback 到達的較晚者為區間。
  同時提供實際發送 Hz；不把每筆大小除以平均延遲當作吞吐量。
- CPU 是各程序 OS user＋kernel 累計時間差除以該程序量測期間，**100% 為一個邏輯核心**。
  Module 的資源期間為 on_start 到保存統計前，中央包含啟停；包含暖機、初始化、等待與回收，
  不是正式資料區間專用 profiler。資源採樣失敗標記 unavailable，不填假零。
- 記憶體為各程序 lifetime peak RSS／Windows PeakWorkingSetSize。共享頁面會重複計入不同程序，
  **不可把 Publisher 與 Subscriber 的峰值相加當成實體 RAM 使用量**；亦不等於 private commit。
- 這不是隔離實驗室環境；其他專案、遊戲、電源模式、記憶體壓力與 OS 排程皆會影響結果。
  不替使用者關閉應用或更動系統設定。Windows 之外的效能與資源計量須另行實機驗證。
  固定發布節奏與 pump 的相位亦可能使延遲分布呈現多個群集，P50 不適合單獨用來比較兩條路徑。

## 輸出與收尾

- `REPORT.md`：精簡延遲與吞吐量表。
- `summary.csv`：合格正式組的全部統計、容量、負載與各程序資源指標。
- `attempts.csv`：校準與正式測試的完整紀錄，包括未合格的測量。
- `environment.json`：機器、Python、CMake 組態、命令列與基底 Git revision。
- 各案例目錄的 `sent.csv`／`received.csv`／`samples.csv`：原始時間戳、配對與未收到的樣本。
- `publisher.csv`／`subscriber.csv`／`*_resources.csv`／`driver.log`：原始統計及診斷。

中央有每組期限、停止與 OS 程序回收；Python 外層另有總期限。
Ctrl+C／外層逾時先留下 cancel marker，讓中央正常 shutdown。中央逾時仍不退出才終止該中央，
由 Host 的中央失聯政策停止子程序；這個 fallback 不承諾 POSIX 命名共享區無殘留。

## 驗證現況

2026-10-06 Windows x64／MSVC Release 完成 16 組 × 1,000 筆正式量測；
全部收到，最終組碰撞／遺失皆為零，最大共享配置約 3.938 GiB。
另保留 46 次校準／正式嘗試，其中 8 次有碰撞或遺失，未列為合格結果。
完整數字、環境與原始紀錄見 [首次結果](RESULTS.md)。

最終工具 Release 建置無警告；分析器 5／5 測試通過，
Context／跨程序 Message Bus／RingBuffer 回歸 3／3 通過（12.51 秒）。
中文目錄的 push／loan 冒煙測試亦通過；冒煙數字不混入正式效能結果。
Linux／macOS 未實機驗證；本輪未重跑全專案測試，也不構成即時性保證。
