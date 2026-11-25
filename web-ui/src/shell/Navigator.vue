<script setup lang="ts">
import { computed, ref, watch } from "vue";
import { t } from "../i18n";
import { viewName } from "./viewName";
import { send } from "./send";
import { links, query, select, measurements, openView, openViews, scheduleList, sheets, state, storeys, type ViewSummary } from "../bimble";
// ---- SEC1 ---- new views, duplicate, delete (verbs view_add, view_remove; viewing.rs:781-840)
import { viewAdd, viewRemove } from "../bimble";
import { editableModel, has } from "./capability";
// ---- end SEC1 ----

// `open_views` is `{views, active, tabs}` (bridge.rs); bimble.ts types it loosely until then.
const ov = computed(() => openViews.value as unknown as { views?: ViewSummary[]; active?: string | null });
const activeView = computed(() => ov.value.views?.find((v) => v.id === ov.value.active) ?? null);
// The reference's order: 3D views, plans, ceiling plans, sections; stored order within each.
const rank = (v: ViewSummary) => v.type === "three_d" ? 0 : v.type === "plan" ? (v.look === "up" ? 2 : 1) : 3;
const groupedViews = computed(() => [...(ov.value.views ?? [])].sort((a, b) => rank(a) - rank(b)));

const open = ref(true);
// ---- SEC1 ---- Rust's "New view…" menu (viewing.rs:806-840): a plan and a ceiling plan per level,
// a section looking each way, a 3D view; a view's menu: Duplicate, Delete (Reset to default for a
// changed default view). Each is one verb, one undo step; the new view opens.
const canNewView = computed(() => { void state.value.rev; return has("views") && editableModel(); });
const menuOpen = ref(false);
const viewMenu = ref<string | null>(null);
const navError = ref<string | null>(null);
const FACINGS = ["north", "east", "south", "west"] as const;
function addView(args: Parameters<typeof viewAdd>[0]): void {
  menuOpen.value = false;
  viewMenu.value = null;
  const r = viewAdd(args);
  navError.value = r.error;
  if (r.id) openView(r.id);
}
function removeView(id: string): void {
  viewMenu.value = null;
  navError.value = viewRemove(id);
}
// A default view's id is derived (auto_name set while it keeps its default name): Reset, else Delete.
const isDefault = (v: ViewSummary) => v.auto_name != null;
// ---- end SEC1 ----
// Model-derived counts: re-read when the model changes (rev) or once open_views arrives.
const sheetCount = computed(() => {
  void state.value.rev; void openViews.value;
  try { return sheets().length; } catch { return null; }
});
const scheduleCount = computed(() => { void state.value.rev; void openViews.value; return scheduleList().length; });
const linkCount = computed(() => { void state.value.rev; void openViews.value; return links().length; });
const measureCount = computed(() => { void state.value.rev; void openViews.value; return measurements().length; });
// Links and measurements: read-only lists from Rust, shown when their group opens.
const linkOpen = ref(false);
const measureOpen = ref(false);
const linkRows = computed(() => { void state.value.rev; return linkOpen.value ? links() : []; });
const measureRows = computed(() => { void state.value.rev; return measureOpen.value ? measurements() : []; });
const objects = computed(() => storeys.value.reduce((n, s) => n + s.element_count, 0));
const grouped = (n: number) => String(n).replace(/\B(?=(\d{3})+(?!\d))/g, " ");

// Object tree (storey → IFC class → elements), lazy: children load when a row opens. Local UI state only.
type El = { id: string; class: string; name: string };
const LIMIT = 200;
const objOpen = ref(false);
const openKeys = ref(new Set<string>());
const classes = ref(new Map<string, [string, number][]>());
const elems = ref(new Map<string, El[]>());
function toggle(key: string, load: () => void) {
  const s = new Set(openKeys.value);
  if (s.delete(key)) { openKeys.value = s; return; }
  load(); s.add(key); openKeys.value = s;
}
function openStorey(id: string) {
  toggle(id, () => {
    // ponytail: counts by grouping the storey's elements (MAX_ELEMENTS 10 000); a per-class count query if storeys grow past that.
    const n = new Map<string, number>();
    for (const e of query<El[]>("elements", { storey: id, limit: 10000 })) n.set(e.class, (n.get(e.class) ?? 0) + 1);
    classes.value = new Map(classes.value).set(id, [...n].sort((a, b) => a[0].localeCompare(b[0])));
  });
}
function openClass(storey: string, cls: string) {
  const key = `${storey}/${cls}`;
  toggle(key, () => { elems.value = new Map(elems.value).set(key, query<El[]>("elements", { storey, class: cls, limit: LIMIT })); });
}
// The model changed: drop loaded children so open rows reload.
watch(() => state.value.rev, () => { classes.value = new Map(); elems.value = new Map(); openKeys.value = new Set(); });

function icon(v: ViewSummary): "three_d" | "plan" | "section" {
  return v.type === "section" ? "section" : v.type === "plan" ? "plan" : "three_d";
}
</script>

