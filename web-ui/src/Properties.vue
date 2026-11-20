<script setup lang="ts">
import { computed, ref, watch } from "vue";
import { t } from "./i18n";
import { state, element, rename, type ElementDetail } from "./bimble";

const detail = ref<ElementDetail | null>(null);
const loadError = ref<string | null>(null);
const name = ref("");
const renameError = ref<string | null>(null);

const one = computed(() => (state.value.selection.length === 1 ? state.value.selection[0] ?? null : null));

// Reload on selection or model change (rev). The watcher runs after the frame, so the viewer is free.
watch(
  () => [one.value, state.value.rev] as const,
  ([id]) => {
    loadError.value = null;
    if (!id) { detail.value = null; return; }
    try {
      const d = element(id);
      if (d.id !== detail.value?.id) renameError.value = null;
      detail.value = d;
      name.value = d.name;
    } catch (e) {
      detail.value = null;
      loadError.value = e instanceof Error ? e.message : String(e);
    }
  },
);

function commit(): void {
  const d = detail.value;
  if (!d || name.value === d.name) return;
  renameError.value = rename(d.id, name.value);
}

// Numbers among the generator parameters: the sizes, in mm.
const sizes = computed(() =>
  Object.entries(detail.value?.generator_params ?? {}).filter(
    (e): e is [string, number] => typeof e[1] === "number",
  ),
);
</script>

<template>
  <aside class="panel" aria-labelledby="props-title">
    <h2 id="props-title">{{ t('props.title') }}</h2>
    <p v-if="state.selection.length === 0" class="quiet">{{ t('props.empty') }}</p>
    <p v-else-if="state.selection.length > 1" class="many">
      <span class="dot" aria-hidden="true" />{{ t('props.selected', { n: state.selection.length }) }}
    </p>
    <p v-else-if="loadError" class="error" role="alert">{{ t('props.error', { msg: loadError }) }}</p>
    <template v-else-if="detail">
      <label for="el-name">{{ t('props.name') }}</label>
      <input
        id="el-name"
        v-model="name"
        type="text"
        :aria-invalid="renameError ? 'true' : 'false'"
        aria-describedby="el-name-error"
        @keydown.enter="commit"
        @blur="commit"
      />
      <p id="el-name-error" role="alert" :class="{ error: renameError }">{{ renameError ? t('props.error', { msg: renameError }) : "" }}</p>
      <dl>
        <dt>{{ t('props.class') }}</dt><dd>{{ detail.class }}</dd>
        <dt>{{ t('props.storey') }}</dt><dd>{{ detail.storey_name ?? "—" }}</dd>
        <dt>{{ t('props.material') }}</dt><dd>{{ detail.material_name ?? "—" }}</dd>
        <dt>{{ t('props.generator') }}</dt><dd>{{ detail.generator }}</dd>
        <template v-for="[k, v] in sizes" :key="k">
          <dt>{{ k }}</dt><dd class="mono">{{ v }}</dd>
        </template>
      </dl>
    </template>
  </aside>
</template>

<style scoped>
/* Panel: cream ground, 16px padding; line border is decoration only. */
.panel { flex: 0 0 280px; max-width: 100%; padding: var(--bb-space-4); border-right: var(--bb-border-hairline); background: var(--bb-cream); }
@media (max-width: 600px) { .panel { flex: 1 1 100%; border-right: 0; border-bottom: var(--bb-border-hairline); } }
/* Panel title: Sora 500 14px. */
h2 { font: 500 var(--bb-size-ui) var(--bb-font-display); margin: 0 0 var(--bb-space-3); }
.quiet { color: var(--bb-stone); margin: 0; }
.many { display: flex; align-items: center; gap: var(--bb-space-2); margin: 0; }
/* Status dot: 12px pink disc with a 1px ink outline. */
.dot { width: 12px; height: 12px; border-radius: 50%; background: var(--bb-selection); border: 1px solid var(--bb-ink); }
/* Input: label stone 13px above; sheet fill, 1px stone border, 8px radius, 32px, ink text. */
label { display: block; font-size: var(--bb-size-small); color: var(--bb-stone); margin-bottom: var(--bb-space-1); }
input { width: 100%; height: var(--bb-target); font: var(--bb-size-body) var(--bb-font-text); color: var(--bb-ink); background: var(--bb-sheet); border: var(--bb-border-control); border-radius: var(--bb-radius-sm); padding: 0 var(--bb-space-2); }
input:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
#el-name-error { margin: var(--bb-space-1) 0 0; min-height: 1.2em; }
/* Error: ink text on an ochre band, with the word "Error". */
.error { color: var(--bb-ink); background: var(--bb-error); font-size: var(--bb-size-ui); margin: var(--bb-space-1) 0 0; padding: 2px var(--bb-space-2); overflow-wrap: anywhere; }
dl { display: grid; grid-template-columns: auto 1fr; gap: var(--bb-space-1) var(--bb-space-3); margin: var(--bb-space-3) 0 0; }
dt { color: var(--bb-stone); }
dd { margin: 0; overflow-wrap: anywhere; }
.mono { font-family: var(--bb-font-data); }
</style>
