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
不修改上游檔案，也不套用本地補丁。C++ 封裝、CMake 整合與文件放在本目錄或
Open-VINC 自有原始碼目錄，編譯產物放到根目錄 `build/`。

## 整合狀態與平台邊界

已透過 CMake ExternalProject 接入上游建置，靜態連結至 Shell，取代舊腳本引擎。
使用單一組態產生器（例如 Ninja），Debug／Release 分開建置目錄。

- Windows：MSVC／NMAKE、`OPTS=static,noembed`；Debug 加 `symbols` 配合 `/MDd`。
  使用編譯器公開選項 `/utf-8`，避免系統字碼頁造成上游編譯失敗。
  NMAKE 需要在來源 `win/` 工作，因此 `stage.cmake` 將未修改來源複製到 `build` 下，
  並依 `revision.txt` 生成上游正常建置使用的 `manifest.uuid`；不在 submodule 產生輔助檔。
- Linux／macOS：上游 `unix/configure --disable-shared --disable-zipfs` 與 make，需 zlib 開發套件。
  已提供建置路徑，尚未實機驗證。
- Tcl 核心不依賴系統 Tcl 安裝；`lib/tcl9.1` 的初始化、編碼與腳本資源隨執行檔部署。
  各執行檔從自己的目錄定位資源，不依賴目前工作目錄或本機來源樹。
  同時部署 Tcl、utf8proc、libtommath 與 zlib 授權文件，不納入 Tk。

Windows 整合測試包含實際搬移執行檔與 Tcl 資源、Unicode 路徑／參數、無終端部署、
腳本錯誤與取消、Ctrl+C 完整關閉。未執行完整 Tcl 上游測試套件。
Open-VINC 的測試結果見 [Shell 文件](../../src/vinc/shell/README.md#驗證)。

### CMake 部署 helper

`vinc_deploy_tcl(target)` 接受 executable target，預設把資源放到
`$<TARGET_FILE_DIR:target>/lib/tcl9.1`，不使用呼叫處的 binary directory。
即使父專案在 add_subdirectory 之後設定 `RUNTIME_OUTPUT_DIRECTORY`，也跟隨最終輸出位置。
可用 `vinc_deploy_tcl(target destination)` 指定根目錄；相對的字面路徑以呼叫處的
binary directory 為基準。此覆寫只控制複製位置，runtime 仍從執行檔旁尋找資源。

部署 target 每次建置都執行，資源被刪除時不需重新連結即可補回；
`deploy.cmake` 在目的地 lib 目錄取得建置期間的檔案鎖，最多等待 60 秒，
避免不同 target 的目的地運算式解析成同一目錄後同時寫入。
鎖檔可以保留在部署目錄，不是 runtime 功能。複製 Tcl、utf8proc、libtommath、zlib 授權。
中央預設服務與搬移／資源補回的 CTest 為 `sdk.external_consumer`，結果見 SDK README。

上游 `win/README` 記載 Tcl 9.1 的 Windows 最低目標為 Windows 10，並提供 Visual C++／NMAKE
建置路徑。這是選定相依套件的限制，Open-VINC 的完整最低 OS／工具鏈矩陣仍須在整合時確認。
Linux Ubuntu 22.04 與 macOS 的實際建置、部署與行為尚待驗證。

Ctrl+D／scancel 經公開 `Tcl_CancelEval(..., TCL_CANCEL_UNWIND)` 取消腳本；
Ctrl+C 另外觸發框架完整關閉。標準 Tcl 的阻塞式 `after ms` 由封裝提供可取消等待，
其他 after 形式仍由上游實作處理；原生擴充或其他阻塞操作仍可能延遲取消。

## 後續更新

更新時先核對官方正式發布與鏡像 tag，記錄新的完整 commit、發布來源及平台限制；
再執行對應的建置與行為驗證，更新本文件、`revision.txt`、套件總表及主專案的 gitlink。
版本變更時也需核對 CMake 的庫名稱、資源目錄及保留字清單，重新建立 Tcl 建置產物。
來源 tag 方便辨識版本，實際回溯依據為主專案保存的完整 commit。

## 待處理事項

### 增量建置與父專案整合（2026-10-06 CMake 重審）

- **來源與生成建置圖確認，尚未做版本切換實測：** Windows ExternalProject 的 configure
  stamp 只依賴 cfgcmd 與前一步 stamp；`stage.cmake`、`revision.txt` 與固定來源版本沒有
  形成重建相依。現有 Ninja 的 RERUN_CMAKE 亦未列入 revision 檔及 submodule HEAD。
  只更新固定版本或 staging 腳本後沿用舊 build，可能沿用舊 staging／靜態庫；資源部署卻從
  當前 upstream 複製，存在新資源配舊庫的風險。尚未修復；建議讓 pin 與 staging 腳本變化
  明確觸發重新驗證、staging 及重建。版本更新時先沿用重新建立產物的既有維護要求。
- **已確認的路徑占用：** Shell 的 CMake 使用 `${CMAKE_BINARY_DIR}/library/tcl`，
  外部 add_subdirectory 整合時會占用父專案的該目錄；父專案也使用該 binary directory
  時可能衝突。尚未實際配置碰撞案例；建議改用 Open-VINC 所屬的 binary subtree，
  保持執行檔旁資源部署契約不變。

### Windows 建置目錄含空白（2026-10-06；已重現，尚未修復）

外部 SDK 專案使用含空白的 build root 時，Tcl `win/rules.vc` 的無引號 inference rules
發生 NMAKE U1087。嘗試以 NMAKE 參數提供相對 ROOT／OUT_DIR／TMP_DIR 後，
上游仍將 mkdir 的路徑展開為無引號絕對路徑並發生 U1077；該嘗試未保留。
不修改固定上游，也未引入短路徑、junction 或外部暫存建置目錄。

目前 Windows 建置目錄須避免空白；執行檔的輸出／部署目錄含空白是獨立情境，
由 SDK 外部測試驗證。後續須先討論建置位置策略及清理契約，再實作完整的建置路徑支援。
