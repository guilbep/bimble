<script lang="ts">
export const hint = "teammember.hint";
</script>

<script setup lang="ts">
// Screen A4, Team member (wireframes v01 fr/TeamMember.html; ADR 0055): the 420-wide panel over
// Project information, opened by "Add a team member". No member verbs in Rust yet (member_add,
// member_set, member_mine): every field is drawn disabled with data-missing. No Cancel/Save (ADR 0052, 0055 §5).
import { onMounted, ref } from "vue";
import { t } from "../i18n";
import type * as copy from "../i18n/parts/teammember";
import { currentDialog } from ".";

const tx = (k: keyof typeof copy.fr) => t(k);
// Layout only: the reference's aside covers the page area (position:absolute;top:0;bottom:0 in the page row).
const box = ref<Record<string, string>>({});
onMounted(() => {
  const r = document.querySelector('[data-bb="page"]')?.getBoundingClientRect();
  if (r) box.value = { top: `${r.top}px`, height: `${r.height}px` };
});
const FIELDS = ["company", "contact", "address"] as const;
const LAST = ["phone", "email"] as const;
const miss = (f: string) => `verb member_set {n, field: ${f}, value} (ADR 0055)`;
</script>

<template>
  <aside class="tm-ov" :style="box" :aria-label="tx('teammember.title')" data-bb="team-member">
    <div style="display:flex;align-items:center">
      <div><div class="eyebrow">{{ tx('teammember.title') }}</div><h2 class="disp" data-bb="team-member-title">{{ tx('teammember.new') }}</h2></div>
      <span style="flex:1"></span>
      <button type="button" class="ib" :aria-label="tx('teammember.close')" data-bb="team-member-close" @click="currentDialog = null"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button>
    </div>
    <div v-for="f in FIELDS" :key="f" class="fld">
      <label class="lbl" :for="`tm-${f}`">{{ tx(`teammember.${f}`) }} <span class="mono k">{member.&lt;n&gt;.{{ f }}}</span></label>
      <input :id="`tm-${f}`" class="inp" disabled :data-missing="miss(f)" :data-bb="`team-member-${f}`">
    </div>
    <div style="display:grid;grid-template-columns:110px minmax(0,1fr);gap:10px">
      <div v-for="f in ['postcode', 'city'] as const" :key="f" class="fld">
        <label class="lbl" :for="`tm-${f}`">{{ tx(`teammember.${f}`) }} <span class="mono k">{member.&lt;n&gt;.{{ f }}}</span></label>
        <input :id="`tm-${f}`" class="inp" disabled :data-missing="miss(f)" :data-bb="`team-member-${f}`">
      </div>
    </div>
    <div v-for="f in LAST" :key="f" class="fld">
      <label class="lbl" :for="`tm-${f}`">{{ tx(`teammember.${f}`) }} <span class="mono k">{member.&lt;n&gt;.{{ f }}}</span></label>
      <input :id="`tm-${f}`" class="inp" disabled :data-missing="miss(f)" :data-bb="`team-member-${f}`">
    </div>
    <label style="display:flex;gap:8px;align-items:center;font-size:14px"><input type="checkbox" disabled data-missing="verb member_mine {n} (ADR 0055)" data-bb="team-member-mine"> {{ tx('teammember.mine') }}</label>
    <button type="button" class="qb" disabled data-missing="verb member_set with a free field (ADR 0055)" data-bb="team-member-add-info"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 5v14M5 12h14"/></svg> {{ tx('teammember.addInfo') }}</button>
  </aside>
</template>

<style>
/* The reference draws the panel over the page, no dim; clicks outside pass through. */
.bb-app > .bb-backdrop:has(> .tm-ov) { background: none; pointer-events: none; }
</style>

<style scoped>
/* aside style="position:absolute;right:0;top:0;bottom:0;width:420px;background:#F5F2EB;border-left:1px solid #5C574D;padding:22px;display:flex;flex-direction:column;gap:12px" */
.tm-ov { pointer-events: auto; position: fixed; right: 0; box-sizing: border-box; width: 420px; background: var(--bb-cream); border-left: 1px solid var(--bb-stone); padding: 22px; display: flex; flex-direction: column; gap: 12px; font-size: 14px; line-height: 1.35; color: var(--bb-ink); }
.tm-ov * { box-sizing: border-box; }
/* .eyebrow{font-family:'DM Mono',monospace;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#5C574D} */
.eyebrow { font-family: 'DM Mono', monospace; font-size: 12px; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); }
/* h2.disp style="margin:0;font-size:20px;font-weight:700" */
h2 { font-family: 'Sora', 'Source Sans 3', sans-serif; margin: 0; font-size: 20px; font-weight: 700; }
.mono { font-family: 'DM Mono', ui-monospace, Menlo, monospace; }
.k { font-size: 11.5px; }
/* .fld{display:flex;flex-direction:column;gap:3px} .lbl{font-size:13px;color:#5C574D} */
.fld { display: flex; flex-direction: column; gap: 3px; }
.lbl { font-size: 13px; color: var(--bb-stone); }
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
/* .ib{width:32px;height:32px;border-radius:999px;border:0;background:none;...} .ib:hover{background:#EDE7DB} */
.ib { width: 32px; height: 32px; border-radius: 999px; border: 0; background: none; display: inline-flex; align-items: center; justify-content: center; color: var(--bb-ink); cursor: pointer; padding: 0; flex: none; }
.ib:hover { background: var(--bb-sunk); }
/* .qb{...} style="align-self:flex-start;color:#A35A37" */
.qb { display: inline-flex; align-items: center; gap: 4px; height: 32px; padding: 0 12px; border-radius: 999px; border: 0; background: none; font: inherit; cursor: pointer; white-space: nowrap; align-self: flex-start; color: var(--bb-clay-text); }
.ib:focus-visible, .qb:focus-visible, .inp:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
