# Notification wakeup measurements / 通知喚醒量測

Date: 2026-10-28. Windows 11 x64 (10.0.26200), Ryzen 7 5700X3D, MSVC 19.51.36257, Release.

Working tree based on `91d26bca95d80a6354323c58bea3f5fab9f9fe2c` with the notification integration. These are single-machine, paced workloads, not hard real-time bounds or saturation throughput.

本次量測使用通知整合工作樹；只代表這台電腦與下列負載，不是硬即時上限、三平台保證或飽和吞吐量。

## Method / 方法

- 1,000 measured messages plus 64 warmups per case, eight slots initially, 4 GiB shared-memory admission budget. Small/fan-out cases use eight slots and request 100 Hz. Large cases use the existing pilot/capacity/rate calibration; actual values are shown below.
- End-to-end latency is push/commit entry to callback entry, including shared-memory copy and validation. Loan preparation is separate; compare full-path P99 as well.
- No agent-started compiler/build/test workload ran concurrently with measurement; this was a normal interactive Windows session, not an isolated laboratory host. No priority, affinity, global timer-resolution, or framework pump-default changes were made.
- Percentiles use nearest rank. 1,000 samples give only about ten upper-1% observations. CPU is startup-to-report, including warmup/setup/drain, as percent of one logical core; timer granularity can produce 0.00% for very small costs. Pure idle CPU was not measured.
- Raw CSV, hashes, and environment records are in ignored build directories, never substituted by screenshots or inferred delivery counts.

每組正式 1,000 筆、暖機 64 筆；共享預算 4 GiB。延遲包含框架複製及驗證；loan 的借用／resize／填寫另計。量測未與建置或測試並行，也未調優先權、affinity 或全域計時器。CPU 含暖機／初始化／收尾，不是純閒置成本；P99 僅為有限樣本統計。

## Latency and send cost / 延遲與發布成本

| Payload | Path | Slots | Requested / actual Hz | P50 ms | P90 ms | P99 ms | Send P99 ms | Full path P99 ms | Clean |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 64 B | push | 8 | 100.00 / 64.20 | 0.0311 | 0.0534 | 0.0957 | 0.0543 | 0.0964 | True |
| 64 B | loan | 8 | 100.00 / 64.12 | 0.0262 | 0.0388 | 0.0718 | 0.0420 | 0.0992 | True |
| 1 KiB | push | 8 | 100.00 / 64.11 | 0.0326 | 0.0523 | 0.0971 | 0.0601 | 0.1008 | True |
| 1 KiB | loan | 8 | 100.00 / 64.17 | 0.0271 | 0.0387 | 0.0748 | 0.0410 | 0.1005 | True |
| 1 MiB | push | 8 | 100.00 / 64.10 | 0.4011 | 0.4729 | 0.5890 | 0.2511 | 0.5897 | True |
| 1 MiB | loan | 8 | 100.00 / 64.18 | 0.2523 | 0.2914 | 0.3668 | 0.0203 | 0.7883 | True |
| 2 MiB | push | 8 | 100.00 / 64.06 | 0.6157 | 0.7104 | 0.9067 | 0.3252 | 0.9074 | True |
| 2 MiB | loan | 8 | 100.00 / 64.12 | 0.4243 | 0.4678 | 0.7072 | 0.0193 | 1.5978 | True |
| 4 MiB | push | 8 | 100.00 / 64.15 | 1.1172 | 1.1985 | 1.7762 | 0.5161 | 1.7768 | True |
| 4 MiB | loan | 8 | 100.00 / 64.14 | 0.7433 | 0.8387 | 1.4761 | 0.0189 | 3.2339 | True |
| 8 MiB | push | 8 | 100.00 / 64.02 | 2.0922 | 2.2911 | 3.7406 | 1.1545 | 3.7414 | True |
| 8 MiB | loan | 8 | 100.00 / 64.01 | 1.4719 | 1.7145 | 2.7356 | 0.0199 | 6.2645 | True |
| 16 MiB | push | 8 | 100.00 / 64.09 | 4.0167 | 4.2915 | 7.3270 | 2.1819 | 7.3278 | True |
| 16 MiB | loan | 8 | 100.00 / 63.78 | 2.7684 | 3.8530 | 5.8563 | 0.0215 | 12.6883 | True |
| 32 MiB | push | 8 | 100.00 / 63.72 | 7.7186 | 11.2095 | 14.2527 | 4.3890 | 14.2536 | True |
| 32 MiB | loan | 8 | 48.33 / 32.05 | 5.2134 | 6.1220 | 10.0529 | 0.0211 | 20.1005 | True |
| 64 MiB | push | 32 | 50.00 / 31.90 | 15.7529 | 21.1521 | 23.1380 | 10.3516 | 23.1389 | True |
| 64 MiB | loan | 8 | 23.85 / 21.32 | 10.5630 | 19.5231 | 21.6003 | 0.0214 | 40.2196 | True |
| 128 MiB | push | 31 | 26.40 / 21.19 | 31.3842 | 32.6714 | 37.0495 | 12.4733 | 37.0515 | True |
| 128 MiB | loan | 8 | 17.55 / 15.99 | 21.2194 | 25.3456 | 27.3820 | 0.0247 | 61.5135 | True |

