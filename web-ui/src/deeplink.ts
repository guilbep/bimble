// Deep links: the address names a state of the lab, read once after the model loaded.
// ?tab=<TabId>&view=3d|plan:<storey name or index>|<GlobalId>&select=first|first:<IfcClass>|<id,id>
//  &page=<id>&dialog=<id>&panel=<id>&paper=1&action=<name,name>
// Each wasm step waits for the next state change (or 1 s) before the next one; never called inside on_change.
// data-bb-ready is set when all steps are done: the design check waits for it.
import { watch } from "vue";
import { modelReady, state, storeys, openView, select, runAction, openViews, query, type ViewSummary } from "./bimble";
import { activeTab, isTabId } from "./shell/tabs";
import { currentPage } from "./pages";
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

export async function applyDeepLink(): Promise<void> {
  await modelReady;
  const q = new URLSearchParams(location.search);
  try {
    const tab = q.get("tab");
    if (tab && isTabId(tab)) activeTab.value = tab;
    const view = q.get("view");
    if (view) {
      const id = viewId(view);
      if (id) await step("view", () => openView(id)); else console.warn(`deep link view: none for ${view}`);
    }
    const sel = q.get("select");
    if (sel) await step("select", () => select(selection(sel)));
    if (q.get("paper") === "1" && openViews.value.paper.offered && !openViews.value.paper.on) {
      await step("paper", () => runAction("paper_view"));
    }
    for (const a of (q.get("action") ?? "").split(",").filter(Boolean)) await step(a, () => runAction(a));
    currentPage.value = q.get("page");
    currentPanel.value = q.get("panel");
    currentDialog.value = q.get("dialog");
  } catch (e) {
    console.warn("deep link:", e);
  }
  document.documentElement.dataset.bbReady = "1";
}
