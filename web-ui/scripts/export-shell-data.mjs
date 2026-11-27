// The lab shell's declarative data, written ONCE as JSON for every UI (spike NAT2, ticket NUI1: the
// native app `bimble` embeds these files at build time; ADR 0084).
//
//   node web-ui/scripts/export-shell-data.mjs [outdir]      (default: ccore/native/ui/data)
//   node web-ui/scripts/export-shell-data.mjs --check       the committed files equal what the sources give
//                                                           (ccore/ci/run.sh unit: the two UIs cannot drift)
//
// Reads the TypeScript sources the Vue shell runs on (no copy by hand, so the native shell and the
// Vue shell cannot drift):
//   shell/tabs.ts          TABS (ids, groups)                 -> ribbon.json .tabs
//   shell/ribbon-tabs.ts   RIBBON (tab -> groups -> tools)     -> ribbon.json .ribbon
//   shell/capability.ts    ACTION_CAPABILITY                   -> ribbon.json .actionCapability
//   shell/keys.ts          KEYS (the key table)                -> keys.json
//   shell/machine.ts       KINDS, EVENTS, TRANSITIONS' shape   -> machine.json (state -> event -> row name)
//   i18n/fr.ts, en.ts, parts/*.ts                              -> strings.fr.json, strings.en.json
//   shell/theme.ts         BASE, THEMES (the six themes' tokens) -> themes.json
// Node strips the TypeScript types itself (node >= 23.6); a file whose runtime imports Node cannot
// follow (keys.ts and capability.ts import Vue and the client) is read as text: the one array or
// object literal is cut out and evaluated alone (it is plain data).
// The web UI could consume the same files later: `import ribbon from "../../ccore/native/ui/data/ribbon.json"`
// in place of the literals (Vite imports JSON), with ribbon-tabs.ts reduced to its types.
import { readFileSync, writeFileSync as writeFile, mkdirSync, readdirSync, mkdtempSync, rmSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const src = resolve(here, "../src");
const check = process.argv[2] === "--check";
const committed = resolve(here, "../../ccore/native/ui/data");
const out = check ? mkdtempSync(join(tmpdir(), "export-shell-data-")) : resolve(process.argv[2] ?? committed);
mkdirSync(out, { recursive: true });
const written = [];
const writeFileSync = (p, text) => { written.push(p); writeFile(p, text); };

/** The literal that follows `marker` (an array or object), evaluated as data. */
function literal(file, marker) {
  const text = readFileSync(join(src, file), "utf8");
  const at = text.indexOf(marker);
  if (at < 0) throw new Error(`${file}: no "${marker}"`);
  let i = text.indexOf("=", at) + 1;
  while (/\s/.test(text[i])) i++;
  const open = text[i], close = open === "[" ? "]" : "}";
  let depth = 0, j = i, str = null;
  for (; j < text.length; j++) {
    const c = text[j];
    if (str) { if (c === "\\") j++; else if (c === str) str = null; continue; }
    if (c === '"' || c === "'" || c === "`") { str = c; continue; }
    if (c === "/" && text[j + 1] === "/") { j = text.indexOf("\n", j); continue; }
    if (c === open) depth++;
    else if (c === close && --depth === 0) break;
  }
  return new Function(`return (${text.slice(i, j + 1)});`)();
}
const mod = (p) => import(pathToFileURL(join(src, p)).href);

// ribbon
const { RIBBON } = await mod("shell/ribbon-tabs.ts");
const TABS = literal("shell/tabs.ts", "export const TABS");
const ACTION_CAPABILITY = literal("shell/capability.ts", "export const ACTION_CAPABILITY");
writeFileSync(join(out, "ribbon.json"), JSON.stringify({ tabs: TABS, ribbon: RIBBON, actionCapability: ACTION_CAPABILITY }, null, 1) + "\n");

// keys
const KEYS = literal("shell/keys.ts", "export const KEYS");
writeFileSync(join(out, "keys.json"), JSON.stringify(KEYS, null, 1) + "\n");

// machine: the table's shape (its rows are functions: their names stand for them)
const M = await mod("shell/machine.ts");
const table = {};
for (const k of M.KINDS) {
  table[k] = {};
  for (const [e, row] of Object.entries(M.TRANSITIONS[k] ?? {})) table[k][e] = row.name || "row";
}
writeFileSync(join(out, "machine.json"), JSON.stringify({ kinds: M.KINDS, events: M.EVENTS, transitions: table }, null, 1) + "\n");

// strings
const fr = { ...(await mod("i18n/fr.ts")).fr }, en = { ...(await mod("i18n/en.ts")).en };
for (const f of readdirSync(join(src, "i18n/parts")).filter((f) => f.endsWith(".ts")).sort()) {
  const p = await mod(`i18n/parts/${f}`);
  Object.assign(fr, p.fr), Object.assign(en, p.en);
}
const sorted = (o) => Object.fromEntries(Object.keys(o).sort().map((k) => [k, o[k]]));
writeFileSync(join(out, "strings.fr.json"), JSON.stringify(sorted(fr), null, 0) + "\n");
writeFileSync(join(out, "strings.en.json"), JSON.stringify(sorted(en), null, 0) + "\n");

// themes (theme.ts is plain TypeScript: imported as it is)
const TH = await mod("shell/theme.ts");
writeFileSync(join(out, "themes.json"), JSON.stringify({ base: TH.BASE, themes: TH.THEMES }, null, 1) + "\n");

if (check) {
  let bad = 0;
  for (const p of written) {
    const name = p.slice(out.length + 1), mine = join(committed, name);
    if (!existsSync(mine) || readFileSync(mine, "utf8") !== readFileSync(p, "utf8")) {
      console.error(`export-shell-data --check: ccore/native/ui/data/${name} differs from the TypeScript sources: run node web-ui/scripts/export-shell-data.mjs and commit it`);
      bad++;
    }
  }
  rmSync(out, { recursive: true, force: true });
  if (bad) process.exit(1);
  console.log(`export-shell-data --check: ${written.length} files match the TypeScript sources`);
  process.exit(0);
}
console.log(`export-shell-data: ${out}: ${TABS.length} tabs, ${Object.values(RIBBON).flat().reduce((n, g) => n + g.tools.length, 0)} tools, ${KEYS.length} keys, ${M.KINDS.length} states, ${Object.keys(fr).length} strings`);
