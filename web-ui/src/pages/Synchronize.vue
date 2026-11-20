<script lang="ts">
export const hint = "synchronize.hint";
</script>

<script setup lang="ts">
// C3 Synchronize with per-element conflicts (wireframes v01 Conflicts.html; ADR 0060, Proposed). The bridge has no
// sync state yet (query `sync`, verbs sync_start / sync_choose / sync_choose_all / sync_finish / sync_abort): counts
// show "—", the conflict cards are absent, choices are disabled. "Finish synchronizing" runs the existing
// `synchronize` action (whole-sync, ADR 0038) while there is no pending merge to finish.
import { computed } from "vue";
import { actions, runAction } from "../bimble";
import { t } from "../i18n";

const NONE = "—";
const sync = computed(() => actions.value.find((a) => a.name === "synchronize"));
</script>

<template>
  <div class="sy" data-bb="synchronize">
    <div class="banner b-o" data-bb="sync-banner" data-missing="query: sync (state, conflicts)">
      <b>{{ t('synchronize.stopped') }}</b> {{ t('synchronize.stoppedText', { n: NONE }) }}
    </div>
    <div class="body">
      <div class="list">
        <div class="head">
          <h2 class="disp" data-bb="sync-title" data-missing="query: sync (conflicts)">{{ t('synchronize.title', { n: NONE }) }}</h2>
          <span style="flex:1" />
          <button class="btn s" type="button" disabled data-missing="verb: sync_choose_all {keep: mine}" data-bb="sync-all-mine">{{ t('synchronize.allMine') }}</button>
          <button class="btn s" type="button" disabled data-missing="verb: sync_choose_all {keep: theirs}" data-bb="sync-all-theirs">{{ t('synchronize.allTheirs') }}</button>
        </div>
        <!-- One .card per conflicting element (class name, by · at, Show in 3D, Keep mine / Keep theirs, fields mine /
             theirs): rows absent until query `sync` returns conflicts, and verb sync_choose {id, keep} exists. -->
        <div class="merged" data-bb="sync-merged" data-missing="query: sync (merged count)">
          <span class="dot" style="background:var(--bb-moss)" />{{ t('synchronize.merged', { n: NONE }) }}
        </div>
        <div class="foot">
          <span class="note" data-missing="query: sync (conflicts without a choice)">{{ t('synchronize.chooseMore', { n: NONE }) }}</span>
          <button type="button" class="btn" :class="sync?.enabled ? 'p' : 'd'" :disabled="!sync?.enabled" data-bb="sync-finish"
                  @click="runAction('synchronize')">{{ t('synchronize.finish') }}</button>
        </div>
      </div>
      <div class="view" role="img" :aria-label="t('synchronize.view', { id: NONE })" data-bb="sync-view"
           data-missing="mine / theirs preview of the chosen element: query sync + a two-version mesh">
        <div class="legend">
          <div class="li"><span class="dot" style="background:var(--bb-pink)" />{{ t('synchronize.legendMine', { v: NONE }) }}</div>
          <div class="li"><span class="dash" />{{ t('synchronize.legendTheirs', { who: NONE, v: NONE }) }}</div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
/* The reference fills the viewport area edge to edge: undo .bb-page padding (24px 32px). */
.sy { margin: -24px -32px; min-height: calc(100% + 48px); display: flex; flex-direction: column; }
/* .banner{padding:10px 16px;font-size:14px;display:flex;gap:10px;align-items:center;flex-wrap:wrap} .b-o{background:#D2B347} */
.banner { padding: 10px 16px; font-size: 14px; display: flex; gap: 10px; align-items: center; flex-wrap: wrap; background: var(--bb-ochre); }
/* style="flex:1;display:flex;flex-wrap:wrap;min-height:0" */
.body { flex: 1; display: flex; flex-wrap: wrap; min-height: 0; }
/* style="flex:1 1 560px;min-width:0;padding:18px 20px;display:flex;flex-direction:column;gap:12px;overflow:hidden;background:#F5F2EB" */
.list { flex: 1 1 560px; min-width: 0; padding: 18px 20px; display: flex; flex-direction: column; gap: 12px; overflow: hidden; background: var(--bb-cream); }
.head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
/* .disp{font-family:'Sora','Source Sans 3',sans-serif} style="margin:0;font-size:20px;font-weight:700" */
h2 { font-family: 'Sora', 'Source Sans 3', sans-serif; margin: 0; font-size: 20px; font-weight: 700; }
.merged { display: flex; gap: 8px; align-items: center; font-size: 13.5px; }
.foot { margin-top: auto; display: flex; gap: 10px; align-items: center; justify-content: flex-end; }
/* .note{font-size:13px;color:#5C574D} */
.note { font-size: 13px; color: var(--bb-stone); }
/* .dot{width:12px;height:12px;border-radius:50%;border:1px solid #141414;display:inline-block;flex:none} */
.dot { width: 12px; height: 12px; border-radius: 50%; border: 1px solid var(--bb-ink); display: inline-block; flex: none; }
/* style="flex:1 1 420px;position:relative;background:#FFFFFF;min-width:0" */
.view { flex: 1 1 420px; position: relative; background: var(--bb-sheet); min-width: 0; }
/* style="position:absolute;left:14px;top:12px;display:flex;flex-direction:column;gap:6px;font-size:13px" */
.legend { position: absolute; left: 14px; top: 12px; display: flex; flex-direction: column; gap: 6px; font-size: 13px; }
.li { display: flex; gap: 8px; align-items: center; }
/* style="width:18px;border-top:1.4px dashed #6E88A3" (theirs is slate, ADR 0049) */
.dash { width: 18px; border-top: 1.4px dashed var(--bb-slate); }
.btn:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
