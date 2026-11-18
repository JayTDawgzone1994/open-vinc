# macOS Recorder／控制面驗收（2026-11-19）

基底 `7f9815563338412807b38cb91b5af0441e6c833f`，由當日 `origin/develop` 同步。
本輪涵蓋 Bus v6、Plugin entry 23、Bag v3、Storage image v2。
Repo 的程式差異限 opt-in 成本工具的 Darwin 採樣，測試負載、Runtime 和 ABI 維持基底提交。
編譯、CTest、SDK 與成本各自保留命令和輸出，歷史 entry-19 證據仍適用原版本。

macOS 26.3.1(a) build 25D771280a，Apple M1 Pro arm64，macOS SDK 26.5，
Apple Clang 21.0.0（clang-2100.1.1.101）、libc++ 210106、CMake 4.4.4、
Ninja 1.13.2.git.kitware.jobserver-pipe-1、CPython 3.12.14、Node 24.22.0。
既有固定 submodule 與前端依賴照原 pin 使用，版本清單在 `environment.log`。
Debug／Release 分開建置，來源及 SDK producer 分別有 343／98 筆 C++23 編譯命令。

## 功能與交付

| 矩陣 | Debug | Release |
| --- | --- | --- |
| 來源相關 CTest | 89/89，234.36 s | 89/89，203.37 s |
| 來源 Python 與跨語言範例 | 7/7，42.44 s | 7/7，39.97 s |
| 拓樸重跑 | 7 個案例 × 3 通過，41.01 s | 7 個案例 × 3 通過，40.39 s |
| 同名 Topic 跨 App 重啟重跑 | 1 個案例 × 3 通過，1.82 s | 1 個案例 × 3 通過，1.73 s |
| C++ consumer | 通過，10.50 s | 通過，9.97 s |
| 搬移 App | 通過，79.66 s | 通過，80.76 s |
| 下游 Python | 通過，162.73 s | 通過，197.19 s |
| 搬移雙 Recorder App | 通過，33.43 s | 通過，34.97 s |
| Publisher 預設專用 App | 通過，21.88 s | 通過，22.87 s |

來源矩陣、來源 Python 與 SDK 腳本分別計數。SDK 每組共 5/5。

- Storage／Ribosome 覆蓋通用映像及 typed/canonical bytes 一致、壞映像、未知 hash、
  typed schema 界線、配置失敗重試、名稱表生成與相依。Context／Bus 驗證映像所有權、
  parallel callback 及停止。Bag 驗證未知型別、正文與巢狀欄位、筆數／bytes 配額、worker
  credit、排空、故障及 v2 格式拒絕。
- 控制接收喚醒覆蓋入睡交界、最多 40 幀的測試負載與跨 Module 公平掃描、
  RPC 滿載後等待、FIFO 與一次入列、blocked Module 停止及確認退出。
  既有生命週期／故障／退出、Bus、RPC 與 Param 首次快取收斂均納入。
- 拓樸負載包含 1×128、16×128、160 個不同 Topic、2,200 ms 派送暫停與 2,000 ms
  測試心跳期限。快 Host 持續收發，慢 Host 恢復後在無新操作下追上。
  真實獨立 Host 的異常退出、141 個訂閱在同一 callback 清除後立即重註冊、
  延後 ACK、斷線、Topic 上限與重新啟動後的持久設定均有維護中的案例。
- 原生 POSIX EMFILE 的重試、中央資料紀錄及隔離由 CTest 實際核對。三次 EMFILE 後 journal 仍為 9，程序退出後為 0。重啟重跑 A 每次回收 9 個舊名稱，B 保留原 9 名稱並持續收發。
  Linux linker 專用的 `osal.shared_memory_rollback` 列為平台不適用。
- 每組 SDK producer 由空目錄建置，`VINC_INSTALL_SDK=ON`、`VINC_BUILD_APPS=OFF`、
  `BUILD_TESTING=OFF`。建置命令及產物確認沒有 App 與先行編譯的 Python binding。
  SDK 安裝搬移後才建立下游 App 與 binding。Recorder 另以外部工作目錄執行
  `HIDE_PRODUCER=ON`，隱藏 producer、consumer、SDK，從無關 cwd 同時啟動搬移後兩個 App。
  每組兩個 App 各 64 筆，typed decoder 逐筆核對正文、序列、publisher ID、時間 metadata，
  並要求 finish、零 loss／drop／oversized，及所有子程序回收。
