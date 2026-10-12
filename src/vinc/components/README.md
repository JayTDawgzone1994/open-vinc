# Components

此目錄集中供其他框架元件使用的工具與基礎設施。各工具保留自己的 CMake target，
由使用它的元件建立及管理生命週期；分類目錄本身不啟動服務，也不提供整包連結 target。

- [Concurrency](concurrency/README.md)：使用 OSAL SharedMemory／SharedWord 的框架跨程序互斥鎖；中央監督與 Host 接線由 [Module System](../module_system/INSTANCE.md) 維護。
- [Message Storage](message_storage/README.md)：生成訊息的儲存、容器與可搬移映像；runtime 只依賴 common。

`components` 可以依賴 OSAL；`common` 則繼續放置只依賴標準函式庫的共用型別與契約。
目錄分類不加入 C++ 命名空間，例如 Concurrency 使用 `vinc::concurrency`。
此分類不代表 API 皆為框架私有；各元件仍自行區分正式 API 與 detail 接線，
例如 `MessageArray` 是使用者可直接使用的正式 API。
