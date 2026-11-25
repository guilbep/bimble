// Ticket J1: the page side of the protocol, in Node (V8, as Chromium): JSON.parse of the answers
// json_bench wrote (<dir>/proto-*.json), median of 5 after one warm-up. Prints JSON.
import { readFileSync, readdirSync } from "node:fs";
const dir = process.argv[2];
const out = {};
for (const f of readdirSync(dir).filter((f) => /^proto-.*\.json$/.test(f)).sort()) {
  const text = readFileSync(`${dir}/${f}`, "utf8");
  JSON.parse(text);
  const v = [];
  for (let i = 0; i < 5; i++) {
    const t0 = performance.now();
    JSON.parse(text);
    v.push(performance.now() - t0);
  }
  v.sort((a, b) => a - b);
  out[f.slice(6, -5)] = { bytes: Buffer.byteLength(text), parse_ms: Math.round(v[2] * 1000) / 1000 };
}
console.log(JSON.stringify(out));
