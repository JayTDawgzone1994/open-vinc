import { build } from "vite";
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
const notices = [];
const licenses = [
    ["cpp-httplib v0.61.0", path.join(repository, "library/cpp-httplib/upstream/LICENSE")],
    ["nlohmann/json v3.12.0", path.join(repository, "library/json/upstream/LICENSE.MIT")],
    ["PicoSHA2 v1.0.1", path.join(repository, "library/picosha2/upstream/LICENSE")],
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
const source = path.join(web, "plugins", "example");
const destination = path.join(dist, "plugins", "vinc.example");
await rm(destination, { recursive: true, force: true });
await cp(source, destination, { recursive: true });
const hash = createHash("sha256");
const names = (await readdir(source, { recursive: true, withFileTypes: true }))
    .filter((entry) => entry.isFile())
    .map((entry) => path.relative(source, path.join(entry.parentPath, entry.name)).split(path.sep).join("/"))
    .sort();
for (const name of names) {
    const bytes = await readFile(path.join(source, name));
    hash.update(`${name}\0${bytes.length}\0`);
    hash.update(bytes);
}
const digest = hash.digest("hex");
const header = `#pragma once\nnamespace vinc::ui::detail {\ninline constexpr auto official_package_hash = "${digest}";\n}\n`;
const output = path.join(generated, "vinc_ui_official.hpp");
await writeFile(output, header);
await writeFile(path.join(dist, "ready.stamp"), digest + "\n");
