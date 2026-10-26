import { test, expect } from "@playwright/test";
import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { cp, mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import path from "node:path";

const build = path.resolve(process.env.VINC_UI_BUILD ?? "../../../../build/windows-msvc-debug");
const executable = path.join(
    build,
    "vinc/ui/vinc_ui_test_server" + (process.platform === "win32" ? ".exe" : ""),
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
    await cp(path.join(build, "vinc/ui/dist/app"), path.join(root, "app"), { recursive: true });
    await cp(path.join(build, "vinc/ui/dist/plugins"), path.join(root, "plugins"), { recursive: true });
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
