<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, watch } from "vue";
import type { PanelInstance, PluginInfo } from "../sdk";
import { loadPlugin, PanelScope } from "./runtime";

const props = defineProps<{ plugin: PluginInfo; panelType: string; instanceId: string; active: boolean }>();
const emit = defineEmits<{ title: [value: string]; notice: [value: string] }>();
const host = ref<HTMLElement>();
const error = ref("");
const loading = ref(true);
let closed = false;
let panel: PanelInstance | undefined;
let container: HTMLElement | undefined;
const scope = new PanelScope(
    props.instanceId,
    (value) => emit("title", value),
    (value) => emit("notice", value),
);

function release(): void {
    for (const failure of scope.dispose()) {
        emit("notice", failure);
    }
    const owned = panel;
    panel = undefined;
    try {
        owned?.dispose();
    } catch {
        emit("notice", "Plugin dispose() failed; registered resources were still released.");
    }
    container?.replaceChildren();
}

function visibility(): void {
    try {
        panel?.setVisible(props.active);
    } catch {
        error.value = "This panel failed while changing visibility.";
        release();
    }
}

onMounted(async () => {
    try {
        const plugin = await loadPlugin(props.plugin);
        if (closed || !host.value) {
            return;
        }
        // Shadow DOM contains ordinary styles, not plugin authority or execution.
        const shadow = host.value.attachShadow({ mode: "open" });
        container = document.createElement("div");
        shadow.append(container);
        const created = plugin.createPanel(props.panelType, container, scope.context);
        if (!created || typeof created.dispose !== "function" || typeof created.setVisible !== "function") {
            if (created && typeof created.dispose === "function") {
                panel = created;
            }
            throw new Error("Invalid panel lifecycle interface.");
        }
        panel = created;
        visibility();
    } catch (failure) {
        if (!closed) {
            error.value = failure instanceof Error ? failure.message : "Panel could not be opened.";
            release();
        }
    } finally {
        loading.value = false;
    }
});
watch(() => props.active, visibility);
onBeforeUnmount(() => {
    closed = true;
    release();
});
</script>

<template>
    <section class="panel-host">
        <div
            v-if="loading"
            class="panel-message"
        >
            Loading panel…
        </div>
        <div
            v-if="error"
            role="alert"
            class="panel-error"
        >
            <span class="eyebrow">PANEL ERROR</span>
            <h2>This panel could not continue.</h2>
            <p>{{ error }}</p>
            <p>Other panels remain available. Close this tab to release its resources.</p>
        </div>
        <div
            ref="host"
            v-show="!error"
            class="plugin-surface"
        ></div>
    </section>
</template>
