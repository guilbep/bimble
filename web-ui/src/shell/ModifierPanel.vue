<script setup lang="ts">
import { computed, ref, watch } from "vue";
import { state, element, actions, runAction, runVerb, rotate, mirror } from "../bimble";
import { currentPage } from "../pages";
import { t } from "../i18n";
import { classNoun, parseMm } from "../i18n/parts/modify";

// Reference: the `section[aria-label=Modifier]` under `.vp` (fr/SelectModify.html). Shown while something is selected.
const ids = computed(() => state.value.selection);
const open = computed(() => ids.value.length > 0 && !currentPage.value);

// "1 poutre : B12", or "n éléments".
const note = ref("");
watch(
  () => [ids.value, state.value.rev] as const,
  ([sel]) => {
    note.value = "";
    if (sel.length !== 1) { note.value = sel.length > 1 ? t("modify.many", { n: sel.length }) : ""; return; }
    try {
      const d = element(sel[0]!);
      const c = classNoun(d.class, t);
      note.value = t("modify.one", { cls: c.known ? c.noun.toLocaleLowerCase() : c.noun, name: d.name });
    } catch { /* the properties panel shows the error */ }
  },
  { immediate: true },
);

// Panel state only: which verb the vector button runs, and the vector.
const mode = ref<"move" | "copy" | "rotate" | "mirror">("move");
const vec = ref(["0", "0", "0"]);
const angle = ref("90");
// Mirror axis: point X, Y then direction X, Y.
const axis = ref(["0", "0", "0", "1"]);
const error = ref<string | null>(null);

function byVector(): void {
  const d = vec.value.map(parseMm);
  if (d.some((x) => x === null)) { error.value = t("modify.vector"); return; }
  error.value = runVerb(mode.value, { ids: ids.value, d });
}
function byAngle(): void {
  const a = Number(angle.value.replace(",", "."));
  if (!angle.value.trim() || !Number.isFinite(a)) { error.value = t("modify.angle"); return; }
  error.value = rotate(ids.value, a);
}
function byAxis(): void {
  const v = axis.value.map(parseMm);
  if (v.some((x) => x === null)) { error.value = t("modify.axis"); return; }
  const [px, py, dx, dy] = v as number[];
  error.value = mirror(ids.value, { point: [px!, py!], dir: [dx!, dy!] });
}
function remove(): void { error.value = runVerb("delete", { ids: ids.value }); }

interface Tool { key: string; icon: string; on?: boolean; off?: boolean; missing?: string; run?: () => void }
const act = (name: string) => actions.value.find((a) => a.name === name);
const actionTool = (key: string, name: string, icon: string): Tool => ({
  key, icon, on: act(name)?.pressed === true, off: !act(name)?.enabled, run: () => { error.value = runAction(name); },
});
const missing = (key: string, call: string, icon: string): Tool => ({ key, icon, off: true, missing: call });
const groups = computed<{ key: string; tools: Tool[] }[]>(() => [
  { key: "modify.transform", tools: [
    { key: "modify.move", icon: '<path d="M12 3v18M3 12h18"/><path d="m9 6 3-3 3 3M9 18l3 3 3-3M6 9l-3 3 3 3M18 9l3 3-3 3"/>', on: mode.value === "move", run: () => (mode.value = "move") },
    { key: "modify.copy", icon: '<rect x="8" y="8" width="12" height="12"/><path d="M16 8V4H4v12h4"/>', on: mode.value === "copy", run: () => (mode.value = "copy") },
    { key: "modify.rotate", icon: '<path d="M20 12a8 8 0 1 1-3-6.2"/><path d="M20 4v5h-5"/>', on: mode.value === "rotate", run: () => (mode.value = "rotate") },
    { key: "modify.mirror", icon: '<path d="M12 3v18"/><path d="M9 7 3 17h6zM15 7l6 10h-6z"/>', on: mode.value === "mirror", run: () => (mode.value = "mirror") },
  ] },
  { key: "modify.geometry", tools: [
    missing("modify.align", "align verb", '<path d="M4 4v16"/><rect x="7" y="6" width="10" height="4"/><rect x="7" y="14" width="14" height="4"/>'),
    missing("modify.offset", "offset verb", '<path d="M4 18 18 4"/><path d="M8 21 21 8" stroke-dasharray="2 2"/>'),
    missing("modify.trim", "trim/extend verb", '<path d="M8 8l12 10M8 16 20 6"/>'),
    missing("modify.split", "split verb", '<path d="M3 12h7M14 12h7"/><path d="M12 5v14"/>'),
  ] },
  { key: "modify.visibility", tools: [
    actionTool("modify.hide", "hide_now", '<path d="M3 3l18 18"/><path d="M10 6a10 10 0 0 1 2 0c6 0 10 6 10 6a17 17 0 0 1-3 4M6 7a17 17 0 0 0-4 5s4 7 10 7a9 9 0 0 0 4-1"/>'),
    actionTool("modify.isolate", "isolate_now", '<rect x="8" y="8" width="8" height="8"/><path d="M3 3h4M17 3h4M3 21h4M17 21h4M3 3v4M21 3v4M3 17v4M21 17v4"/>'),
  ] },
]);
function click(tool: Tool): void { if (!tool.off) tool.run?.(); }
</script>

