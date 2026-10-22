import { defineConfig } from "vite";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.dirname(fileURLToPath(import.meta.url));

function soilDevBridge() {
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
          injectTo: "head"
        }
      ];
    }
  };
}

export default defineConfig(({ command }) => ({
  base: command === "build" ? "./" : "/",
  plugins: [soilDevBridge()],
  build: {
    outDir: "dist",
    emptyOutDir: true
  }
}));
