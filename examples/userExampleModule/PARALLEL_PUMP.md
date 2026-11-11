# Parallel callbacks / 並行 callback

The runnable pair is [C++](cpp/parallel_module.cpp) and
[Python](python/parallelPython/parallelPython.py). Both receive `parallel/left`
and `parallel/right`, retain the most recent owned message for each Topic, and
share a `gain` parameter with their control callbacks. These are opt-in examples,
not a change to Recorder or other Modules' default scheduling.

可執行的 C++／Python 範例都訂閱上述兩個 Topic，保留各自最新訊息，並讓 Param、
Shell 與 Topic callback 共用有鎖保護的應用狀態。範例必須自行選入 App，不改其他
Module 的預設執行方式。完整行為以 [排程契約](../../src/vinc/module_system/PARALLEL_PUMP.md) 為準。

Select these in the App Kconfig / 在 App 設定選入：

```ini
CONFIG_VINC_MODULE_parallelExample=y
CONFIG_VINC_APP_PYTHON=y
CONFIG_VINC_MODULE_parallelPython=y
```

After building and starting the App / 建置並啟動 App 後：

```text
parallelExample
parallelPython --name parallelPy
parallelCpp send
parallelPy send
parallelCpp status
parallelPy status
param set parallelCpp.gain 3
param set parallelPy.gain 3
parallelCpp send
```

Commands can arrive before all Topic callbacks finish; repeat `status` to observe
the count. Param changes apply when pumped, not as an atomic boundary around the
next publication. The two Modules share Topic names intentionally: each receives
both Publishers' messages. Exit through the App's normal `exit` command.

`status` 可能先於 Topic callback 完成，稍後再查即可。Param 在 pump 時套用，不保證
與下一筆訊息形成原子切換。兩個 Module 刻意使用相同 Topic，因此都能收到雙方發布的
資料；用 `exit` 正常關閉整個 App。

## Choosing execution / 選擇執行方式

```cpp
// FrameworkLoop: set at construction, N additional Topic workers.
ParallelExample() : BaseModule("parallelCpp", {.topic_workers = 2}) {}

// Manual: choose when entering the loop; check/return its Result.
return context.pumpLoop({.topic_workers = 2});
```

```python
class Module(vinc.BaseModule, mode=vinc.RunMode.FrameworkLoop,
             pump_options=vinc.PumpOptions(topic_workers=2)):
    ...

# In a Manual main():
self.context.pumpLoop(vinc.PumpOptions(topic_workers=2))
```

Omitting options means **zero additional workers**. `1` adds one Topic worker,
which can overlap the control pump. `pumpOnce()` stays serial and finite;
`waitForWork()` never dispatches. Do not launch several public pumps on one Context.

省略設定就是原有單執行緒派送。`1` 仍會新增一條 Topic worker，與控制 pump 並行。
同 Topic 保持序列，不能依賴固定的 OS thread；不要自己開多條 thread 同時呼叫 pump。

## State and ownership / 狀態與所有權

- Copy the needed configuration under a short lock, then release it for processing.
  Param/Shell/RPC may run concurrently with Topic callbacks; the framework does not
  protect your fields. Different Topics also need this synchronization.
- C++ `std::move(message)` transfers the delivered `unique_ptr`; Python retaining
  `message` keeps that delivery alive without another payload copy. The framework
  does not overwrite retained messages. Synchronize access if handing one object
  to other threads; Python aliases do not create independent copies.
- Do not hold the application lock across blocking RPC, SDK operations, I/O or a
  whole expensive callback. A bounded application work queue needs its own full
  policy; the example only retains one message per Topic.
- GIL-enabled Python does not gain pure-Python CPU parallelism. I/O/native code
  that releases the GIL can overlap. The GIL is not a replacement for protecting
  multi-step application state or shared payloads during released-GIL operations.

短暫持鎖取設定副本、鎖外計算、再持鎖交付結果。C++ 可 move 走訊息；Python 可直接保存
訊息物件，但把同一物件交給多條 thread 後仍須自行同步。框架不會重用仍被你持有的訊息
空間，也不替你管理無界佇列。不要把 GIL 當成應用層的完整互斥契約。

## Stop and errors / 停止與失敗

Callbacks must finish independently of `on_stop()`: use bounded work or stop-aware
waits. Framework callbacks/workers drain before `on_stop`; that hook joins your
own threads. A stuck callback still faces the existing Module stop deadline.
Synchronous RPC in dispatched callbacks is rejected; use asynchronous RpcClient.
An escaping Python callback exception fails its Module, even if a Manual caller
ignores the pump failure. Central and unrelated Modules remain supervised separately.

callback 不可等 `on_stop()` 才放行。框架先排空自身 callback／worker，再呼叫該 hook；
你自建的 thread 仍由你停止與 join。同步 RPC 在 callback 內會被拒絕，改用非同步
RpcClient；Python callback 未處理例外依原政策使該 Module 故障，不會默默丟掉 worker。

The source test `module.python_parallel` compiles the same C++ example and loads
the same Python file, verifies bidirectional Topic delivery and normal shutdown.
Its other fixtures verify ordering, callback overlap, retained messages and GIL-safe
drain/failure. Platform evidence belongs in the linked contract, not in this tutorial.

來源測試使用同一份範例檔案，驗證雙向傳送及正常關閉；實際平台結果集中記錄於契約文件。