<template>
  <section v-if="open" :aria-label="t('modify.title')" class="mod" data-bb="modifier">
    <div class="head">
      <span class="pn">{{ t('modify.title') }}</span><span class="note">{{ note }}</span>
    </div>
    <div v-for="g in groups" :key="g.key" class="grp">
      <div class="tools">
        <button v-for="tool in g.tools" :key="tool.key" class="tool" :class="{ on: tool.on, off: tool.off }" type="button"
                :aria-pressed="tool.on ? 'true' : undefined" :aria-disabled="tool.off ? 'true' : undefined"
                :data-missing="tool.missing" @click="click(tool)">
          <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"
               stroke-linejoin="round" aria-hidden="true" v-html="tool.icon" />
          <span>{{ t(tool.key) }}</span>
        </button>
      </div>
      <div class="gname">{{ t(g.key) }}</div>
    </div>
    <div class="vec">
      <template v-if="mode === 'rotate'">
        <label class="lbl" for="mod-angle">{{ t('modify.angle') }}</label>
        <input id="mod-angle" v-model="angle" class="inp mono" style="width:86px;text-align:right" @keydown.enter="byAngle" />
      </template>
      <template v-else-if="mode === 'mirror'">
        <span class="lbl">{{ t('modify.axis') }}</span>
        <div style="display:flex;gap:8px;flex-wrap:wrap">
          <label v-for="(a, i) in ['X', 'Y', 'dX', 'dY']" :key="a" style="display:inline-flex;gap:6px;align-items:center">
            <span class="mono" style="font-size:12px">{{ a }}</span>
            <input v-model="axis[i]" class="inp mono" style="width:70px;text-align:right" @keydown.enter="byAxis" />
          </label>
        </div>
      </template>
      <template v-else>
        <span class="lbl">{{ t('modify.vector') }}</span>
        <div style="display:flex;gap:8px;flex-wrap:wrap">
          <label v-for="(a, i) in ['X', 'Y', 'Z']" :key="a" style="display:inline-flex;gap:6px;align-items:center">
            <span class="mono" style="font-size:12px">{{ a }}</span>
            <input v-model="vec[i]" class="inp mono" style="width:86px;text-align:right" @keydown.enter="byVector" />
          </label>
        </div>
      </template>
      <div style="display:flex;gap:8px">
        <button v-if="mode === 'rotate'" class="btn p" type="button" @click="byAngle">{{ t('modify.rotateBy') }}</button>
        <button v-else-if="mode === 'mirror'" class="btn p" type="button" @click="byAxis">{{ t('modify.mirrorBy') }}</button>
        <button v-else class="btn p" type="button" @click="byVector">{{ t(mode === 'move' ? 'modify.moveBy' : 'modify.copyBy') }}</button>
        <button class="btn s" type="button" @click="remove">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 7h16"/><path d="M9 7V4h6v3"/><path d="M6 7l1 13h10l1-13"/></svg>{{ t('modify.delete') }}
        </button>
      </div>
      <p v-if="error" role="alert" class="error">{{ t('modify.error', { msg: error }) }}</p>
    </div>
  </section>
