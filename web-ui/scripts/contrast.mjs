// WCAG AA contrast of every theme of the lab (ticket SET1, src/shell/theme.ts): text on the grounds
// it is drawn on, ink on the disc fills, borders that matter against the ground. Exits 1 on a failure.
//   node scripts/contrast.mjs
import { pathToFileURL } from "node:url";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const T = await import(pathToFileURL(join(here, "../src/shell/theme.ts")).href);

const lin = (c) => { c /= 255; return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; };
const lum = (hex) => { const n = parseInt(hex.slice(1), 16); return 0.2126 * lin(n >> 16) + 0.7152 * lin((n >> 8) & 255) + 0.0722 * lin(n & 255); };
const ratio = (a, b) => { const [x, y] = [lum(a), lum(b)].sort((p, q) => q - p); return (x + 0.05) / (y + 0.05); };

// [foreground, background, minimum, what]
const PAIRS = [
  ...["cream", "sheet", "sunk"].flatMap((g) => [["ink", g, 4.5, "text"], ["stone", g, 4.5, "quiet text"]]),
  ...["clay", "pink", "moss", "ochre", "slate"].flatMap((d) => [[`${d}-text`, "cream", 4.5, "colour as text"], [`${d}-text`, "sheet", 4.5, "colour as text"], ["ink", d, 4.5, "text on a fill"]]),
  ["on-ink", "ink", 4.5, "text on ink"],
  ["stone", "cream", 3, "control border"], ["stone", "sheet", 3, "control border"],
];
let fails = 0, checks = 0;
for (const id of Object.keys(T.THEMES)) {
  const t = T.tokensOf(id), bad = [];
  for (const [f, b, min, what] of PAIRS) {
    checks++;
    const r = ratio(t[f], t[b]);
    if (r < min) bad.push(`${f} on ${b} ${r.toFixed(2)} < ${min} (${what})`);
  }
  fails += bad.length;
  const worst = Math.min(...PAIRS.filter((p) => p[2] === 4.5).map(([f, b]) => ratio(t[f], t[b])));
  console.log(`${id.padEnd(16)} ${bad.length ? `FAIL ${bad.join("; ")}` : "ok"} (lowest text ratio ${worst.toFixed(2)})`);
}
console.log(`contrast: ${checks - fails} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
