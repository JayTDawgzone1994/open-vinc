# Recorder cost comparison / Recorder 成本對照

Opt-in acceptance tools; not default CTest and not a public benchmark claim.
Both adapters use the **same prebuilt Release SDK**. The typed Bag implementation
and adapter come from commit `bdde615a5182741e0f6e30b071deef2b8ed1c068`; the generic
ones come from the current checkout. No production target or installed SDK is
edited. Keep that baseline revision available when reproducing this comparison.

這些是手動驗收工具，不加入預設 CTest。比較對象是同一新版核心上的舊 typed／新通用
Recorder，並非兩個完整歷史版本。SQLite v2／v3 分別沿用兩組實作，不以版本號不同判失敗。

## Reproduction / 重跑

Prepare a clean Release SDK with `VINC_INSTALL_SDK=ON`, `VINC_BUILD_APPS=OFF`,
`BUILD_TESTING=OFF`, then install component `VincSDK` into an isolated prefix.
Stop other builds/benchmarks before measuring. Choose fresh absolute work paths.

```sh
python3 prepare.py /path/to/open-vinc /path/to/fresh-cost-work
python3 build_cost.py /path/to/fresh-cost-work /path/to/sdk \
    --compiler /usr/bin/g++-13 --ninja /usr/bin/ninja
python3 runtime_cost.py /path/to/open-vinc /path/to/fresh-cost-work
```

On Windows use the selected Python and compiler paths. Add
`--msvc-state <producer-build>/.vinc-build-toolchain.json` to `build_cost.py`;
it restores the recorded MSVC include/library environment and the compiler/SDK
tool directories. Both variants use `/bigobj`, Release and one build job.
Do not mix SDK compiler versions or Debug/Release configurations.

Windows 請指定同一份 SDK 的工具鏈紀錄、`cl.exe` 與 Ninja 路徑。
不更動使用者全域環境，也不安裝套件；報告與生成測試專案只放在指定的新目錄。

## What is measured / 量測範圍

- 128 additional unused message classes plus one published custom class and the
  SDK's built-in catalog. Distinct types and active topics are different counts.
- Clean **external App** build time, not rebuilding the prebuilt framework.
- Three alternating direct compilations per Recorder TU, with identical generated
  message inputs. Windows reports that `cl.exe`'s peak working set/commit; Linux
  uses GNU time's maximum RSS for the compiler invocation including its child.
  These are not whole-build memory peaks or Recorder runtime RAM.
- Deployed file sizes and one comment-only message edit/rebuild per variant.
  Timings are descriptive: no statistical claim from a single full/incremental run.
- Real App/Host/Bus/SQLite recording, one topic, 1,000 records each with 1 KiB or
  64 KiB array payloads, 1,024 ring slots and 131,072-byte message capacity.
  Each group has three alternating A/B pairs. Publication uses 2 ms sleeps in ten
  100-record commands; actual timing includes OS scheduling and Shell overhead.
  The rate is **observed completion rate**, not saturated throughput or a 500 Hz guarantee.
- Queue budget 64 MiB and message limit 131,072 bytes; zero Bus loss/queue drops
  required. Compare record sequence, publisher metadata and all canonical payload
  bytes. Preserve finish completion and normal child-process cleanup.
- Recorder-only process CPU delta, one-second idle CPU, RSS sampled every 20 ms,
  post-publication drain and finish time. Sampled RSS can miss shorter peaks and
  includes resident shared mappings; it is not exclusive physical-memory usage.

此測試沒有模擬 MMDD 的 672 個 visitor 入口／大量活躍 Topic，也沒有證明所有負載都更快。
正式過載、byte／筆數配額、worker 保留容量與未知型別行為由 `bag.storage/module/loop`
及 Bus／Context 測試驗證。Mac 原生成本採樣尚未實作；不能以 Linux `/proc` 假裝支援 Mac。

Artifacts: `manifest.json`, `compile-recipes.json`, `build-results.json`,
`runtime-results.json`, per-command logs and recorded SQLite databases. Keep initial
failed runs separately; do not overwrite them with successful results.
The dated acceptance results live in [IMAGE_RECORDING.md](../../IMAGE_RECORDING.md).
