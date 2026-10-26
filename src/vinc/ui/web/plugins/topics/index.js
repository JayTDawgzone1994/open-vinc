import { read } from "./api.js";

// Standalone official plugin: owns its DOM and polling, with no workspace/Vue internals.
// Retry defaults before the first snapshot. Successful samples supply config.hpp policy.
const retryIntervalMs = 1000;
const labels = { green: "Healthy", yellow: "Warning", red: "Problem", gray: "Unknown" };

function cell(row, text) {
    const element = document.createElement("td");
    element.textContent = text;
    row.append(element);
    return element;
}

function duration(value) {
    return `${(Number(value) / 1e6).toFixed(3)} ms`;
}

export function createPanel(panelType, container, context) {
    if (panelType !== "topics") {
        throw new Error("Unknown Topic Monitor panel type");
    }
    const style = document.createElement("link");
    style.rel = "stylesheet";
    style.href = new URL("style.css", import.meta.url).href;
    container.append(style);
    const root = document.createElement("section");
    root.className = "topics-panel";
    root.innerHTML = `
        <header><span class="eyebrow">OFFICIAL · READ ONLY</span><h1>Topic Monitor</h1>
        <p>Publication rate, reader contention and the latest message.</p></header>
        <div class="toolbar"><label>Filter topics <input type="search" placeholder="Topic or message type"></label>
        <output role="status" class="connection">Connecting</output></div>
        <p class="sampling rate-policy">Hz uses publication counters. 0 Hz can mean idle.</p>
        <div class="table-wrap"><table><thead><tr><th>Status</th><th>Topic</th><th>Type</th><th>Hz</th><th>Pub / Sub</th><th>Collisions*</th><th>Lost</th></tr></thead><tbody></tbody></table></div>
        <p class="empty">Waiting for the first diagnostic sample…</p>
        <p class="legend"><span>● Green: no recent issue observed</span><span>● Yellow: contention / loss; recovering</span><span class="red-policy">● Red: prolonged current wait or read error</span><span>● Gray: monitoring unavailable</span></p>
        <p class="sampling">* Collisions cover active publisher instances. Lost counts reader misses, not unique publications. Opening a preview adds a reader.</p>
        <section class="details" hidden><div class="detail-heading"><h2></h2><button type="button" class="pause">Pause preview</button></div>
        <p class="reason"></p><pre class="publishers"></pre>
        <h3>Latest message</h3><p class="preview-status">Waiting for a message</p><pre class="payload"></pre>
        <p class="sampling preview-policy">Bounded preview. Escaped string bytes are not guessed as an encoding. This is a sample, not a message history.</p></section>`;
    container.append(root);

    const body = root.querySelector("tbody");
    const filter = root.querySelector("input");
    const connection = root.querySelector(".connection");
    const details = root.querySelector(".details");
    const previewStatus = root.querySelector(".preview-status");
    const payload = root.querySelector(".payload");
    const pauseButton = root.querySelector(".pause");
    let topics = [];
    let selected = "";
    let paused = false;
    let visible = false;
    let disposed = false;
    let connected = false;
    let sampleTimer;
    let previewTimer;
    let sampleRequest;
    let previewRequest;
    let selectionVersion = 0;
    let sampleVersion = 0;
    let sampleIntervalMs = retryIntervalMs;
    let previewIntervalMs = retryIntervalMs;

    function renderDetails() {
        const topic = topics.find((value) => value.name === selected);
        details.hidden = !selected;
        root.querySelector("h2").textContent = selected;
        if (!topic) {
            root.querySelector(".reason").textContent = "Topic is no longer registered";
            root.querySelector(".publishers").textContent = "";
            return;
        }
        const reason = connected ? topic.reason : "Monitoring unavailable — last sample is stale";
        root.querySelector(".reason").textContent = `${reason} · schema ${topic.schema} · lost ${topic.lost} · oversized ${topic.oversized}`;
        root.querySelector(".publishers").textContent = topic.publishers.map((publisher) =>
            `${publisher.name} · MID ${publisher.mid} · Publisher ${publisher.id}\n` +
            `  ${publisher.hz === null ? "Measuring" : publisher.hz.toFixed(2) + " Hz"} · published ${publisher.published}\n` +
            `  ${publisher.slots} slots (requested ${publisher.requested_slots}) · ${publisher.message_bytes} bytes/message\n` +
            `  collisions ${publisher.collisions} · current ${duration(publisher.current_wait_ns)}\n` +
            `  total wait ${duration(publisher.wait_ns)} · longest ${duration(publisher.longest_wait_ns)}`,
        ).join("\n\n") || "No active publisher";
    }

    function render() {
        body.replaceChildren();
        const query = filter.value.toLowerCase();
        const shown = topics.filter((topic) => `${topic.name} ${topic.type}`.toLowerCase().includes(query));
        for (const topic of shown) {
            const row = document.createElement("tr");
            const status = connected ? topic.status : "gray";
            row.dataset.status = status;
            row.classList.toggle("selected", topic.name === selected);
            const state = cell(row, "");
            const light = document.createElement("span");
            light.className = "light";
            light.textContent = `● ${labels[status] ?? labels.gray}`;
            light.title = connected ? topic.reason : "Monitoring unavailable";
            state.append(light);
            const name = cell(row, "");
            const button = document.createElement("button");
            button.type = "button";
            button.textContent = topic.name;
            button.addEventListener("click", () => select(topic.name));
            name.append(button);
            cell(row, topic.type);
            cell(row, !connected || topic.hz === null ? "—" : topic.hz.toFixed(2));
            cell(row, `${topic.publishers.length} / ${topic.subscribers}`);
            cell(row, topic.publishers.reduce((total, publisher) => total + BigInt(publisher.collisions), 0n).toString());
            cell(row, topic.lost);
            body.append(row);
        }
        const empty = root.querySelector(".empty");
        empty.hidden = shown.length !== 0;
        empty.textContent = "Waiting for diagnostics";
        if (connected) {
            empty.textContent = topics.length ? "No topics match this filter" : "No registered topics";
        }
        renderDetails();
    }

    function select(name) {
        selected = name;
        selectionVersion += 1;
        previewRequest?.abort();
        clearTimeout(previewTimer);
        payload.textContent = "";
        previewStatus.textContent = paused ? "Preview paused" : "Loading latest message…";
        render();
        if (!paused) {
            void refreshPreview();
        }
    }

    async function refresh() {
        if (!visible || disposed) {
            return;
        }
        const version = ++sampleVersion;
        sampleRequest = new AbortController();
        try {
            const data = await read("/api/topics", AbortSignal.any([context.signal, sampleRequest.signal]));
            if (disposed || !visible || version !== sampleVersion) {
                return;
            }
            if (data.apiVersion !== 1 || !data.available || !Array.isArray(data.topics)) {
                throw new Error(data.error || "No diagnostic sample available");
            }
            topics = data.topics;
            if (data.sample_ms > 0 && data.preview_ms > 0) {
                sampleIntervalMs = data.sample_ms;
                previewIntervalMs = data.preview_ms;
            }
            if (data.policy) {
                root.querySelector(".rate-policy").textContent = `Hz: approximately ${data.policy.rate_window_ms / 1000} s rolling window from publication counters. Recovery: ${data.policy.recovery_ms / 1000} s without new issues. 0 Hz can mean idle.`;
                root.querySelector(".red-policy").textContent = `● Red: current wait ≥ ${data.policy.red_wait_ms / 1000} s or read error`;
                root.querySelector(".preview-policy").textContent = `Preview: at most ${1000 / previewIntervalMs} updates/s, ${data.policy.preview_bytes} byte receive limit; up to ${data.policy.preview_elements} elements per array and ${data.policy.preview_text_bytes} text bytes overall. Strings use explicit byte escapes. This is a sample, not message history.`;
            }
            connected = true;
            connection.textContent = `Live · ${sampleIntervalMs / 1000} s samples`;
        } catch (error) {
            if (disposed || !visible || version !== sampleVersion) {
                return;
            }
            connected = false;
            connection.textContent = `Disconnected · ${error.message}`;
        } finally {
            if (!disposed && visible && version === sampleVersion) {
                render();
                sampleTimer = setTimeout(refresh, sampleIntervalMs);
            }
        }
    }

    async function refreshPreview() {
        if (!visible || disposed || paused || !selected) {
            return;
        }
        const version = selectionVersion;
        previewRequest = new AbortController();
        try {
            const data = await read(`/api/topics/preview?topic=${encodeURIComponent(selected)}`,
                AbortSignal.any([context.signal, previewRequest.signal]));
            if (disposed || !visible || paused || version !== selectionVersion) {
                return;
            }
            if (data.apiVersion !== 1) {
                throw new Error("Unsupported preview API version");
            }
            const messages = {
                pending: "Preparing preview…", empty: "No retained message",
                unknown: "Unknown type — not compiled into this UI module",
                too_large: "Message exceeds the UI preview receive limit",
                error: `Read failed: ${data.error || "unavailable"}`,
            };
            if (data.state === "ready") {
                previewStatus.textContent = `${data.publisher} · ${data.utc} · topic count ${data.count}`;
                payload.textContent = data.text;
            } else {
                previewStatus.textContent = messages[data.state] || "Preview unavailable";
                payload.textContent = "";
            }
        } catch (error) {
            if (!disposed && visible && version === selectionVersion) {
                previewStatus.textContent = `Preview unavailable · ${error.message}`;
            }
        } finally {
            if (!disposed && visible && !paused && version === selectionVersion) {
                previewTimer = setTimeout(refreshPreview, previewIntervalMs);
            }
        }
    }

    const onFilter = () => render();
    const onPause = () => {
        paused = !paused;
        selectionVersion += 1;
        previewRequest?.abort();
        clearTimeout(previewTimer);
        pauseButton.textContent = paused ? "Resume preview" : "Pause preview";
        if (paused) {
            previewStatus.textContent += " · Paused (showing last sample)";
        } else {
            void refreshPreview();
        }
    };
    filter.addEventListener("input", onFilter);
    pauseButton.addEventListener("click", onPause);
    const stop = () => {
        sampleVersion += 1;
        selectionVersion += 1;
        clearTimeout(sampleTimer);
        clearTimeout(previewTimer);
        sampleRequest?.abort();
        previewRequest?.abort();
    };
    context.onDispose(stop);
    return {
        setVisible(value) {
            if (visible === value) {
                return;
            }
            visible = value;
            stop();
            if (visible) {
                void refresh();
                void refreshPreview();
            }
        },
        dispose() {
            disposed = true;
            stop();
            filter.removeEventListener("input", onFilter);
            pauseButton.removeEventListener("click", onPause);
            root.remove();
            style.remove();
        },
    };
}
