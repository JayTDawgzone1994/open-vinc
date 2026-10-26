<a id="python-module-範例"></a>

# Python Module examples / Python Module 範例

Complete C++/Python comparison, containers, Param, Log, and Manual operation: [userExampleModule](../userExampleModule/README.md).

完整 C++／Python 對照、容器、Param、Log 與 Manual 操作見
[userExampleModule](../userExampleModule/README.md)。

Build the [Python SDK](../../src/vinc/module_system/python/README.md), copy pythonExample.py to **beside Vinc** at data/module/pythonExample/pythonExample.py, then restart central.

先依 [Python Module 文件](../../src/vinc/module_system/python/README.md) 建置 Python SDK。
將 `pythonExample.py` 複製到 **Vinc 執行檔旁**的
`data/module/pythonExample/pythonExample.py`，然後重新啟動 Vinc。

In Vinc Shell:

在 VINC Shell 輸入：

```text
pythonExample --name cameraFront
cameraFront status
cameraFront gain 10
cameraFront status
exit
```

cameraFront is the instance name and must not collide with a ModuleType/instance. Runtime defaults to PATH Python; override with Vinc --python <interpreter>. This introductory gain is a local variable, **not Param**. print goes through OutputHub/central Log.

`cameraFront` 是實例名稱，不得與 ModuleType 或其他實例撞名。
Vinc 預設選 PATH 中的 Python；需要指定時，以 `Vinc --python <Python執行檔路徑>` 啟動。
這個入門範例的 gain 是本地變數，**不是 Param**。
Python `print()` 由既有 OutputHub／中央 Log 收集。

<a id="context-服務範例"></a>

## Context service example / Context 服務範例

Deploy pythonBusExample.py as data/module/pythonBusExample/pythonBusExample.py, restart central, then:

將 `pythonBusExample.py` 放到 `data/module/pythonBusExample/pythonBusExample.py`，重啟中央後：

```text
pythonBusExample --name samples
samples send 1 2 3
samples loan 4 5 6
param set samples.gain 2.5
```

This example uses real Param/Log/Publisher/pump callbacks. send copies a local message; loan fills shared Publisher storage before commit. Loan/fields are invalid after commit. Subscription callbacks own independent local copies. Failed loans are not automatically cancelled; stop/restart the instance before retrying.

此範例使用真正的 Param、Log、Publisher 與 pump callback。
`send` 從本地訊息複製發布；`loan` 直接填寫 Publisher 借出的訊息再提交。
commit 後不可使用 loan 或其欄位。訂閱 callback 收到的是框架已複製的獨立本地訊息。
範例不會自行取消填寫失敗的 loan；發生這種錯誤時，關閉／重啟該實例再試。
