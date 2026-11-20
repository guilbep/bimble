// Pixel-perfect check: wireframes v01 reference vs the Vue lab.
// node design-check/compare.mjs --screen D1[,B2|all] --lang fr|en --lab <base-url|none|ref:<lang>/<File>.html>
//   none          reference against itself (self-check, must be 0 / 0 / 0)
//   ref:<path>    another reference page as the "lab" (uses reference selectors; proves the check fails)
//   <base-url>    the Vue lab; screens.json "lab" is appended; landmarks found by [data-bb]
// Exit 0 when every screen matches exactly, 1 on any difference, 2 on usage error.
import { createServer } from "node:http";
import { readFileSync, mkdirSync, writeFileSync } from "node:fs";
import { join, resolve, normalize, extname, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "@playwright/test";
import pixelmatch from "pixelmatch";
import { PNG } from "pngjs";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "../../docs/design/wireframes-v01");
const outDir = join(here, "out");
const landmarks = JSON.parse(readFileSync(join(here, "landmarks.json"), "utf8"));
const screens = JSON.parse(readFileSync(join(here, "screens.json"), "utf8"));
delete landmarks._doc;
delete screens._doc;
const STYLES = ["color", "background-color", "border-top", "border-right", "border-bottom", "border-left",
  "font-family", "font-size", "font-weight", "line-height", "padding", "border-radius"];
const VIEW = { width: 1440, height: 900 };

const argv = process.argv.slice(2);
const args = {};
for (let i = 0; i < argv.length; i += 2) args[argv[i].replace(/^--/, "")] = argv[i + 1];
const lang = args.lang;
const lab = args.lab;
const codes = args.screen === "all" ? Object.keys(screens) : (args.screen || "").split(",").filter(Boolean);
if (!["fr", "en"].includes(lang) || !lab || !codes.length || codes.some((c) => !screens[c])) {
  console.error("usage: compare.mjs --screen <code[,code]|all> --lang fr|en --lab <base-url|none|ref:<lang>/<File>.html>");
  process.exit(2);
}

