// The weight of each feature and file (ticket SIZE1): node ccore/tools/weight.mjs [--out FILE]
// writes ccore/results/weight.md (or FILE) and prints the change against the file it replaces.
// Run after the builds it reads (ci/run.sh unit and full do: build-release, then this):
//   - lines of C: each src/, session/, host/ .c file and its .h of the same name (wc -l);
//   - editor wasm: `build.sh wasm-size` (one object per file, linked with a map: code + data per
//     object, as wasm-ld lays them out). Error: the map is before wasm-opt, which clang runs on
//     the link when it finds one (build.sh wasm) and build.sh wasm-view runs itself: the shipped
//     files are about 10 % smaller than the column sums (the totals table gives both);
//   - viewer wasm: `build.sh wasm-size view`, the same for the viewer-only build;
//   - native: the GNU ld map of the release link (out/release/bimble-ccore.map, build.sh release on
//     Linux): the input sections per object in the loaded output sections (.text, .rodata, .data,
//     .data.rel.ro, .eh_frame, .gcc_except_table, init/fini arrays; not .bss). What the map gives to
//     no object (ELF headers, dynamic tables, .eh_frame_hdr, alignment) is the row "linker";
//     the rows sum to the stripped binary's size exactly. No map (macOS, no release build): "-".
//   - files: the release archive's (out/release/stage/) and the web build's (`npm run build:ccore`:
//     Vite with VITE_CCORE=1 into out/weight/web, then ccore/ccore.wasm, ccore-view.wasm and
//     frame.json from out/wasm and golden/, as copy-ccore.mjs puts them: public/ccore is not
//     touched), raw and gzip -9 (zlib level 9, node's).
// Every object must be in FEATURES below: an object that is not (a new file) fails the run, so it
// gets classified. Growth never fails. Exit 0, or 1 on an unclassified object or a missing build.
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { gzipSync } from "node:zlib";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const UI = join(CCORE, "../web-ui");
const outArg = process.argv.indexOf("--out");
const OUT = outArg > 0 ? resolve(process.argv[outArg + 1]) : join(CCORE, "results/weight.md");
const VIEW_MAX = 1000000; // build.sh VIEW_MAX (ADR 0069 as amended)

