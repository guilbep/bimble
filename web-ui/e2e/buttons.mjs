// #79 BD button audit of the Vue lab, French UI, 1440 x 900.
// node e2e/buttons.mjs --lab <base-url> [--screen A2,D1] [--jobs 8]
// On every screen of design-check/screens.json with a lab deep link, clicks each visible, enabled
// button / [role=button|menuitem|tab] / a[href], each in a fresh load of the deep link, and says:
//   works: URL, model revision, a wasm call, a browser dialog, file chooser, download, new page, or an
//          element changed (per-element diff; elements that change in 300 ms without a click are noise)
//   active: nothing changed, but the control is already the current one (aria-selected/pressed/current, .on/.active)
//   dead:  nothing of that, no error        error: pageerror or console.error during the click
//   blocked: the click could not land (moved away, timed out): not counted as dead
//   covered: under a modal backdrop or another layer when listed: counted, not clicked
// Disabled controls and data-missing ones (known: Rust call missing) are counted, not clicked.
// The wasm glue (/bimble/bimble_web.js) is rewritten in flight to count calls into Rust and to read
// the revision from on_change: src/ is untouched. Full result: design-check/out/buttons.json.
// Exit 0 when it ran (an audit, not a gate), 2 on usage error.
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "@playwright/test";

const argv = process.argv.slice(2);
const arg = (n) => (argv.includes(n) ? argv[argv.indexOf(n) + 1] : undefined);
const lab = arg("--lab");
const jobs = Number(arg("--jobs") ?? 8);
if (!lab || !(jobs >= 1)) { console.error("usage: buttons.mjs --lab <base-url> [--screen A2,D1] [--jobs 8]"); process.exit(2); }
const only = arg("--screen")?.split(",");

const here = dirname(fileURLToPath(import.meta.url));
const screens = JSON.parse(readFileSync(join(here, "../design-check/screens.json"), "utf8"));
delete screens._doc;
const outDir = join(here, "../design-check/out"); // e2e/out is not ignored by git; design-check/out is

// Calls into Rust that do something (query is read-only and polled: not counted).
const CALLS = ["action", "apply", "dismiss_belongs", "open", "redo", "select", "set_placing", "standards_action", "tab", "undo", "verb"];
function instrument(js) {
  for (const f of CALLS) {
    js = js.replace(`export function ${f}(`,
      `export function ${f}(...a) { window.__bbCalls = (window.__bbCalls || 0) + 1; return __bb_${f}(...a); }\nfunction __bb_${f}(`);
  }
  return js.replace("export function on_change(",
    "export function on_change(cb) { return __bb_on_change((j) => { try { window.__bbRev = JSON.parse(j).rev; } catch {} cb(j); }); }\nfunction __bb_on_change(");
}

