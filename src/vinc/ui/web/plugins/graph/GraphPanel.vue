<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef, watch } from "vue";
import { VueFlow, useVueFlow, type Node, type Edge } from "@vue-flow/core";
import dagre from "@dagrejs/dagre";
import { buildGraph, states, validate, notificationState, type NodeData } from "./model";

// Slightly longer than the native 10 s refresh budget to receive its error response.
const fetchTimeoutMs = 12000;
const props = defineProps<{ signal: AbortSignal }>();
const nodes = shallowRef<Node<NodeData>[]>([]);
const edges = shallowRef<Edge[]>([]);
const busy = ref(false);
const loaded = ref(false);
const error = ref("");
const captured = ref("");
const search = ref("");
const selected = ref("");
const flow = useVueFlow();
let controller: AbortController | undefined;
let initialFitDone = false;
watch(
    () => loaded.value && flow.areNodesInitialized.value && flow.viewportHelper.value.viewportInitialized,
    async (ready) => {
        if (ready && !initialFitDone) {
            initialFitDone = true;
            await nextTick();
            await flow.fitView({ padding: 0.2 });
        }
    },
    { flush: "post" },
);
const details = computed(() => nodes.value.find((node) => node.id === selected.value)?.data);
const related = computed(() => {
    const ids = new Set([selected.value]);
    for (const edge of edges.value) {
        if (edge.source === selected.value || edge.target === selected.value) {
            ids.add(edge.source);
            ids.add(edge.target);
        }
    }
    return ids;
});
const displayedNodes = computed(() =>
    nodes.value.map((node) => {
        const matches = `${node.data!.label} ${node.data!.mid ?? ""}`
            .toLowerCase()
            .includes(search.value.toLowerCase());
        const neighbour = !selected.value || related.value.has(node.id);
        return { ...node, style: { opacity: matches && neighbour ? 1 : 0.25 } };
    }),
);
const displayedEdges = computed(() =>
    edges.value.map((edge) => ({
        ...edge,
        style: {
            opacity:
                !selected.value || edge.source === selected.value || edge.target === selected.value
                    ? 1
                    : 0.15,
        },
    })),
);

function arrange() {
    const graph = new dagre.graphlib.Graph().setGraph({ rankdir: "LR", nodesep: 36, ranksep: 100 });
    graph.setDefaultEdgeLabel(() => ({}));
    for (const node of nodes.value) {
        graph.setNode(node.id, { width: 210, height: 70 });
    }
    for (const edge of edges.value) {
        graph.setEdge(edge.source, edge.target);
    }
    dagre.layout(graph);
    nodes.value = nodes.value.map((node) => {
        const position = graph.node(node.id);
        return { ...node, position: { x: position.x - 105, y: position.y - 35 } };
    });
}
async function autoLayout() {
    arrange();
    await nextTick();
    await flow.fitView({ padding: 0.2, duration: 200 });
}
async function refresh() {
    if (busy.value || props.signal.aborted) {
        return;
    }
    busy.value = true;
    error.value = "";
    controller = new AbortController();
    const signal = AbortSignal.any([props.signal, controller.signal, AbortSignal.timeout(fetchTimeoutMs)]);
    try {
        const session = await fetch("/api/session", {
            headers: { "X-Vinc-UI": "1" },
            signal,
            cache: "no-store",
        });
        if (!session.ok) {
            throw new Error(`Session unavailable (${session.status})`);
        }
        const token = (await session.json()).token;
        const response = await fetch("/api/graph", {
            headers: { Authorization: `Bearer ${token}` },
            signal,
            cache: "no-store",
        });
        if (!response.ok) {
            throw new Error((await response.text()) || `Refresh unavailable (${response.status})`);
        }
        const snapshot = validate(await response.json());
        if (signal.aborted) {
            return;
        }
        const graph = buildGraph(snapshot);
        // Vue Flow owns drag positions; sample them before replacing graph data.
        const positions = new Map(flow.getNodes.value.map((node) => [node.id, { ...node.position }]));
        const right = Math.max(0, ...[...positions.values()].map((position) => position.x)) + 280;
        let added = 0;
        graph.nodes = graph.nodes.map((node) => ({
            ...node,
            position: positions.get(node.id) ?? { x: right, y: added++ * 110 },
        }));
        nodes.value = graph.nodes;
        edges.value = graph.edges;
        if (!graph.nodes.some((node) => node.id === selected.value)) {
            selected.value = "";
        }
        captured.value = new Date(snapshot.capturedAt).toISOString();
        if (!loaded.value) {
            // Vue Flow fits once after measuring nodes; do not race that with
            // an animated fit before dimensions/stylesheet are ready.
            arrange();
        }
        loaded.value = true;
    } catch (failure) {
        if (!props.signal.aborted && !controller?.signal.aborted) {
            error.value = failure instanceof Error ? failure.message : "Refresh failed";
        }
    } finally {
        busy.value = false;
    }
}
onMounted(refresh);
onBeforeUnmount(() => controller?.abort());
</script>

