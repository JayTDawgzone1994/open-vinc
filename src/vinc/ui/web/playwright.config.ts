import { defineConfig } from "@playwright/test";
import path from "node:path";
export default defineConfig({
    testDir: "tests",
    workers: 1,
    timeout: 30000,
    outputDir: path.resolve("../../../../build/ui-browser-results"),
    reporter: "list",
    use: { browserName: "chromium", viewport: { width: 1360, height: 900 } },
});
