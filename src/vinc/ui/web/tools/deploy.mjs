import { cp, lstat, mkdir, rm } from "node:fs/promises";
import path from "node:path";

const [sourceArgument, destinationArgument, buildArgument] = process.argv.slice(2);
if (!sourceArgument || !destinationArgument || !buildArgument) {
    throw new Error("Usage: deploy.mjs <dist> <deployment root> <build root>");
}
const source = path.resolve(sourceArgument);
const destination = path.resolve(destinationArgument);
const build = path.resolve(buildArgument);
const relative = path.relative(build, destination);
const parts = relative.split(path.sep);
if (
    parts.length !== 3 ||
    parts[0] !== "vinc_data" ||
    parts[2] !== "ui" ||
    !parts[1] ||
    path.isAbsolute(relative)
) {
    throw new Error("Refusing a deployment outside build/vinc_data/<name>/ui");
}

async function rejectSymlinks(target) {
    let current = target;
    while (true) {
        const info = await lstat(current).catch((error) => {
            if (error.code === "ENOENT") {
                return undefined;
            }
            throw error;
        });
        if (info?.isSymbolicLink()) {
            throw new Error(`Refusing a symlink deployment: ${current}`);
        }
        const parent = path.dirname(current);
        if (parent === current) {
            return;
        }
        current = parent;
    }
}

// These two generated directories are owned by the build. Other plugin packages
// are user-owned and are never copied over or removed by deployment.
for (const name of ["app", "plugins/vinc.example", "plugins/vinc.topics", "plugins/vinc.graph"]) {
    const target = path.join(destination, name);
    await rejectSymlinks(target);
    await rm(target, { recursive: true, force: true });
    await mkdir(path.dirname(target), { recursive: true });
    await cp(path.join(source, name), target, { recursive: true });
}