<template>
  <nav class="nav" data-bb="navigator" :aria-label="t('nav.title')">
    <div class="pn" style="display:flex;align-items:center;justify-content:space-between">{{ t('nav.title') }}<button
      class="ib" type="button" :aria-label="t('nav.newView')" style="width:26px;height:26px"
      :disabled="!canNewView" :aria-expanded="menuOpen" data-bb="new-view"
      @click="menuOpen = !menuOpen"
    ><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 5v14M5 12h14"/></svg></button></div>
    <!-- SEC1: Rust's New view… menu -->
    <div v-if="menuOpen && canNewView" class="vmenu" role="menu" data-bb="new-view-menu">
      <button v-for="s in storeys" :key="`p${s.id}`" type="button" role="menuitem" @click="addView({ kind: 'plan', storey: s.id })">{{ t('sec1.planOf', { level: s.name }) }}</button>
      <button v-for="s in storeys" :key="`c${s.id}`" type="button" role="menuitem" @click="addView({ kind: 'ceiling', storey: s.id })">{{ t('sec1.ceilingOf', { level: s.name }) }}</button>
      <hr>
      <button v-for="f in FACINGS" :key="f" type="button" role="menuitem" :data-bb="`new-section-${f}`" @click="addView({ kind: 'section', facing: f })">{{ t('sec1.sectionLooking', { facing: t(`sec1.${f}`) }) }}</button>
      <hr>
      <button type="button" role="menuitem" @click="addView({ kind: 'three_d' })">{{ t('sec1.threeD') }}</button>
    </div>
    <p v-if="navError" role="alert" class="nav-err">{{ navError }}</p>
    <!-- end SEC1 -->
    <a class="ti h" href="#" style="padding-left:6px" :aria-expanded="open" @click.prevent="open = !open">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="open ? 'm6 9 6 6 6-6' : 'm9 6 6 6-6 6'"/></svg>
      <span>{{ t('nav.views') }}</span>
    </a>
    <template v-if="open">
      <!-- Pink is selection only (ADR 0049): the reference does not mark the active view here. -->
      <template v-for="v in groupedViews" :key="v.id"><a
        class="ti" href="#" :data-view="v.id"
        style="padding-left:22px" @click.prevent="openView(v.id)" @contextmenu.prevent="viewMenu = canNewView ? v.id : null"
      >
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
          <template v-if="icon(v) === 'three_d'"><path d="M12 3 20 7v10l-8 4-8-4V7z"/><path d="M4 7l8 4 8-4M12 11v10"/></template>
          <template v-else-if="icon(v) === 'plan'"><rect x="4" y="4" width="16" height="16"/><path d="M4 12h9v8M13 4v4"/></template>
          <template v-else><path d="M3 12h18"/><path d="m7 8-4 4 4 4"/><rect x="9" y="5" width="6" height="14" stroke-dasharray="2 2"/></template>
        </svg>
        <span>{{ viewName(v) }}</span>
        <span v-if="v.references > 0" class="n">{{ t('nav.refs', { n: v.references }) }}</span>
      </a>
      <!-- SEC1: the view's menu (right click), as Rust's context menu -->
      <div v-if="viewMenu === v.id" class="vmenu" role="menu" :aria-label="t('sec1.menu', { name: viewName(v) })" data-bb="view-menu">
        <button type="button" role="menuitem" @click="addView({ from: v.id })">{{ t('sec1.duplicate') }}</button>
        <button type="button" role="menuitem" @click="removeView(v.id)">{{ t(isDefault(v) ? 'sec1.reset' : 'sec1.delete') }}</button>
      </div></template>
    </template>
    <a class="ti h" href="#" style="padding-left:6px" data-missing="schedule-list-in-navigator" @click.prevent="send({ do: 'showPage', page: 'quantities' })">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9 6 6 6-6 6"/></svg>
      <span>{{ t('nav.schedules') }}</span><span class="n">{{ scheduleCount }}</span>
    </a>
    <a class="ti h" href="#" style="padding-left:6px" data-missing="sheet-list-in-navigator" @click.prevent="send({ do: 'showPage', page: 'sheets' })">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9 6 6 6-6 6"/></svg>
      <span>{{ t('nav.sheets') }}</span><span class="n">{{ sheetCount ?? "" }}</span>
    </a>
    <a class="ti h" href="#" style="padding-left:6px" :aria-expanded="objOpen" @click.prevent="objOpen = !objOpen">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="objOpen ? 'm6 9 6 6 6-6' : 'm9 6 6 6-6 6'"/></svg>
      <span>{{ t('nav.objects') }}</span><span class="n">{{ grouped(objects) }}</span>
    </a>
    <template v-if="objOpen">
      <template v-for="s in storeys" :key="s.id">
        <a class="ti" href="#" style="padding-left:22px" :aria-expanded="openKeys.has(s.id)" @click.prevent="openStorey(s.id)">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="openKeys.has(s.id) ? 'm6 9 6 6 6-6' : 'm9 6 6 6-6 6'"/></svg>
          <span>{{ s.name }}</span><span class="n">{{ grouped(s.element_count) }}</span>
        </a>
        <template v-if="openKeys.has(s.id)">
          <template v-for="[c, n] in classes.get(s.id) ?? []" :key="c">
            <a class="ti" href="#" style="padding-left:38px" :aria-expanded="openKeys.has(`${s.id}/${c}`)" @click.prevent="openClass(s.id, c)">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="openKeys.has(`${s.id}/${c}`) ? 'm6 9 6 6 6-6' : 'm9 6 6 6-6 6'"/></svg>
              <span>{{ c }}</span><span class="n">{{ grouped(n) }}</span>
            </a>
            <template v-if="openKeys.has(`${s.id}/${c}`)">
              <a
                v-for="e in elems.get(`${s.id}/${c}`) ?? []" :key="e.id" class="ti" :class="{ sel: state.selection.includes(e.id) }"
                href="#" style="padding-left:54px" @click.prevent="select([e.id])"
              ><span>{{ e.name || e.id }}</span></a>
              <span v-if="n > LIMIT" class="ti" style="padding-left:54px;color:var(--bb-stone)">{{ t('nav.more', { n: n - LIMIT }) }}</span>
            </template>
          </template>
        </template>
      </template>
    </template>
    <!-- No sub-project data in Rust yet (ADR 0059 proposed): the group opens the Sub-projects page. -->
    <a class="ti h" href="#" style="padding-left:6px" data-missing="subprojects query" @click.prevent="send({ do: 'showPage', page: 'subprojects' })">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9 6 6 6-6 6"/></svg>
      <span>{{ t('nav.subprojects') }}</span><span class="n" />
    </a>
    <a class="ti h" href="#" style="padding-left:6px" :aria-expanded="linkOpen" @click.prevent="linkOpen = !linkOpen">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="linkOpen ? 'm6 9 6 6 6-6' : 'm9 6 6 6-6 6'"/></svg>
      <span>{{ t('nav.links') }}</span><span class="n">{{ linkCount }}</span>
    </a>
    <span v-for="l in linkRows" :key="l.id" class="ti" style="padding-left:22px"><span>{{ l.name }}</span><span class="n">{{ l.kind }}</span></span>
    <a class="ti h" href="#" style="padding-left:6px" :aria-expanded="measureOpen" @click.prevent="measureOpen = !measureOpen">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="measureOpen ? 'm6 9 6 6 6-6' : 'm9 6 6 6-6 6'"/></svg>
      <span>{{ t('nav.measures') }}</span><span class="n">{{ measureCount }}</span>
    </a>
    <span v-for="m in measureRows" :key="m.id" class="ti" style="padding-left:22px"><span>{{ m.label }}</span></span>
  </nav>
