import { build } from "vite";
import vue from "@vitejs/plugin-vue";
import { createHash } from "node:crypto";
import { cp, mkdir, readFile, readdir, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const [distArgument, generatedArgument] = process.argv.slice(2);
if (!distArgument || !generatedArgument) {
    throw new Error("Usage: build.mjs <dist> <generated>");
}
const dist = path.resolve(distArgument);
const generated = path.resolve(generatedArgument);
if (path.basename(dist) !== "dist" || dist === web || web.startsWith(dist + path.sep)) {
    throw new Error("Refusing an unsafe output directory");
}
await mkdir(generated, { recursive: true });
await build({
    configFile: path.join(web, "vite.config.ts"),
    build: { outDir: path.join(dist, "app"), emptyOutDir: true },
});
const repository = path.resolve(web, "../../../..");
// Keep the standalone browser package self-contained. The shared inventory also
// preserves attributions inside native dependencies, not just their top-level LICENSE.
const attributionDirectory = path.join(dist, "app", "licenses", "open-vinc");
await mkdir(attributionDirectory, { recursive: true });
const notices = [];
for (const name of ["LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.txt"]) {
    const source = path.join(repository, name);
    notices.push(`${name}\n${await readFile(source, "utf8")}`);
    await cp(source, path.join(attributionDirectory, name));
}
const licenses = [
    ["d3-ease 3.0.1", path.join(web, "node_modules/d3-ease/LICENSE")],
    ["d3-timer 3.0.1", path.join(web, "node_modules/d3-timer/LICENSE")],
    ["d3-transition 3.0.1", path.join(web, "node_modules/d3-transition/LICENSE")],
    ["d3-zoom 3.0.0", path.join(web, "node_modules/d3-zoom/LICENSE")],
    ["d3-color 3.1.0", path.join(web, "node_modules/d3-color/LICENSE")],
    ["d3-interpolate 3.0.1", path.join(web, "node_modules/d3-interpolate/LICENSE")],
    ["d3-selection 3.0.0", path.join(web, "node_modules/d3-selection/LICENSE")],
    ["d3-dispatch 3.0.1", path.join(web, "node_modules/d3-dispatch/LICENSE")],
    ["d3-drag 3.0.0", path.join(web, "node_modules/d3-drag/LICENSE")],
    ["@dagrejs/graphlib 3.0.4", path.join(web, "node_modules/@dagrejs/graphlib/LICENSE")],
    ["@dagrejs/dagre 2.0.4", path.join(web, "node_modules/@dagrejs/dagre/LICENSE")],
    [
        "@vueuse/shared/node_modules/vue-demi 0.14.10",
        path.join(web, "node_modules/@vueuse/shared/node_modules/vue-demi/LICENSE"),
    ],
    [
        "@vueuse/core/node_modules/vue-demi 0.14.10",
        path.join(web, "node_modules/@vueuse/core/node_modules/vue-demi/LICENSE"),
    ],
    ["@vueuse/metadata 10.11.1", path.join(web, "node_modules/@vueuse/metadata/LICENSE")],
    ["@vueuse/shared 10.11.1", path.join(web, "node_modules/@vueuse/shared/LICENSE")],
    ["@vueuse/core 10.11.1", path.join(web, "node_modules/@vueuse/core/LICENSE")],
    ["@vue-flow/core 1.48.2", path.join(web, "node_modules/@vue-flow/core/LICENSE")],
    ["Vite 8.3.4 (generated browser helpers)", path.join(web, "node_modules/vite/LICENSE.md")],
    ...["vue", "@vue/runtime-core", "@vue/runtime-dom", "@vue/shared", "@vue/reactivity"].map((name) => [
        name + " 3.5.44",
        path.join(web, "node_modules", name, "LICENSE"),
    ]),
];
for (const [name, filename] of licenses) {
    notices.push(`${name}\n${await readFile(filename, "utf8")}`);
}
await writeFile(path.join(dist, "app", "third-party.txt"), notices.join("\n\n----------------\n\n"));
const hashes = [];
for (const [directory, id, symbol] of [
    ["example", "vinc.example", "official_package_hash"],
    ["topics", "vinc.topics", "official_topics_hash"],
    ["graph", "vinc.graph", "official_graph_hash"],
]) {
    const source = path.join(web, "plugins", directory);
    const destination = path.join(dist, "plugins", id);
    await rm(destination, { recursive: true, force: true });
    if (id === "vinc.graph") {
        await build({
            configFile: false,
            define: { "process.env.NODE_ENV": JSON.stringify("production") },
            plugins: [vue()],
            build: {
                target: "es2022",
                outDir: destination,
                emptyOutDir: true,
                sourcemap: false,
                lib: {
                    entry: path.join(source, "index.ts"),
                    formats: ["es"],
                    fileName: () => "index.js",
                    cssFileName: "style",
                },
            },
        });
        await cp(path.join(source, "plugin.json"), path.join(destination, "plugin.json"));
    } else {
        await cp(source, destination, { recursive: true });
    }
    const hash = createHash("sha256");
    const names = (await readdir(destination, { recursive: true, withFileTypes: true }))
        .filter((entry) => entry.isFile())
        .map((entry) =>
            path.relative(destination, path.join(entry.parentPath, entry.name)).split(path.sep).join("/"),
        )
        .sort();
    for (const name of names) {
        const bytes = await readFile(path.join(destination, name));
        hash.update(`${name}\0${bytes.length}\0`);
        hash.update(bytes);
    }
    hashes.push({ symbol, digest: hash.digest("hex") });
}
const header =
    "#pragma once\nnamespace vinc::ui::detail {\n" +
    hashes.map(({ symbol, digest }) => `inline constexpr auto ${symbol} = "${digest}";\n`).join("") +
    "}\n";
await writeFile(path.join(generated, "vinc_ui_official.hpp"), header);
await writeFile(path.join(dist, "ready.stamp"), hashes.map(({ digest }) => digest).join("\n") + "\n");
