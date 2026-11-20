// Copy the wasm build (scripts/build-web.sh) into public/bimble/, where Vite serves it as is.
import { copyFileSync, existsSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const src = join(root, "..", "web", "dist");
const dst = join(root, "public", "bimble");
const files = ["bimble_web.js", "bimble_web_bg.wasm", "manifold.js", "manifold.wasm", "manifold-bridge.js"];

const missing = files.filter((f) => !existsSync(join(src, f)));
if (missing.length) {
  console.error(`web/dist is missing ${missing.join(", ")}: run scripts/build-web.sh first.`);
  process.exit(1);
}
mkdirSync(dst, { recursive: true });
for (const f of files) copyFileSync(join(src, f), join(dst, f));
console.log(`copied ${files.length} files to public/bimble/`);