// Tiny static server for the reference, so relative fonts load over http.
const TYPES = { ".html": "text/html; charset=utf-8", ".woff2": "font/woff2", ".css": "text/css", ".svg": "image/svg+xml", ".png": "image/png" };
const server = createServer((req, res) => {
  const p = normalize(join(root, decodeURIComponent(new URL(req.url, "http://x").pathname)));
  if (!p.startsWith(root)) return res.writeHead(403).end();
  try {
    const body = readFileSync(p);
    res.writeHead(200, { "content-type": TYPES[extname(p)] || "application/octet-stream" }).end(body);
  } catch {
    res.writeHead(404).end();
  }
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const base = `http://127.0.0.1:${server.address().port}/`;

async function capture(page, url, useRef) {
  await page.goto(url, { waitUntil: "load" });
  // The lab draws real state only after the wasm loaded the model (bimble.ts sets bbReady).
  if (!url.startsWith(base)) {
    await page.waitForFunction(() => document.documentElement.dataset.bbReady === "1", null, { timeout: 30000 });
    await page.waitForTimeout(300);
  }
  await page.evaluate(() => document.fonts.ready);
  const sel = Object.fromEntries(Object.entries(landmarks).map(([n, l]) => [n, useRef ? l.ref : l.lab]));
  const marks = await page.evaluate(([sel, props]) => {
    const out = {};
    for (const [name, s] of Object.entries(sel)) {
      const el = s && document.querySelector(s);
      if (!el) continue;
      const r = el.getBoundingClientRect();
      const cs = getComputedStyle(el);
      out[name] = { box: [r.x, r.y, r.width, r.height], style: Object.fromEntries(props.map((p) => [p, cs.getPropertyValue(p)])) };
    }
    return out;
  }, [sel, STYLES]);
  return { marks, png: PNG.sync.read(await page.screenshot()) };
}

function blank(png, box) {
  const [x0, y0] = [Math.max(0, Math.floor(box[0])), Math.max(0, Math.floor(box[1]))];
  const [x1, y1] = [Math.min(png.width, Math.ceil(box[0] + box[2])), Math.min(png.height, Math.ceil(box[1] + box[3]))];
  for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) png.data.fill(255, (y * png.width + x) * 4, (y * png.width + x) * 4 + 4);
}

const browser = await chromium.launch({ channel: "chrome" });
const ctx = await browser.newContext({ viewport: VIEW, deviceScaleFactor: 1 });
// The lab reads its language from localStorage (web-ui/src/i18n): set it to the reference's.
await ctx.addInitScript((l) => { try { localStorage.setItem("bimble.lang", l); } catch {} }, lang);
const page = await ctx.newPage();
mkdirSync(outDir, { recursive: true });
let failed = false;

for (const code of codes) {
  const scr = screens[code];
  const refPath = `${lang}/${scr.ref}`;
  const labIsRef = lab === "none" || lab.startsWith("ref:");
  const labUrl = lab === "none" ? base + refPath : lab.startsWith("ref:") ? base + lab.slice(4) : new URL(scr.lab, lab).href;
  const ref = await capture(page, base + refPath, true);
  const got = await capture(page, labUrl, labIsRef);

  const boxes = [], styles = [], skipped = [];
  for (const name of Object.keys(landmarks)) {
    const a = ref.marks[name], b = got.marks[name];
    if (!a && !b) { skipped.push(name); continue; }
    if (!a || !b) { boxes.push({ name, ref: a?.box ?? null, lab: b?.box ?? null }); continue; }
    if (a.box.some((v, i) => v !== b.box[i])) boxes.push({ name, ref: a.box, lab: b.box });
    for (const p of STYLES) if (a.style[p] !== b.style[p]) styles.push({ name, prop: p, ref: a.style[p], lab: b.style[p] });
  }

  for (const m of scr.mask) for (const side of [ref, got]) {
    for (const src of [ref, got]) if (src.marks[m]) blank(side.png, src.marks[m].box); // union of both rects
  }
  const { width, height } = ref.png;
  const sameSize = got.png.width === width && got.png.height === height;
  const diff = new PNG({ width, height });
  const total = width * height;
  const pixels = sameSize ? pixelmatch(ref.png.data, got.png.data, diff.data, width, height, { threshold: 0, includeAA: true }) : total;
  const side = new PNG({ width: width * 2, height });
  PNG.bitblt(ref.png, side, 0, 0, width, height, 0, 0);
  if (sameSize) PNG.bitblt(got.png, side, 0, 0, width, height, width, 0);

  const stem = join(outDir, `${code}-${lang}`);
  for (const [k, png] of Object.entries({ ref: ref.png, lab: got.png, diff, side })) writeFileSync(`${stem}-${k}.png`, PNG.sync.write(png));
  const pct = (100 * pixels / total).toFixed(3);
  writeFileSync(`${stem}.json`, JSON.stringify({ screen: code, lang, ref: refPath, lab: labUrl.replace(base, "ref:"), viewport: VIEW,
    masked: scr.mask, skipped, boxes, styles, pixels: { count: pixels, total, percent: Number(pct) } }, null, 2) + "\n");

  const bad = boxes.length || styles.length || pixels;
  failed ||= Boolean(bad);
  console.log(`${bad ? "FAIL" : "OK  "} ${code} ${lang}: boxes off ${boxes.length}, styles off ${styles.length}, pixels off ${pixels} (${pct}%)` +
    (skipped.length ? ` · skipped (missing on both): ${skipped.join(", ")}` : ""));
  for (const b of boxes) console.log(`  box ${b.name}: ref ${JSON.stringify(b.ref)} lab ${JSON.stringify(b.lab)}`);
  for (const s of styles.slice(0, 20)) console.log(`  style ${s.name} ${s.prop}: ref ${s.ref} | lab ${s.lab}`);
  if (styles.length > 20) console.log(`  … ${styles.length - 20} more in ${stem}.json`);
}

await browser.close();
server.close();
process.exit(failed ? 1 : 0);
