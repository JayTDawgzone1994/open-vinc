# Topic registration failure recovery / Topic 註冊失敗回收契約

**狀態（2026-11-12）**　Windows 與 WSL 已驗證兩個 App 同時收發同名 Topic，以及其中一個中央崩潰後的重啟隔離。macOS 原生驗收待執行。

When exclusive creation of a Topic counter fails, the central process must retire its journal reservation as soon as it knows that this attempt left no owned native object to clean up. An uncertain rollback must remain recoverable after a crash.

## 已重現的問題

中央先記下要建立的資源名稱，再交給 OSAL 建立共享計數器。即使 Topic 只有 Subscriber，也會經過這條路徑。這個順序讓中央在建立途中崩潰後，同名 App 下次啟動仍能追查原資源。見 [BusManager 註冊實作](src/bus_manager.cpp)、[NamedCounter](src/ring_buffer.cpp) 與 [Instance 復原契約](../module_system/INSTANCE.md)。

在版本 `820d4ba`，WSL2 Ubuntu 24.04.5 的中央耗盡 FD 時，建立共享記憶體回報 `EMFILE`。測試程序的 FD 軟上限為 384 時，持有 350 個純訂閱 Topic 後可重現；沿用環境的 10,240 時，持有 10,206 個後同樣重現。兩組另有一個 Topic 持續驗證跨程序收發。釋放一個名額後可註冊新名稱，但三次失敗使登記數在釋放全部壓力訂閱後由基線 11 增至 14。正常退出後仍有三筆登記，`/dev/shm` 中均無對應物件；同名 App 重啟才清掉它們。

Windows Release（MSVC 19.51）的壓力測試先碰到中央 16,384 筆資源登記上限，正常退出後登記歸零。這是登記階段的對照；Windows 原生建立失敗與 macOS FD 耗盡仍須各自驗收。

## 回收契約

| 建立或回滾結果 | 中央的責任 |
| --- | --- |
| 預留名稱本身失敗 | 回傳登記錯誤；沒有取得的預留不可退役。 |
| 已預留名稱，OSAL 確認本次未建立新物件，包括獨占建立遇到同名物件 | 立即撤銷自己的登記。既有同名物件的內容、名稱與所有權均須保留。 |
| 本次建立了物件，後續初始化失敗，已確認自有名稱清理完成 | 撤銷登記一次，回報最初的初始化錯誤。 |
| 原生清理結果不確定，或移除中央登記本身失敗 | 保留復原依據與清理診斷；對呼叫端保留最初故障、operation 與 native 原因。不得宣告回收成功。 |
| 計數器成功交付 | 由既有計數器所有者負責日後清理。 |

OSAL 負責說明**本次建立有沒有留下待清理的自有物件**。Bus 負責依這個結果處理預留；Instance 負責增減持久登記。Bus 不得由單一錯誤碼推斷所有權。尤其 POSIX 在 `shm_open()` 成功後，搬移 FD 或調整大小仍可能失敗，此時要核對自有名稱的回滾結果。Windows 遇到同名 mapping 時須關閉本次取得的 handle，並保留原物件。見 [POSIX 後端](../osal/src/posix/shared_memory.cpp)、[Windows 後端](../osal/src/windows/shared_memory.cpp)。

## 開工順序

1. **先留下會失敗的回歸測試。** 在隔離的測試子程序降低 FD 額度，以真實中央、OSAL 與跨程序收發重現。基線分別取在施壓前及第一次被拒前。對不同新名稱重試，每次失敗都比對當下的登記數，並核對錯誤鏈最底層為原生 `EMFILE`。為等待設期限，清理測試子程序，保留舊行為的失敗輸出。
2. **補齊 OSAL 的建立結果。** 由知道原生建立階段的後端提供框架內部所需的結果，保持既有公開 `SharedMemory::create()` 與 `Result` 用法。檢查 POSIX 回滾時的 `shm_unlink()` 結果；覆蓋未建立、已建立且回滾完成、回滾不確定及同名物件。測試以受控故障區分難以穩定重現的分支。
3. **接上 Topic 計數器的預留撤銷。** 讓建立階段與成功後的計數器所有者各自只清理一次。確認無自有物件待清理時，只撤銷登記；沿用 [ResourceNames](../common/include/vinc/detail/resource_names.hpp) 與 [Instance](../module_system/src/host/instance.cpp) 的退役機制，同步更新允許撤銷未取得所有權之預留的契約。現有 Bus 的 `unlink()` helper 會操作原生名稱，不可拿來當成單純撤銷登記。驗證清理錯誤可診斷、原始錯誤可追溯。
4. **完成生命週期與平台驗收。** WSL 重跑真實 FD 耗盡；Windows 跑共用政策、原生同名案例與中央登記額滿對照；macOS 分部用同一提交重跑 POSIX 案例。列出各平台的提交、組態、實際結果及待驗項目。受控注入與原生重現分開記錄。

## 第一步驗證紀錄

