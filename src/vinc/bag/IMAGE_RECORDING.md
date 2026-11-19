# Generic image recording / 通用映像錄製契約

Recorder records valid message images without compiling a subscriber for every C++
message class. It remains a supervised System Module using the inherited Context.

Recorder 不必認識具體 C++ 型別就能錄製合法正文；它仍使用一般 Context 與既有程序隔離。
操作與 SQLite 格式見 [Bag README](README.md)。

## Data flow and ownership / 資料流與所有權

```text
Publisher RingBuffer → Bus local copy → Storage validation
                    → Context callback → bounded Recorder queue
                    → disk worker → canonical image → SQLite
```

- Bus releases Reader protection immediately after copying. Validation, callbacks,
  canonicalization and disk I/O never retain the shared-memory read lease.
- Each delivery owns an immutable local image plus framework metadata. Moving it
  into the queue does not copy the payload; later receives or Publisher exit cannot
  overwrite that image. Canonicalization may still allocate and copy.
- Storage owns image validation and canonical rendering. Bus and Context reuse
  their existing registration, source selection, cursors, notifications and shutdown.
  Recorder does not access RingBuffer internals.
- The image subscription is designated framework `detail` integration, not a new
  public Context/Python API or a security sandbox.

Bus 完成有界複製就解除共享讀取保護。交付物獨立持有正文與 metadata，可 move 進 queue；
下一筆訊息不會覆寫它。Recorder 不另建 Context、不直接讀 RingBuffer，也不承諾端到端零拷貝。

## Validation and unknown types / 驗證與未知型別

Generic validation checks format version, sizes, offsets, node ownership/reachability,
exactly one record root and the subscription's nonzero schema hash. It does **not**
validate concrete C++ field definitions or authenticate the Publisher.

合法結構但不符合具體欄位 schema 的映像仍可能被錄下；日後 typed 解碼端須拒絕不符內容。
一般 `subscribe<T>()` 的 schema 檢查維持不變。未知名稱仍錄正文，顯示 `unknown type`。
這不提供 Player、動態 schema 解譯或未知型別播放。

Ribosome generates `message_names.hpp`, containing only hashes/names and standard
library types. Recorder must not include the concrete-class catalog or `visit_types()`.

## Scheduling and capacity / 排程與容量

- Recorder uses Manual `pumpOnce()` plus `waitForWork()` and its existing disk worker.
  It does not opt into Topic workers automatically.
- Preserve NewOnly/batch boundaries, Publisher ordering, loss statistics, source
  retirement checks and acknowledged close. A source without data must not stall
  other work.
- Invalid external images and resource exhaustion return Result errors, not contract termination.
- Check the message-size limit before payload allocation. The queue has both byte
  and record limits and uses DropNewest. Callback admission does not wait for disk I/O.
- Charge the retained payload allocation until the worker commits and releases it,
  including a batch already removed from the queue. Canonicalization runs outside
  the queue mutex.
- Queue bytes are not a process RSS limit: validator metadata, temporary output,
  SQLite and OS caches are additional. Keep `received`, `written`, `bus_lost`,
  `queue_dropped` and oversized counts distinct.

容量不足可丟棄新錄製資料，不能宣稱保證不漏或保證跟上發布 Hz。磁碟／錄製失敗沿用
停止該次錄製、保留 incomplete 與錯誤原因的政策；pump 接收故障依 Module 故障政策處理，
不因 Recorder 失敗終止中央。完整容量及收尾規則見 [Bag](README.md)。

## Metadata and compatibility / Metadata 與相容性

Preserve Topic/hash, Publisher name and Publisher ID, Topic count, original UTC and
monotonic publication times; Recorder additionally stores receive monotonic time.
Publisher ID is not MID, and timestamps are not resampled to replace originals.

Bag format **3** uses `type_known` for catalog knowledge, independent of whether
payloads can be recorded. Bag v2 is explicitly rejected. Storage image v2, Bus v6
and bootstrap v16 are unchanged by this work. Context layout requires **Plugin
entry 23**; rebuild matching Host, Plugin, Python binding and SDK consumers.

## Validation / 驗證

- [Windows/WSL source, clean SDK, relocation and A/B evidence](IMAGE_RECORDING_VALIDATION.md).
- [macOS source/Python, clean SDK, two-App relocation and costs](../sdk/MACOS_RECORDER_CONTROL_VALIDATION.md).
- [Cost reproduction](tests/image_cost/README.md); [remaining issues](README.md#驗證與待處理事項).

三平台都有原生或 WSL 的所列驗收證據，不等於所有平台版本或真實應用皆已驗證。
編譯成本下降不能外推為每種 runtime 負載都更省；Windows 小訊息 CPU 仍需後續 profiling。
