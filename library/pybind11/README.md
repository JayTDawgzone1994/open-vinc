# pybind11

- 上游：[pybind/pybind11](https://github.com/pybind/pybind11)。
- 固定版本：**v3.1.0**，commit `97bf890db679505a14dfe547a5e77bb2bd05dc90`。
- `upstream/` 為未修改的 Git submodule；主專案 gitlink 固定版本，`revision.txt` 記錄相同 commit。
- 授權：BSD-3-Clause，完整保留 `upstream/LICENSE`。散布原始碼或二進位時須保留其要求的版權、條件及免責聲明。
- 取得方式：在專案根目錄執行 `git submodule update --init --recursive`。

## 用途與現況

用於 Python Module 的 C++ SDK 綁定。套件本身不包含 Python 直譯器；使用系統 Python，並允許使用者指定其他 Python 路徑。

`VINC_BUILD_PYTHON=ON` 啟用 Python 綁定：生命週期、Shell hooks、Context pump、Message Bus／訊息容器、Param、Log
與 Result 到 Python exception 的轉換。完整 API 與驗證狀態見 [Python Module](../../src/vinc/module_system/python/README.md)。
預設 OFF；完整功能範圍、建置及驗證見 [Python Module](../../src/vinc/module_system/python/README.md)。

上游 v3.1.0 最低支援 Python 3.9；這不是 Open-VINC 已驗證的版本範圍。實際 CPython 版本、平台、架構與建置組態的支援矩陣須由後續整合及測試確認。

## 建置整合注意事項

初步閱讀上游 CMake：支援 `add_subdirectory` 與 target 介面；作為子專案時，測試與安裝選項預設關閉，但仍有 Python 探測及 cache 設定。不能據此宣稱完全不影響父專案設定。

本目錄 CMake wrapper 在區域 scope 關閉上游測試／安裝，使用 FindPython 與 target 相依。
Python 相依只由綁定 target 使用，不讓中央與一般 C++ Module 因此必須安裝 Python。
不修改上游來源，不自行建立或安裝 Python 環境。部署同時複製上游 LICENSE。

## 驗證

2026-10-12：核對官方 v3.1.0 tag／commit，上游工作目錄保持乾淨。
Windows x64／MSVC／CPython 3.13.12 的 Debug／Release 已建置並通過 Python import 與程序生命週期測試。
Linux／macOS 尚未驗證；完整回歸結果由 Python Module 文件維護。
