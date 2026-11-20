// B1 end to end: place a slab with two real mouse clicks on the plan, then undo it.
// node e2e/place-slab.mjs --lab <base-url> [--headed] [--one-click]
//   --one-click  skip the second click (proves the check fails: exit 1)
// Exit 0 when one IfcSlab is added with the clicked corners and Undo removes it, 1 otherwise.
import { chromium } from "@playwright/test";

const argv = process.argv.slice(2);
const lab = argv[argv.indexOf("--lab") + 1];
if (!argv.includes("--lab") || !lab) {
  console.error("usage: place-slab.mjs --lab <base-url> [--headed] [--one-click]");
  process.exit(2);
}
const url = new URL("?url=/frame.bimble&tab=concrete&view=plan:1&action=place_slab", lab).href;

const browser = await chromium.launch({
  channel: "chrome", // as design-check: the installed Chrome, no browser download
  headless: !argv.includes("--headed"),
  // ponytail: software WebGL for GPU-less CI; on macOS Chrome's new headless passes without it too.
  args: ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"],
});
const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
page.on("pageerror", (e) => console.log("pageerror:", e.message));
const q = (name, args = "") =>
  page.evaluate(async ([n, a]) => {
    const w = await import("/bimble/bimble_web.js");
    return JSON.parse(w.query(n, typeof a === "string" ? a : JSON.stringify(a)));
  }, [name, args]);
const slabs = async () => (await q("elements", { class: "IfcSlab" })).length;

let ok = false;
try {
  await page.goto(url, { waitUntil: "load" });
  await page.waitForFunction(() => document.documentElement.dataset.bbReady === "1", null, { timeout: 30000 });
  await page.waitForTimeout(500);
  console.log("placing:", JSON.stringify(await q("placing")));
  const beforeIds = new Set((await q("elements", { class: "IfcSlab" })).map((e) => e.id));
  const before = beforeIds.size;

  const box = await page.locator("canvas#bimble").boundingBox();
  const at = [[0.4, 0.4], [0.55, 0.6]].map(([fx, fy]) => [box.x + box.width * fx, box.y + box.height * fy]);
  const clicks = argv.includes("--one-click") ? at.slice(0, 1) : at;
  let first = null;
  for (const [x, y] of clicks) {
    // egui reads the pointer from move events: move there in steps, wait a frame, then press.
    await page.mouse.move(x, y, { steps: 8 });
    await page.waitForTimeout(150);
    await page.mouse.down();
    await page.waitForTimeout(80);
    await page.mouse.up();
    await page.waitForTimeout(250);
    // The first click is a point of the placing state: the slab must have it as a corner.
    first ??= (await q("placing")).points[0] ?? null;
  }
  console.log("first corner (placing.points[0]):", JSON.stringify(first));
  await page.waitForTimeout(300);

  const after = await q("elements", { class: "IfcSlab" });
  const added = after.filter((e) => !beforeIds.has(e.id));
  console.log(`IfcSlab before ${before}, after ${after.length}`);
  let corner = false;
  if (added.length === 1) {
    const el = await q("element", { id: added[0].id });
    console.log("new slab:", JSON.stringify({ id: el.id, storey: el.storey_name, placement: el.placement, params: el.generator_params }));
    const [ox, oy] = el.placement.origin;
    const pts = el.generator_params.outline.map(([x, y]) => [x + ox, y + oy]);
    // ponytail: corner match within 1 mm in plan; z is the storey's, not checked
    corner = !!first && pts.some(([x, y]) => Math.abs(x - first[0]) < 1 && Math.abs(y - first[1]) < 1);
    console.log("first corner in outline:", corner);
  }
  await page.evaluate(async () => (await import("/bimble/bimble_web.js")).undo());
  await page.waitForTimeout(300);
  const undone = await slabs();
  console.log(`IfcSlab after undo ${undone}`);
  ok = added.length === 1 && corner && undone === before;
} catch (e) {
  console.log("error:", e.message);
}
await browser.close();
console.log(ok ? "PASS" : "FAIL");
process.exit(ok ? 0 : 1);