</template>

<style scoped>
/* section[aria-label]{border-top:1px solid #5C574D;background:#F5F2EB;padding:8px 12px;display:flex;gap:10px;align-items:stretch;flex-wrap:wrap} */
.mod { border-top: var(--bb-border-control); background: var(--bb-cream); padding: 8px 12px; display: flex; gap: 10px; align-items: stretch; flex-wrap: wrap; }
/* div{display:flex;flex-direction:column;justify-content:center;gap:2px;padding-right:8px;border-right:1px solid #DDD6C8} */
.head { display: flex; flex-direction: column; justify-content: center; gap: 2px; padding-right: 8px; border-right: var(--bb-border-hairline); }
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0} */
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0; }
/* .note{font-size:13px;color:#5C574D} */
.note { font-size: 13px; color: var(--bb-stone); }
/* .grp{display:flex;flex-direction:column;padding:0 8px;border-right:1px solid #EDE7DB} .grp:last-child{border-right:0} */
.grp { display: flex; flex-direction: column; padding: 0 8px; border-right: 1px solid var(--bb-sunk); }
/* .tools{display:flex;gap:2px;flex:1;align-items:flex-start} */
.tools { display: flex; gap: 2px; flex: 1; align-items: flex-start; }
/* .gname{font-size:12px;color:#5C574D;text-align:center;padding-top:3px;white-space:nowrap} */
.gname { font-size: 12px; color: var(--bb-stone); text-align: center; padding-top: 3px; white-space: nowrap; }
/* .tool{display:flex;flex-direction:column;align-items:center;gap:3px;min-width:58px;max-width:92px;padding:6px 6px 5px;border-radius:12px;color:#141414;text-decoration:none;font-size:12.5px;line-height:1.15;text-align:center;border:1px solid transparent;background:none;font-family:inherit;cursor:pointer} */
.tool { display: flex; flex-direction: column; align-items: center; gap: 3px; min-width: 58px; max-width: 92px; padding: 6px 6px 5px; border-radius: 12px; color: var(--bb-ink); text-decoration: none; font-size: 12.5px; line-height: 1.15; text-align: center; border: 1px solid transparent; background: none; font-family: inherit; cursor: pointer; }
.tool:hover { background: var(--bb-sunk); }
.tool:focus-visible, .btn:focus-visible, .inp:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
.tool.on { background: var(--bb-sunk); border-color: var(--bb-ink); }
.tool.off { color: var(--bb-stone); opacity: 0.55; }
/* div{display:flex;flex-direction:column;gap:6px;justify-content:center;padding-left:6px} */
.vec { display: flex; flex-direction: column; gap: 6px; justify-content: center; padding-left: 6px; }
/* .lbl{font-size:13px;color:#5C574D} */
.lbl { font-size: 13px; color: var(--bb-stone); }
.mono { font-family: var(--bb-font-data); }
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} .inp.mono{font-family:'DM Mono',monospace;font-size:13px} */
.inp { height: 30px; border: var(--bb-border-control); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.inp.mono { font-family: var(--bb-font-data); font-size: 13px; }
/* .btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;min-height:32px;padding:5px 16px;border-radius:999px;font:inherit;font-weight:600;color:#141414;cursor:pointer;text-decoration:none;white-space:nowrap} */
.btn { display: inline-flex; align-items: center; justify-content: center; gap: 6px; min-height: 32px; padding: 5px 16px; border-radius: 999px; font: inherit; font-weight: 600; color: var(--bb-ink); cursor: pointer; text-decoration: none; white-space: nowrap; }
/* .btn.p{background:#C06A41;border:0} .btn.s{background:#FFFFFF;border:1px solid #141414;font-weight:400} */
.btn.p { background: var(--bb-action); border: 0; }
.btn.s { background: var(--bb-sheet); border: 1px solid var(--bb-ink); font-weight: 400; }
/* Error: ink text on an ochre band, with the word "Error". */
.error { color: var(--bb-ink); background: var(--bb-error); font-size: 13.5px; margin: 0; padding: 2px 8px; overflow-wrap: anywhere; }
</style>
