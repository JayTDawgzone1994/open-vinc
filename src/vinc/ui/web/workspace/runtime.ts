import type { PanelContext, PluginInfo, UiPlugin } from "../sdk";

export const limits = {
    panels: 24,
    titleCharacters: 80,
    notificationCharacters: 240,
    savedApprovals: 64,
    connectionPollMs: 5000,
    requestTimeoutMs: 3000,
} as const;

// Approvals belong to this browser origin and exact immutable package digest.
const approvalKey = "open-vinc.ui.approvals.v1";
const approved = new Set<string>();
try {
    const stored: unknown = JSON.parse(localStorage.getItem(approvalKey) ?? "[]");
    if (Array.isArray(stored)) {
        for (const value of stored.slice(-limits.savedApprovals)) {
            if (typeof value === "string" && value.length < 200) {
                approved.add(value);
            }
        }
    }
} catch {
    // Restricted storage does not prevent explicit consent for the current page.
}

function identity(plugin: PluginInfo): string {
    return `${plugin.id}:${plugin.digest}`;
}

export function isTrusted(plugin: PluginInfo): boolean {
    return plugin.official || approved.has(identity(plugin));
}

export function trust(plugin: PluginInfo): void {
    approved.add(identity(plugin));
    while (approved.size > limits.savedApprovals) {
        approved.delete(approved.values().next().value!);
    }
    try {
        localStorage.setItem(approvalKey, JSON.stringify([...approved]));
    } catch {
        // The current page still remembers the approval.
    }
}

const imports = new Map<string, Promise<UiPlugin>>();
export function loadPlugin(plugin: PluginInfo): Promise<UiPlugin> {
    if (!isTrusted(plugin)) {
        return Promise.reject(new Error("Plugin has not been trusted."));
    }

    const key = identity(plugin);
    let loading = imports.get(key);
    if (!loading) {
        loading = import(/* @vite-ignore */ plugin.entry).then((value: unknown) => {
            if (
                typeof value !== "object" ||
                value === null ||
                !("createPanel" in value) ||
                typeof value.createPanel !== "function"
            ) {
                throw new Error("Plugin does not export createPanel().");
            }
            return value as UiPlugin;
        });
        imports.set(key, loading);
    }

    return loading;
}

export class PanelScope {
    private readonly controller = new AbortController();
    private cleanups: (() => void)[] = [];
    private closed = false;
    readonly context: PanelContext;

    constructor(instanceId: string, title: (value: string) => void, notice: (value: string) => void) {
        this.context = {
            instanceId,
            signal: this.controller.signal,
            setTitle: (value) => {
                if (!this.closed) {
                    title(String(value).slice(0, limits.titleCharacters));
                }
            },
            notify: (value) => {
                if (!this.closed) {
                    notice(String(value).slice(0, limits.notificationCharacters));
                }
            },
            onDispose: (cleanup) => {
                if (this.closed) {
                    cleanup();
                } else {
                    this.cleanups.push(cleanup);
                }
            },
        };
    }

    dispose(): string[] {
        if (this.closed) {
            return [];
        }
        this.closed = true;
        this.controller.abort();
        const failures: string[] = [];
        for (const cleanup of this.cleanups.reverse()) {
            try {
                cleanup();
            } catch {
                failures.push("A plugin cleanup failed.");
            }
        }
        this.cleanups = [];
        return failures;
    }
}
