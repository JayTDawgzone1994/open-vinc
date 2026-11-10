# Pump cost probe / Pump 成本探針

This opt-in installed-SDK consumer is not part of a normal build or CTest. It
compares native Manual `pumpLoop` with 0/1/2/4 additional Topic workers, 1/4 Topics,
and cheap/deterministic CPU-heavy callbacks. It does not enable Recorder workers.
此為手動執行的 installed SDK 驗收工具，不加入一般建置或 CTest，也不改 Recorder。

```sh
cmake -S src/vinc/module_system/tests/pump_cost -B build/pump-cost -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -Dvinc_DIR=/absolute/sdk/lib/cmake/vinc
cmake --build build/pump-cost --target PumpCostApp --parallel 4
python src/vinc/module_system/tests/pump_cost/run.py \
  --app build/pump-cost/output/PumpCostApp/Release/PumpCostApp \
  --output build/pump-cost-results --environment "OS, compiler, standard library, Release"
```

On Windows use the matching x64 developer environment and `.exe` suffix. The
output directory must not exist; no prior result is deleted. Each bounded case
uses a new App/Module process and orderly shutdown. Timeout cleanup targets only
the process tree/group created by that case. Do not run another copy with the
same App identity concurrently. Complete other builds/tests before measuring.
Windows 使用相同工具鏈的 x64 環境與 `.exe`；輸出須為新目錄。勿同時執行同名 App 或
其他建置／測試，避免資源競爭扭曲比較。

The native Module both publishes and receives through real Context/Bus shared
memory; this measures dispatch costs under a finite burst, **not cross-process
transport throughput or paced real-time latency**. Each Topic gets 2,048 retained
slots of 1,024 bytes. Each generated message has two uint64 fields plus framework
metadata. No ring overwrite is needed: every Topic must deliver all 2,048 samples
in order. CPU-heavy callbacks execute 50,000 dependent integer-mixing steps;
their checksum is observable. Per-Topic measurement storage is preallocated.
同一原生 Module 自發自收，測量有限 burst 下的派送成本；不是跨程序傳輸 benchmark。
預留整批 ring 容量、檢查逐筆順序與漏收；大量排隊造成的 latency 不能當成低負載基線。

- Latency is from before `push()` to callback **completion**, including queue,
  copy and user work. P50/P99 use nearest rank; the summary is the median of
  three independent runs' percentiles, not a pooled percentile.
- Throughput spans first push to last completion. CPU is **whole Module process**
  user+kernel time over that interval (publisher, Host, pump and workers). It
  excludes central/UI/Recorder and teardown. CPU/wall × 100 may exceed 100%.
- Idle CPU is a one-second window after a 500 ms settling period, not a power
  measurement or proof of zero wakeups. Windows process-time resolution can hide
  tiny costs; a reported zero is below measurement resolution.
- Peak RSS is the OS process high-water mark after idle and after load. It includes
  Runtime, rings and fixture data; it is not an isolated worker allocation count.
  Uncommitted stack reservation is not reflected. Linux/macOS RSS units differ
  and are normalized in the fixture.
- Native worker concurrency can improve expensive callbacks on different Topics;
  one Topic remains serial. These native numbers do not predict Python GIL scaling.

延遲含排隊、copy 與 callback 工作；CPU 是整個 Module，記憶體為程序歷史峰值。
閒置觀察只有一秒，Windows 的零值可能低於計時解析度。摘要為各次指標的中位數，
不是把所有樣本混合計算；不同平台請分開比較。Python 不由本探針推論多核心能力。

Results retain environment metadata, every case's Tcl/log/CSV, all.csv and median.csv.
Record actual platform findings in [the parallel pump contract](../../PARALLEL_PUMP.md).
