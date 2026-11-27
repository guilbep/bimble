// The bytes of each section of a link cache (`<file>.ifc.bimbleview`, ADR 0079): HEAD, META, GEOR, ELEM (the
// chunks summed: the geometry), SRCE, HLOD (baked proxies), PROP (property sets), any other by its name.
//   node ccore/tools/cache_sections.mjs <file.bimbleview>... [--json]
//   import { cacheSections } from "../tools/cache_sections.mjs";
// A gzipped view file (not a cache) is reported as one "gzip" entry. Reads only the sections' heads.
import { closeSync, fstatSync, openSync, readSync } from "node:fs";
import { fileURLToPath } from "node:url";

/** {section: bytes} of a view file, plus `total` (the file's size) and `head` (magic and version). */
export function cacheSections(path) {
  const fd = openSync(path, "r");
  try {
    const size = fstatSync(fd).size, h = Buffer.alloc(12);
    const out = { total: size, head: 12 };
    if (readSync(fd, h, 0, 12, 0) < 12) return { total: size, damaged: size };
    if (h[0] === 0x1f && h[1] === 0x8b) return { total: size, gzip: size };
    let at = 12;
    while (at + 12 <= size) {
      readSync(fd, h, 0, 12, at);
      const kind = h.toString("latin1", 0, 4), len = Number(h.readBigUInt64LE(4));
      if (at + 12 + len > size) { out.damaged = size - at; break; }
      out[kind] = (out[kind] ?? 0) + 12 + len;
      at += 12 + len;
    }
    return out;
  } finally { closeSync(fd); }
}

/** The sums of several files' sections. */
export function sumSections(paths) {
  const s = {};
  for (const p of paths) for (const [k, v] of Object.entries(cacheSections(p))) s[k] = (s[k] ?? 0) + v;
  return s;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const files = process.argv.slice(2).filter((a) => a !== "--json");
  const all = sumSections(files);
  if (process.argv.includes("--json")) console.log(JSON.stringify(all));
  else for (const [k, v] of Object.entries(all)) console.log(`${k.padEnd(6)} ${v.toLocaleString("en-US").padStart(15)} bytes  ${(100 * v / all.total).toFixed(1)} %`);
}
