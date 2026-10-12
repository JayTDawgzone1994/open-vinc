# 專案訊息套件

本套件位於儲存庫根目錄的 `vinc_messages/`，與 `apps/` 同層。

`UserExampleFrame.msg`／`UserExampleItem.msg` 供
[Module SDK 對照範例](../examples/userExampleModule/README.md) 使用，並由同一份編譯型別清單提供給 C++ 與 Python。
它們是示範應用訊息，不是中央控制協定或框架必需訊息。

使用者在 `messages/` 新增或修改 `.msg`；本套件維護對應的生成建置規則。
連結 `vinc::messages`（或既有的 `vinc_messages`）即可取得生成相依、標頭路徑及訊息儲存 library。

```cpp
#include <vinc_messages/test.hpp>
```

標頭輸出至本次建置目錄的 `generated/include/vinc_messages/`，不再寫回原始碼。
Debug／Release 使用各自的建置目錄，避免不同建置互相覆蓋輸出。
`generate_messages` 保留每次執行重新生成的行為；失敗不保留可用的舊標頭。
不可對同一輸出目錄同時執行兩個 generator。

格式與失敗處理見 [Ribosome](../src/vinc/ribosome/README.md)，訊息 API 見 [訊息儲存](../src/vinc/components/message_storage/README.md)。

## 搬移驗證

2026-10-04：訊息套件搬到儲存庫根目錄，6 個 `.msg` 內容保持一致。
Windows x64／MSVC Debug、Release 完整建置通過，各組態的 `ribosome.hash`、
`ribosome.schema` 與 `message_bus.shell` 共 3 項 CTest 通過。
隔離副本的生成回歸測試共 13 個情境通過，包含標頭編譯、輸入／模板失敗與恢復、
完整建置及巢狀訊息；外部 SDK consumer 與 Module 建置通過，consumer 執行返回 0。
本次未重跑其他 CTest；Linux／macOS 未驗證。
