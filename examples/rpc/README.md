# C++ / Python RPC example

Both Modules register `echo<int3, int3>` and can call each other or themselves.
The C++ ModuleType is `rpcExample`, with instance name `rpcCpp`.

Build with `VINC_BUILD_EXAMPLES=ON`. For Python, also enable `VINC_BUILD_PYTHON`,
install a compatible system Python, and copy `rpcPython.py` to
`data/module/rpcPython/rpcPython.py` beside Vinc before launching it.
This follows the normal Python discovery contract; no interpreter is embedded.

In the Vinc Shell:

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

See [RPC contracts](../../src/vinc/rpc/README.md) for fixed capacity, QueueFull,
timeout outcomes, callback ownership and failure handling.