The earlier same-machine 1 MiB / eight-slot / requested-100-Hz investigation recorded approximately 16 ms P99 with timed polling. The notification run measured 0.5890 ms (push) and 0.3668 ms (loan). These are comparable workload observations, not a controlled cross-platform speedup guarantee. Publisher sleep still achieved about 64 Hz at a requested 100 Hz; notification wakeup removes receiver polling latency, not every Windows scheduling delay. See [the baseline and integration contract](../../module_system/PUMP_WAKEUP.md).

同機器舊基準約 16 ms P99；本輪 1 MiB push／loan 為 0.5890／0.3668 ms。發布端要求 100 Hz 實際約 64 Hz 的 sleep 排程現象仍存在，這輪改善的是接收端等待，不是所有 OS 排程延遲。

## Fan-out / 多接收者成本

64 B push, eight slots, 1,000 measured publications per case. The same Publisher signals distinct Module generations; every receiver is checked independently.

| Receiver Modules | Send P50 ms | Send P99 ms | Worst receiver P99 ms | All receivers clean |
| ---: | ---: | ---: | ---: | --- |
| 0 | 0.0075 | 0.0391 | — | No receiver |
| 1 | 0.0139 | 0.0543 | 0.0957 | True |
| 8 | 0.0433 | 0.1273 | 0.1478 | True |
| 32 | 0.0876 | 0.2267 | 0.2408 | True |

The cost grows with destinations. Coalesced wakeups are not constant-cost broadcast: each normal publication still visits its destination snapshot and normally performs one native notify per destination. No maximum-fan-out/high-frequency guarantee follows from this paced 32-receiver test.

通知成本隨目的 Module 數量增加；合併喚醒不等於固定成本廣播。這組 32 接收者的有節奏負載不代表最大扇出或高頻壓力測試。

## Resource window / 資源觀察

