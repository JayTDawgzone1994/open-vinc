# UI Plugin API v1

UI plugin 是瀏覽器 ES module，與 C++ 的 Module DLL／ModuleHost 無關。
它取得 DOM container 與有限的 PanelContext；第一版不提供中央操作 API。
信任、路徑、大小與重啟契約見 [README](README.md)。

## 套件

把以下目錄放到 UI root 的 `plugins/your.plugin/`，重啟 `webUi` 並 refresh：

```json
{
    "id": "your.plugin",
    "name": "My tools",
    "version": "1.0.0",
    "apiVersion": 1,
    "entry": "index.js",
    "panels": [
        { "id": "status", "title": "My status", "multiple": true }
    ]
}
```

ID 為 ASCII 字母數字、`_- .`（不含空白、斜線、`..`）；`apiVersion` 必須是整數 1。
未知版本拒絕，無相容猜測。`entry` 是套件內存在的 `.js` 檔。
標題／內容字串可用 UTF-8，檔名限 README 的 ASCII 路徑規則。
不要帶 node_modules；自行 build 成可在瀏覽器直接 import 的 JS，相對 import／CSS／資源與 manifest 一起部署。
不支援 npm bare import、遠端 CDN scripts、TypeScript 原始檔或 `.vue` 原始檔直接執行。
`multiple: false` 再次開啟時切到現有面板；true 每次建立新實例。

`index.js`：

```js
export function createPanel(panelType, container, context) {
    const text = document.createElement("p");
    text.textContent = `Hello from ${context.instanceId}`;
    container.append(text);

    const button = document.createElement("button");
    button.textContent = "Notify";
    container.append(button);
    const notify = () => context.notify("Hello!");
    button.addEventListener("click", notify);
    context.onDispose(() => button.removeEventListener("click", notify));

    return {
        setVisible(visible) {
            // Pause expensive rendering while the tab is hidden.
        },
        dispose() {
            text.remove();
            button.remove();
        },
    };
}
```

`createPanel` 必須同步回傳含 `setVisible`／`dispose` 的物件。
非同步資料初始化由 plugin 啟動並用 `context.signal` 取消；不能回傳 Promise 當 PanelInstance。
工作區在建立完成後呼叫一次 setVisible，其後切換再呼叫；不要假設建立後一定還是目前分頁。

## SDK 邊界

型別來源：`web/sdk/index.ts`。TypeScript plugin 可以 `import type` 它，編譯後不產生 runtime 相依。

| API | 契約 |
| --- | --- |
| instanceId | 當前頁面的唯一 panel 身分，不是 Module MID |
| signal | 關閉或 lifecycle 失敗時 abort；plugin 必須讓自己的非同步工作觀察它 |
| setTitle(text) | 更改此實例分頁名稱，截至 80 字元；銷毀後無作用 |
| notify(text) | 工作區的單一通知列，後訊息取代前訊息，最多 240 字元；不是 Vinc Log |
| onDispose(fn) | 登記資源清理，按反序執行；一筆拋出不阻擋其餘清理 |
| setVisible(bool) | 暫停／恢復繪圖等昂貴工作；隱藏分頁不自動銷毀 |
| dispose() | 一次釋放自己擁有的資源；工作區同時清除 container 子節點 |

勿把 DOM、Vue instance、callback 等保存到 module-wide cache 造成面板關閉後仍被引用。
若使用 Vue，plugin 自己 createApp(container) 並於 dispose 呼叫 app.unmount()。
CSS 放在自己的 container／ShadowRoot 範圍，可用相對 URL 的 stylesheet；SDK 不提供全域 UI 元件庫。

JavaScript import cache 以整頁為生命週期，關閉分頁不等於卸載 JS。
plugin 有完整同 origin JavaScript 能力，Shadow DOM **不是 sandbox**；不可信程式碼不能靠這個介面安全執行。

## Theme tokens / 主題變數

The workspace owns the following `--vinc-ui-*` CSS custom properties. They inherit
through Shadow DOM, including nested roots. Official plugins consume them rather
than force a color scheme. Third-party plugins may opt in; the workspace does not
rewrite their CSS. This adds no PanelContext method or plugin API version change.

