import { defineConfig, type Plugin } from "vite";
import vue from "@vitejs/plugin-vue";

// PERF1: the C lab's build (`npm run build:ccore`, VITE_CCORE=1) starts fetching its wasm and model
// from index.html, while index.js loads, instead of after index.js ran and Vue mounted. ccore.ts
// fetches the same URLs (resolved against the document, the same defaults as bimble.ts start())
// and gets these responses. The Rust build and the dev server have no such script.
const CCORE_PRELOAD = `(() => {
  const q = new URLSearchParams(location.search);
  if (q.has("ccore") && q.get("ccore") !== "1") return;
  const wasm = q.get("ccore_wasm") ?? (q.get("view") === "1" ? "ccore/ccore-view.wasm" : "ccore/ccore.wasm");
  for (const href of [wasm, q.get("model") ?? "ccore/frame.json"]) {
    const l = document.createElement("link");
    l.rel = "preload"; l.as = "fetch"; l.crossOrigin = "anonymous"; l.href = href;
    document.head.appendChild(l);
  }
})();`;
let ccoreBuild = false;
const ccorePreload: Plugin = {
  name: "bimble-ccore-preload",
  apply: "build",
  configResolved: (c) => { ccoreBuild = c.env["VITE_CCORE"] === "1"; }, // VITE_* of the environment are in c.env
  // After the meta tags, before index.js and the stylesheet (a classic script after a stylesheet waits for it).
  transformIndexHtml: (html) => (ccoreBuild ? html.replace(/(<meta name="viewport"[^>]*>)/, `$1\n  <script>${CCORE_PRELOAD}</script>`) : html),
};

export default defineConfig({ plugins: [vue(), ccorePreload], server: { port: 5173 } });
