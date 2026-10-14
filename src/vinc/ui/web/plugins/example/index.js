// A standalone plugin: imports no workspace internals or Vue runtime.
export function createPanel(panelType, container, context) {
    if (panelType !== "example") {
        throw new Error("Unknown example panel type.");
    }
    const style = document.createElement("link");
    style.rel = "stylesheet";
    style.href = new URL("style.css", import.meta.url).href;
    container.append(style);
    const content = document.createElement("section");
    content.innerHTML = `
        <span class="eyebrow">OFFICIAL EXAMPLE · API 1</span>
        <h1>A place for your tools.</h1>
        <p class="intro">This panel is a plugin. Your next one could be a camera feed,<br>a live chart, or a view into your world.</p>
        <div class="card">
            <div><span class="eyebrow">LOCAL PANEL STATE</span><p class="count">0</p></div>
            <button type="button">Increment</button>
        </div>
        <p class="hint">Open another Example panel. Each tab keeps its own counter.</p>
        <div class="details"><div>Instance<span class="instance"></span></div><div>Visibility<span class="visibility">Visible</span></div><div>Connection<span>Workspace only · no VINC management API</span></div></div>
    `;
    container.append(content);
    content.querySelector(".instance").textContent = context.instanceId;
    let count = 0;
    const button = content.querySelector("button");
    const increment = () => {
        count += 1;
        content.querySelector(".count").textContent = String(count);
        context.setTitle(`Example · ${count}`);
    };
    button.addEventListener("click", increment);
    context.onDispose(() => button.removeEventListener("click", increment));
    return {
        setVisible(visible) {
            content.querySelector(".visibility").textContent = visible ? "Visible" : "Hidden";
        },
        dispose() {
            content.remove();
            style.remove();
        },
    };
}
