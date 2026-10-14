/** Browser-only v1 contracts. No Vue objects or native Context cross this boundary. */
export interface PanelContext {
    readonly instanceId: string;
    readonly signal: AbortSignal;
    setTitle(title: string): void;
    notify(message: string): void;
    onDispose(cleanup: () => void): void;
}

export interface PanelInstance {
    setVisible(visible: boolean): void;
    dispose(): void;
}

export interface UiPlugin {
    createPanel(panelType: string, container: HTMLElement, context: PanelContext): PanelInstance;
}

export interface PanelDefinition {
    id: string;
    title: string;
    multiple: boolean;
}

export interface PluginInfo {
    id: string;
    name: string;
    version: string;
    apiVersion: 1;
    entry: string;
    digest: string;
    official: boolean;
    panels: PanelDefinition[];
}

export interface Catalog {
    apiVersion: 1;
    plugins: PluginInfo[];
    diagnostics: string[];
}