</template>

<style scoped>
/* SEC1: the New view… and view menus */
.vmenu { display: flex; flex-direction: column; margin: 2px 6px 4px 22px; border: 1px solid var(--bb-line); border-radius: 8px; background: var(--bb-sheet); padding: 4px 0; }
.vmenu button { text-align: left; background: none; border: 0; padding: 4px 10px; font: inherit; font-size: 13px; color: var(--bb-ink); cursor: pointer; }
.vmenu button:hover, .vmenu button:focus-visible { background: var(--bb-sunk); }
.vmenu hr { border: 0; border-top: 1px solid var(--bb-line); margin: 4px 0; width: 100%; }
.nav-err { margin: 2px 6px; font-size: 12px; color: var(--bb-ink); background: var(--bb-ochre, #e8c26a); padding: 2px 6px; border-radius: 4px; }
/* .nav{flex:none;overflow:hidden;padding:12px 12px 8px;border-bottom:1px solid #DDD6C8;display:flex;flex-direction:column;gap:1px} */
.nav { flex: none; overflow: hidden; padding: 12px 12px 8px; border-bottom: var(--bb-border-hairline); display: flex; flex-direction: column; gap: 1px; }
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: var(--bb-font-display); font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .ib{width:32px;height:32px;border-radius:999px;border:0;background:none;display:inline-flex;align-items:center;justify-content:center;color:#141414;cursor:pointer;text-decoration:none;padding:0;flex:none} */
.ib { width: 32px; height: 32px; border-radius: 999px; border: 0; background: none; display: inline-flex; align-items: center; justify-content: center; color: var(--bb-ink); cursor: pointer; text-decoration: none; padding: 0; flex: none; }
.ib:hover { background: var(--bb-sunk); }
.ib:disabled { color: var(--bb-stone); opacity: .55; cursor: default; background: none; }
/* .ti{display:flex;gap:6px;align-items:center;padding:2px 6px;border-radius:8px;color:#141414;text-decoration:none;font-size:13.5px;line-height:20px;white-space:nowrap} */
.ti { display: flex; gap: 6px; align-items: center; padding: 2px 6px; border-radius: 8px; color: var(--bb-ink); text-decoration: none; font-size: 13.5px; line-height: 20px; white-space: nowrap; }
.ti:hover { background: var(--bb-sunk); }
.ti.h { font-weight: 600; }
.ti.sel { background: var(--bb-selection); }
.ti .n { margin-left: auto; color: var(--bb-stone); font-family: 'DM Mono', monospace; font-size: 12px; }
.ib:focus-visible, .ti:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
