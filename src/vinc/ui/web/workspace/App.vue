<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from "vue";
import type { Catalog, PanelDefinition, PluginInfo } from "../sdk";
import PanelHost from "./PanelHost.vue";
import { isTrusted, limits, trust } from "./runtime";

interface Tab {
    id: string;
    plugin: PluginInfo;
    panel: PanelDefinition;
    title: string;
}
const catalog = ref<Catalog>();
const tabs = ref<Tab[]>([]);
const active = ref("");
const status = ref("Connecting");
const notice = ref("");
const pending = ref<{ plugin: PluginInfo; panel: PanelDefinition }>();
const dialog = ref<HTMLDialogElement>();
let nextId = 1;
let timer: ReturnType<typeof setTimeout> | undefined;
let request: AbortController | undefined;
let disposed = false;

async function refresh(): Promise<void> {
    request = new AbortController();
    const timeout = setTimeout(() => request?.abort(), limits.requestTimeoutMs);
    try {
        const response = await fetch("/api/ui", { signal: request.signal, cache: "no-store" });
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        const value: Catalog = await response.json();
        if (value.apiVersion !== 1 || !Array.isArray(value.plugins) || !Array.isArray(value.diagnostics)) {
            throw new Error("Unsupported workspace API");
        }
        if (disposed) {
            return;
        }
        if (catalog.value && JSON.stringify(value) !== JSON.stringify(catalog.value)) {
            status.value = "Refresh required";
            notice.value = "The UI service changed. Refresh this page to load its new plugin catalog.";
        } else {
            catalog.value = value;
            status.value = "Connected";
        }
    } catch {
        if (!disposed) {
            status.value = "Disconnected";
        }
    } finally {
        clearTimeout(timeout);
        if (!disposed) {
            timer = setTimeout(refresh, limits.connectionPollMs);
        }
    }
}

function openPanel(plugin: PluginInfo, panel: PanelDefinition): void {
    if (status.value === "Refresh required") {
        notice.value = "Refresh this page before opening another panel.";
        return;
    }
    const existing = tabs.value.find((tab) => tab.plugin.id === plugin.id && tab.panel.id === panel.id);
    if (!panel.multiple && existing) {
        active.value = existing.id;
        return;
    }
    if (tabs.value.length >= limits.panels) {
        notice.value = `Close a tab first. This workspace allows ${limits.panels} open panels.`;
        return;
    }
    if (!isTrusted(plugin)) {
        pending.value = { plugin, panel };
        dialog.value?.showModal();
        return;
    }
    const id = `panel-${nextId++}`;
    tabs.value.push({ id, plugin, panel, title: panel.title });
    active.value = id;
}

function approve(): void {
    const selected = pending.value;
    dialog.value?.close();
    pending.value = undefined;
    if (selected) {
        trust(selected.plugin);
        openPanel(selected.plugin, selected.panel);
    }
}

function cancel(): void {
    dialog.value?.close();
    pending.value = undefined;
}

function closeTab(id: string): void {
    const index = tabs.value.findIndex((tab) => tab.id === id);
    tabs.value = tabs.value.filter((tab) => tab.id !== id);
    if (active.value === id) {
        active.value = tabs.value[Math.max(0, index - 1)]?.id ?? "";
    }
}

onMounted(refresh);
onBeforeUnmount(() => {
    disposed = true;
    clearTimeout(timer);
    request?.abort();
});
</script>

