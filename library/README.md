# 第三方相依套件

本目錄集中管理 Open-VINC 使用的第三方原始碼、固定版本與整合說明。

| 套件 | 固定版本 | 來源管理 | 目前狀態 |
| --- | --- | --- | --- |
| [Tcl](tcl/README.md) | 9.1.0 | 官方 GitHub 鏡像 submodule，固定 commit | 已接入 CMake 與 Shell；Windows 驗證，Linux／macOS 待驗證 |
| [toml++](tomlplusplus/README.md) | 3.4.0 | 上游 submodule，固定 commit | 中央 Param 的 TOML 解析與輸出，Plugin 不相依 |
| [pybind11](pybind11/README.md) | 3.1.0 | 上游 submodule，固定 commit | 可選 Python Module 綁定；生命週期、Shell hooks 與 pump |

## 維護原則

- 上游原始碼保持原樣，使用正式公開的 API 與支援的建置選項進行二次封裝。
- Open-VINC 的封裝、建置整合與文件放在 `upstream/` 外，不維護魔改版或本地補丁。
- 依各套件授權要求保留來源、授權及必要聲明；套件自身及所包含元件的授權文件一併保留。
- 使用明確版本與完整 commit 固定相依內容。一般建置不自動追蹤上游最新分支。
- 更新相依版本需明確進行，記錄原因與驗證結果，再提交主專案的 submodule 指標。
- 編譯產物統一放到根目錄 `build/`，保持第三方原始碼工作目錄乾淨。
- 遇到上游缺陷或 API 不足時，先說明影響並評估上游修正、正式版本或替代方案。

## 取得與還原

首次下載 Open-VINC 時，使用 `git clone --recurse-submodules <儲存庫網址>`。
已下載、pull 或切換分支後，從專案根目錄執行：

```sh
git submodule update --init --recursive
git submodule status --recursive
```

上述 update 取得的是主專案記錄的 commit，不是上游最新版本。
初次取得或缺少指定 Git 物件時需要連線；所需物件已在本地時可以離線還原。
若子模組有本地變更，先檢查並保存，不使用 `--force` 覆寫。

submodule 是來源管理方式；套件的建置、連結及執行時資源部署須各自完成整合。


## Vinc Log 相依

- fmt/upstream：12.2.0，1be298e1bd68957e4cd352e1f676f00e07dcfb57，MIT；SDK 格式化相依。
- spdlog/upstream：1.17.0，79524ddd08a4ec981b7fea76afd08ee05f83755d，MIT；中央保存端私有相依。

上游未修改。區域 wrapper 固定 static、不 FORCE 父層 cache；spdlog 使用同一份 external fmt。
完整授權保留於 upstream/LICENSE*，散布時須攜帶。

## SQLite Bag 相依

[SQLite](sqlite/README.md) 3.54.0 使用官方固定 amalgamation，原樣保存並核對雜湊及 SOURCE_ID。
它是 Bag 的私有 static 相依；一般建置不下載，SDK 不暴露 SQLite 標頭。

## Web UI 私有相依

- [cpp-httplib](cpp-httplib/README.md) v0.61.0：HTTP server，MIT。
- [nlohmann/json](json/README.md) v3.12.0：plugin manifest／catalog，MIT。
- [PicoSHA2](picosha2/README.md) v1.0.1：套件 SHA-256，MIT。

三者固定 submodule commit；wrapper 僅提供 SYSTEM include 與必要連結，未執行上游 CMake、
未修改來源或全域編譯選項。HTTP 只在 UI Module 程序使用；一般 SDK 無新增 HTTP runtime 相依。
UI 生成部署的 `app/third-party.txt` 包含這三者及打包 Vue runtime 的原始授權文字。

前端 Vue 3.5.44、Vite 8.3.4、TypeScript 5.9.3 與開發工具由
`src/vinc/ui/web/package-lock.json` 固定版本／integrity；npm ci 明確還原到被 Git 忽略的 node_modules。
這些 npm 套件不另以 submodule 或複製來源維護；不修改其內容。
Node/npm 只在建置／測試時需要，部署只帶生成靜態資源，詳見 [UI 文件](../src/vinc/ui/README.md)。