<template>
    <section class="graph-panel">
        <header>
            <small>OFFICIAL · READ ONLY</small>
            <h1>Module Graph</h1>
            <p>Module → Topic publishes · Topic → Module subscribes</p>
        </header>
        <div class="graph-toolbar">
            <button
                :disabled="busy"
                @click="refresh"
            >
                {{ busy ? "Refreshing…" : "Refresh" }}
            </button>
            <button
                :disabled="!nodes.length"
                @click="autoLayout"
            >
                Auto-layout
            </button>
            <button
                :disabled="!nodes.length"
                @click="flow.fitView({ padding: 0.2 })"
            >
                Fit view
            </button>
            <label
                >Find
                <input
                    v-model="search"
                    type="search"
                    placeholder="Module, MID or Topic"
            /></label>
        </div>
        <p
            class="graph-note"
            role="status"
        >
            {{ nodes.length }} nodes · {{ edges.length }} relations
            <span v-if="captured"> · Updated {{ captured }}</span> · Manual refresh only
        </p>
        <p
            v-if="error"
            class="graph-error"
            role="alert"
        >
            {{ error }}. {{ loaded ? "Previous graph retained." : "Press Refresh to retry." }}
        </p>
        <div class="graph-body">
            <div class="graph-canvas">
                <VueFlow
                    :nodes="displayedNodes"
                    :edges="displayedEdges"
                    :nodes-connectable="false"
                    :edges-updatable="false"
                    :delete-key-code="null"
                    :min-zoom="0.08"
                    :max-zoom="2.5"
                    @node-drag-stop="
                        nodes = nodes.map((node) => ({
                            ...node,
                            position: { ...flow.findNode(node.id)!.position },
                        }))
                    "
                    @node-click="selected = $event.node.id"
                    @pane-click="selected = ''"
                >
                    <div
                        v-if="loaded && !nodes.length"
                        class="graph-empty"
                    >
                        No Modules or Topic connections in this sample.
                    </div>
                </VueFlow>
            </div>
            <aside class="graph-details">
                <template v-if="details">
                    <h2>{{ details.label }}</h2>
                    <p>{{ details.kind === "module" ? "Module" : "Topic" }}</p>
                    <template v-if="details.kind === 'module'">
                        <p>MID: {{ details.mid }}</p>
                        <p v-if="details.module">
                            Type: {{ details.module.type }}<br />State: {{ states[details.module.state] }}
                        </p>
                        <p v-else>Identity unavailable in this sample. Refresh to check again.</p>
                        <template v-if="details.module">
                            <h3>Notification health: {{ notificationState(details.module) }}</h3>
                            <p v-if="!details.module.notifications.observed">No Host report received yet.</p>
                            <div
                                v-for="(health, label) in {
                                    'Receiver wait': details.module.notifications.receiver,
                                    'Local wake': details.module.notifications.localSender,
                                    'Publisher routes': details.module.notifications.publisherRoutes,
                                }"
                                :key="label"
                            >
                                <strong
                                    >{{ label }}:
                                    {{
                                        health.degraded
                                            ? "degraded — fallback active"
                                            : "no active fault reported"
                                    }}</strong
                                >
                                <p>
                                    Failures {{ health.failures }} · Open attempts
                                    {{ health.reopenAttempts }} · Recoveries {{ health.recoveries }}
                                </p>
                                <p v-if="health.targetMid !== '0'">
                                    Last affected destination MID: {{ health.targetMid }}
                                </p>
                                <p v-if="health.firstFailure !== '0'">
                                    First failure (Unix ns): {{ health.firstFailure }}
                                </p>
                                <p v-if="health.lastError">Last error: {{ health.lastError }}</p>
                            </div>
                            <p>
                                Captured at refresh. Notification health does not prove callback or
                                application health.
                            </p>
                        </template>
                    </template>
                    <h3>Registrations ({{ details.connections.length }})</h3>
                    <ul>
                        <li
                            v-for="connection in details.connections"
                            :key="JSON.stringify(connection)"
                        >
                            {{ connection.role }} · {{ connection.topic }}<br />MID {{ connection.mid }} · ID
                            {{ connection.id }}<br />Schema {{ connection.schema }}
                        </li>
                    </ul>
                </template>
                <template v-else
                    ><h2>Explore connections</h2>
                    <p>Select a node to inspect its identity and neighbours.</p>
                    <p>Drag nodes to arrange. Scroll to zoom. Drag the background to pan.</p></template
                >
            </aside>
        </div>
        <p class="graph-note">
            Pages are sampled separately; this is not an atomic snapshot. Stopped Modules may remain listed.
            Layout is kept in this panel only. Module borders: green = no reported notification fault, yellow
            = notification degradation, red = lifecycle failure, gray = inactive or unobserved. Refresh to
            obtain a new sample; these are not live heartbeat indicators.
        </p>
    </section>
</template>
