import { createApp } from "vue";
import type { PanelContext } from "../../sdk";
import GraphPanel from "./GraphPanel.vue";
import "@vue-flow/core/dist/style.css";
import "@vue-flow/core/dist/theme-default.css";
import "./style.css";

export function createPanel(panelType: string, container: HTMLElement, context: PanelContext) {
    if (panelType !== "graph") {
        throw new Error("Unknown Module Graph panel type");
    }
    const host = document.createElement("div");
    host.style.height = "100%";
    container.append(host);
    const shadow = host.attachShadow({ mode: "open" });
    const stylesheet = document.createElement("link");
    stylesheet.rel = "stylesheet";
    stylesheet.href = new URL(/* @vite-ignore */ "./style.css", import.meta.url).href;
    const root = document.createElement("div");
    root.style.height = "100%";
    shadow.append(stylesheet, root);
    const app = createApp(GraphPanel, { signal: context.signal });
    let disposed = false;
    let mounted = false;
    const dispose = () => {
        if (disposed) {
            return;
        }
        disposed = true;
        if (mounted) {
            app.unmount();
        }
        stylesheet.onload = null;
        stylesheet.onerror = null;
        host.remove();
    };
    // Scope cleanup also covers a failed mount before createPanel can return.
    context.onDispose(dispose);
    // Graph measurement must start after its Shadow DOM stylesheet has loaded.
    // Otherwise initial node/viewport dimensions describe an unstyled document.
    stylesheet.onload = () => {
        if (!disposed) {
            mounted = true;
            app.mount(root);
        }
    };
    stylesheet.onerror = () => {
        if (!disposed) {
            root.textContent = "Module Graph stylesheet could not be loaded. Close this tab and retry.";
        }
    };
    return { setVisible(_visible: boolean) {}, dispose };
}