<template>
    <div class="workspace">
        <header class="topbar">
            <a
                href="/"
                class="brand"
                aria-label="Open VINC workspace"
                ><span class="brand-mark">V</span> OPEN<span class="brand-light">VINC</span></a
            >
            <span class="topbar-label">WORKSPACE</span>
            <div
                class="connection"
                :class="{ online: status === 'Connected' }"
                role="status"
            >
                <span></span>{{ status }}
            </div>
        </header>
        <aside class="sidebar">
            <div class="sidebar-heading">
                <span class="eyebrow">YOUR WORKSPACE</span><span class="version">01</span>
            </div>
            <h1>Panels</h1>
            <p class="muted sidebar-intro">Tools for your next idea.</p>
            <div
                v-if="!catalog"
                class="empty-catalog"
            >
                Waiting for the UI service…<br />The workspace will reconnect automatically.
            </div>
            <section
                v-for="plugin in catalog?.plugins"
                :key="plugin.id"
                class="plugin-group"
            >
                <div class="plugin-heading">
                    <span>{{ plugin.name }}</span
                    ><span
                        class="badge"
                        :class="{ official: plugin.official }"
                        >{{ plugin.official ? "OFFICIAL" : "PLUGIN" }}</span
                    >
                </div>
                <button
                    v-for="panel in plugin.panels"
                    :key="panel.id"
                    class="open-panel"
                    @click="openPanel(plugin, panel)"
                >
                    <span class="panel-icon">▧</span><span>{{ panel.title }}</span
                    ><span class="plus">+</span>
                </button>
                <div class="plugin-version">{{ plugin.id }} · v{{ plugin.version }}</div>
            </section>
            <details
                v-if="catalog?.diagnostics.length"
                class="diagnostics"
            >
                <summary>Package diagnostics ({{ catalog.diagnostics.length }})</summary>
                <p
                    v-for="message in catalog.diagnostics"
                    :key="message"
                >
                    {{ message }}
                </p>
            </details>
            <div class="sidebar-footer">
                <span class="eyebrow">BUILT TO EXTEND</span>
                <p>One workspace.<br />Your own tools.</p>
                <span class="muted">Plugin API v1</span>
            </div>
        </aside>
        <main class="main">
            <div
                class="tabbar"
                role="tablist"
                aria-label="Open panels"
            >
                <span
                    v-if="!tabs.length"
                    class="tab-placeholder"
                    >Workspace overview</span
                >
                <div
                    v-for="tab in tabs"
                    :key="tab.id"
                    class="tab"
                    :class="{ selected: active === tab.id }"
                >
                    <button
                        role="tab"
                        :aria-selected="active === tab.id"
                        :aria-controls="tab.id"
                        @click="active = tab.id"
                    >
                        {{ tab.title }}
                    </button>
                    <button
                        class="close-tab"
                        :aria-label="`Close ${tab.title}`"
                        @click="closeTab(tab.id)"
                    >
                        ×
                    </button>
                </div>
                <span class="tab-count">{{ tabs.length }} / {{ limits.panels }}</span>
            </div>
            <div
                v-if="notice"
                class="notice"
                role="status"
            >
                <span>{{ notice }}</span
                ><button
                    aria-label="Dismiss notification"
                    @click="notice = ''"
                >
                    ×
                </button>
            </div>
            <section
                v-if="!tabs.length"
                class="welcome"
            >
                <div class="welcome-grid"></div>
                <div class="welcome-content">
                    <span class="eyebrow accent">OPEN-VINC / UI</span>
                    <h2>Your framework.<br /><span>Your workspace.</span></h2>
                    <p>
                        Open a panel and make room for what comes next.<br />Every tab is an independent view,
                        ready for your tools.
                    </p>
                    <button
                        v-if="catalog?.plugins.some((plugin) => plugin.official)"
                        class="primary"
                        @click="
                            openPanel(
                                catalog.plugins.find((plugin) => plugin.official)!,
                                catalog.plugins.find((plugin) => plugin.official)!.panels[0]!,
                            )
                        "
                    >
                        Open example panel <span>↗</span>
                    </button>
                    <div class="welcome-notes">
                        <div>
                            <span>01</span><strong>Open a panel</strong>
                            <p>Choose a tool in the sidebar.</p>
                        </div>
                        <div>
                            <span>02</span><strong>Make it yours</strong>
                            <p>Open multiple views side by side in tabs.</p>
                        </div>
                        <div>
                            <span>03</span><strong>Build a plugin</strong>
                            <p>Bring your own interface to the workspace.</p>
                        </div>
                    </div>
                </div>
            </section>
            <div
                v-for="tab in tabs"
                :id="tab.id"
                :key="tab.id"
                v-show="active === tab.id"
                role="tabpanel"
                class="tab-content"
            >
                <PanelHost
                    :plugin="tab.plugin"
                    :panel-type="tab.panel.id"
                    :instance-id="tab.id"
                    :active="active === tab.id"
                    @title="tab.title = $event"
                    @notice="notice = $event"
                />
            </div>
            <footer class="statusbar">
                <span
                    >Open-VINC UI ·
                    <a
                        href="/third-party.txt"
                        target="_blank"
                        rel="noopener"
                        >Licenses</a
                    ></span
                ><span>Plugin workspace · No management panels installed</span>
            </footer>
        </main>
        <dialog
            ref="dialog"
            class="trust-dialog"
            @cancel="cancel"
        >
            <template v-if="pending"
                ><span class="eyebrow accent">PLUGIN TRUST</span>
                <h2>Trust {{ pending.plugin.name }}?</h2>
                <p>
                    This plugin can execute code in this workspace and access data available to this browser
                    page. Only continue if you trust its author and contents.
                </p>
                <dl>
                    <dt>Package</dt>
                    <dd>{{ pending.plugin.id }} · {{ pending.plugin.version }}</dd>
                    <dt>SHA-256</dt>
                    <dd class="digest">{{ pending.plugin.digest }}</dd>
                </dl>
                <p class="muted">
                    Approval is saved in this browser for this exact package. Changed contents require
                    approval again.
                </p>
                <div class="dialog-actions">
                    <button
                        autofocus
                        @click="cancel"
                    >
                        Cancel</button
                    ><button
                        class="primary"
                        @click="approve"
                    >
                        Trust and open
                    </button>
                </div></template
            >
        </dialog>
    </div>
</template>