// Feature -> objects (basename without .c / .o). Native-only libraries are matched by name below.
const FEATURES = [
  ["base (L0: arena, buffers, numbers, JSON, CBOR, ids, inflate)", "arena buf fmt strtod json_r json_w cbor math guid inflate jobs"],
  ["model (L1: model, commands, history, library, materials, annotations)", "model command history library material_class annot"],
  ["IFC read (STEP parser, scene of an IFC)", "step ifc_read ifc_math"],
  ["IFC import (IFC -> model)", "ifc_in"],
  ["IFC export (model -> IFC)", "ifc_out ifc_out_grid"],
  [".bimble file (read, write, view files)", "file viewfile"],
  ["verbs (edits by name, verb table)", "verbs verb_table verbs_views"],
  ["relations", "relation"],
  ["levels and grids", "grid verbs_levels"],
  ["materials (MAT1: verbs, library and page queries)", "verbs_materials materials_page"],
  ["georef and setting out (SETOUT: markers, north)", "georef setting_out"],
  ["views and sheets from the ribbon (VIEWSHEET: section tool, annotation types)", "view_sheet"],
  ["queries (model queries)", "query"],
  ["quantities and schedules", "quantity schedules"],
  ["command palette", "palette"],
  ["scene and mesh (triangulation, BMSH)", "scene earcut meshcodec mesh mesh_model prism_cut"],
  ["booleans glue (kernel calls)", "boolean"],
  ["views and visibility (2D views, ranges)", "view view_effective view_range views visibility"],
  ["drawing D1-D5 (cut, plan, annotations, style, sheets, paper, SVG)", "draw_cut draw_plan draw_annot draw_style draw_view draw_sheet paper sheet_svg toggles paper_scene"],
  ["sheets (verbs and session)", "verbs_sheets sheets"],
  ["PDF writer (D6)", "pdf_out"],
  ["PDF reader (L1)", "pdf_in"],
  ["DXF reader (L2)", "dxf_in"],
  ["links and references", "ref_read links links_ref links_dxf links_panel"],
  ["history store (H1 read, H2a write)", "repo repo_write diff versions merge repo_merge"],
  ["session (calls, tools, platform, view-only)", "session calls tool platform view_only capabilities"],
  ["measure", "measure"],
  ["wasm carrier", "wasm_api wasm_stub"],
  ["host server (HTTP, static files, main, platform, fonts)", "server static main platform_native platform_win fonts"],
  ["MCP", "mcp"],
  ["files from the page (FS1: the dialog's queries, POST /stage; the settings file, recent files; SET1: the settings, About)", "fs settings_file settings"],
  ["merged string literals (wasm <internal>)", "<internal>"],
  ["kernel manifold (C++, native only)", "lib:manifold lib:manifoldc lib:Clipper2"],
  ["C++ runtime (libstdc++, libgcc, unwinder)", "lib:stdc++ lib:gcc lib:gcc_eh"],
  ["C runtime start files", "lib:crt"],
  ["linker (headers, dynamic tables, eh_frame_hdr, padding)", "linker"],
  ["lab web UI (Vite: JS, CSS, UI fonts; static files)", "file:web"],
  ["licence and README (static files)", "file:doc"],
  ["web/ccore/ in the archive (a public/ccore leftover; none in CI)", "file:leftover"],
];
const featureOf = new Map();
for (const [f, objs] of FEATURES) for (const o of objs.split(" ")) featureOf.set(o, f);

const errors = [];
const run = (cmd, args, opts = {}) => execFileSync(cmd, args, { encoding: "utf8", maxBuffer: 1 << 28, ...opts });

// ---- lines of C per source ----
const lines = new Map();
for (const d of ["src", "session", "host"])
  for (const f of readdirSync(join(CCORE, d)).filter((x) => x.endsWith(".c")).sort()) {
    const name = f.slice(0, -2);
    let n = readFileSync(join(CCORE, d, f), "utf8").split("\n").length - 1;
    const h = join(CCORE, d, `${name}.h`);
    if (existsSync(h)) n += readFileSync(h, "utf8").split("\n").length - 1;
    lines.set(name, (lines.get(name) ?? 0) + n);
  }

// ---- wasm per object (build.sh wasm-size [view]) ----
function wasmSize(view) {
  const txt = run(join(CCORE, "build.sh"), view ? ["wasm-size", "view"] : ["wasm-size"], { cwd: CCORE });
  const total = Number(/: (\d+) bytes, gzip/.exec(txt)?.[1] ?? 0);
  const per = new Map();
  for (const m of txt.matchAll(/^\| (\S+?)(?:\.o)? +\| +(\d+) \|/gm)) if (m[1] !== "file") per.set(m[1], Number(m[2]));
  return { total, per };
}
const editor = wasmSize(false);
const viewer = wasmSize(true);
const shipped = (f) => (existsSync(join(CCORE, "out/wasm", f)) ? statSync(join(CCORE, "out/wasm", f)).size : 0);

