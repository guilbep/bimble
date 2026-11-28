<script setup lang="ts">
// The Navigator's linked models (ticket M1LINKS, ADR 0081; ccore/session/m1links.h): each link, its
// storeys (at their placed elevation), its IFC classes and elements, read only, with an eye on every
// row (the hidden action view_rule: a rule in the active view, one undo step) and Isolate; a click on
// an element selects it (its properties, Box to selection, a note). Then the notes on linked elements.
import { computed, ref, watch } from "vue";
import { t } from "../i18n";
import {
  linkElements, linkNotes, linksTree, linkTree, noteRemove, query, selectLinked, state, viewRule,
  type LinkTree,
} from "../bimble";

type Vis = { hidden_classes: string[]; hidden_elements: string[]; hidden_links: string[]; hidden_storeys: string[] } | null;
const open = ref(true);
const openKeys = ref(new Set<string>());
const trees = ref(new Map<string, LinkTree>());
const elems = ref(new Map<string, { id: string; name: string; class: string }[]>());
const err = ref<string | null>(null);
const rev = computed(() => state.value.rev);
// (VIS2: through a computed key: reading state.value.links here made every state change, a click's selection, ask the host
// for the links' tree again, synchronously: 90 ms of each press on a project of 17 links)
const linksKey = computed(() => `${state.value.rev}|${state.value.links ?? 0}`);
const links = computed(() => { void linksKey.value; return linksTree(); });
const vis = computed<Vis>(() => { void rev.value; try { return query<Vis>("view_visibility"); } catch { return null; } });
const notes = computed(() => { void linksKey.value; return linkNotes(); });
watch([rev, () => state.value.links], () => { trees.value = new Map(); elems.value = new Map(); });

const grouped = (n: number) => String(n).replace(/\B(?=(\d{3})+(?!\d))/g, " ");
const m = (mm: number | null) => mm === null ? "" : `${(mm / 1000).toFixed(2)} m`;
function toggle(key: string, load?: () => void) {
  const s = new Set(openKeys.value);
  if (s.delete(key)) { openKeys.value = s; return; }
  load?.(); s.add(key); openKeys.value = s;
}
function tree(link: string): LinkTree | null {
  let x = trees.value.get(link);
  if (!x) { const y = linkTree(link); if (y) { x = y; trees.value = new Map(trees.value).set(link, y); } }
  return x ?? null;
}
function loadElems(link: string, storey: string, cls: string) {
  const k = `${link}/${storey}/${cls}`;
  toggle(k, () => { elems.value = new Map(elems.value).set(k, linkElements(link, storey, cls)); });
}
const hiddenLink = (l: string) => vis.value?.hidden_links.includes(l) ?? false;
const hiddenStorey = (l: string, s: string) => vis.value?.hidden_storeys.includes(`${l}/${s}`) ?? false;
const hiddenClass = (c: string) => vis.value?.hidden_classes.includes(c) ?? false;
const hiddenEl = (id: string) => vis.value?.hidden_elements.includes(id) ?? false;
function eye(hidden: boolean, rule: Parameters<typeof viewRule>[1]) { err.value = viewRule(hidden ? "show" : "hide", rule); }
function iso(rule: Parameters<typeof viewRule>[1]) { err.value = viewRule("isolate", rule); }
</script>

