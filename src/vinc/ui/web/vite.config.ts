import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";
export default defineConfig({
    root: "workspace",
    plugins: [vue()],
    build: { target: "es2022", sourcemap: false },
});
