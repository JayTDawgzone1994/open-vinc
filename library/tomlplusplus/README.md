# toml++

- 上游：[marzer/tomlplusplus](https://github.com/marzer/tomlplusplus)
- 版本：**v3.4.0**，commit `30172438cee64926dc41fdd9c11fb3ba5b2ba9de`。
- `upstream/` 為未修改的 Git submodule；`revision.txt` 與 configure 檢查固定版本。
- 授權：MIT，保留 `upstream/LICENSE`；Vinc 建置部署至 `licenses/tomlplusplus/LICENSE`。
  自行包裝中央執行檔的 SDK 消費端亦須隨交付保留此授權。
- 以正式 header-only 介面解析／輸出 TOML 1.0，設定上游支援的 `TOML_EXCEPTIONS=0`，
  語法錯誤由 parse_result 處理；這不保證 STL 配置不拋例外。
- `vinc_toml` 僅供中央 Param persistence 使用，Plugin 與 ModuleHost 不需要 TOML parser。
  include 標記 SYSTEM；沒有修改上游來消除編譯器的 deprecated literal-operator 警告。
- 取得方式：專案根目錄 `git submodule update --init --recursive`。

資料格式、保存契約與測試見 [Param README](../../src/vinc/param/README.md)。
