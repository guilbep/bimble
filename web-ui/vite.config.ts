import { defineConfig, type Plugin } from "vite";
import vue from "@vitejs/plugin-vue";
import { execFileSync } from "node:child_process";

// PERF1: the C lab's build (`npm run build:ccore`, VITE_CCORE=1) starts fetching its wasm and model
// from index.html, while index.js loads, instead of after index.js ran and Vue mounted. ccore.ts
// fetches the same URLs (resolved against the document, the same defaults as bimble.ts start())
// and gets these responses. The Rust build and the dev server have no such script. URL1: a link's
// model is not preloaded (it is fetched after the origin question, credentials omitted: openurl.ts).
const CCORE_PRELOAD = `(() => {
  const q = new URLSearchParams(location.search);
  if (q.has("ccore") && q.get("ccore") !== "1") return;
  const wasm = q.get("ccore_wasm") ?? (q.get("view") === "1" ? "ccore/ccore-view.wasm" : "ccore/ccore.wasm");
  // URL1 (ADR 0086): this site's files only; with a link (#open= / ?open=) no demo model.
  const link = q.has("open") || new URLSearchParams(location.hash.slice(1)).has("open");
  const mine = (u) => { try { return new URL(u, location.href).origin === location.origin; } catch { return false; } };
  for (const href of link ? [wasm] : [wasm, q.get("model") ?? "ccore/frame.json"]) {
    if (!mine(href)) continue;
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

// SET1: About shows the commit the page was built from ("" without git).
let commit = "";
try { commit = execFileSync("git", ["rev-parse", "--short=12", "HEAD"], { encoding: "utf8" }).trim(); } catch { /* no git */ }

// VIS1: ccore/tools/drive.mjs runs the dev server frozen (BIMBLE_VITE_FROZEN=1): no file watching, no hot reload, so an
// edit in the tree during a measured run does not reload the page under measurement.
const frozen = process.env["BIMBLE_VITE_FROZEN"] === "1";
export default defineConfig({ plugins: [vue(), ccorePreload], server: { port: 5173, ...(frozen ? { hmr: false, watch: null } : {}) }, define: { __BB_COMMIT__: JSON.stringify(commit) } });
