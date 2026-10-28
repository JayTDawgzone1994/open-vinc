import { Position, type Edge, type Node } from "@vue-flow/core";

export interface NotificationHealth {
    degraded: boolean;
    targetMid: string;
    failures: string;
    reopenAttempts: string;
    recoveries: string;
    firstFailure: string;
    lastError: string;
}
export interface ModuleInfo {
    mid: string;
    name: string;
    type: string;
    state: number;
    notifications: {
        observed: boolean;
        receiver: NotificationHealth;
        localSender: NotificationHealth;
        publisherRoutes: NotificationHealth;
    };
}
export interface Connection {
    mid: string;
    topic: string;
    role: "publish" | "subscribe";
    id: string;
    schema: string;
}
export interface Snapshot {
    apiVersion: 2;
    capturedAt: number;
    atomicSnapshot: false;
    modules: ModuleInfo[];
    connections: Connection[];
}
export interface NodeData {
    kind: "module" | "topic";
    label: string;
    mid?: string;
    module?: ModuleInfo;
    connections: Connection[];
}

// API v2 browser ceilings match the server defaults documented in UI README.
const maximumModules = 256;
const maximumConnections = 16384;
const maximumTopics = 1024;
const maximumNameLength = 256;
// ECMAScript Date's inclusive millisecond bound; reject before replacing graph state.
const maximumDateMillis = 8_640_000_000_000_000;

// Validate the external response before replacing a complete graph. IDs stay
// decimal strings; converting them to JS numbers would silently lose precision.
export function validate(value: unknown): Snapshot {
    const data = value as Snapshot;
    const identity = (id: unknown) => typeof id === "string" && /^[1-9][0-9]{0,19}$/.test(id);
    const text = (name: unknown) =>
        typeof name === "string" && name.length <= maximumNameLength && !name.includes("\0");
    if (
        !data ||
        data.apiVersion !== 2 ||
        data.atomicSnapshot !== false ||
        !Number.isSafeInteger(data.capturedAt) ||
        data.capturedAt < 0 ||
        data.capturedAt > maximumDateMillis ||
        !Array.isArray(data.modules) ||
        !Array.isArray(data.connections) ||
        data.modules.length > maximumModules ||
        data.connections.length > maximumConnections
    ) {
        throw new Error("Invalid graph response");
    }
    const modules = new Set<string>();
    for (const module of data.modules) {
        if (
            !module ||
            !identity(module.mid) ||
            !text(module.name) ||
            !text(module.type) ||
            !Number.isInteger(module.state) ||
            module.state < 0 ||
            module.state > 6 ||
            modules.has(module.mid)
        ) {
            throw new Error("Invalid Module identity");
        }
        const notifications = module.notifications;
        const decimal = (value: unknown) => typeof value === "string" && /^(0|[1-9][0-9]{0,19})$/.test(value);
        if (!notifications || typeof notifications.observed !== "boolean") {
            throw new Error("Invalid notification observation");
        }
        for (const health of [
            notifications.receiver,
            notifications.localSender,
            notifications.publisherRoutes,
        ]) {
            if (
                !health ||
                typeof health.degraded !== "boolean" ||
                ![
                    health.targetMid,
                    health.failures,
                    health.reopenAttempts,
                    health.recoveries,
                    health.firstFailure,
                ].every(decimal) ||
                typeof health.lastError !== "string" ||
                health.lastError.length > 8192
            ) {
                throw new Error("Invalid notification diagnostics");
            }
        }
        modules.add(module.mid);
    }
    const registrations = new Set<string>();
    const topics = new Set<string>();
    for (const connection of data.connections) {
        if (
            !connection ||
            !identity(connection.mid) ||
            !identity(connection.id) ||
            !identity(connection.schema) ||
            !text(connection.topic) ||
            !connection.topic ||
            connection.topic.includes(".") ||
            !["publish", "subscribe"].includes(connection.role)
        ) {
            throw new Error("Invalid Topic connection");
        }
        const key = JSON.stringify([connection.topic, connection.role, connection.id]);
        if (registrations.has(key)) {
            throw new Error("Duplicate Topic registration");
        }
        registrations.add(key);
        topics.add(connection.topic);
    }
    if (topics.size > maximumTopics) {
        throw new Error("Graph topic limit exceeded");
    }
    return data;
}

export const states = ["Queued", "Starting", "Running", "Stopping", "Restart waiting", "Stopped", "Failed"];

export function notificationState(module: ModuleInfo): string {
    if (module.state === 6 || module.state === 4) {
        return "red";
    }
    if (module.state !== 2 || !module.notifications.observed) {
        return "gray";
    }
    const status = module.notifications;
    return status.receiver.degraded || status.localSender.degraded || status.publisherRoutes.degraded
        ? "yellow"
        : "green";
}

export function buildGraph(snapshot: Snapshot): { nodes: Node<NodeData>[]; edges: Edge[] } {
    const nodes = new Map<string, Node<NodeData>>();
    const edges = new Map<string, Edge>();
    const moduleId = (mid: string) => JSON.stringify(["module", mid]);
    const topicId = (topic: string) => JSON.stringify(["topic", topic]);
    for (const module of snapshot.modules) {
        const label = module.name || `MID ${module.mid} (name pending)`;
        nodes.set(moduleId(module.mid), {
            id: moduleId(module.mid),
            position: { x: 0, y: 0 },
            label,
            class: `module-node notification-${notificationState(module)}`,
            data: { kind: "module", label, mid: module.mid, module, connections: [] },
        });
    }
    for (const connection of snapshot.connections) {
        const module = moduleId(connection.mid);
        const topic = topicId(connection.topic);
        if (!nodes.has(module)) {
            const label = `Unresolved MID ${connection.mid}`;
            nodes.set(module, {
                id: module,
                label,
                class: "module-node unresolved-node",
                position: { x: 0, y: 0 },
                data: { kind: "module", label, mid: connection.mid, connections: [] },
            });
        }
        if (!nodes.has(topic)) {
            nodes.set(topic, {
                id: topic,
                label: connection.topic,
                class: "topic-node",
                position: { x: 0, y: 0 },
                data: { kind: "topic", label: connection.topic, connections: [] },
            });
        }
        nodes.get(module)!.data!.connections.push(connection);
        nodes.get(topic)!.data!.connections.push(connection);
        const id = JSON.stringify([connection.mid, connection.topic, connection.role]);
        const existing = edges.get(id);
        if (existing) {
            existing.data.count++;
            existing.label = `${connection.role} ×${existing.data.count}`;
        } else {
            edges.set(id, {
                id,
                source: connection.role === "publish" ? module : topic,
                target: connection.role === "publish" ? topic : module,
                label: connection.role,
                data: { count: 1 },
                markerEnd: "arrowclosed",
            });
        }
    }
    return {
        nodes: [...nodes.values()].map((node) => ({
            ...node,
            sourcePosition: Position.Right,
            targetPosition: Position.Left,
        })),
        edges: [...edges.values()],
    };
}