- 來源 Python 使用基底提交的獨立快照，只在快照 Kconfig 啟用 System Python 與
  C++／Python 範例，重建 binding、Vinc 與型別相關案例。詳見 `python-vinc.conf` 和測試命令。

## Release 成本取樣

### 中央控制接收喚醒

12/12 組配對通過。每種策略及負載各三輪。下表箭號為 polling → notified，延遲為各輪分位數的中位數。

| 啟動／後續各註冊 | 啟動時間 ms | 後續註冊 ms | 啟動 Subscribe 等中央取件 P50／P99 ms | 啟動／後續中央 CPU ms |
| --- | --- | --- | --- | --- |
| 4 | 333.32 → 46.51 | 317.44 → 4.50 | 22.4354／29.9218 → 0.0049／0.0057 | 5.59／6.77 → 1.59／3.42 |
| 128 | 6819.68 → 49.82 | 9991.80 → 48.66 | 26.0463／29.9269 → 0.0043／0.0113 | 161.75／202.85 → 15.83／35.34 |

| 筆數 | 閒置中央 CPU ms／約 10 秒 | 閒置 supervisor passes | 執行緒／FD | 閒置 RSS MiB | 結束前 RSS 峰值 MiB |
| --- | --- | --- | --- | --- | --- |
| 4 | 52.32 → 49.68 | 381 → 382 | 11／20 → 11／20 | 2.69 → 2.67 | 2.78 → 2.80 |
| 128 | 47.42 → 53.30 | 383 → 383 | 11／144 → 11／144 | 4.86 → 4.83 | 7.11 → 7.06 |

範圍為中央程序，CPU 含採樣與同步 trace，RSS 含整個程序。閒置仍有健康補查。

### Bus 拓樸

load／snapshot／replace 各三次。load 的三種負載每輪在 2,200 ms ACK 等待區間的額外 Bus worker 喚醒皆為 0。

| 負載 | 頁數／wire bytes | 中央歷史／準備頁峰值 bytes | Host 暫存峰值 bytes | 收斂 ms，中位數（範圍） |
| --- | --- | --- | --- | --- |
| 1 × 128 | 2／3,884 | 0／2,704 | 2,704 | 0.523（0.489–0.610） |
| 16 × 128 | 6／60,376 | 40,560／40,560 | 40,560 | 2.945（2.351–3.029） |
| 160 個 Topic | 4／26,708 | 10,496／16,252 | 26,256 | 2.112（1.942–2.117） |

整個 fixture 的 RSS 峰值範圍為 load 42,942,464–42,991,616 bytes，snapshot 8,372,224–8,421,376 bytes，replace 8,372,224–8,421,376 bytes。協定及暫存 bytes 來自 fixture 計數，各峰值的時點可能不同。

### Recorder typed／generic

同一份 Release SDK，137 型別 catalog。相同輸入、單建置 job、各三次交錯重編。

| 指標 | typed | generic |
| --- | --- | --- |
| 乾淨外部 App 建置 s | 53.63 | 35.14 |
| 增量訊息註解更新建置 s | 47.39 | 29.97 |
| Recorder TU 重編 s（各三次中位數） | 19.186 | 1.231 |
| Recorder TU 編譯 peak RSS MiB（各三次中位數） | 1269.84 | 220.61 |
| Recorder object KiB（各三次中位數） | 4684.44 | 59.51 |

實際錄製 12/12 次。每次 1,000 筆，兩種尺寸各三對，逐筆 canonical payload 的 SHA-256 配對核對。下表為 typed → generic 的三輪中位數。

| 尺寸 | Recorder CPU ms | 取樣 peak RSS MiB | 完成 records/s | 發布後排空 ms | finish ms |
| --- | --- | --- | --- | --- | --- |
| 1 KiB | 186.24 → 183.63 | 27.20 → 26.42 | 309.38 → 310.19 | 82.15 → 80.67 | 23.72 → 23.41 |
| 64 KiB | 391.90 → 391.34 | 105.08 → 102.30 | 302.37 → 300.93 | 64.88 → 42.98 | 23.04 → 22.88 |

CPU 與 RSS 取 Recorder Host PID。完成速率涵蓋 2 ms paced 發布和 Shell 開銷。

### Parallel pump

48/48 次，合計 245,760 筆、零 loss／collision。各組三次。延遲從 push 前到 callback 完成，以下是每輪指標的中位數。

