# Parallel pump costs / 多執行緒 pump 成本

2026-11-10, local `codex/parallel-pump-contract` working tree, Plugin entry 19.
Reproduction and measurement definitions: [probe README](tests/pump_cost/README.md).
Raw per-run numbers: [Windows](tests/pump_cost/results/windows.csv),
[WSL](tests/pump_cost/results/wsl.csv). No Recorder configuration was changed.
本紀錄是本機工作樹驗收，並非已發布 commit 的效能保證；方法與逐次數據見上述連結。

## Environment and limits / 環境與限制

Both runs used one Ryzen 7 5700X3D (8 cores / 16 logical CPUs), sequentially,
after the SDK builds and functional tests finished. No CPU affinity or real-time
priority was imposed. Other desktop/OS activity was not eliminated.

| Platform | Toolchain |
| --- | --- |
| Windows 11 x64, build 26200 | MSVC 19.51.36257, CMake 4.3.2, Ninja, Release `/O2 /MD`; driver CPython 3.13.12 |
| WSL2 Ubuntu 24.04 x64, kernel 6.18.54.1, glibc 2.39 | GCC/libstdc++ 13.3.0, CMake 3.28.3, Ninja, Release `-O3`; driver CPython 3.12.3 |
| Native macOS | Pending; root TODO contains the native test/cost matrix |

The driver executes 48 fresh Module processes per platform: 0/1/2/4 additional
workers × 1/4 Topics × cheap/heavy callbacks × 3 repetitions, deterministically
shuffled. Each Topic publishes a finite 2,048-message burst, with 2,048 ring slots
of 1,024 bytes. The message has two uint64 data fields plus generated metadata.
Heavy callbacks perform 50,000 dependent integer-mixing iterations; cheap ones
still execute validation/statistics. Both use actual Context, copy delivery and
shared-memory Bus, with producer and consumer in the **same native Module**.

每平台 48 組、每組新程序。這是派送成本探針，不是跨程序傳輸、固定 Hz 或產品 workload
benchmark。發布者也在被量測的 Module 內，CPU 包含發送與 Host 工作；不是只有 callback。
足量 ring 保留整批，避免用覆寫來提高數字；每平台實際交付 **245,760 筆、零漏收、零碰撞**，
逐 Topic 序號完整，所有 App 正常走完關閉等待。

Latency ends after user work, before returning from the callback body: it includes
queueing/copy/CPU work, not subsequent framework capture destruction. Throughput
ends when the measurement thread observes the final completion. Tables are medians
of three runs; P50/P99 are medians of per-run nearest-rank percentiles, not pooled
percentiles. Short cheap cases are sensitive to scheduling. These numbers neither
predict pure-Python GIL scaling nor replace application profiling.

## Four Topics, CPU-heavy / 四 Topic 重工作

| OS | Workers | Throughput (k msg/s) | P50 (ms) | P99 (ms) | Module CPU (ms) | Peak RSS (MiB) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Windows | 0 | 12.43 | 314.677 | 632.325 | 656.25 | 28.04 |
| Windows | 1 | 12.49 | 319.245 | 627.290 | 687.50 | 28.07 |
| Windows | 2 | 24.74 | 153.452 | 305.006 | 687.50 | 28.13 |
| Windows | 4 | 48.78 | 71.797 | 141.358 | 671.88 | 28.22 |
| WSL | 0 | 12.47 | 315.578 | 625.264 | 680.34 | 23.17 |
| WSL | 1 | 12.54 | 307.660 | 605.594 | 726.76 | 23.01 |
| WSL | 2 | 24.79 | 144.953 | 281.803 | 734.81 | 23.12 |
| WSL | 4 | 48.58 | 56.021 | 118.072 | 738.32 | 22.80 |

Four workers improved throughput by approximately **3.92× Windows / 3.90× WSL**
for this workload. Total process CPU time did not fall with wall time: the same
work runs on more cores, with extra dispatch cost. WSL process CPU increased
about 8.5% in this comparison; Windows about 2.4%, subject to its accounting
resolution. The large latency values represent burst queue drain, not idle latency.

四 worker 的收益是用多核心縮短完成時間，不是省掉運算。重工作有明確收益，不能推論
任意 callback 都會加速，也不能把數百毫秒的 burst 排隊時間當成通知延遲。

## Cheap work and one-Topic serialization / 輕工作與單 Topic 序列

