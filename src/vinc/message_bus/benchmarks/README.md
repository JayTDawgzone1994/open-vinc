<a id="message-bus-延遲-benchmark"></a>

# Message Bus latency benchmark / Message Bus 延遲 benchmark

This opt-in tool is outside normal CTest and changes no framework defaults. A test central starts two Hosts with one Plugin as Publisher/Subscriber, using real Context, generated messages, shared memory, and callbacks.

這是 opt-in 量測工具，不加入一般 CTest，也不更動框架預設設定。
一個測試中央透過 ModuleManager 啟動兩個 ModuleHost，載入同一個 Plugin，
分別作為 Publisher 與 Subscriber。使用真正的 Context、Ribosome 訊息、共享記憶體與 callback。

<a id="建置與執行"></a>

## Build and run / 建置與執行

From repository root in an MSVC x64 developer environment:

在 MSVC x64 developer environment 中，從儲存庫根目錄執行：

```powershell
cmake -S src -B build/windows-msvc-release -DVINC_BUILD_BUS_BENCHMARK=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows-msvc-release --target bus_benchmark
python src/vinc/message_bus/benchmarks/run.py --exe build/windows-msvc-release/framework/message_bus/benchmarks/bus_benchmark.exe --plugin build/windows-msvc-release/framework/message_bus/benchmarks/bus_benchmark_plugin.dll --output build/bus-benchmark-results
```

Python uses only the standard library, no psutil. Output directories must not exist. Narrow runs with `--sizes 1 128`, `--modes push`, or `--samples 1000`; `--sizes 1 --samples 16` is only smoke coverage, not reliable P99. Windows wmain/OSAL explicitly convert Module/report paths as UTF-8.

Python 僅使用標準函式庫，不需要 psutil。輸出目錄必須尚不存在，避免覆蓋舊結果。
可用 `--sizes 1 128`、`--modes push`、`--samples 1000` 縮小測試。
冒煙測試可用 `--sizes 1 --samples 16`，少量樣本不可用來估計穩定 P99。
Windows 執行檔使用 wmain 與 OSAL UTF-8 轉換；Module 參數、報告路徑都明確使用 UTF-8。

Analyzer validation:

分析器驗證：

```powershell
python src/vinc/message_bus/benchmarks/test_analysis.py
```

<a id="範圍與容量政策"></a>

## Scope and capacity policy / 範圍與容量政策

- Payloads 1,2,4,8,16,32,64, 128 MiB (1048576 bytes/MiB), plus sequence/image metadata; report image and slot limits separately.
- One Publisher/Subscriber pair at a time; complete shutdown/exit/recovery before another.
- Test central shared budget 4 GiB includes ring controls/aligned payload/topic counter, excluding OS page rounding/local copies/Host/Plugin/other resources.
- Begin 8 slots; calibration reserves 1 MiB control margin, but actual BusManager admission and shared_bytes+8<=4 GiB checks govern, not estimates.
- Pilot 128, default formal 1000, warmup>=64 and two ring cycles. Pause 1 s afterward; keep capacity/rate fixed during formal runs.
- On collision/loss, increase capacity first, then lower rate at capacity, at most 12 calibrations. If P99 prepare+send exceeds interval, lower rate before retry. Unsatisfied conditions fail with evidence, not a claimed completed result.
- Initial 100 Hz, recording requested/actual rate under normal Windows scheduling; no timer-resolution/priority/affinity/pump changes or catch-up bursts. Sleep jitter alone is not overload proof.
- Invalid formal runs retain evidence and recalibrate; summary contains only qualified runs, attempts.csv retains successful pilots/unqualified runs.

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

This measures paced low-contention latency, with throughput at that workload, not saturation/maximum speed. Different sizes/paths can use different rates; comparisons must retain load conditions.

這輪是**有節奏、低碰撞的延遲測量**，報告吞吐量為該負載下實際收到的資料率，
不是飽和吞吐量或最高效能。不同大小／路徑可能採不同頻率，不能忽略負載條件直接比較。

<a id="兩條路徑與時間邊界"></a>

## Paths and timing boundaries / 兩條路徑與時間邊界

