<a id="專案訊息套件"></a>

# Project message package / 專案訊息套件

This package lives at repository-root vinc_messages/, alongside apps/.

本套件位於儲存庫根目錄的 `vinc_messages/`，與 `apps/` 同層。

UserExampleFrame.msg/UserExampleItem.msg support the [C++/Python examples](../examples/userExampleModule/README.md) through one compiled type list. They are demonstration application messages, not central protocols or required framework messages.

`UserExampleFrame.msg`／`UserExampleItem.msg` 供
[Module SDK 對照範例](../examples/userExampleModule/README.md) 使用，並由同一份編譯型別清單提供給 C++ 與 Python。
它們是示範應用訊息，不是中央控制協定或框架必需訊息。

Add/edit .msg under messages/. This package owns generation rules. Linking vinc::messages (or existing vinc_messages) propagates generation, headers, and Storage linkage.

使用者在 `messages/` 新增或修改 `.msg`；本套件維護對應的生成建置規則。
連結 `vinc::messages`（或既有的 `vinc_messages`）即可取得生成相依、標頭路徑及訊息儲存 library。

```cpp
#include <vinc_messages/test.hpp>
```

Headers go to build-local generated/include/vinc_messages, never the source tree. Separate Debug/Release outputs avoid collisions. generate_messages regenerates every invocation and failure leaves no old-header fallback. Never run concurrent generators into one output directory.

標頭輸出至本次建置目錄的 `generated/include/vinc_messages/`，不再寫回原始碼。
Debug／Release 使用各自的建置目錄，避免不同建置互相覆蓋輸出。
`generate_messages` 保留每次執行重新生成的行為；失敗不保留可用的舊標頭。
不可對同一輸出目錄同時執行兩個 generator。

See [Ribosome](../src/vinc/ribosome/README.md) for format/failure handling and [Storage](../src/vinc/components/message_storage/README.md) for APIs.

格式與失敗處理見 [Ribosome](../src/vinc/ribosome/README.md)，訊息 API 見 [訊息儲存](../src/vinc/components/message_storage/README.md)。

<a id="搬移驗證"></a>

## Move validation / 搬移驗證

Recorded 2026-10-04: moved the package to repository root, retaining six message contents. Windows x64/MSVC full Debug/Release builds and three CTests each (hash/schema/Bus Shell) passed. An isolated 13-case generation regression covered header compilation, input/template failures/recovery, full build/nesting. External SDK/Module built; consumer returned 0. Other suites and Linux/macOS were not tested in that round.

2026-10-04：訊息套件搬到儲存庫根目錄，6 個 `.msg` 內容保持一致。
Windows x64／MSVC Debug、Release 完整建置通過，各組態的 `ribosome.hash`、
`ribosome.schema` 與 `message_bus.shell` 共 3 項 CTest 通過。
隔離副本的生成回歸測試共 13 個情境通過，包含標頭編譯、輸入／模板失敗與恢復、
完整建置及巢狀訊息；外部 SDK consumer 與 Module 建置通過，consumer 執行返回 0。
本次未重跑其他 CTest；Linux／macOS 未驗證。

<a id="外部訊息目錄"></a>

## External message directories / 外部訊息目錄

Public add_message_dir collects built-in/external directories together with cross-directory references. Only direct .msg children are collected; global type names reject case collisions. Ordinary CMake registration completes before collection, with no user finalize. Added/deleted inputs update generation lists; every build reruns generation without stale deleted headers. Configure registers only; Ribosome builds before generation, and vinc::messages consumers wait. See [SDK functions](../src/vinc/sdk/README.md#公開-cmake-functions).

SDK 公開 `add_message_dir(directory)`；內建目錄與外部登記目錄一起生成，允許跨目錄引用。
每個目錄只收集直接包含的 `.msg`，型別名稱全域唯一，大小寫碰撞拒絕。
所有一般 CMake 登記完成後才收集生成規則，使用者不必呼叫 finalize。
新增／刪除檔案會更新 CMake 生成清單；生成器每次建置重跑，刪除的訊息不留下舊標頭。
配置階段只登記，Ribosome 建置完成後才生成，所有連結 vinc::messages 的使用端等待生成。
完整使用例見 [SDK 公開 functions](../src/vinc/sdk/README.md#公開-cmake-functions)。
