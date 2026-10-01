# Tcl 相依套件

## 固定來源

| 項目 | 值 |
| --- | --- |
| 實作 | 標準 Tcl，非 Jim Tcl；不納入 Tk GUI |
| 正式版本 | **9.1.0** |
| 官方發布日 | 2026-09-29 |
| 選版確認日 | 2026-10-01 |
| 官方來源 | <https://core.tcl-lang.org/tcl/> |
| 官方 GitHub 鏡像 | <https://github.com/tcltk/tcl> |
| submodule URL | `https://github.com/tcltk/tcl.git` |
| 上游 tag | `core-9-1-0` |
| 固定 Git commit | `0c6bad08d91fa2ad51f05a50a41f8c1ea82ce985` |
| 工作目錄 | `library/tcl/upstream/` |

選擇依據：確認時官方下載頁列出的最新正式版本為 Tcl 9.1.0，且 GitHub 鏡像具有
對應正式 tag；未選用 alpha、beta、release candidate 或開發分支。
此版本剛發布不久，「正式版」不代表 Open-VINC 已完成相容性驗證。

- [官方下載清單](https://www.tcl-lang.org/software/tcltk/download.html)
- [9.1 發布說明](https://www.tcl-lang.org/software/tcltk/9.1.html)
- [固定 tag](https://github.com/tcltk/tcl/tree/core-9-1-0)
- [固定 commit](https://github.com/tcltk/tcl/commit/0c6bad08d91fa2ad51f05a50a41f8c1ea82ce985)

主專案的 Git gitlink 記錄完整 commit；`.gitmodules` 記錄 URL 與目錄。
不設定追蹤上游分支，不在一般取得流程使用 `git submodule update --remote`。

## 取得與驗證

從 Open-VINC 根目錄執行：

```sh
git submodule update --init --recursive -- library/tcl/upstream
git -C library/tcl/upstream rev-parse HEAD
git -C library/tcl/upstream rev-parse 'core-9-1-0^{commit}'
git -C library/tcl/upstream status --short
```

兩個 rev-parse 結果應為上表的完整 commit；status 應無輸出。
detached HEAD 是預期狀態，代表使用主專案固定的上游快照。
從 Open-VINC 的歷史版本回溯相依套件時，先切換主專案版本，再執行 submodule update。

## 原始碼與授權

`upstream/` 完整保留上游內容，包括 [license.terms](upstream/license.terms) 與各元件自身的授權聲明。
不修改上游檔案，也不套用本地補丁。未來的 C++ 封裝、CMake 整合與文件放在本目錄或
Open-VINC 自有原始碼目錄，編譯產物放到根目錄 `build/`。

## 整合狀態與平台邊界

目前僅加入原始碼與版本記錄，尚未加入 CMake 建置、連結、初始化資源部署或替換 Shell 腳本引擎。
未執行 Tcl 建置、上游測試或 Open-VINC 與 Tcl 的整合測試；三平台執行能力均待驗證。

上游 `win/README` 記載 Tcl 9.1 的 Windows 最低目標為 Windows 10，並提供 Visual C++／NMAKE
建置路徑。這是選定相依套件的限制，Open-VINC 的完整最低 OS／工具鏈矩陣仍須在整合時確認。
Linux Ubuntu 22.04 與 macOS 的實際建置、部署與行為尚待驗證。

後續整合需處理 Tcl 初始化腳本與其他執行時資源，不能只以成功連結代表部署完成。
Ctrl+C 需串接 Tcl 公開取消 API 與框架完整關閉流程，並驗證無限迴圈、等待模組與錯誤路徑。

## 後續更新

更新時先核對官方正式發布與鏡像 tag，記錄新的完整 commit、發布來源及平台限制；
再執行對應的建置與行為驗證，更新本文件、套件總表及主專案的 gitlink。
來源 tag 方便辨識版本，實際回溯依據為主專案保存的完整 commit。
