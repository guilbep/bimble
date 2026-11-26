<script setup lang="ts">
// The section box's orientation (ticket ISS71, issue #71): a two-state control beside the View tab's
// Section box button. Project north (the default: the building's axes) or true north (the map's), the
// C core's actions box_north_project and box_north_true (pressed: the choice the session keeps;
// enabled only in 3D when the model's georeference makes the two differ). Changing it turns the box
// and keeps what it held (ccore/session/visibility.h cc_vis_orient).
import { computed } from "vue";
import { actions, northInfo, runAction } from "../bimble";
import { t } from "../i18n";

const ROWS = [
  { action: "box_north_project", key: "boxNorth.project", tip: "boxNorth.projectTip" },
  { action: "box_north_true", key: "boxNorth.true", tip: "boxNorth.trueTip" },
] as const;
const state = (a: string) => actions.value.find((s) => s.name === a);
// Why it is off: the same turn both ways (no georeference, or one that does not turn), else not in 3D.
const why = computed(() => {
  const n = northInfo.value;
  return t(!n || n.plan_turn_project === n.plan_turn_true ? "boxNorth.off" : "boxNorth.flat");
});
function pick(a: string): void { if (state(a)?.enabled && !state(a)?.pressed) runAction(a); }
</script>

<template>
  <div class="bn" role="radiogroup" :aria-label="t('boxNorth.label')" data-bb="box-north">
    <span class="lbl">{{ t('boxNorth.label') }}</span>
    <div class="seg">
      <button v-for="r in ROWS" :key="r.action" type="button" role="radio" :aria-checked="state(r.action)?.pressed === true"
              :disabled="!state(r.action)?.enabled" :class="{ on: state(r.action)?.pressed }" :data-bb="`box-north-${r.action}`"
              :title="state(r.action)?.enabled ? t(r.tip) : `${t(r.tip)}. ${why}`" @click="pick(r.action)">{{ t(r.key) }}</button>
    </div>
  </div>
</template>

<style scoped>
.bn { display: flex; flex-direction: column; justify-content: center; gap: 4px; padding: 4px 6px; font-size: 12.5px; align-self: center; }
.lbl { font-size: var(--bb-size-eyebrow); color: var(--bb-stone); text-align: center; }
.seg { display: inline-flex; border: 1px solid var(--bb-line); border-radius: var(--bb-radius-pill, 999px); overflow: hidden; }
.seg button { border: 0; background: none; padding: 3px 9px; font: inherit; font-size: 12px; color: var(--bb-ink); cursor: pointer; }
.seg button + button { border-left: 1px solid var(--bb-line); }
.seg button.on { background: var(--bb-sunk); font-weight: 600; box-shadow: inset 0 0 0 1px var(--bb-ink); }
.seg button:hover:not([disabled]):not(.on) { background: var(--bb-sunk); }
.seg button:focus-visible { outline: var(--bb-focus); outline-offset: -2px; }
.seg button[disabled] { color: var(--bb-stone); opacity: 0.55; cursor: default; }
</style>
