// #79 AC/AD accessibility check of the Vue lab, French UI, 1440 x 900.
// node e2e/a11y.mjs --lab <base-url> [--screen A2,D1]
//  1. axe-core on every screen of design-check/screens.json with a lab deep link: prints each
//     violation by rule id, impact, count and the first elements.
//  2. Focus in dialogs (AD): the File menu opened with the keyboard and the palette (Ctrl K) take
//     focus, keep it on Tab x30, and give it back to the File button on Esc.
// Exit 1 when a "serious" or "critical" violation exists or a focus check fails, 2 on usage error.
// axe-core: no new dependency; the copy inside Lighthouse 12.6.1 (the perf tool, run with npx).
import { execFileSync } from "node:child_process";
import { readFileSync, realpathSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "@playwright/test";

const argv = process.argv.slice(2);
const arg = (n) => (argv.includes(n) ? argv[argv.indexOf(n) + 1] : undefined);
const lab = arg("--lab");
if (!lab) { console.error("usage: a11y.mjs --lab <base-url> [--screen A2,D1]"); process.exit(2); }
const only = arg("--screen")?.split(",");

const here = dirname(fileURLToPath(import.meta.url));
const screens = JSON.parse(readFileSync(join(here, "../design-check/screens.json"), "utf8"));
delete screens._doc;
// ponytail: axe-core found through Lighthouse's npx install; add axe-core as a devDependency if this moves to CI.
const lhBin = execFileSync("npm", ["exec", "-y", "--package=lighthouse@12.6.1", "--", "sh", "-c", "command -v lighthouse"], { encoding: "utf8" }).trim();
const axePath = join(dirname(realpathSync(lhBin)), "../../axe-core/axe.min.js");

const browser = await chromium.launch({
  channel: "chrome",
  args: ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"],
});
const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, locale: "fr-FR" });
await ctx.addInitScript(() => { try { localStorage.setItem("bimble.lang", "fr"); } catch {} });
const page = await ctx.newPage();
page.on("pageerror", (e) => console.log("pageerror:", e.message));

async function open(path) {
  await page.goto(new URL(path, lab).href, { waitUntil: "load" });
  await page.waitForFunction(() => document.documentElement.dataset.bbReady === "1", null, { timeout: 30000 });
  await page.waitForTimeout(300);
}

let bad = 0;
for (const [code, s] of Object.entries(screens)) {
  if (!s.lab || (only && !only.includes(code))) continue;
  await open(s.lab);
  await page.addScriptTag({ path: axePath });
  const v = await page.evaluate(async () =>
    (await window.axe.run(document, { resultTypes: ["violations"] })).violations.map((x) => ({
      id: x.id, impact: x.impact, n: x.nodes.length, at: x.nodes.slice(0, 3).map((n) => n.target.join(" ")),
    })));
  const sev = v.filter((x) => x.impact === "serious" || x.impact === "critical").length;
  bad += sev;
  console.log(`${sev ? "FAIL" : "OK  "} ${code} ${s.ref}: ${v.length} rules, ${sev} serious/critical`);
  for (const x of v) console.log(`     ${x.impact.padEnd(8)} ${x.id} x${x.n}: ${x.at.join(" | ")}`);
}

// AD: focus in dialogs and menus.
const fileBtn = '[data-bb="quickbar-file"]';
const inBackdrop = () => page.evaluate(() => !!document.activeElement?.closest(".bb-backdrop"));
const onFile = () => page.evaluate((s) => document.activeElement === document.querySelector(s), fileBtn);
async function focusCheck(name, openIt) {
  await page.focus(fileBtn);
  await openIt();
  await page.waitForSelector(".bb-backdrop");
  let inside = await inBackdrop();
  for (let i = 0; i < 30 && inside; i++) { await page.keyboard.press("Tab"); inside = await inBackdrop(); }
  await page.keyboard.press("Escape");
  const back = await onFile();
  const ok = inside && back;
  if (!ok) bad++;
  console.log(`${ok ? "OK  " : "FAIL"} focus ${name}: stays inside after Tab x30 ${inside}, back on File button ${back}`);
}
await open("?url=/frame.bimble");
await focusCheck("file-menu", () => page.keyboard.press("Enter"));
await focusCheck("palette", () => page.keyboard.press("Control+k"));

await browser.close();
process.exit(bad ? 1 : 0);