工作區維護下列 CSS 變數，透過一般繼承穿過 Shadow DOM（含巢狀 root）。官方 Plugin
共用它們，不固定暗色；第三方可自願採用，框架不覆寫其 CSS。不增加 PanelContext 方法
或變更 Plugin API 版本。

| Properties / 變數（prefix: `--vinc-ui-`） | Purpose / 用途 |
| --- | --- |
| `bg`, `surface`, `raised`, `hover` | Page, panels, controls and hover backgrounds / 背景層次 |
| `text`, `muted`, `border` | Primary/secondary text and separators / 文字與分隔線 |
| `brand`, `brand-hover`, `on-brand` | Brand button backgrounds and foreground / 品牌按鈕 |
| `accent-text`, `focus`, `selected` | Readable accents, focus outlines, selected backgrounds / 強調、焦點、選取 |
| `success`, `warning`, `danger`, `inactive` | Diagnostic foregrounds/borders; accompany with text or icons / 診斷語意 |
| `warning-bg`, `grid`, `shadow`, `backdrop` | Warning surfaces, graph dots, dialog shadow/overlay / 輔助表面 |

```css
:host {
    color-scheme: inherit;
    color: var(--vinc-ui-text, #292929);
}
.panel {
    background: var(--vinc-ui-surface, #faf9f6);
    border: 1px solid var(--vinc-ui-border, #d8d7d2);
}
```

Use fallbacks for standalone previews outside this workspace. Do not redefine the
inherited tokens on `:host` unless deliberately choosing a custom theme: doing so
blocks updates from the workspace. CSS resolves system changes; no JS listener,
polling or panel recreation is needed. Color is not a substitute for status text.

獨立預覽時使用 fallback；不要在 `:host` 重新指定繼承變數，除非刻意自訂主題，否則會
擋住工作區更新。系統切換由 CSS 處理，不需要 JS listener、輪詢或重建面板。
狀態不能只依賴顏色辨認。

## 唯讀 Topic API（2026-10-25）

官方 `vinc.topics` 使用一般同 origin fetch，PanelContext v1 不新增中央管理能力。
先帶 `X-Vinc-UI: 1` 取得 `/api/session` 的 token，再以單一 Bearer Authorization 呼叫
`/api/topics` 或 `/api/topics/preview?topic=<URL-encoded-name>`。使用 AbortSignal 並停止隱藏面板輪詢。
token 只留記憶體；401 可重取，重啟清單改變仍應依工作區提示 refresh。
HTTP 401/403 表示存取未通過，400 為非法 query，503 為快取／預覽名額等不可用；不要當成空清單或綠燈。
回覆 apiVersion 1；preview state 包含 pending、empty、unknown、too_large、error、ready。
ready 的 text 是有界診斷文字，不是可執行 HTML，也不是可重新發布的完整訊息編碼。
政策、信任與限制以 [Topic Monitor 契約](README.md#topic-monitor-契約2026-10-25) 為準。

## Read-only graph API / 唯讀關係圖 API

`GET /api/graph` uses the same capability as `/api/topics`, accepts no query parameters,
and returns `{apiVersion: 1, capturedAt: <Unix milliseconds>, atomicSnapshot: false,
modules: [...], connections: [...]}`. Module records contain decimal-string `mid`,
`name`, `type` and numeric `state` (Queued 0, Starting 1, Running 2, Stopping 3,
RestartWaiting 4, Stopped 5, Failed 6). Connection records contain decimal-string
`mid`, `id`, `schema`, full `topic`, and `role` (`publish` or `subscribe`). A Module
awaiting name registration can have an empty name. Schema identity does not require a
compiled message decoder. `HEAD` validates access without starting collection.

`GET /api/graph` 使用相同 capability，不接受 query 參數；欄位如上。
MID、登記 ID、schema 都是十進位字串；`capturedAt` 是 Unix 毫秒。
名稱登記前 `name` 可以為空；顯示 schema 不需要已編譯的訊息解碼器。
HEAD 只檢查存取，不觸發收集。失敗、容量、429／504 及一致性契約見
[Module Graph](README.md#module-graph-2026-10-27--module-關係圖)。不要把失敗當成空圖。
