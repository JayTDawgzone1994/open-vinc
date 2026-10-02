# Components

此目錄集中供其他框架元件使用的工具與基礎設施。各工具保留自己的 CMake target，
由使用它的元件建立及管理生命週期；分類目錄本身不啟動服務，也不提供整包連結 target。

- [Concurrency](concurrency/README.md)：並行控制工具，目前提供使用 OSAL FileLock 的跨程序互斥鎖。

`components` 可以依賴 OSAL；`common` 則繼續放置只依賴標準函式庫的共用型別與契約。
目錄分類不加入 C++ 命名空間，例如 Concurrency 使用 `vinc::concurrency`。
