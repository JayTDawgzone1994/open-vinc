import { test, expect } from "@playwright/test";
import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { cp, mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import path from "node:path";

const build = path.resolve(process.env.VINC_UI_BUILD ?? "../../../../build/windows-msvc-debug");
const executable = path.join(
    build,
    "framework/ui/vinc_ui_test_server" + (process.platform === "win32" ? ".exe" : ""),
);
let processServer: ChildProcessWithoutNullStreams;
let root: string;
let url: string;

async function start(): Promise<void> {
    processServer = spawn(executable, [root], { windowsHide: true });
    url = await new Promise<string>((resolve, reject) => {
        let stdout = "";
        let stderr = "";
        const timeout = setTimeout(() => reject(new Error("UI server start timeout")), 10000);
        processServer.stderr.on("data", (bytes) => {
            stderr += String(bytes);
        });
        processServer.on("error", reject);
        processServer.on("exit", (code) => {
            clearTimeout(timeout);
            reject(new Error(`Server exit ${code}: ${stderr}`));
        });
        processServer.stdout.on("data", (bytes) => {
            stdout += String(bytes);
            if (stdout.includes("\n")) {
                clearTimeout(timeout);
                resolve(`http://127.0.0.1:${Number(stdout.trim())}`);
            }
        });
    });
}

async function stop(): Promise<void> {
    if (!processServer || processServer.exitCode !== null) {
        return;
    }
    await new Promise<void>((resolve) => {
        const timeout = setTimeout(() => processServer.kill(), 5000);
        processServer.once("exit", () => {
            clearTimeout(timeout);
            resolve();
        });
        processServer.stdin.end("stop\n");
    });
}

async function addPlugin(id: string, source: string): Promise<void> {
    const directory = path.join(root, "plugins", id);
    await mkdir(directory, { recursive: true });
    await writeFile(
        path.join(directory, "plugin.json"),
        JSON.stringify({
            id,
            name: id,
            version: "1",
            apiVersion: 1,
            entry: "index.js",
            panels: [{ id: "main", title: id, multiple: false }],
        }),
    );
    await writeFile(path.join(directory, "index.js"), source);
}

test.beforeAll(async () => {
    root = await mkdtemp(path.join(build, "ui-browser-"));
    await cp(path.join(build, "framework/ui/dist/app"), path.join(root, "app"), { recursive: true });
    await cp(path.join(build, "framework/ui/dist/plugins"), path.join(root, "plugins"), { recursive: true });
    await addPlugin(
        "test.lifecycle",
        `
        window.pluginEvaluations = (window.pluginEvaluations || 0) + 1;
        export function createPanel(type, root, context) {
            window.pluginCreates = (window.pluginCreates || 0) + 1;
            root.textContent = "External test panel";
            context.onDispose(() => {
                window.pluginCleanup = context.signal.aborted;
            });
            return {
                setVisible() {},
                dispose() {
                    window.pluginDisposed = true;
                }
            };
        }
    `,
    );
    await addPlugin(
        "test.broken",
        `export function createPanel() { throw new Error("Expected fixture failure"); }`,
    );
    await start();
});
test.afterAll(async () => {
    await stop();
    if (root) {
        await rm(root, { recursive: true, force: true });
    }
});

test("official panels, independent state and responsive workspace", async ({ page }) => {
    await page.goto(url);
    await expect(page.getByRole("status", { name: "" }).first()).toContainText("Connected");
    await page.screenshot({ path: path.join(build, "ui-workspace-desktop.png"), fullPage: true });
    await page.getByRole("button", { name: "Open example panel" }).click();
    await expect(page.getByRole("dialog")).not.toBeVisible();
    await page.getByRole("button", { name: "Increment" }).click();
    await page.locator(".open-panel").filter({ hasText: "Example panel" }).click();
    await expect(page.getByRole("tab")).toHaveCount(2);
    await expect(page.locator(".tab-content:visible .count")).toHaveText("0");
    await page.getByRole("tab", { name: "Example · 1" }).click();
    await expect(page.locator(".tab-content:visible .count")).toHaveText("1");
    await page.screenshot({ path: path.join(build, "ui-example-desktop.png"), fullPage: true });
    await page.setViewportSize({ width: 390, height: 844 });
    await page.screenshot({ path: path.join(build, "ui-example-mobile.png"), fullPage: true });
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBeTruthy();
});

test("trust precedes evaluation, approval survives refresh, singleton and disposal", async ({ page }) => {
    await page.goto(url);
    const open = page.locator(".open-panel").filter({ hasText: "test.lifecycle" });
    await open.click();
    await expect(page.getByRole("dialog")).toBeVisible();
    expect(await page.evaluate(() => (window as any).pluginEvaluations)).toBeUndefined();
    await page.getByRole("button", { name: "Cancel", exact: true }).click();
    expect(await page.evaluate(() => (window as any).pluginEvaluations)).toBeUndefined();
    await open.click();
    await page.getByRole("button", { name: "Trust and open" }).click();
    await expect(page.getByText("External test panel")).toBeVisible();
    await open.click();
    await expect(page.getByRole("tab")).toHaveCount(1);
    expect(await page.evaluate(() => (window as any).pluginCreates)).toBe(1);
    await page.getByRole("button", { name: "Close test.lifecycle", exact: true }).click();
    expect(await page.evaluate(() => (window as any).pluginCleanup && (window as any).pluginDisposed)).toBe(
        true,
    );
    await page.reload();
    await open.click();
    await expect(page.getByText("External test panel")).toBeVisible();
    await expect(page.getByRole("dialog")).not.toBeVisible();
});

test("plugin failure stays in its panel", async ({ page }) => {
    await page.goto(url);
    await page.getByRole("button", { name: /test.broken/ }).click();
    await page.getByRole("button", { name: "Trust and open" }).click();
    await expect(page.getByRole("alert")).toContainText("Expected fixture failure");
    await page.locator(".open-panel").filter({ hasText: "Example panel" }).click();
    await expect(page.getByRole("button", { name: "Increment" })).toBeVisible();
});

test("changed content requires fresh consent and disconnect is visible", async ({ page }) => {
    await page.goto(url);
    await page.getByRole("button", { name: /test.lifecycle/ }).click();
    await page.getByRole("button", { name: "Trust and open" }).click();
    await expect(page.getByText("External test panel")).toBeVisible();
    // Keep the origin and every valid schema field; only the server-issued digest
    // changes. The native test separately proves file changes produce that digest.
    await page.route("**/api/ui", async (route) => {
        const response = await route.fetch();
        const data = await response.json();
        data.plugins.find((plugin: any) => plugin.id === "test.lifecycle").digest = "f".repeat(64);
        await route.fulfill({ response, json: data });
    });
    await page.reload();
    await page.getByRole("button", { name: /test.lifecycle/ }).click();
    await expect(page.getByRole("dialog")).toBeVisible();
    expect(await page.evaluate(() => (window as any).pluginEvaluations)).toBeUndefined();
    await page.getByRole("button", { name: "Cancel", exact: true }).click();
    await page.unroute("**/api/ui");
    await page.route("**/api/ui", (route) => route.abort());
    await expect(page.locator(".connection")).toHaveText("Disconnected", { timeout: 10000 });
});

test("Topic Monitor renders diagnostics, safe previews, pause and stale states", async ({ page }) => {
    let state = "green";
    let available = true;
    let stale = false;
    let previewReads = 0;
    const topic = () => ({
        name: "camera/frame", type: "UserExampleFrame", schema: "18446744073709551615", known_type: true,
        hz: 30.5, status: state, reason: "Writer waiting for reader copy", subscribers: 1,
        lost: "2", overwrites: "1", oversized: "0",
        publishers: [{ id: "18446744073709551615", mid: "12", name: "camera", hz: 30.5,
            slots: 4, requested_slots: 4, message_bytes: "4096", shared_bytes: "20000",
            published: "18446744073709551615", collisions: "3", wait_ns: "1500000000",
            longest_wait_ns: "1100000000", current_wait_ns: "1000000000" }],
    });
    await page.route("**/api/topics", async (route) => {
        if (!available) {
            await route.abort();
            return;
        }
        if (stale) {
            await route.fulfill({ json: { apiVersion: 1, available: false, topics: [], error: "Diagnostic samples are stale" } });
            return;
        }
        await route.fulfill({ json: { apiVersion: 1, available: true, topics: [topic()] } });
    });
    await page.route("**/api/topics/preview?*", async (route) => {
        previewReads += 1;
        await route.fulfill({ json: {
            apiVersion: 1, state: "ready", publisher: "camera", utc: "2026-10-25T00:00:00.000000000Z", count: "7",
            text: '{sequence: 42, note: "<img src=x onerror=alert(1)>"}',
        } });
    });
    await page.goto(url);
    await page.locator(".open-panel").filter({ hasText: "Topic Monitor" }).click();
    await expect(page.getByRole("dialog")).not.toBeVisible();
    const row = page.locator("tr").filter({ hasText: "camera/frame" });
    await expect(row).toHaveAttribute("data-status", "green");
    await expect(row).toContainText("30.50");
    await row.getByRole("button", { name: "camera/frame" }).click();
    await expect(page.locator(".payload")).toContainText("<img src=x onerror=alert(1)>");
    await expect(page.locator(".payload img")).toHaveCount(0);
    await expect(page.locator(".publishers")).toContainText("18446744073709551615");
    state = "yellow";
    await expect(row).toHaveAttribute("data-status", "yellow");
    state = "red";
    await expect(row).toHaveAttribute("data-status", "red");
    await page.getByRole("button", { name: "Pause preview", exact: true }).click();
    await expect(page.locator(".preview-status")).toContainText("Paused");
    const pausedReads = previewReads;
    // Observe the unrelated diagnostics round trip instead of sleeping to infer cancellation.
    state = "green";
    await expect(row).toHaveAttribute("data-status", "green");
    expect(previewReads).toBe(pausedReads);
    await page.getByRole("button", { name: "Resume preview", exact: true }).click();
    await expect.poll(() => previewReads).toBeGreaterThan(pausedReads);
    await page.screenshot({ path: path.join(build, "ui-topics-desktop.png"), fullPage: true });
    await page.setViewportSize({ width: 390, height: 844 });
    await page.screenshot({ path: path.join(build, "ui-topics-mobile.png"), fullPage: true });
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBeTruthy();
    stale = true;
    await expect(row).toHaveAttribute("data-status", "gray");
    await expect(page.locator(".topics-panel .connection")).toContainText("stale");
    stale = false;
    await expect(row).toHaveAttribute("data-status", "green");
    available = false;
    await expect(row).toHaveAttribute("data-status", "gray");
    await expect(page.locator(".topics-panel .connection")).toContainText("Disconnected");
    await page.getByRole("button", { name: "Close Topic Monitor", exact: true }).click();
    await expect(page.locator(".topics-panel")).toHaveCount(0);
});

test("Module Graph refreshes only on request and preserves manual arrangement", async ({ page }) => {
    const failures: string[] = [];
    page.on("pageerror", (error) => failures.push(error.message));
    let requests = 0;
    let fail = false;
    const snapshot: any = {
        apiVersion: 1, atomicSnapshot: false, capturedAt: 1793088000000,
        modules: [
            { mid: "1", name: "cameraFront", type: "camera", state: 2 },
            { mid: "2", name: "detector", type: "vision", state: 2 },
            { mid: "3", name: "idleModule", type: "worker", state: 5 },
        ],
        connections: [
            { mid: "1", topic: "camera/frames", role: "publish", id: "1", schema: "10" },
            { mid: "1", topic: "camera/frames", role: "publish", id: "2", schema: "10" },
            { mid: "2", topic: "camera/frames", role: "subscribe", id: "3", schema: "10" },
            { mid: "2", topic: "vision/results", role: "publish", id: "4", schema: "11" },
            { mid: "9007199254740999", topic: "vision/results", role: "subscribe", id: "5", schema: "11" },
        ],
    };
    await page.route("**/api/graph", (route) => {
        requests++;
        return fail ? route.fulfill({ status: 503, body: "Query unavailable" }) :
            route.fulfill({ json: snapshot });
    });
    await page.goto(url);
    await page.locator(".open-panel").filter({ hasText: "Module Graph" }).click();
    await expect(page.getByRole("dialog")).not.toBeVisible();
    await expect(page.locator(".vue-flow__node")).toHaveCount(6);
    await expect(page.locator(".vue-flow__edge")).toHaveCount(4);
    await expect(page.locator(".graph-note").first()).toContainText("Updated");
    await expect(page.getByText("publish ×2", { exact: true })).toBeVisible();
    await expect(page.locator(".vue-flow__node").filter({ hasText: "Unresolved MID" })).toBeInViewport();
    const camera = page.locator(".vue-flow__node").filter({ hasText: "cameraFront" });
    const initialPosition = await camera.getAttribute("style");
    const before = await camera.boundingBox();
    expect(before).not.toBeNull();
    await page.mouse.move(before!.x + 50, before!.y + 20);
    await page.mouse.down();
    await page.mouse.move(before!.x + 100, before!.y + 110, { steps: 8 });
    await page.mouse.up();
    const position = await camera.getAttribute("style");
    expect(position).not.toBe(initialPosition);
    const viewport = await page.locator(".vue-flow__transformationpane").getAttribute("style");
    snapshot.modules.push({ mid: "4", name: "newModule", type: "worker", state: 2 });
    await page.getByRole("button", { name: "Refresh", exact: true }).click();
    await expect(page.locator(".vue-flow__node")).toHaveCount(7);
    expect(await camera.getAttribute("style")).toBe(position);
    expect(await page.locator(".vue-flow__transformationpane").getAttribute("style")).toBe(viewport);
    expect(requests).toBe(2);
    // Exceed the existing Topic Monitor polling interval; graph must remain idle.
    await page.waitForTimeout(1300);
    expect(requests).toBe(2);
    await page.getByRole("button", { name: "Fit view", exact: true }).click();
    await page.locator(".vue-flow__node").filter({ hasText: "Unresolved MID" }).click();
    await expect(page.locator(".graph-details")).toContainText("9007199254740999");
    await expect(page.locator(".graph-details")).toContainText("Identity unavailable");
    await page.getByPlaceholder("Module, MID or Topic").fill("camera");
    await expect(camera).toBeVisible();
    await page.getByPlaceholder("Module, MID or Topic").fill("");
    await page.getByRole("button", { name: "Auto-layout", exact: true }).click();
    await page.getByRole("button", { name: "Fit view", exact: true }).click();
    await page.screenshot({ path: path.join(build, "ui-graph-desktop.png"), fullPage: true });
    fail = true;
    await page.getByRole("button", { name: "Refresh", exact: true }).click();
    await expect(page.locator(".graph-error")).toContainText("Previous graph retained");
    await expect(page.locator(".vue-flow__node")).toHaveCount(7);
    await page.setViewportSize({ width: 390, height: 844 });
    await page.screenshot({ path: path.join(build, "ui-graph-mobile.png"), fullPage: true });
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBeTruthy();
    fail = false;
    // A finite but unrepresentable timestamp must not replace the previous graph.
    snapshot.capturedAt = 1e20;
    await page.getByRole("button", { name: "Refresh", exact: true }).click();
    await expect(page.locator(".graph-error")).toContainText("Invalid graph response");
    await expect(page.locator(".vue-flow__node")).toHaveCount(7);
    snapshot.capturedAt = 1793088000000;
    snapshot.modules = [];
    snapshot.connections = [];
    await page.getByRole("button", { name: "Refresh", exact: true }).click();
    await expect(page.locator(".graph-empty")).toBeVisible();
    await expect(page.locator(".vue-flow__node")).toHaveCount(0);
    await page.getByRole("button", { name: "Close Module Graph", exact: true }).click();
    const closedRequests = requests;
    await page.waitForTimeout(1300);
    expect(requests).toBe(closedRequests);
    expect(failures).toEqual([]);
});