BenchmarkPayload has uint64 sequence/uint8 array. Push reuses initialized local messages, updating sequence. Loan calls get_next, resizes/zeros shared arrays, sets edge markers, then commits. No writable public data() exists and the benchmark never reaches private raw layout. Actual SDK costs include possible allocation/initialization/movement, even in loan mode.

`BenchmarkPayload` 使用一個 uint64 sequence 與 uint8 陣列。
本機 `push` 路徑重用已配置、初始化的訊息，每筆更新 sequence。
`loan` 路徑每筆呼叫 `get_next()`、resize／清零共享陣列、設定邊界標記，再 commit。
目前公開 MessageArray 沒有可寫 `data()`，benchmark 不繞進私有布局取得 raw pointer。
這是實際 SDK 操作成本；不宣稱整條 loan 路徑沒有配置、初始化或內部資料搬移。

Use OSAL same-host monotonic nanoseconds, not wall clocks or raw cross-process steady time_points. Preallocated small vectors retain samples and write CSV afterward, without per-sample OutputHub output.

每筆時間使用 OSAL 同主機單調 nanoseconds，不使用 wall clock，也不直接比較不同程序的
`steady_clock::time_point`。記錄存入預留的小型 vector，完成後才寫 CSV；不逐筆印到 OutputHub。

| Metric | Boundary |
| --- | --- |
| latency | Before push/commit → callback entry timestamp |
| send_call | Before push/commit → after return |
| prepare | Local sequence update or loan/resize/fill → before push/commit |
| full_path | Prepare start → callback entry |

| 指標 | 邊界 |
| --- | --- |
| latency | push／commit 呼叫前 → 接收端 callback 第一個時間戳 |
| send_call | push／commit 呼叫前 → 返回後 |
| prepare | 更新本地 sequence，或借用＋resize＋填寫 → push／commit 呼叫前 |
| full_path | prepare 開始 → callback 進入 |

Latency includes commit/pump wait/receive-copy/validation. Loan get_next waiting is prepare, so compare full_path/collisions rather than commit alone. Callback checks sequence/length/edges, records preallocated samples, then releases local messages. Validation/destruction after entry is outside that sample latency but affects subsequent load. No application full 128 MiB scan; framework validation stays. Edge markers are not full hashes.

latency 包含框架提交、pump 等待、接收複製與驗證。loan 的 get_next 等待歸在 prepare，
因此必須一起看 full_path／碰撞統計，不能僅用 commit latency 宣稱整條路徑快。
callback 只讀取 sequence、驗證資料長度與頭尾標記、記錄到預留 vector，隨後釋放本機訊息。
callback 進入後的驗證／析構不計入該筆 latency，但仍會影響下一筆與持續吞吐量。
不在應用 callback 再完整掃描 128 MiB；框架映像驗證照常執行，頭尾標記不是全內容雜湊。

<a id="統計與限制"></a>

## Statistics and limits / 統計與限制

- P50/P90/P99 use nearest rank ceil(p*n), successful formal samples only.1000 samples provide about 10 top 1% observations, not stable tail/worst-case guarantees. First/last-quarter P50 helps detect accumulating queue delay; zero collisions does not prove indefinite sustainability.
- Compare all expected/received sequences including trailing missing data, not only Bus loss counters. Duplicates/unknowns/negative latency/bad markers are integrity failures, not hidden by rate reductions.
- Collisions/waits/loss include warmup; latency percentiles exclude it. Reader waits up to 5 s at end while Publisher stays alive.
- Received MiB/s spans first formal send to later of last send-return/callback; report actual Hz. Size divided by mean latency is not throughput.
- CPU=user+kernel process-time delta/measurement duration,100%=one logical core. Module interval spans on_start→stats; central includes lifecycle, warmup/init/wait/recovery, not formal-only profiling. Unavailable samples stay unavailable, never zero.
- Memory is lifetime peak RSS/PeakWorkingSetSize. Shared pages count in multiple processes; **do not sum peaks as physical RAM**, nor interpret them as private commit.
- Other apps/games/power/memory/scheduling influence results; no apps/settings are changed. Non-Windows measurements need native verification. Publishing/pump phase can create clusters, making P50 alone misleading.

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