| Topic | callback | workers | 完成筆數/s | P50／P99 µs | 負載／閒置 CPU ms | 負載後 RSS 峰值 MiB |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 輕 | 0 | 151990 | 11.7／411.7 | 21.88／2.91 | 11.19 |
| 1 | 輕 | 1 | 133467 | 37.8／306.6 | 35.27／4.65 | 11.23 |
| 1 | 輕 | 2 | 113993 | 165.2／1640.5 | 44.77／5.75 | 11.25 |
| 1 | 輕 | 4 | 97399 | 3881.7／4877.0 | 65.15／8.38 | 11.28 |
| 1 | CPU-heavy | 0 | 9371 | 115161.0／206008.0 | 228.82／3.60 | 11.16 |
| 1 | CPU-heavy | 1 | 9420 | 112040.0／203082.0 | 232.63／4.96 | 11.22 |
| 1 | CPU-heavy | 2 | 9100 | 117619.0／210549.0 | 250.41／5.91 | 11.22 |
| 1 | CPU-heavy | 4 | 8634 | 117915.0／221960.0 | 275.61／8.01 | 11.23 |
| 4 | 輕 | 0 | 222472 | 21.5／150.1 | 72.77／3.68 | 24.20 |
| 4 | 輕 | 1 | 227617 | 26.7／515.5 | 97.57／5.49 | 24.19 |
| 4 | 輕 | 2 | 206000 | 2364.7／4161.0 | 131.92／8.99 | 24.30 |
| 4 | 輕 | 4 | 182996 | 229.1／980.9 | 209.99／12.12 | 24.52 |
| 4 | CPU-heavy | 0 | 10341 | 391486.0／761561.0 | 815.16／4.01 | 24.09 |
| 4 | CPU-heavy | 1 | 10272 | 393129.0／763506.0 | 855.79／5.53 | 24.19 |
| 4 | CPU-heavy | 2 | 19639 | 187160.0／372909.0 | 897.32／9.09 | 24.31 |
| 4 | CPU-heavy | 4 | 34990 | 100640.0／185615.0 | 976.25／11.81 | 24.41 |

編譯 RSS 使用 BSD `time -l`，以 bytes 記錄。Recorder 用實際 Host PID 讀 libproc
CPU／RSS，Mach ticks 經 timebase 換算為秒。校準時曾以 ns 直接解讀 CPU ticks，
和同一程序的 CPU 時間比對後改用 timebase。修正後兩者約 100.06／100.07 ms；
RSS 樣本同時保持有效。中央程序採樣用 `getrusage` 取得 CPU 及 bytes 峰值，
以 libproc 取得當下 RSS、執行緒與 FD。開啟、關閉一對 pipe 時 FD 讀數為 3 → 5 → 3。
成本在功能測試與其他建置結束後依序執行，報告限本機及既有工作負載。

## 重跑與證據

```sh
cmake -S src -B BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DVINC_BUILD_APPS=ON -DVINC_BUILD_BUS_BENCHMARK=OFF
cmake --build BUILD --parallel 5
ctest --test-dir BUILD -j1 --no-tests=error --output-on-failure \
  -R '^(control|module|message|message_bus|rpc|param|bag|ribosome)\.|^osal\.(shared_memory|descriptor|shared_memory_rollback)$'
ctest --test-dir BUILD -j1 --no-tests=error --repeat until-fail:3 -V \
  -R '^message_bus\.(control_plane\.topology-|app_restart_isolation$)'
```

SDK、成本與各案例重跑入口見
[Recorder 映像契約](../bag/IMAGE_RECORDING.md#stage-4-validation)、
[控制接收喚醒](../module_system/CONTROL_WAKEUP_VALIDATION.md)、
[Bus 控制面](../message_bus/CONTROL_PLANE_REPAIR.md)與
[Topic 登記回收](../message_bus/TOPIC_REGISTRATION_RECOVERY.md#第四步驗收)。
完整命令與環境、JUnit、LastTest、成本 JSON／phase traces 另外保存在
`open-vinc-macos-recorder-control-20261119/evidence.tar.gz`。

安裝版與來源版 Ribosome 對同一份內建及外部 schema 產出的名稱表逐位元一致。
兩組來源／搬移 Recorder 的 Ninja 生成標頭相依只有 `message_names.hpp`。
採樣校準、依賴稽核與最終程序／連接埠檢查均留有紀錄。

功能及成本矩陣的正式執行無失敗。兩個 Recorder App 同時啟動時，當中一個的
UI 固定埠衝突由原權責隔離，兩個 Recorder 各自完成內容核對及程序回收。
本輪限 Apple Silicon 及同工具鏈、同配置的 SDK。外部應用 491 筆 Ready 驗收、
Windows C1128、跨平台埠隔離及 Recorder 真實負載 profiling 仍保留 TODO。
