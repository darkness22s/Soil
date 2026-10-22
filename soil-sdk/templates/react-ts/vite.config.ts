import { defineConfig, type Plugin } from "vite";
import react from "@vitejs/plugin-react";
import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.dirname(fileURLToPath(import.meta.url));

function soilDevBridge(): Plugin {
  return {
    name: "soil-dev-bridge",
    apply: "serve",
    configureServer(server) {
      server.middlewares.use("/__soil/bridge.js", (_req, res) => {
        res.setHeader("Content-Type", "text/javascript; charset=utf-8");
        fs.createReadStream(path.join(root, "dev/soil-bridge.js")).pipe(res);
      });
    },
    transformIndexHtml() {
      return [
        {
          tag: "script",
          attrs: { src: "/__soil/bridge.js" },
          injectTo: "head" as const
        }
      ];
    }
  };
}

export default defineConfig(({ command }) => ({
  base: command === "build" ? "./" : "/",
  plugins: [react(), soilDevBridge()],
  build: {
    outDir: "dist",
    emptyOutDir: true
  }
}));