| OS | Topics | Work | Workers | Throughput (k msg/s) | P50 (ms) | P99 (ms) |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| Windows | 1 | cheap | 0 | 336.38 | 0.395 | 0.479 |
| Windows | 1 | cheap | 4 | 264.10 | 1.058 | 1.735 |
| Windows | 1 | heavy | 0 | 12.39 | 80.008 | 159.199 |
| Windows | 1 | heavy | 4 | 11.56 | 85.328 | 168.990 |
| Windows | 4 | cheap | 0 | 342.27 | 0.181 | 1.839 |
| Windows | 4 | cheap | 4 | 339.73 | 0.009 | 0.070 |
| WSL | 1 | cheap | 0 | 252.69 | 0.007 | 0.043 |
| WSL | 1 | cheap | 4 | 145.70 | 3.220 | 5.673 |
| WSL | 1 | heavy | 0 | 12.55 | 78.013 | 154.350 |
| WSL | 1 | heavy | 4 | 9.65 | 102.887 | 201.895 |
| WSL | 4 | cheap | 0 | 262.36 | 0.005 | 0.052 |
| WSL | 4 | cheap | 4 | 254.41 | 0.077 | 0.364 |

One Topic stays serial; extra workers add scheduling/synchronization without
splitting that callback stream. Cheap multi-Topic results are mixed: Windows
tail latency improved, WSL latency worsened; neither showed a general throughput
gain at four workers. Retain **zero as the default** and select concurrency from
real workload evidence. Counts one and two are retained in the CSV, not omitted
from the experiment.

單 Topic 增加 worker 沒有並行收益，甚至更慢；輕工作也沒有通用改善。維持零 worker
預設，用真實負載決定是否開啟，不替 Recorder 或所有 Module 自動設定。

## CPU and memory limits / CPU、記憶體與待改善成本

CPU uses GetProcessTimes/getrusage for the whole Module process. Central, UI,
Recorder and shutdown are excluded. Windows cheap cases last only about 6–24 ms;
observed CPU values are quantized in 15.625 ms steps, sometimes zero. They are
**insufficient for precise cheap-work CPU attribution**. Retain raw readings
rather than interpreting zero as zero cost. One-second Windows idle windows are
also noisy (all cases 0–109.375 ms CPU); longer isolated sampling is needed before
claiming a precise Windows idle-worker increment.

WSL idle windows are more consistent. For four registered Topics, combining the
six pre-load windows at each worker count (cheap/heavy cases have the same idle
setup), process CPU medians were:

| Workers | CPU ms / approximately 1 s idle | Share of one logical CPU |
| ---: | ---: | ---: |
| 0 | 3.859 | 0.39% |
| 1 | 7.339 | 0.73% |
| 2 | 10.644 | 1.06% |
| 4 | 21.692 | 2.17% |

Workers block on a condition variable rather than spin. There is still recurring
work: `Context::dispatch_parallel()` constructs/submits a batch for each subscribed
Topic on coordinator passes, including the existing 10 ms fallback; an empty
batch is discovered by the worker. `TopicScheduler::finish()` notifies all workers.
These are confirmed code paths and plausible contributors, **not profiler-proven
attribution of all measured CPU**. A follow-up may profile empty batch allocation
and unnecessary wakeups while preserving notification fallback, finite snapshots,
same-Topic exclusion and shutdown. No scheduler optimization was silently added.

閒置不是忙等，但也不是零成本。WSL 四 Topic、四 worker 的整個 Module 約占一個邏輯
CPU 的 2.17%；目前會重建空 batch，finish 也會喚醒其他 worker。這些路徑值得後續
profile，尚未證明所有成本都來自它們；本輪不改通知補查或排程契約。

RSS is the **process high-water mark**, including rings, copied messages and fixture
storage. Windows four-Topic heavy median rose about 0.18 MiB between zero/four
workers; WSL's median was lower with four workers. The latter does not mean workers
save memory: allocator/working-set variation and earlier peaks dominate this small
difference. Virtual stack reservation is not measured. Do not promise a per-worker
byte cost from these samples. Native macOS and sustained real-world workloads
remain separate validation.

## Evidence locations / 證據位置

The tracked CSVs preserve all 96 numeric rows; the probe source and driver are
maintained beside them. Ignored Windows logs/Tcl/environment metadata are under
`build/parallel-stage4-cost-windows/`; WSL originals are under
`/home/vinc-test/python-module-deploy-20261105/parallel-stage4-cost-linux/`.
The earlier concurrent-build pilot is excluded from these CSVs and conclusions.
Windows Module SHA-256: `3f1b2ba0aedc00fdf8dfca94bbdbacc2060b9891b96f9d4a1e7fe310cb2787d7`.
WSL Module SHA-256: `db144012890c44c0e9201c80918bc3582afc03e927b561097654abf0aa61ddb5`.
