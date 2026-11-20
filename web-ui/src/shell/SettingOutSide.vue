<script setup lang="ts">
// Setting out tab's left panel (fr/Implantation.html aside.left > .props). The base point fields need an
// IfcMapConversion query and verb (none yet): drawn empty and disabled. Orientation = the active view's true_north.
import { computed } from "vue";
import { openViews, setView } from "../bimble";
import { t } from "../i18n";

const MISSING = "IfcMapConversion (query map_conversion, verb set_map_conversion)";
const fields = [["settingout.east", "m"], ["settingout.north", "m"], ["settingout.elevation", "m"], ["settingout.angle", "°"]] as const;
const view = computed(() => openViews.value.views.find((v) => v.id === openViews.value.active) ?? null);
const orient = (e: Event) => view.value && setView(view.value.id, { true_north: (e.target as HTMLSelectElement).value === "true" });
</script>

<template>
  <div class="pn">{{ t("settingout.title") }}</div>
  <div v-for="([k, u], i) in fields" :key="k" class="row">
    <label class="k" :for="`so-${i}`">{{ t(k) }}</label>
    <span style="display:inline-flex;align-items:center;gap:6px"><input :id="`so-${i}`" class="inp mono" value="" disabled :data-missing="MISSING" style="width:140px;text-align:right"><span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">{{ u }}</span></span>
  </div>
  <div class="row"><span class="k">{{ t("settingout.system") }}</span><span class="mono" :data-missing="MISSING">—</span></div>
  <div class="row"><span class="k">{{ t("settingout.readFrom") }}</span><span :data-missing="MISSING">—</span></div>
  <div class="sec">{{ t("settingout.view") }}</div>
  <div class="row"><label class="k" for="so-orient">{{ t("settingout.orientation") }}</label>
    <select id="so-orient" class="sel-inp" style="width:150px" data-bb="settingout-orientation" :disabled="!view" :value="String(view?.true_north ?? false)" @change="orient">
      <option value="false">{{ t("settingout.projectNorth") }}</option><option value="true">{{ t("settingout.trueNorth") }}</option>
    </select></div>
  <div class="row"><span class="k">{{ t("settingout.shown") }}</span><span :data-missing="MISSING">—</span></div>
  <button type="button" class="btn s" style="align-self:flex-start;margin-top:4px" disabled data-missing="move base point verb (set_map_conversion)" data-bb="settingout-move">{{ t("settingout.move") }}</button>
</template>

<style scoped>
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.k { color: var(--bb-stone); white-space: nowrap; }
.mono { font-family: 'DM Mono', monospace; }
.sec { font-size: 12px; font-family: 'DM Mono', monospace; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
</style>