// ---- native per object (GNU ld map of the release link) ----
const MAP = join(CCORE, "out/release/bimble-ccore.map");
const RELEASE_STAGE = join(CCORE, "out/release/stage");
let native = null;
if (existsSync(MAP)) {
  const LOADED = /^\.(text|rodata1?|eh_frame|gcc_except_table|data\.rel\.ro|data|init_array|fini_array|preinit_array|tdata|init|fini)$/;
  const per = new Map();
  const add = (k, n) => per.set(k, (per.get(k) ?? 0) + n);
  let on = false, out = "";
  const who = (file) => {
    if (/\/obj\/[^/]+\.o$/.test(file)) return file.replace(/.*\//, "").slice(0, -2);
    const lib = /lib(manifoldc|manifold|Clipper2|stdc\+\+|gcc_eh|gcc)\.a\(/.exec(file);
    if (lib) return `lib:${lib[1]}`;
    if (/crt[^/]*\.o$|libc_nonshared|\/libc\.|\/libm\./.test(file)) return "lib:crt";
    return `?${file}`;
  };
  for (const line of readFileSync(MAP, "utf8").split("\n")) {
    if (!on) { on = line.startsWith("Linker script and memory map"); continue; }
    const top = /^(\.\S+)/.exec(line);
    if (top) { out = top[1]; continue; }
    if (!LOADED.test(out)) continue;
    let m = /^ (\S+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S.*)$/.exec(line); // " .text.f 0xADDR 0xSIZE file"
    let name = m?.[1];
    if (!m && (m = /^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S.*)$/.exec(line))) m = [m[0], undefined, ...m.slice(1)]; // its second line
    if (!m) continue;
    const size = parseInt(m[3], 16), file = m[4].trim();
    if (!size) continue;
    if (name === "*fill*") { add("linker", size); continue; }
    add(who(file), size);
  }
  const bins = existsSync(RELEASE_STAGE) ? readdirSync(RELEASE_STAGE).map((d) => join(RELEASE_STAGE, d, "bimble-ccore")).filter(existsSync) : [];
  const size = bins.length ? statSync(bins[0]).size : 0;
  const sum = [...per.values()].reduce((a, b) => a + b, 0);
  per.set("linker", (per.get("linker") ?? 0) + Math.max(0, size - sum));
  native = { per, size };
}

// ---- classify ----
const rows = new Map(FEATURES.map(([f]) => [f, { lines: 0, editor: 0, viewer: 0, native: 0, files: 0, objs: [] }]));
const seen = new Set();
const put = (obj, col, n) => {
  const f = featureOf.get(obj);
  if (!f) { errors.push(`object ${obj} (${col}) is in no feature: add it to FEATURES in ccore/tools/weight.mjs`); return; }
  rows.get(f)[col] += n;
  if (!seen.has(obj) && !/^(lib|file):|^linker$|^<internal>$/.test(obj)) rows.get(f).objs.push(obj);
  seen.add(obj);
};
for (const [o, n] of lines) put(o, "lines", n);
for (const [o, n] of editor.per) put(o, "editor", n);
for (const [o, n] of viewer.per) put(o, "viewer", n);
if (native) for (const [o, n] of native.per) put(o, "native", n);

// ---- files: release archive and web build ----
const gz = (p) => gzipSync(readFileSync(p), { level: 9 }).length;
function listFiles(dir, pre = "") {
  const out = [];
  for (const e of readdirSync(join(dir, pre), { withFileTypes: true })) {
    const p = pre ? `${pre}/${e.name}` : e.name;
    if (e.isDirectory()) out.push(...listFiles(dir, p));
    else if (e.isFile()) out.push(p);
  }
  return out.sort();
}
const fileRows = (dir) => listFiles(dir).map((f) => ({ f, raw: statSync(join(dir, f)).size, gz: gz(join(dir, f)) }));
let archive = null;
if (existsSync(RELEASE_STAGE)) {
  const top = readdirSync(RELEASE_STAGE)[0];
  const tgz = join(CCORE, "out/release", `${top}.tar.gz`);
  archive = { name: top, files: fileRows(join(RELEASE_STAGE, top)), tgz: existsSync(tgz) ? statSync(tgz).size : 0 };
}
let web = null;
if (existsSync(join(UI, "node_modules")) && shipped("ccore.wasm") && shipped("ccore-view.wasm")) {
  const dir = join(CCORE, "out/weight/web");
  rmSync(dir, { recursive: true, force: true });
  const rev = (() => { try { return run("git", ["rev-parse", "--short", "HEAD"], { cwd: CCORE }).trim(); } catch { return "dev"; } })();
  run("npx", ["vite", "build", "--base", "./", "--outDir", dir, "--emptyOutDir", "--logLevel", "warn"], { cwd: UI, env: { ...process.env, VITE_CCORE: "1", VITE_CCORE_REV: rev } });
  rmSync(join(dir, "ccore"), { recursive: true, force: true }); // public/ccore leftovers: replaced by what build:ccore copies
  mkdirSync(join(dir, "ccore"));
  for (const [from, to] of [["out/wasm/ccore.wasm", "ccore.wasm"], ["out/wasm/ccore-view.wasm", "ccore-view.wasm"], ["golden/frame.json", "frame.json"]])
    writeFileSync(join(dir, "ccore", to), readFileSync(join(CCORE, from)));
  web = { files: fileRows(dir) };
}

// static files of the archive per feature (the program itself is the native column)
if (archive)
  for (const x of archive.files) {
    if (x.f === "bimble-ccore") continue;
    put(/^web\/ccore\//.test(x.f) ? "file:leftover" : /^web\//.test(x.f) ? "file:web" : "file:doc", "files", x.raw);
  }

// ---- the page ----
const fmt = (n) => (n === null || n === undefined ? "-" : String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ","));
const L = [];
L.push("# Weight of each feature and file (ticket SIZE1)", "");
L.push("Generated by `node ccore/tools/weight.mjs` (`ccore/ci/run.sh unit|full` runs it after `build.sh release`).");
L.push("Do not edit: run the tool. Method and error in the tool's header: wasm columns from the `build.sh wasm-size`");
L.push("link maps, before wasm-opt (the shipped files are smaller: see the totals), native from the release link's GNU ld");
L.push("map (rows sum to the stripped binary exactly; `linker` = what no object owns). Lines: the `.c` and its `.h`.", "");
L.push("## Features", "");
L.push("| feature | lines of C | editor wasm | viewer wasm | native | static files (archive) | objects |", "|---|---:|---:|---:|---:|---:|---|");
const feats = [...rows.entries()].sort((a, b) => (b[1].native + b[1].editor + b[1].files) - (a[1].native + a[1].editor + a[1].files));
const tot = { lines: 0, editor: 0, viewer: 0, native: 0, files: 0 };
for (const [f, r] of feats) {
  for (const k of Object.keys(tot)) tot[k] += r[k];
  L.push(`| ${f} | ${fmt(r.lines)} | ${fmt(r.editor)} | ${fmt(r.viewer)} | ${native ? fmt(r.native) : "-"} | ${fmt(r.files)} | ${r.objs.sort().join(" ")} |`);
}
L.push(`| **total** | ${fmt(tot.lines)} | ${fmt(tot.editor)} | ${fmt(tot.viewer)} | ${native ? fmt(tot.native) : "-"} | ${fmt(tot.files)} | |`, "");
L.push("## Totals and limits", "");
L.push("| artefact | bytes | gzip -9 | limit |", "|---|---:|---:|---|");
const gzw = (f) => (shipped(f) ? gz(join(CCORE, "out/wasm", f)) : 0);
L.push(`| ccore.wasm (editor) | ${fmt(shipped("ccore.wasm"))} | ${fmt(gzw("ccore.wasm"))} | none yet |`);
L.push(`| ccore-view.wasm (viewer, shipped, after wasm-opt) | ${fmt(shipped("ccore-view.wasm"))} | ${fmt(gzw("ccore-view.wasm"))} | ${fmt(VIEW_MAX)} (ADR 0069 as amended; ${shipped("ccore-view.wasm") ? (100 * shipped("ccore-view.wasm") / VIEW_MAX).toFixed(1) : "-"} % used) |`);
L.push(`| editor, sum of the objects (before wasm-opt) | ${fmt(tot.editor)} | | |`);
L.push(`| viewer, sum of the objects (before wasm-opt) | ${fmt(tot.viewer)} | | |`);
if (native) L.push(`| bimble-ccore (native, stripped) | ${fmt(native.size)} | | none yet |`);
if (archive) L.push(`| ${archive.name}.tar.gz (release archive) | ${fmt(archive.tgz)} | | none yet |`);
if (web) L.push(`| web build (build:ccore), ${web.files.length} files | ${fmt(web.files.reduce((a, x) => a + x.raw, 0))} | ${fmt(web.files.reduce((a, x) => a + x.gz, 0))} | none yet |`);
L.push("");
const fileTable = (title, list, note) => {
  L.push(`## ${title}`, "");
  if (note) L.push(note, "");
  L.push("| file | bytes | gzip -9 |", "|---|---:|---:|");
  for (const x of [...list].sort((a, b) => b.raw - a.raw || (a.f < b.f ? -1 : 1))) L.push(`| ${x.f} | ${fmt(x.raw)} | ${fmt(x.gz)} |`);
  L.push(`| **total** | ${fmt(list.reduce((a, x) => a + x.raw, 0))} | ${fmt(list.reduce((a, x) => a + x.gz, 0))} |`, "");
};
if (archive) fileTable(`Files of the release archive (${archive.name})`, archive.files.map((x) => ({ ...x, f: x.f.replace(/^web\/assets\/(.*)-[A-Za-z0-9_-]{8}(\.\w+)$/, "web/assets/$1-*$2") })),
  "web/ccore/ appears only when web-ui/public/ccore/ held a copy (copy-ccore.mjs, the lab tests) at build time: the CI archive has none.");
else L.push("## Files of the release archive", "", "No out/release/stage/: run `build.sh release`.", "");
if (web) fileTable("Files of the web build (npm run build:ccore)", web.files.map((x) => ({ ...x, f: x.f.replace(/^assets\/(.*)-[A-Za-z0-9_-]{8}(\.\w+)$/, "assets/$1-*$2") })));
else L.push("## Files of the web build", "", "Skipped: needs web-ui/node_modules and out/wasm/ccore.wasm, ccore-view.wasm.", "");
const page = L.join("\n");

// ---- the change against the committed file ----
const parse = (txt) => new Map([...txt.matchAll(/^\| ([^|*][^|]*?) \| ([\d,-]+) \| ([\d,-]+) \| ([\d,-]+) \| ([\d,-]+) \| ([\d,-]+) \|/gm)].map((m) => [m[1], m.slice(2, 7).map((v) => Number(v.replace(/,/g, "")) || 0)]));
const old = existsSync(OUT) ? parse(readFileSync(OUT, "utf8")) : new Map();
const now = parse(page);
const COLS = ["lines", "editor wasm", "viewer wasm", "native", "static files"];
let changes = 0;
for (const [f, v] of now) {
  const o = old.get(f) ?? [0, 0, 0, 0, 0];
  const d = v.map((x, i) => x - o[i]).map((x, i) => (x ? `${COLS[i]} ${x > 0 ? "+" : ""}${x}` : "")).filter(Boolean);
  if (d.length) { changes++; console.log(`weight: ${f}: ${d.join(", ")}`); }
}
for (const f of old.keys()) if (!now.has(f)) console.log(`weight: ${f}: gone`), changes++;
console.log(`weight: ${changes} feature rows changed against ${relative(process.cwd(), OUT)}`);
writeFileSync(OUT, page + "\n");
if (!native) console.log("weight: no native map (out/release/bimble-ccore.map): the native column is -");
if (errors.length) {
  for (const e of errors) console.log(`FAIL weight: ${e}`);
  process.exit(1);
}
console.log(`weight: ${seen.size} objects classified in ${FEATURES.length} features, 0 unclassified`);
