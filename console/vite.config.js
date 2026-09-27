import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";
import { resolve } from "path";
import { viteSingleFile } from "vite-plugin-singlefile";

// `single` inlines every asset into dist-single/index.html so the page runs
// from file:// with no server. The default build (GitHub Pages) uses relative
// asset URLs (base "") and the same hash router, so no rewrite rules are needed.
export default defineConfig(({ mode }) => {
  const single = mode === "single";
  return {
    base: "",
    plugins: [vue(), single && viteSingleFile()],
    server: {
      port: 9810,
    },
    build: {
      outDir: single ? "dist-single" : "dist",
      emptyOutDir: true,
      rollupOptions: {
        input: resolve(__dirname, "index.html"),
      },
    },
    resolve: {
      alias: {
        "@": resolve(__dirname, "src"),
      },
    },
  };
});
