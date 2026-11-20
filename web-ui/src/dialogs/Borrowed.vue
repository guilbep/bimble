<script lang="ts">
export const hint = "borrowed.hint";
</script>

<script setup lang="ts">
// Screen C2, Borrowed element (wireframes v01 fr/Borrowed.html): the alert egui shows when a change
// touches an element someone else borrowed (ADR 0038, collab.rs `belongs_ui`). Synchronize or Cancel.
import { computed, onMounted, onUnmounted, ref } from "vue";
import { actions, collab, dismissBelongs, runAction, state } from "../bimble";
import { t } from "../i18n";
import { currentDialog } from ".";

// The first refused element of query('collab').belongs; "—" when missing (the borrow time is null today).
const NONE = computed(() => {
  const b = (state.value.rev, collab().belongs?.[0]);
  const what = b ? [b.class, b.element_name].filter(Boolean).join(" ") : "";
  const time = b?.since == null ? "—" : new Date(b.since * 1000).toLocaleString();
  return { what: what || "—", who: b?.owner || "—", time };
});
const sync = computed(() => actions.value.find((a) => a.name === "synchronize"));
// Anchored 20px inside the viewport's bottom right corner, as the reference (it lives in .vp there).
const at = ref<Record<string, string>>({});
function place() {
  const r = document.querySelector('[data-bb="viewport"]')?.getBoundingClientRect();
  if (r) at.value = { right: `${innerWidth - r.right + 20}px`, bottom: `${innerHeight - r.bottom + 20}px` };
}
onMounted(() => { place(); addEventListener("resize", place); });
onUnmounted(() => removeEventListener("resize", place));
function cancel() {
  const err = dismissBelongs();
  if (err) console.warn(`borrowed cancel: ${err}`);
  currentDialog.value = null;
}
function synchronize() {
  runAction("synchronize");
  currentDialog.value = null;
}
</script>

<template>
  <div class="bb-borrowed" role="alertdialog" :aria-label="t('borrowed.title')" data-bb="borrowed" :style="at">
    <div class="banner">{{ t('borrowed.belongs', NONE) }}</div>
    <p :data-missing="NONE.time === '—' ? 'the borrow time: remote::borrowed drops Borrows.time' : undefined">{{ t('borrowed.text', NONE) }}</p>
    <div class="acts">
      <!-- Cancel: closes the alert and clears Editor.belongs, as egui. -->
      <button class="btn s" type="button" @click="cancel">{{ t('borrowed.cancel') }}</button>
      <button class="btn" :class="sync?.enabled ? 'p' : 'd'" type="button" :disabled="!sync?.enabled" @click="synchronize">{{ t('borrowed.sync') }}</button>
    </div>
  </div>
</template>

<style>
/* The reference draws the alert in the viewport, no dim; clicks pass through to the view. */
.bb-app > .bb-backdrop:has(> .bb-borrowed) { background: none; pointer-events: none; }
</style>

<style scoped>
/* style="position:absolute;right:20px;bottom:20px;width:440px;background:#F5F2EB;border:1px solid #5C574D;border-radius:16px;padding:18px;display:flex;flex-direction:column;gap:10px" (in .vp) */
.bb-borrowed { pointer-events: auto; position: fixed; right: 20px; bottom: 54px; box-sizing: border-box; width: 440px; background: var(--bb-cream); border: 1px solid var(--bb-stone); border-radius: 16px; padding: 18px; display: flex; flex-direction: column; gap: 10px; font-size: 14px; line-height: 1.35; color: var(--bb-ink); }
/* .banner{padding:10px 16px;font-size:14px;display:flex;gap:10px;align-items:center;flex-wrap:wrap} .b-o{background:#D2B347} + style="border-radius:12px;padding:8px 12px" */
.banner { padding: 8px 12px; border-radius: 12px; font-size: 14px; display: flex; gap: 10px; align-items: center; flex-wrap: wrap; background: var(--bb-ochre); }
p { margin: 0; }
.acts { display: flex; gap: 10px; justify-content: flex-end; }
/* .btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;min-height:32px;padding:5px 16px;border-radius:999px;font:inherit;font-weight:600;color:#141414;cursor:pointer;text-decoration:none;white-space:nowrap} */
.btn:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