const CANDIDATES = 'button, [role=button], [role=menuitem], [role=tab], a[href]';
// In the page: the candidates in document order, with what we need to name and count them.
const listControls = (sel) => [...document.querySelectorAll(sel)].map((el, i) => {
  const r = el.getBoundingClientRect();
  const cs = getComputedStyle(el);
  const visible = r.width > 0 && r.height > 0 && cs.visibility !== "hidden" && cs.display !== "none" && !el.closest("[hidden],[inert]");
  const active = ["aria-selected", "aria-pressed"].some((a) => el.getAttribute(a) === "true")
    || (el.hasAttribute("aria-current") && el.getAttribute("aria-current") !== "false")
    || el.classList.contains("on") || el.classList.contains("active");
  const name = (el.getAttribute("aria-label") || el.getAttribute("title") || el.textContent || "").replace(/\s+/g, " ").trim().slice(0, 60);
  const bb = el.closest("[data-bb]")?.getAttribute("data-bb");
  const short = bb ? `[data-bb="${bb}"]` : `${el.tagName.toLowerCase()}${el.className && typeof el.className === "string" ? "." + el.className.trim().split(/\s+/).slice(0, 2).join(".") : ""}`;
  return {
    i, visible, name, sel: short, active,
    disabled: el.disabled === true || el.getAttribute("aria-disabled") === "true",
    missing: el.closest("[data-missing]") !== null,
    // Under a modal backdrop or another layer: not clickable by a user, so not clicked.
    covered: (() => { const h = document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2); return h !== null && !el.contains(h); })(),
  };
});
// Per-element state (tag, attributes, own text nodes) keyed by a path, kept in the page as
// window.__bbSnap[key]. Left out: the wasm canvas, eframe's text-agent input, the status bar's text.
const snapshot = (key) => {
  const m = {};
  let h = 0;
  const walk = (el, path, statusText) => {
    if (el.id === "bimble" || (el.tagName === "INPUT" && el.previousElementSibling?.id === "bimble")) return;
    const status = statusText || (el.getAttribute("data-bb") ?? "").startsWith("status");
    const attrs = [...el.attributes].map((a) => `${a.name}=${a.value}`).sort().join(" ");
    const text = status ? "" : [...el.childNodes].filter((n) => n.nodeType === 3).map((n) => n.data).join("").trim();
    const v = `${attrs}|${text}`;
    m[path] = v;
    for (let i = 0; i < v.length; i++) h = (h * 31 + v.charCodeAt(i)) | 0;
    [...el.children].forEach((c, i) => {
      const bb = c.getAttribute("data-bb");
      walk(c, `${path}>${c.tagName.toLowerCase()}${bb ? `[data-bb="${bb}"]` : ""}:${i}`, status);
    });
  };
  walk(document.body, "body", false);
  (window.__bbSnap ??= {})[key] = m;
  return { href: location.href, dom: h, rev: window.__bbRev ?? null, calls: window.__bbCalls ?? 0 };
};
// Paths that changed from a to b and did not change from n0 to n1 (noise); added or removed count.
const domDiff = ([a, b, n0, n1]) => {
  const S = window.__bbSnap, d = (x, y) => [...new Set([...Object.keys(S[x]), ...Object.keys(S[y])])].filter((k) => S[x][k] !== S[y][k]);
  const noise = new Set(n0 ? d(n0, n1) : []);
  return d(a, b).filter((k) => !noise.has(k));
};

// Fetched and rewritten once; a fetch per page load raced aborted navigations (ECONNRESET).
const glue = instrument(await (await fetch(new URL("bimble/bimble_web.js", lab))).text());
const browser = await chromium.launch({
  channel: "chrome",
  args: ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"],
});

async function newPage() {
  const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, locale: "fr-FR", acceptDownloads: true });
  await ctx.addInitScript(() => { try { localStorage.setItem("bimble.lang", "fr"); } catch {} });
  await ctx.route("**/bimble/bimble_web.js", (route) => route.fulfill({ contentType: "text/javascript", body: glue }).catch(() => {}));
  const page = await ctx.newPage();
  const ev = { errors: [], other: [] };
  page.on("pageerror", (e) => ev.errors.push(`pageerror: ${e.message}`));
  page.on("console", (m) => { if (m.type() === "error") ev.errors.push(`console.error: ${m.text()}`); });
  // beforeunload (unsaved model) is accepted, else the next goto aborts; it is not a click outcome.
  page.on("dialog", (d) => {
    if (d.type() === "beforeunload") return void d.accept().catch(() => {});
    ev.other.push(`dialog ${d.type()}`); d.dismiss().catch(() => {});
  });
  page.on("filechooser", () => ev.other.push("filechooser"));
  page.on("download", () => ev.other.push("download"));
  ctx.on("page", (p) => { if (p !== page) { ev.other.push("new page"); p.close().catch(() => {}); } });
  return { page, ev };
}

async function open(page, path) {
  await page.goto(new URL(path, lab).href, { waitUntil: "load" });
  await page.waitForFunction(() => document.documentElement.dataset.bbReady === "1", null, { timeout: 30000 });
  await page.waitForTimeout(300);
}

