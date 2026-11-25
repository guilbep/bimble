// Copy the C core's wasm build (ccore/build.sh wasm) and its demo model into public/ccore/, where
// Vite serves them as is for the lab's `?ccore=1` (src/ccore.ts).
import { copyFileSync, existsSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const ccore = join(root, "..", "ccore");
const files = [["out/wasm/ccore.wasm", "ccore.wasm"], ["golden/frame.json", "frame.json"]];
const dst = join(root, "public", "ccore");

const missing = files.filter(([f]) => !existsSync(join(ccore, f))).map(([f]) => f);
if (missing.length) {
  console.error(`ccore/ is missing ${missing.join(", ")}: run "ccore/build.sh wasm" first.`);
  process.exit(1);
}
mkdirSync(dst, { recursive: true });
for (const [f, to] of files) copyFileSync(join(ccore, f), join(dst, to));
console.log(`copied ${files.length} files to public/ccore/`);
