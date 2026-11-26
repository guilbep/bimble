<script lang="ts">
export const capability = "views"; // VIEWSHEET: verb view_add (ccore/src/verbs_views.c, SEC1)
</script>

<script setup lang="ts">
// View → New view… (ticket VIEWSHEET, ADR 0076): kind, level (plans), facing (sections), name and scale, then one
// view_add (one undo step, the scale and name in it) and the new view opens. What the core cannot make (an
// elevation) is listed "not available yet". The section's cut goes through the middle of the model (the session
// fills `origin`, views.rs new_section); the section tool (Section) draws one by two clicks instead.
import { computed, ref } from "vue";
import { openView, storeys, viewAdd } from "../bimble";
import { t } from "../i18n";
import { send } from "../shell/send";
import { currentDialog } from ".";

type Kind = "plan" | "ceiling" | "section" | "elevation" | "three_d";
const KINDS: Kind[] = ["plan", "ceiling", "section", "elevation", "three_d"];
const FACINGS = ["north", "east", "south", "west"] as const;
const kind = ref<Kind>("plan");
const level = ref(storeys.value[0]?.id ?? "");
const facing = ref<(typeof FACINGS)[number]>("north");
const name = ref("");
const scale = ref(100);
const error = ref<string | null>(null);
const needsLevel = computed(() => kind.value === "plan" || kind.value === "ceiling");
const close = () => { currentDialog.value = null; };

function create(): void {
  if (kind.value === "elevation") return;
  const args: Parameters<typeof viewAdd>[0] = { kind: kind.value };
  if (needsLevel.value) args.storey = level.value;
  if (kind.value === "section") args.facing = facing.value;
  if (name.value.trim()) args.name = name.value.trim();
  if (kind.value !== "three_d") args.scale = Math.round(scale.value);
  const r = viewAdd(args);
  error.value = r.error;
  if (r.error || !r.id) return;
  close();
  error.value = openView(r.id);
  send({ do: "showView" }); // shell_graph: control newview-create
}
</script>

<template>
  <div class="bb-dialog nv" role="dialog" aria-modal="true" :aria-label="t('vs.newView.title')" data-bb="new-view-dialog">
    <div class="head">
      <h2 class="disp">{{ t('vs.newView.title') }}</h2>
      <span style="flex:1"></span>
      <button class="btn s" type="button" data-bb="new-view-close" @click="close">{{ t('vs.close') }}</button>
    </div>
    <fieldset class="kinds">
      <legend class="k">{{ t('vs.kind') }}</legend>
      <label v-for="k in KINDS" :key="k" :class="{ ny: k === 'elevation' }" :title="k === 'elevation' ? t('vs.elevationMissing') : undefined">
        <input v-model="kind" type="radio" name="nv-kind" :value="k" :disabled="k === 'elevation'" :data-bb="`new-view-kind-${k}`">
        {{ t(`vs.kind.${k}`) }}<span v-if="k === 'elevation'" class="na" data-not-yet="">{{ t('vs.notYet') }}</span>
      </label>
    </fieldset>
    <div class="grid">
      <template v-if="needsLevel">
        <label class="k" for="nv-level">{{ t('vs.level') }}</label>
        <select id="nv-level" v-model="level" class="inp" data-bb="new-view-level">
          <option v-for="s in storeys" :key="s.id" :value="s.id">{{ s.name }}</option>
        </select>
      </template>
      <template v-if="kind === 'section'">
        <label class="k" for="nv-facing">{{ t('vs.facing') }}</label>
        <select id="nv-facing" v-model="facing" class="inp" data-bb="new-view-facing">
          <option v-for="f in FACINGS" :key="f" :value="f">{{ t(`vs.facing.${f}`) }}</option>
        </select>
      </template>
      <label class="k" for="nv-name">{{ t('vs.name') }}</label>
      <input id="nv-name" v-model="name" class="inp" :placeholder="t('vs.namePlaceholder')" maxlength="200" data-bb="new-view-name">
      <template v-if="kind !== 'three_d'">
        <label class="k" for="nv-scale">{{ t('vs.scale') }}</label>
        <input id="nv-scale" v-model.number="scale" class="inp mono" type="number" min="1" max="5000" step="1" data-bb="new-view-scale">
      </template>
    </div>
    <p v-if="error" class="err" role="alert" data-bb="new-view-error">{{ error }}</p>
    <div><button class="btn p" type="button" data-bb="new-view-create" :disabled="kind === 'elevation' || (needsLevel && !level)" @click="create">{{ t('vs.create') }}</button></div>
  </div>
</template>

<style scoped>
.nv { width: min(420px, calc(100vw - 32px)); color: var(--bb-ink); display: flex; flex-direction: column; gap: 12px; }
.head { display: flex; align-items: center; gap: 10px; }
.disp { font-family: var(--bb-font-display); margin: 0; font-size: var(--bb-size-heading); font-weight: 700; }
.kinds { border: 0; padding: 0; margin: 0; display: flex; flex-wrap: wrap; gap: 6px 14px; }
.kinds legend { padding: 0; margin-bottom: 4px; }
.kinds label { display: inline-flex; align-items: center; gap: 4px; }
.kinds label.ny { color: var(--bb-stone); cursor: help; }
.na { margin-left: 4px; font-size: 11.5px; border: 1px dashed var(--bb-line); border-radius: 8px; padding: 0 5px; }
.grid { display: grid; grid-template-columns: 110px 1fr; gap: 8px 10px; align-items: center; }
.k { color: var(--bb-stone); font-size: 13px; }
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.mono { font-family: var(--bb-font-data); }
.err { margin: 0; color: var(--bb-attention); font-size: 13px; }
:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
