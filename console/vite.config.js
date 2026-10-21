import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";
import { resolve } from "path";
import { readFileSync } from "node:fs";
import { viteSingleFile } from "vite-plugin-singlefile";

// `single` inlines every asset into dist-single/index.html to run from file:// with no server;
// the default build (GitHub Pages) uses relative asset URLs (base "").
export default defineConfig(({ mode }) => {
  const single = mode === "single";
  return {
    base: "",
    plugins: [vue(), single && viteSingleFile()],
    server: {
      port: 9810,
      https: process.env.FSK_TLS_CERT && process.env.FSK_TLS_KEY ? {
        cert: readFileSync(process.env.FSK_TLS_CERT),
        key: readFileSync(process.env.FSK_TLS_KEY),
      } : undefined,
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