// One click in a fresh load: returns the classification and its evidence.
async function probe(w, path, c) {
  const { page, ev } = w;
  await open(page, path);
  const now = await page.evaluate(listControls, CANDIDATES);
  const same = now[c.i];
  if (!same || same.name !== c.name) return { result: "blocked", why: "control moved after reload" };
  ev.errors.length = 0; ev.other.length = 0;
  await page.mouse.move(0, 0);
  // Late loads (status, thumbnails) change the DOM on their own: wait until it holds still (max 3 s).
  let before = await page.evaluate(snapshot, "s1");
  for (let t = 0; t < 20; t++) {
    await page.waitForTimeout(150);
    const again = await page.evaluate(snapshot, "s1");
    if (again.dom === before.dom && again.rev === before.rev && again.calls === before.calls) break;
    before = again;
  }
  // S0 -> S1 300 ms apart without a click: what changes there is noise for this click.
  await page.evaluate(snapshot, "s0");
  await page.waitForTimeout(300);
  before = await page.evaluate(snapshot, "s1");
  try {
    await page.locator(CANDIDATES).nth(c.i).click({ timeout: 2000 });
  } catch (e) {
    const lines = e.message.replace(/\x1b\[[0-9;]*m/g, "").split("\n");
    return { result: "blocked", why: (lines.find((l) => /intercepts|outside|not visible|detached/.test(l)) ?? lines[0]).trim() };
  }
  await page.waitForTimeout(300);
  const after = await page.evaluate(snapshot, "s2").catch(() => null); // null: the page navigated away mid-read
  const changed = [];
  if (!after || after.href !== before.href) changed.push("url");
  if (after && after.rev !== before.rev) changed.push("rev");
  if (after && after.calls !== before.calls) changed.push(`call x${after.calls - before.calls}`);
  changed.push(...ev.other);
  if (after) {
    const dom = await page.evaluate(domDiff, ["s1", "s2", "s0", "s1"]).catch(() => []);
    if (dom.length) changed.push(`dom:${dom[0].split(">").slice(-2).join(">")} (+${dom.length - 1})`); // last 2 path steps
  }
  if (ev.errors.length) return { result: "error", changed, errors: [...ev.errors] };
  if (changed.length) return { result: "works", changed };
  return { result: c.active ? "active" : "dead", changed };
}

const all = [];
const total = { screens: 0, controls: 0, works: 0, active: 0, dead: 0, error: 0, blocked: 0, covered: 0, disabled: 0, missing: 0 };
const workers = await Promise.all(Array.from({ length: jobs }, newPage));
for (const [code, s] of Object.entries(screens)) {
  if (!s.lab || (only && !only.includes(code))) continue;
  total.screens++;
  const w0 = workers[0];
  await open(w0.page, s.lab);
  // DOM noise check: the same snapshot twice, 300 ms apart, without a click.
  await w0.page.evaluate(snapshot, "s0");
  await w0.page.waitForTimeout(300);
  await w0.page.evaluate(snapshot, "s1");
  const idleNoise = (await w0.page.evaluate(domDiff, ["s0", "s1"])).length > 0;
  const listed = (await w0.page.evaluate(listControls, CANDIDATES)).filter((c) => c.visible);
  const sum = { works: 0, active: 0, dead: 0, error: 0, blocked: 0, covered: 0, disabled: 0, missing: 0 };
  const todo = [];
  for (const c of listed) {
    if (c.missing) sum.missing++;
    if (c.disabled) { sum.disabled++; continue; }
    if (c.covered) { sum.covered++; continue; }
    todo.push(c);
  }
  const results = new Array(todo.length);
  let next = 0;
  await Promise.all(workers.map(async (w) => {
    while (next < todo.length) {
      const k = next++;
      results[k] = { screen: code, ...todo[k], ...(await probe(w, s.lab, todo[k]).catch((e) => ({ result: "blocked", why: e.message.split("\n")[0] }))) };
    }
  }));
  for (const r of results) {
    sum[r.result]++;
    if (r.result !== "active") {
      const detail = r.errors?.join(" | ") ?? r.why ?? (r.result === "works" ? r.changed.join(" | ") : "");
      console.log(`${r.result.padEnd(7)} ${code} "${r.name}" ${r.sel}${r.missing ? " (data-missing)" : ""}${detail ? " -- " + detail : ""}`);
    }
    all.push(r);
  }
  const n = todo.length + sum.disabled + sum.covered;
  console.log(`== ${code} ${s.ref}: controls ${n}, works ${sum.works}, active ${sum.active}, dead ${sum.dead}, error ${sum.error}, blocked ${sum.blocked}, covered ${sum.covered}, disabled ${sum.disabled} (data-missing ${sum.missing})${idleNoise ? " [DOM changes when idle: those elements are ignored per click]" : ""}`);
  total.controls += n;
  for (const k of ["works", "active", "dead", "error", "blocked", "covered", "disabled", "missing"]) total[k] += sum[k];
}
console.log(`screens ${total.screens}, controls ${total.controls}, works ${total.works}, active ${total.active}, dead ${total.dead}, error ${total.error}, disabled ${total.disabled} (data-missing ${total.missing}), blocked ${total.blocked}, covered ${total.covered}`);
mkdirSync(outDir, { recursive: true });
writeFileSync(join(outDir, "buttons.json"), JSON.stringify({ total, controls: all }, null, 1));
await browser.close();
process.exit(0);
