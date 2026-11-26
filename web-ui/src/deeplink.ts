// Deep links: the address names a state of the lab, read once after the model loaded.
// ?tab=<TabId>&view=3d|plan:<storey name or index>|<GlobalId>&select=first|first:<IfcClass>|<id,id>
//  &page=<id>&dialog=<id>&panel=<id>&paper=1&action=<name,name>&edit=move-first-beam
// edit=move-first-beam: one real `move` verb (first IfcBeam, 1 mm in x): unsaved, one undo step.
// view=plan:N opens the plan as the first tab, the 3D view after it (close and re-open 3D).
// Each wasm step waits for the next state change (or 1 s) before the next one; never called inside on_change.
// data-bb-ready is set when all steps are done and the page is on screen (FIRST1, `onScreen`): the
// design check and the lab checks wait for it.
import { watch } from "vue";
import { modelReady, state, storeys, openView, select, runAction, runVerb, tabCall, openViews, query, type ViewSummary } from "./bimble";
import { activeTab, isTabId } from "./shell/tabs";
import { send } from "./shell/send";
import { currentDialog } from "./dialogs";
import { currentPanel } from "./panels";

/** The next state refresh, or `ms` without one (a step that changes nothing). */
function nextChange(ms = 1000): Promise<void> {
  return new Promise((resolve) => {
    const stop = watch(state, () => { stop(); clearTimeout(timer); resolve(); });
    const timer = setTimeout(() => { stop(); resolve(); }, ms);
  });
}

/** Run one wasm call, then wait until its change is in the refs. Logs the Rust error. */
async function step(what: string, call: () => string | null): Promise<void> {
  const err = call();
  if (err) { console.warn(`deep link ${what}: ${err}`); return; }
  await nextChange();
}

function viewId(v: string): string | null {
  if (!v.startsWith("plan:") && v !== "3d") return v; // a GlobalId
  // The views the viewer offers (stored and default ones); `views()` lists stored views only.
  const all = (openViews.value as unknown as { views?: ViewSummary[] }).views ?? [];
  if (v === "3d") return all.find((x) => x.type === "three_d")?.id ?? null;
  const key = v.slice(5);
  const s = storeys.value.find((x, i) => x.name === key || String(i) === key);
  return all.find((x) => x.type === "plan" && x.look !== "up" && x.storey === s?.id)?.id ?? null;
}

function selection(v: string): string[] {
  if (v !== "first" && !v.startsWith("first:")) return v.split(",").filter(Boolean);
  const cls = v.slice(6) || undefined;
  return query<{ id: string }[]>("elements", { class: cls, limit: 1 }).map((e) => e.id);
}

// ---- FIRST1: ready means on screen and answering (ADR 0074) ----
/** The next animation frame's time, or null when none came within `ms` (a hidden page has none). */
const frame = (ms: number): Promise<number | null> => new Promise((resolve) => {
  const timer = setTimeout(() => resolve(null), ms);
  requestAnimationFrame((t) => { clearTimeout(timer); resolve(t); });
});
/** Resolves once the viewport drew the model (when there is one) and the page's frames are shown at
 *  the display's pace again: CALM frames in a row, each under GAP_MS after the one before, each with
 *  a change of its own to show (a few-pixel probe that changes colour), so the GPU has to show it,
 *  not only the page to draw it. Headless Chromium's GPU process (SwiftShader) takes 200 to 900 ms
 *  per frame for the first frames of a session (it compiles the pipelines each new kind of drawing
 *  needs); until they are on screen every new frame waits behind them, and an input given then
 *  waited as long for its own (FIRST1: 190 to 710 ms after the old ready). 10 s at most. */
async function onScreen(): Promise<void> {
  const CALM = 3, GAP_MS = 50, until = performance.now() + 10000;
  const late = () => performance.now() > until || document.visibilityState === "hidden";
  for (let g = window.__bbGl; g && g.firstFrameMs === null && g.error === null && state.value.file !== null && !late(); g = window.__bbGl) await frame(100);
  const probe = document.createElement("div");
  probe.setAttribute("aria-hidden", "true");
  // Over the viewport's canvas, as a tooltip, a menu or the palette comes over it: the GPU composes
  // a layer over the WebGL picture before ready, not first at the user's first input.
  const vp = document.getElementById("bimble")?.getBoundingClientRect();
  const at = vp && vp.width > 4 && vp.height > 4 ? `left:${Math.round(vp.left + 2)}px;top:${Math.round(vp.bottom - 4)}px` : "left:0;bottom:0";
  probe.style.cssText = `position:fixed;${at};z-index:50;width:2px;height:2px;border-radius:1px;pointer-events:none;background:rgba(20,20,20,0.02)`;
  document.body.append(probe);
  let last = await frame(100), calm = 0, odd = false;
  while (calm < CALM && !late()) {
    odd = !odd;
    probe.style.background = odd ? "rgba(20,20,20,0.03)" : "rgba(20,20,20,0.02)";
    const t = await frame(1000);
    calm = t !== null && last !== null && t - last < GAP_MS ? calm + 1 : 0;
    last = t;
  }
  probe.remove();
}
// ---- end FIRST1 ----

export async function applyDeepLink(): Promise<void> {
  await modelReady;
  const q = new URLSearchParams(location.search);
  try {
    if (q.get("edit") === "move-first-beam") {
      const ids = selection("first:IfcBeam");
      if (ids.length) await step("edit", () => runVerb("move", { ids, d: [1, 0, 0] }));
    }
    const tab = q.get("tab");
    if (tab && isTabId(tab)) activeTab.value = tab;
    const view = q.get("view");
    if (view) {
      const id = viewId(view);
      if (id) await step("view", () => openView(id)); else console.warn(`deep link view: none for ${view}`);
      // The 3D tab was open before the plan: close and re-open it so it comes after, then back to the plan.
      const d3 = viewId("3d");
      const tabs = openViews.value.tabs;
      const at = (vid: string | null) => tabs.findIndex((t) => t.type === "view" && t.id === vid);
      if (id && view.startsWith("plan:") && d3 && at(d3) >= 0 && at(d3) < at(id)) {
        await step("view 3d close", () => tabCall("close", { type: "view", id: d3 }));
        await step("view 3d open", () => openView(d3));
        await step("view plan", () => tabCall("activate", { type: "view", id }));
      }
    }
    const sel = q.get("select");
    if (sel) await step("select", () => select(selection(sel)));
    if (q.get("paper") === "1" && openViews.value.paper.offered && !openViews.value.paper.on) {
      await step("paper", () => runAction("paper_view"));
    }
    for (const a of (q.get("action") ?? "").split(",").filter(Boolean)) await step(a, () => runAction(a));
    const pg = q.get("page");
    if (pg) send(pg === "home" ? { do: "home" } : { do: "showPage", page: pg });
    currentPanel.value = q.get("panel") ?? (q.get("perf") === "1" ? "performance" : null); // GPU1: ?perf=1
    currentDialog.value = q.get("dialog");
  } catch (e) {
    console.warn("deep link:", e);
  }
  await onScreen(); // FIRST1
  document.documentElement.dataset.bbReady = "1";
}
