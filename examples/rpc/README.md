<a id="c--python-rpc-example"></a>

# C++ / Python RPC example / C++／Python RPC 範例

The example CMake files register **rpcPython** through `add_python_module()`.
Enable Python and select the matching `CONFIG_VINC_MODULE_<ModuleType>=y` options
in the App config, then build the App to deploy them automatically. Manual copying
to the new paths below remains supported; there is no fallback to `data/module`.

範例已用 `add_python_module()` 登記；App 啟用 Python 並選入對應 ModuleType 後，
建置 App 即自動部署。也可沿用下方的新路徑手動放入，不再搜尋舊 `data/module`。


Both Modules retain a reusable asynchronous RpcClient and register `echo<int3, int3>` and can call each other or themselves.
The C++ ModuleType is `rpcExample`, with instance name `rpcCpp`.

兩個 Module 都保存可重複的非同步 RpcClient，並登記 echo<int3,int3>，可互相或自己呼叫。C++ ModuleType 為 rpcExample，實例 rpcCpp。

Select `CONFIG_VINC_MODULE_rpcExample=y` in App Kconfig. For Python, also set `CONFIG_VINC_APP_PYTHON=y` in App Kconfig,
install a compatible system Python, and copy `rpcPython.py` to
`modules/python/rpcPython/rpcPython.py` beside Vinc before launching it.
This follows the normal Python discovery contract; no interpreter is embedded.

在 App Kconfig 選入 `CONFIG_VINC_MODULE_rpcExample=y`。Python 另需在 App Kconfig 設定 CONFIG_VINC_APP_PYTHON=y 與相容系統 Python，於啟動前把 rpcPython.py 放到 Vinc 旁 modules/python/rpcPython/rpcPython.py。沿用一般啟動掃描，不嵌入直譯器。

In the Vinc Shell:

在 Vinc Shell：

```text
rpcExample
rpcPython --name rpcPy
rpcCpp rpcPy
rpcPy rpcCpp
rpcCpp rpcCpp
```

The Shell command returns after local submission; the response prints on a later
pump. Both use FrameworkLoop, so no user-created RPC thread is needed.
Request/response schemas are generated at build time and must match on both sides.

Shell 命令本地提交後即返回，回覆由後續 pump 顯示。兩者都採 FrameworkLoop，不需自行建立 RPC thread。Request／Response schema 在建置時生成，雙方必須相符。

See [RPC contracts](../../src/vinc/rpc/README.md) for fixed capacity, QueueFull,
timeout outcomes, callback ownership and failure handling.

固定容量、QueueFull、逾時結果、callback 所有權與錯誤處理見 [RPC 契約](../../src/vinc/rpc/README.md)。

The client binds `module.echo`, callback and optional CallOptions once. Repeated commands
reuse it; sending again before completion begins returns Busy. Changing the target closes
the previous handle, suppressing any future callback without cancelling remote execution.
`register_rpc()` takes handler before optional ServiceOptions. Synchronous calls are shown
in the RPC README; they must not be made from this pump-dispatched Shell hook.

Client 使用 module.echo 地址，重複命令沿用 handle；前一筆未派送時回 Busy。切換目標會關閉
舊 handle，但不保證取消遠端工作。此 Shell hook 在 pump 中執行，因此採非同步，不能同步等待。
