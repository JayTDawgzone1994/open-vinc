<a id="c--python-rpc-example"></a>

# C++ / Python RPC example / C++／Python RPC 範例

Both Modules register `echo<int3, int3>` and can call each other or themselves.
The C++ ModuleType is `rpcExample`, with instance name `rpcCpp`.

兩個 Module 都登記 echo<int3,int3>，可互相或自己呼叫。C++ ModuleType 為 rpcExample，實例 rpcCpp。

Build with `VINC_BUILD_EXAMPLES=ON`. For Python, also enable `VINC_BUILD_PYTHON`,
install a compatible system Python, and copy `rpcPython.py` to
`data/module/rpcPython/rpcPython.py` beside Vinc before launching it.
This follows the normal Python discovery contract; no interpreter is embedded.

啟用 VINC_BUILD_EXAMPLES。Python 另需 VINC_BUILD_PYTHON 與相容系統 Python，於啟動前把 rpcPython.py 放到 Vinc 旁 data/module/rpcPython/rpcPython.py。沿用一般啟動掃描，不嵌入直譯器。

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