<a id="輸出與收尾"></a>

## Outputs and cleanup / 輸出與收尾

- REPORT.md: compact latency/throughput.
- summary.csv: qualified statistics/capacity/load/process resources.
- attempts.csv: pilots/formal including rejected measurements.
- environment.json: machine/Python/CMake/CLI/base revision.
- Per case sent/received/samples CSV: raw times/matches/missing samples.
- Publisher/Subscriber/resources CSV and driver.log: raw diagnostics.

- `REPORT.md`：精簡延遲與吞吐量表。
- `summary.csv`：合格正式組的全部統計、容量、負載與各程序資源指標。
- `attempts.csv`：校準與正式測試的完整紀錄，包括未合格的測量。
- `environment.json`：機器、Python、CMake 組態、命令列與基底 Git revision。
- 各案例目錄的 `sent.csv`／`received.csv`／`samples.csv`：原始時間戳、配對與未收到的樣本。
- `publisher.csv`／`subscriber.csv`／`*_resources.csv`／`driver.log`：原始統計及診斷。

Central has per-case deadlines/stop/process reaping; Python has a total limit. Ctrl+C/outer timeout first writes a cancel marker for orderly shutdown. Only a still-stuck central is killed, allowing Host central-loss policy to stop children; this fallback does not promise zero POSIX name remnants.

中央有每組期限、停止與 OS 程序回收；Python 外層另有總期限。
Ctrl+C／外層逾時先留下 cancel marker，讓中央正常 shutdown。中央逾時仍不退出才終止該中央，
由 Host 的中央失聯政策停止子程序；這個 fallback 不承諾 POSIX 命名共享區無殘留。

<a id="驗證現況"></a>

## Recorded validation / 驗證現況

Recorded 2026-10-06 Windows x64/MSVC Release: 16 groups × 1000 formal samples, all received with zero final collision/loss, maximum shared allocation~3.938 GiB. 46 attempts retained, 8 with collision/loss excluded from qualified summary. See [first results](RESULTS.md) for exact environment/data.

2026-10-06 Windows x64／MSVC Release 完成 16 組 × 1,000 筆正式量測；
全部收到，最終組碰撞／遺失皆為零，最大共享配置約 3.938 GiB。
另保留 46 次校準／正式嘗試，其中 8 次有碰撞或遺失，未列為合格結果。
完整數字、環境與原始紀錄見 [首次結果](RESULTS.md)。

Final Release build was warning-free; analyzer **5/5**, Context/Bus/Ring regressions **3/3** (12.51 s). Unicode-path push/loan smoke passed but was excluded from formal metrics. No Linux/macOS/full-project rerun or real-time guarantee.

最終工具 Release 建置無警告；分析器 5／5 測試通過，
Context／跨程序 Message Bus／RingBuffer 回歸 3／3 通過（12.51 秒）。
中文目錄的 push／loan 冒煙測試亦通過；冒煙數字不混入正式效能結果。
Linux／macOS 未實機驗證；本輪未重跑全專案測試，也不構成即時性保證。

## Notification fan-out mode / 通知扇出量測

The native driver accepts an optional final subscriber count (default 1, range 0 through the configured per-topic subscriber limit). Zero measures publication without receivers; counts above one start distinct Module instances, writing each receiver under `subscriber-N/`. Each run still uses one Publisher and the existing 4 GiB shared budget. The standard run.py calibration remains a single-receiver benchmark; fan-out analysis must verify every receiver independently. No production thread or framework setting is changed.

原生 driver 最後可加訂閱 Module 數量，預設 1；0 量測無接收者的發布成本，多個接收者各輸出到 subscriber-N/。run.py 仍為單接收者校準，多接收者需逐一核對遺失與完整性。每組沿用 4 GiB 共享預算。

The [2026-10-28 notification measurements](NOTIFICATION_RESULTS_2026-10-28.md) cover 64 B through 128 MiB, both paths, calibrated large-payload capacity/rate, and 0/1/8/32 subscriber fan-out.

[通知喚醒量測報告](NOTIFICATION_RESULTS_2026-10-28.md)包含大小資料、雙路徑、容量／頻率校準及多 Module 通知成本。