| Payload MiB | Path | Publisher CPU % | Subscriber CPU % | Central CPU % | Shared MiB |
| ---: | --- | ---: | ---: | ---: | ---: |
| 6.10352e-05 | push | 0.167 | 0.082 | 0.894 | 0.040 |
| 6.10352e-05 | loan | 0.000 | 0.082 | 0.569 | 0.040 |
| 0.000976562 | push | 0.334 | 0.082 | 1.300 | 0.047 |
| 0.000976562 | loan | 0.335 | 0.165 | 0.813 | 0.047 |
| 1 | push | 0.334 | 1.321 | 1.143 | 8.040 |
| 1 | loan | 1.673 | 0.907 | 0.894 | 8.040 |
| 2 | push | 0.167 | 0.412 | 0.568 | 16.040 |
| 2 | loan | 4.513 | 4.616 | 0.731 | 16.040 |
| 4 | push | 1.505 | 4.787 | 0.897 | 32.040 |
| 4 | loan | 7.272 | 5.862 | 0.490 | 32.040 |
| 8 | push | 1.669 | 4.286 | 0.814 | 64.040 |
| 8 | loan | 9.263 | 9.397 | 0.815 | 64.040 |
| 16 | push | 6.265 | 9.983 | 0.489 | 128.040 |
| 16 | loan | 28.106 | 16.978 | 1.213 | 128.040 |
| 32 | push | 12.731 | 40.953 | 1.616 | 256.040 |
| 32 | loan | 21.396 | 21.523 | 0.394 | 256.040 |
| 64 | push | 21.355 | 37.586 | 0.738 | 2048.135 |
| 64 | loan | 32.460 | 31.026 | 1.042 | 512.040 |
| 128 | push | 25.410 | 52.917 | 0.906 | 3968.131 |
| 128 | loan | 45.978 | 42.446 | 0.856 | 1024.040 |

## Reproduction and evidence / 重現與證據

```powershell
cmake --build build/windows-msvc-release --target bus_benchmark
python src/vinc/message_bus/benchmarks/run.py --exe build/windows-msvc-release/framework/message_bus/benchmarks/bus_benchmark.exe --plugin build/windows-msvc-release/framework/message_bus/benchmarks/bus_benchmark_plugin.dll --output build/new-measurement --sizes 2 4 8 16 32 64 128 --samples 1000
# Create an empty output directory first for each native-driver case:
build/windows-msvc-release/framework/message_bus/benchmarks/bus_benchmark.exe build/windows-msvc-release/framework/message_bus/benchmarks/bus_benchmark_plugin.dll push 64 8 10000000 1000 64 build/new-small-case 32
```

Use the native command with 64, 1024, or 1048576 payload bytes and push/loan for the small cases; omit the final count for one receiver. The standard analyzer in run.py joins sequence IDs and checks missing/duplicate/invalid messages, collisions and loss. For fan-out, analyze each subscriber-N directory against the same publisher timestamps/stats. Report invalid pilots separately rather than including them as successful measurements.

Local evidence: `build/notification-benchmark-20261028/{small,fanout}-summary.csv`, raw per-case CSV and binary hashes; `build/notification-large-20261028/{summary,attempts}.csv`, `environment.json`, `REPORT.md`, and each trial directory. Old evidence was not overwritten.

原始資料保留於上述 build 路徑；小資料可直接用 driver 指定 byte 數與接收者數。多接收者必須逐一用相同發布時間戳核對，不只看其中一個。未合格試跑仍保留，不能混入成功統計。

## Calibration failures retained / 保留未合格試跑

A clean final run does not mean every pilot was clean. The cases below triggered capacity/rate calibration and are excluded from successful latency comparisons. Clean pilots whose prepare+send P99 exceeded their requested period also caused rate reduction; see attempts.csv for every attempt.

正式組通過不代表試跑都通過。以下未合格組保留於原始資料，不能用來宣稱零碰撞；即使無遺失，準備加發送 P99 超過要求間隔時仍會降低頻率。

| Trial | Slots | Actual Hz | Collisions | Lost counter | Missing incl. warmup | Integrity errors |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| push-64MiB-pilot-0 | 8 | 63.62 | 21 | 27 | 27 | 0 |
| push-64MiB-pilot-1 | 16 | 66.89 | 5 | 16 | 16 | 0 |
| push-128MiB-pilot-0 | 8 | 69.23 | 1 | 167 | 167 | 0 |
| push-128MiB-pilot-1 | 16 | 68.45 | 4 | 138 | 138 | 0 |
| push-128MiB-pilot-2 | 31 | 66.70 | 4 | 87 | 87 | 0 |
