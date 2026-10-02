import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";
import { viteSingleFile } from "vite-plugin-singlefile";

export default defineConfig({
  plugins: [vue(), viteSingleFile()],
  base: "./",
  build: {
    assetsInlineLimit: 100000000,
    cssCodeSplit: false,
  },
  server: {
    port: 5174,
    proxy: {
      "/api": "http://127.0.0.1:8140",
      "/metrics": "http://127.0.0.1:8140",
      "/livez": "http://127.0.0.1:8140",
      "/statusz": "http://127.0.0.1:8140",
      "/x-nmos": "http://127.0.0.1:8140",
    },
  },
});