<template>
  <div v-if="links.length" data-bb="links-tree">
    <a class="ti h" href="#" style="padding-left:6px" :aria-expanded="open" data-bb="nav-links-tree" @click.prevent="open = !open">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="open ? 'm6 9 6 6 6-6' : 'm9 6 6 6-6 6'"/></svg>
      <span>{{ t('m1.linkedModels') }}</span><span class="n">{{ links.length }}</span>
    </a>
    <p v-if="err" role="alert" class="nav-err">{{ err }}</p>
    <template v-if="open">
      <template v-for="l in links" :key="l.link">
        <div class="ti" style="padding-left:22px" data-bb="nav-link" :data-link="l.link">
          <a href="#" class="lbl" :aria-expanded="openKeys.has(l.link)" @click.prevent="toggle(l.link)">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><path :d="openKeys.has(l.link) ? 'm6 9 6 6 6-6' : 'm9 6 6 6-6 6'"/></svg>
            <span :class="{ off: hiddenLink(l.link) }">{{ l.link }}</span>
          </a>
          <span class="n">{{ grouped(l.total) }}</span>
          <button class="eb" type="button" data-bb="eye-link" :aria-pressed="!hiddenLink(l.link)" :aria-label="t(hiddenLink(l.link) ? 'm1.show' : 'm1.hide', { what: l.link })" @click="eye(hiddenLink(l.link), { link: l.link })"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z"/><circle v-if="!hiddenLink(l.link)" cx="12" cy="12" r="3"/><path v-else d="M4 4l16 16"/></svg></button>
          <button class="eb" type="button" data-bb="iso-link" :aria-label="t('m1.isolate', { what: l.link })" @click="iso({ link: l.link })"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><circle cx="12" cy="12" r="4"/><path d="M12 2v4M12 18v4M2 12h4M18 12h4"/></svg></button>
        </div>
        <template v-if="openKeys.has(l.link)">
          <template v-for="s in l.storeys" :key="s.name">
            <div class="ti" style="padding-left:38px" data-bb="nav-link-storey" :data-storey="s.name">
              <a href="#" class="lbl" @click.prevent="toggle(`${l.link}/${s.name}`, () => tree(l.link))">
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><path :d="openKeys.has(`${l.link}/${s.name}`) ? 'm6 9 6 6 6-6' : 'm9 6 6 6-6 6'"/></svg>
                <span :class="{ off: hiddenStorey(l.link, s.name) }">{{ s.name || "—" }}</span>
              </a>
              <span class="n">{{ m(s.elevation_mm) }} · {{ grouped(s.count) }}</span>
              <button class="eb" type="button" data-bb="eye-storey" :aria-pressed="!hiddenStorey(l.link, s.name)" :aria-label="t(hiddenStorey(l.link, s.name) ? 'm1.show' : 'm1.hide', { what: s.name })" @click="eye(hiddenStorey(l.link, s.name), { link: l.link, storey: s.name })"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z"/><circle v-if="!hiddenStorey(l.link, s.name)" cx="12" cy="12" r="3"/><path v-else d="M4 4l16 16"/></svg></button>
              <button class="eb" type="button" data-bb="iso-storey" :aria-label="t('m1.isolate', { what: s.name })" @click="iso({ link: l.link, storey: s.name })"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><circle cx="12" cy="12" r="4"/><path d="M12 2v4M12 18v4M2 12h4M18 12h4"/></svg></button>
            </div>
            <template v-if="openKeys.has(`${l.link}/${s.name}`)">
              <template v-for="c in tree(l.link)?.storeys.find((x) => x.name === s.name)?.classes ?? []" :key="c.class">
                <div class="ti" style="padding-left:54px" data-bb="nav-link-class" :data-class="c.class">
                  <a href="#" class="lbl" @click.prevent="loadElems(l.link, s.name, c.class)"><span :class="{ off: hiddenClass(c.class) }">{{ c.class }}</span></a>
                  <span class="n">{{ grouped(c.count) }}</span>
                  <button class="eb" type="button" data-bb="eye-class" :aria-pressed="!hiddenClass(c.class)" :aria-label="t(hiddenClass(c.class) ? 'm1.show' : 'm1.hide', { what: c.class })" @click="eye(hiddenClass(c.class), { class: c.class })"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z"/><circle v-if="!hiddenClass(c.class)" cx="12" cy="12" r="3"/><path v-else d="M4 4l16 16"/></svg></button>
                  <button class="eb" type="button" data-bb="iso-class" :aria-label="t('m1.isolate', { what: c.class })" @click="iso({ class: c.class })"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><circle cx="12" cy="12" r="4"/><path d="M12 2v4M12 18v4M2 12h4M18 12h4"/></svg></button>
                </div>
                <template v-if="openKeys.has(`${l.link}/${s.name}/${c.class}`)">
                  <div v-for="e in elems.get(`${l.link}/${s.name}/${c.class}`) ?? []" :key="e.id" class="ti" style="padding-left:70px"
                       :class="{ sel: state.linked?.id === e.id && state.linked?.link === l.link }" data-bb="nav-link-element" :data-id="e.id">
                    <a href="#" class="lbl" @click.prevent="selectLinked(l.link, e.id)"><span :class="{ off: hiddenEl(e.id) }">{{ e.name || e.id }}</span></a>
                    <button class="eb" type="button" data-bb="eye-element" :aria-pressed="!hiddenEl(e.id)" :aria-label="t(hiddenEl(e.id) ? 'm1.show' : 'm1.hide', { what: e.name || e.id })" @click="eye(hiddenEl(e.id), { element: e.id })"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z"/><circle v-if="!hiddenEl(e.id)" cx="12" cy="12" r="3"/><path v-else d="M4 4l16 16"/></svg></button>
                  </div>
                </template>
              </template>
            </template>
          </template>
        </template>
      </template>
    </template>
    <template v-if="notes.length">
      <span class="ti h" style="padding-left:6px">{{ t('m1.notes') }}<span class="n">{{ notes.length }}</span></span>
      <div v-for="n in notes" :key="`${n.link}#${n.index}`" class="ti" style="padding-left:22px" data-bb="nav-note">
        <a href="#" class="lbl" :title="n.link" @click.prevent="selectLinked(n.link, n.element)"><span>{{ n.text }}</span></a>
        <span v-if="n.orphan" class="n" :title="t('m1.noteOrphanTip')" data-bb="note-orphan">{{ t('m1.noteOrphan') }}</span>
        <button class="eb" type="button" :aria-label="t('m1.noteRemove')" @click="err = noteRemove(n.link, n.index)"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button>
      </div>
    </template>
  </div>
</template>

<style scoped>
.ti { display: flex; gap: 4px; align-items: center; padding: 2px 6px; border-radius: 8px; color: var(--bb-ink); font-size: 13.5px; line-height: 20px; white-space: nowrap; }
.ti:hover { background: var(--bb-sunk); }
.ti.h { font-weight: 600; text-decoration: none; }
.ti.sel { background: var(--bb-selection); }
.ti .n { margin-left: auto; color: var(--bb-stone); font-family: 'DM Mono', monospace; font-size: 12px; }
.lbl { display: inline-flex; gap: 4px; align-items: center; color: inherit; text-decoration: none; overflow: hidden; text-overflow: ellipsis; min-width: 0; }
.lbl + .eb, .n + .eb { margin-left: 2px; }
.lbl:only-of-type { margin-right: auto; }
.off { color: var(--bb-stone); text-decoration: line-through; }
.eb { width: 22px; height: 22px; border: 0; border-radius: 999px; background: none; display: inline-flex; align-items: center; justify-content: center; color: var(--bb-ink); cursor: pointer; padding: 0; flex: none; }
.eb:hover { background: var(--bb-line); }
.eb:focus-visible, .lbl:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
.nav-err { margin: 2px 6px; font-size: 12px; color: var(--bb-ink); background: var(--bb-ochre, #e8c26a); padding: 2px 6px; border-radius: 4px; }
</style>
