<script setup lang="ts">
import { computed, ref, watch } from "vue";
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
    <h2 id="props-title">Properties</h2>
    <p v-if="state.selection.length === 0" class="quiet">Click an element in the view.</p>
    <p v-else-if="state.selection.length > 1" class="many">
      <span class="dot" aria-hidden="true" />{{ state.selection.length }} selected
    </p>
    <p v-else-if="loadError" class="error">{{ loadError }}</p>
    <template v-else-if="detail">
      <label for="el-name">Name</label>
      <input
        id="el-name"
        v-model="name"
        type="text"
        :aria-invalid="renameError ? 'true' : 'false'"
        aria-describedby="el-name-error"
        @keydown.enter="commit"
        @blur="commit"
      />
      <p id="el-name-error" class="error" role="alert">{{ renameError ?? "" }}</p>
      <dl>
        <dt>Class</dt><dd>{{ detail.class }}</dd>
        <dt>Storey</dt><dd>{{ detail.storey_name ?? "—" }}</dd>
        <dt>Material</dt><dd>{{ detail.material_name ?? "—" }}</dd>
        <dt>Generator</dt><dd>{{ detail.generator }}</dd>
        <template v-for="[k, v] in sizes" :key="k">
          <dt>{{ k }}</dt><dd class="mono">{{ v }}</dd>
        </template>
      </dl>
    </template>
  </aside>
</template>

<style scoped>
.panel { flex: 0 0 280px; max-width: 100%; padding: 16px; border-right: 1px solid var(--line); background: var(--cream); }
@media (max-width: 600px) { .panel { flex: 1 1 100%; border-right: 0; border-bottom: 1px solid var(--line); } }
h2 { font: 700 18px var(--font-heading); margin: 0 0 12px; }
.quiet { color: var(--stone); margin: 0; }
.many { display: flex; align-items: center; gap: 8px; margin: 0; }
.dot { width: 12px; height: 12px; border-radius: 50%; background: var(--pink); }
label { display: block; font-weight: 600; font-size: 14px; color: var(--stone); margin-bottom: 4px; }
input { width: 100%; font: 16px var(--font-text); color: var(--ink); background: var(--sheet); border: 1px solid var(--line); border-radius: 6px; padding: 6px 8px; }
input:focus-visible { outline: 2px solid var(--ink); outline-offset: 1px; }
.error { color: var(--ochre-text); font-size: 14px; margin: 4px 0 0; min-height: 1.2em; overflow-wrap: anywhere; }
dl { display: grid; grid-template-columns: auto 1fr; gap: 4px 12px; margin: 12px 0 0; }
dt { color: var(--stone); }
dd { margin: 0; overflow-wrap: anywhere; }
.mono { font-family: var(--font-mono); }
</style>
