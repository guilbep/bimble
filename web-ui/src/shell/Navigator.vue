<script setup lang="ts">
import { computed, ref } from "vue";
import { t } from "../i18n";
import { viewName } from "./viewName";
import { actions, openView, openViews, runAction, scheduleList, sheets, state, storeys, type ViewSummary } from "../bimble";

// `open_views` is `{views, active, tabs}` (bridge.rs); bimble.ts types it loosely until then.
const ov = computed(() => openViews.value as unknown as { views?: ViewSummary[]; active?: string | null });
const activeView = computed(() => ov.value.views?.find((v) => v.id === ov.value.active) ?? null);
// The reference's order: 3D views, plans, ceiling plans, sections; stored order within each.
const rank = (v: ViewSummary) => v.type === "three_d" ? 0 : v.type === "plan" ? (v.look === "up" ? 2 : 1) : 3;
const groupedViews = computed(() => [...(ov.value.views ?? [])].sort((a, b) => rank(a) - rank(b)));

const open = ref(true);
const canNewView = computed(() => actions.value.some((a) => a.name === "new_view"));
// Model-derived counts: re-read when the model changes (rev) or once open_views arrives.
const sheetCount = computed(() => {
  void state.value.rev; void openViews.value;
  try { return sheets().length; } catch { return null; }
});
const scheduleCount = computed(() => { void state.value.rev; void openViews.value; return scheduleList().length; });
const objects = computed(() => storeys.value.reduce((n, s) => n + s.element_count, 0));
const grouped = (n: number) => String(n).replace(/\B(?=(\d{3})+(?!\d))/g, " ");

function icon(v: ViewSummary): "three_d" | "plan" | "section" {
  return v.type === "section" ? "section" : v.type === "plan" ? "plan" : "three_d";
}
</script>

<template>
  <nav class="nav" data-bb="navigator" :aria-label="t('nav.title')">
    <div class="pn" style="display:flex;align-items:center;justify-content:space-between">{{ t('nav.title') }}<button
      class="ib" type="button" :aria-label="t('nav.newView')" style="width:26px;height:26px"
      :disabled="!canNewView" :data-missing="canNewView ? undefined : 'new_view action'"
      @click="runAction('new_view')"
    ><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 5v14M5 12h14"/></svg></button></div>
    <a class="ti h" href="#" style="padding-left:6px" :aria-expanded="open" @click.prevent="open = !open">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="open ? 'm6 9 6 6 6-6' : 'm9 6 6 6-6 6'"/></svg>
      <span>{{ t('nav.views') }}</span>
    </a>
    <template v-if="open">
      <!-- Pink is selection only (ADR 0049): the reference does not mark the active view here. -->
      <a
        v-for="v in groupedViews" :key="v.id" class="ti" href="#"
        style="padding-left:22px" @click.prevent="openView(v.id)"
      >
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
          <template v-if="icon(v) === 'three_d'"><path d="M12 3 20 7v10l-8 4-8-4V7z"/><path d="M4 7l8 4 8-4M12 11v10"/></template>
          <template v-else-if="icon(v) === 'plan'"><rect x="4" y="4" width="16" height="16"/><path d="M4 12h9v8M13 4v4"/></template>
          <template v-else><path d="M3 12h18"/><path d="m7 8-4 4 4 4"/><rect x="9" y="5" width="6" height="14" stroke-dasharray="2 2"/></template>
        </svg>
        <span>{{ viewName(v) }}</span>
        <span v-if="v.references > 0" class="n">{{ t('nav.refs', { n: v.references }) }}</span>
      </a>
    </template>
    <a class="ti h" href="#" style="padding-left:6px" data-missing="open-schedules" @click.prevent>
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9 6 6 6-6 6"/></svg>
      <span>{{ t('nav.schedules') }}</span><span class="n">{{ scheduleCount }}</span>
    </a>
    <a class="ti h" href="#" style="padding-left:6px" data-missing="open-sheets" @click.prevent>
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9 6 6 6-6 6"/></svg>
      <span>{{ t('nav.sheets') }}</span><span class="n">{{ sheetCount ?? "" }}</span>
    </a>
    <a class="ti h" href="#" style="padding-left:6px" data-missing="object-tree" @click.prevent>
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9 6 6 6-6 6"/></svg>
      <span>{{ t('nav.objects') }}</span><span class="n">{{ grouped(objects) }}</span>
    </a>
    <a class="ti h" href="#" style="padding-left:6px" data-missing="subprojects query" @click.prevent>
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9 6 6 6-6 6"/></svg>
      <span>{{ t('nav.subprojects') }}</span><span class="n" />
    </a>
    <a class="ti h" href="#" style="padding-left:6px" data-missing="links query" @click.prevent>
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9 6 6 6-6 6"/></svg>
      <span>{{ t('nav.links') }}</span><span class="n" />
    </a>
    <a class="ti h" href="#" style="padding-left:6px" data-missing="measurements query" @click.prevent>
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9 6 6 6-6 6"/></svg>
      <span>{{ t('nav.measures') }}</span><span class="n" />
    </a>
  </nav>
</template>

<style scoped>
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
