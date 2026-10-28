// Resolve from the UI package, not the caller's cwd or this helper's directory.
import { createRequire } from "node:module";
import path from "node:path";
import { pathToFileURL } from "node:url";

const web = process.argv[2];
if (!web) {
    throw new Error("Usage: check_frontend.mjs <UI package directory>");
}

const require = createRequire(path.join(path.resolve(web), "package.json"));
for (const name of ["vite", "@vitejs/plugin-vue", "vue", "@vue-flow/core", "@dagrejs/dagre", "typescript"]) {
    await import(pathToFileURL(require.resolve(name)).href);
}
console.log("Frontend dependency imports: PASS");