POSIX CTest `message_bus.topic_registration_emfile` 在獨立測試程序啟動中央、Publisher Host 與 Subscriber Host。每輪僅在中央將 FD 軟上限暫設為 0，保留既有連線；發出一次新名稱訂閱，驗證拒絕與原 Topic 傳送的新序號，再恢復額度讀取 journal。三次使用不同名稱，最後另以新名稱註冊、釋放並等待中央正常關閉。該測試以修復後的正確結果為通過條件。

- WSL2 Ubuntu 24.04.5 x86-64 Release：連跑三次，皆在登記回收斷言失敗。三次請求最底層原因均為 `OSAL / ResourceExhausted / CreateSharedMemory / LinuxErrno 24 (EMFILE)`。每輪記錄皆為 `9 → 10 → 11 → 12`，關閉中央後剩 3 筆。收發及解除 FD 壓力後重新登記均通過。這些數字是本次觀察值；斷言比對同一次執行的基線。
- WSL 與 Windows x64 Release 的 `message_bus.module`、`message_bus.defaults_process` 各 2/2 通過。新測試僅在 POSIX 註冊；macOS 原生重現及修復後三平台驗收待執行。

重跑方式（測試已啟用的 POSIX 建置目錄）：

```sh
cmake --build "$build_dir" --config Release --target message_bus_topic_registration_tests
ctest --test-dir "$build_dir" -C Release -R '^message_bus\.topic_registration_emfile$' --output-on-failure
```

## 第二步驗證紀錄

