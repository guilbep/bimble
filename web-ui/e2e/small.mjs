// Small screens (ADR 0064): the lab at a phone, a phone on its side, a tablet and a small laptop.
// node e2e/small.mjs --lab <base-url> [--shots <dir>]
// Checks at each size: nothing is wider than the window, the small frame shows below 1 024 px and
// the desktop frame from 1 024 px; on the phone: select a beam, rename it, write a note, read both
// back from Rust (query "element"), undo twice. Exit 1 on a failed check.
import { mkdirSync } from "node:fs";
import { chromium } from "@playwright/test";

const argv = process.argv.slice(2);
const arg = (n) => (argv.includes(n) ? argv[argv.indexOf(n) + 1] : undefined);
const lab = arg("--lab");
if (!lab) { console.error("usage: small.mjs --lab <base-url> [--shots <dir>]"); process.exit(2); }
const shots = arg("--shots");
if (shots) mkdirSync(shots, { recursive: true });

const SIZES = [
  { name: "phone", w: 375, h: 812, small: true, bar: "mbar" },
  { name: "phone-landscape", w: 812, h: 375, small: true, bar: "rail" },
  { name: "tablet", w: 820, h: 1180, small: true, bar: "rail" },
  { name: "laptop", w: 1280, h: 720, small: false, bar: null },
];
let failed = 0;
const check = (ok, what) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) failed++; };

const browser = await chromium.launch({ channel: "chrome", args: ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"] });
for (const s of SIZES) {
  const ctx = await browser.newContext({ viewport: { width: s.w, height: s.h }, locale: "en-GB", hasTouch: s.small });
  const page = await ctx.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(String(e)));
  await page.goto(`${lab}?url=/frame.bimble&select=first:IfcBeam`);
  await page.waitForFunction(() => document.documentElement.dataset.bbReady === "1", null, { timeout: 60000 });
  const m = await page.evaluate(() => ({
    scroll: document.documentElement.scrollWidth,
    top: !!document.querySelector('[data-bb="small-top"]'),
    quick: !!document.querySelector('[data-bb="quickbar"]'),
    bar: document.querySelector('[data-bb="small-bar"]')?.className.split(" ")[0] ?? null,
    canvas: (() => { const r = document.getElementById("bimble").getBoundingClientRect(); return [Math.round(r.width), Math.round(r.height)]; })(),
    tiny: [...document.querySelectorAll('[data-bb^="small-"] button, button[data-bb^="small-"]')].filter((b) => { const r = b.getBoundingClientRect(); return r.width && (r.width < 40 || r.height < 42); }).length,
  }));
  check(m.scroll <= s.w, `${s.name} ${s.w}x${s.h}: not wider than the window (${m.scroll})`);
  check(m.top === s.small && m.quick === !s.small, `${s.name}: ${s.small ? "small" : "desktop"} frame`);
  check(m.bar === s.bar, `${s.name}: tools as ${s.bar} (${m.bar})`);
  check(m.canvas[0] > 200 && m.canvas[1] > 150, `${s.name}: the view keeps ${m.canvas.join(" x ")} px`);
  if (s.small) check(m.tiny === 0, `${s.name}: no touch target under 40 x 42 px (${m.tiny})`);
  if (s.small) {
    // The selection opened the Objects sheet (deep link select=).
    await page.locator('[data-bb="small-tool-objects"]').click();
    if (!(await page.locator('[data-bb="small-props"]').isVisible())) await page.locator('[data-bb="small-tool-objects"]').click();
    if (shots) await page.screenshot({ path: `${shots}/${s.name}-objects.png` });
    const pill = await page.locator('[data-bb="small-props"]').boundingBox();
    const barTop = s.bar === "mbar" ? (await page.locator('[data-bb="small-bar"]').boundingBox()).y : s.h;
    check(pill.y + pill.height <= barTop && pill.x + pill.width <= s.w, `${s.name}: the Properties button is in view (${Math.round(pill.y + pill.height)} <= ${Math.round(barTop)})`);
    await page.locator('[data-bb="small-close"]').click();
    check(!(await page.locator('[data-bb="small-sheet"]').isVisible()), `${s.name}: the close button closes the sheet`);
    await page.locator('[data-bb="small-tool-objects"]').click();
    await page.locator('[data-bb="small-props"]').click();
    const name = page.locator('[data-bb="small-name"]');
    await name.fill("B12a");
    await name.press("Enter");
    const note = page.locator('[data-bb="small-note"]');
    await note.fill("  check cover ");
    await note.press("Enter");
    await page.waitForTimeout(500);
    if (shots) await page.screenshot({ path: `${shots}/${s.name}-props.png` });
    const got = await page.evaluate(async () => {
      const w = await import(new URL("bimble/bimble_web.js", document.baseURI).href);
      const id = JSON.parse(w.query("state", "{}")).selection?.[0] ?? JSON.parse(w.query("elements", JSON.stringify({ class: "IfcBeam", limit: 1 })))[0].id;
      const read = () => { const e = JSON.parse(w.query("element", JSON.stringify({ id }))); return { name: e.name, note: e.properties.Note ?? null }; };
      const after = read();
      w.undo(); w.undo();
      return { after, undone: read() };
    });
    check(got.after.name === "B12a" && got.after.note === "check cover", `${s.name}: rename and note reached Rust (${JSON.stringify(got.after)})`);
    check(got.undone.name !== "B12a" && got.undone.note === null, `${s.name}: two undos bring both back (${JSON.stringify(got.undone)})`);
    await page.waitForTimeout(300);
    const scroll2 = await page.evaluate(() => document.documentElement.scrollWidth);
    check(scroll2 <= s.w, `${s.name}: properties sheet not wider than the window (${scroll2})`);
    for (const tool of ["views", "cut", "measure", "more"]) {
      await page.locator(`[data-bb="small-tool-${tool}"]`).click();
      await page.waitForTimeout(150);
      check(await page.locator('[data-bb="small-sheet"]').isVisible(), `${s.name}: ${tool} sheet opens`);
      if (shots) await page.screenshot({ path: `${shots}/${s.name}-${tool}.png` });
    }
  } else if (shots) await page.screenshot({ path: `${shots}/${s.name}.png` });
  check(errors.length === 0, `${s.name}: no page error ${errors.join(" | ")}`);
  await ctx.close();
}
await browser.close();
process.exit(failed ? 1 : 0);