測試基於 `cbf4efa` 加上本輪工作樹。內部介面與回傳語意見 [OSAL Shared Memory](../osal/SHARED_MEMORY.md#名稱清理與程序退出)。

- Windows x64、MSVC 19.51、Release：`osal.shared_memory` 通過。排他建立遇到同名 mapping 後，重新 open 仍讀得到原擁有者寫入的資料。另用同名 Win32 event 讓 `CreateFileMappingW` 真實回傳 `ERROR_INVALID_HANDLE`，確認沒有自有名稱待清理，原 event 仍可使用，關閉後能用相同名稱建立 mapping。
- WSL Ubuntu 24.04、GCC 13.3、Release：`osal.shared_memory`、`osal.descriptor`、`osal.shared_memory_rollback` 各連跑三次通過。真實 FD 額滿在建立前回報 `EMFILE`；搬移 FD 失敗後能清理再建。限制檔案大小讓核心在建立名稱後的 `ftruncate` 回報 `EFBIG`，測試分別驗證 unlink 成功、名稱已移除、以及受控拒絕 unlink。拒絕時主因仍為 `EFBIG`，清理診斷為 `EACCES`，名稱可另行開啟及回收。unlink 拒絕僅注入測試執行檔。
- macOS 原生 `osal.shared_memory` 與 `osal.descriptor` 待分部驗收。Linux 的 unlink 注入測試限定 Linux 連結器。第一步的中央登記回歸需等第三步接線後再驗。

## 驗收判準

### 第三步實作與定向驗證

`NamedCounter::create` 收到 OSAL 建立失敗後，只在 OSAL 確認無自有名稱待清理時退役預留。
成功取得原生名稱後若 map／初始化失敗，改由計數器清理一次。建立成功時由既有計數器持有者管理名稱。
呼叫端取得原始註冊失敗；若回滾或登記退役也失敗，中央內部的 `BusManager::last_counter_cleanup_failure()`
保留最近一筆資源名稱、主要錯誤及獨立清理錯誤，供中央存活期間查詢。持久 journal 保留名稱。

- Windows x64 MSVC 19.51 Release、WSL Ubuntu 24.04 x86-64 GCC 13 Release：兩邊的
  `message_bus.counter_registration`、`cleanup`、`ring_buffer`、`module`、`defaults_process` 各 5/5。
  測試以真實控制通道送出訂閱，驗證預留失敗不退役、撞名只撤銷預留、原物件可重新開啟及繼續計數、
  模擬 journal 退役失敗時仍可診斷，以及成功交付後只由計數器退役一次。
- Linux 另在測試執行檔內注入調整大小、unlink 與映射故障。unlink 被拒時，
  訂閱回傳原本的建立錯誤 `ENOSPC`，中央保留 `EACCES` 清理診斷及預留；實際殘留名稱仍可開啟回收。
- 獨立的 WSL 真實 `message_bus.topic_registration_emfile` 連跑三次通過。各次三個新訂閱皆收到
  `CreateSharedMemory / EMFILE`，每次登記數均為 9 → 9，原 Topic 傳遞新序號成功。解除 FD 壓力後
  新名稱可註冊，中央正常退出後登記為 0。基線為此次觀察值。
- 第三步後的 WSL 探針曾用同帳號直接呼叫原生 API，刻意搶占框架已預留的完整名稱。已確認這超出一般 App 的使用契約。名稱保留及重啟清理政策由 [Instance](../module_system/INSTANCE.md#建立前記錄啟動時清理) 定義。

### 整體驗收條件

- 在可確認無自有物件遺留的失敗路徑，重試不增加登記，原 Topic 仍能傳遞並讀回新的已知序號。額滿時釋放一個純訂閱，再用兩個新名稱註冊，恰有一個成功。
- 釋放全部壓力訂閱後，**同一中央仍運行時**，登記數就回到施壓前基線；正常退出後，該次實驗的資源登記歸零。
- 刻意製造同名時，原所有者仍能開啟並使用其物件。可確認的回滾撤銷一次；結果不確定時，保有復原紀錄與清理故障診斷。
- 在預留名稱後、完成交付前中斷中央，同名 App 仍能依既有契約復原。兩個正常 App 同時使用相同 Topic 時，重啟 A 只清理 A 的舊登記；B 的原生名稱仍可開啟，原來的 Publisher 與 Subscriber 仍可交換新資料。

### 第四步驗收

`message_bus.app_restart_isolation` 以兩個獨立的測試中央執行正式 ModuleManager；每個中央各有一個 Publisher Host 與一個 Subscriber Host，Module 名稱與 Topic `recovery/shared` 完全相同。驅動程式只強制終止 A 中央，隨即用原 App 名稱重新建立它。測試直接探查每筆登記對應的原生名稱，也各自讀回消息序號：

- A 重啟後第一次讀取回報沒有已發布資料，再發布 102 成功。A 舊 epoch 的 owner 與資源登記清空，舊名稱均回報 OSAL `NotFound`。POSIX 還先驗證在重啟前至少一個舊共享名稱確實存在。Windows 允許舊 Host 釋放最後一份 handle 後名稱才消失。
- B 從序號 201 開始，在 A 崩潰前、停機時、重啟期間與替代中央正常關閉後，使用同一組端點發送並讀回每個新序號。重啟期間持續收發至 A 回報就緒，所以總筆數依啟動耗時而異。逐筆核對 B 原有名稱仍能開啟，可偵測「既有 mapping 仍能收發，但名稱已遭 unlink」的錯誤。兩個中央正常關閉後各自的資源登記歸零。
- `module.instance` 另外覆蓋「已預留、尚未建立」時崩潰，以及外部仍持有舊 mapping 的情況。

測試基準為 `cbf4efa` 加上第一至第四步工作樹，x86-64 Release。兩邊各輪實際觀察到 A、B 各有 9 個原生名稱；斷言依該次實際清單驗證。

| 平台 | A/B 重啟回歸 | 其他定向回歸 |
| --- | --- | --- |
| Windows 11 build 26200，MSVC 19.51.36257 | 連跑 3 次通過；B 分別讀回到 209、210、209 | 7/7，包括 Win32 共享名稱衝突、Bus 計數器回收及 Instance 重啟 |
| WSL2 Ubuntu 24.04，GCC 13.3 | 連跑 3 次通過；B 各讀回到 209 | 10/10，另含真實 FD 耗盡、descriptor 及受控 rollback |

WSL 額滿對照額外保留一個正常收發的 Topic。將實驗程序的 FD soft limit 設為 384 時，另持有 350 個純訂閱 Topic 便收到原生 `EMFILE`。重試後資源登記維持 361；釋放一個訂閱，再嘗試兩個新名稱，恰有一個成功。釋放全部壓力訂閱後同一中央回到基線 11，正常退出後為 0。

沿用 WSL 原環境的 10,240 FD 軟上限再測，持有 10,206 個純訂閱後收到 `EMFILE`。再次註冊被拒前後均為 10,217 筆登記；同樣驗證釋放一筆後恰能補一筆、既有收發成功、關閉後登記為 0。上述數字為各次環境的實測值。兩平台的 A/B 最終三輪另核對各 27 個中央／Host PID，測試結束後均已退出。

Windows 用同一工作樹重新連結壓力探針，持有 16,373 個純訂閱後觸及中央 16,384 筆登記上限。三次被拒的底層錯誤均為 `ModuleSystem / CapacityExceeded / RecordResource`。釋放一個訂閱後兩個新名稱恰有一個成功；釋放全部後只剩收發測試用的 Topic，仍可傳遞序號 404，關閉後資源登記為 0。

**macOS 交接。** 在同一工作樹的原生 Release 測試建置中，建置以下 targets，再執行 CTest。記錄 macOS、CPU、編譯器、提交與結果。Linux 專用的 linker 注入案例不適用。

```sh
cmake --build "$build_dir" --config Release --target message_bus_recovery_app message_bus_topic_registration_tests message_bus_cleanup_tests module_instance_tests osal_shared_memory_tests osal_descriptor_tests
ctest --test-dir "$build_dir" -C Release --no-tests=error --repeat until-fail:3 -R '^message_bus\.app_restart_isolation$' -V
ctest --test-dir "$build_dir" -C Release --no-tests=error --output-on-failure -R '^(message_bus\.(topic_registration_emfile|counter_registration|cleanup)|module\.instance|osal\.(shared_memory|descriptor))$'
```

中央的 Topic 數量上限已接入 `vinc.message_bus_max_topics`，使用方式見 [Message Bus](README.md)。上面的壓力數據保留當時的測試組態。映射後提早釋放 FD 的優化另行評估。
