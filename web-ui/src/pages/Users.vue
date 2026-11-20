<script lang="ts">
export const hint = "users.hint";
</script>

<script setup lang="ts">
// C4 Users and rights (wireframes v01 Users.html; ADR 0061, Proposed). No `people` list in Rust yet: the only row is
// you (query "state" user, Settings → You) with your right and last sync from query "collab". Editing is data-missing.
import { computed } from "vue";
import { collab, state } from "../bimble";
import { t } from "../i18n";

const RIGHTS = ["view", "edit", "admin"] as const;
const c = computed(() => (state.value.rev, collab()));
const me = computed(() => state.value.user ?? null);
// ponytail: Rust's right string shown through the three keys when it matches one, raw otherwise.
const right = computed(() => {
  const r = c.value.right;
  return r == null ? "—" : (RIGHTS as readonly string[]).includes(r) ? t(`users.right.${r}`) : r;
});
const lastSync = computed(() => (c.value.last_sync == null ? "—" : new Date(c.value.last_sync * 1000).toLocaleString()));
</script>

<template>
  <div class="us" data-bb="users">
    <div class="head">
      <h2 style="font-size:20px" data-bb="users-title">{{ t('users.title') }}</h2>
      <span style="flex:1" />
      <input class="inp" :placeholder="t('users.emailPlaceholder')" style="width:280px" aria-label="E-mail" disabled data-missing="verb: user_add {trigram, name, email, role, right}">
      <button class="btn p" type="button" disabled data-missing="verb: user_add {trigram, name, email, role, right}" data-bb="users-add">{{ t('users.add') }}</button>
    </div>
    <table class="t" data-bb="users-table" data-missing="query: users (people rows with last_sync, borrowed, commits)">
      <thead><tr><th>{{ t('users.user') }}</th><th>{{ t('users.email') }}</th><th>{{ t('users.role') }}</th><th>{{ t('users.right') }}</th><th>{{ t('users.subprojects') }}</th><th>{{ t('users.lastSync') }}</th></tr></thead>
      <tbody>
        <tr v-if="me">
          <td><span style="display:inline-flex;align-items:center;gap:8px"><span class="mono av">{{ me.trigram }}</span>{{ me.name }}</span></td>
          <td class="mono" style="font-size:12.5px">{{ me.email ?? "—" }}</td>
          <td><input class="inp" style="width:130px" :aria-label="t('users.role')" disabled data-missing="verb: user_set {trigram, role}"></td>
          <td><span class="sel-inp off" style="width:140px" data-missing="verb: user_set {trigram, right}">{{ right }}<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6" /></svg></span></td>
          <td data-missing="query: users (subprojects)">—</td>
          <td class="mono">{{ lastSync }}</td>
        </tr>
      </tbody>
    </table>
    <div style="display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px" data-bb="users-rights">
      <div v-for="r in RIGHTS" :key="r" class="card">
        <div class="eyebrow">{{ t(`users.right.${r}`) }}</div>
        <p style="margin:6px 0 0">{{ t(`users.desc.${r}`) }}</p>
      </div>
    </div>
    <div class="banner b-o" style="border-radius:12px" data-bb="users-advisory">{{ t('users.advisory') }}</div>
  </div>
</template>

<style scoped>
/* .page style="display:flex;flex-direction:column;gap:14px" (padding from .bb-page) */
.us { display: flex; flex-direction: column; gap: 14px; }
/* style="display:flex;align-items:center;gap:10px;flex-wrap:wrap" */
.head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
/* .page h2{font-family:'Sora',sans-serif;margin:0} */
h2 { font-family: 'Sora', sans-serif; margin: 0; }
/* .mono{font-family:'DM Mono',ui-monospace,Menlo,monospace} */
.mono { font-family: 'DM Mono', ui-monospace, Menlo, monospace; }
/* style="width:34px;height:34px;border-radius:999px;background:#EDE7DB;display:inline-flex;align-items:center;justify-content:center;font-size:11px" */
.av { width: 34px; height: 34px; border-radius: 999px; background: var(--bb-sunk); display: inline-flex; align-items: center; justify-content: center; font-size: 11px; }
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
/* table.t … */
table.t { border-collapse: collapse; width: 100%; font-size: 13.5px; }
table.t th { text-align: left; font-weight: 600; color: var(--bb-stone); border-bottom: 1px solid var(--bb-stone); padding: 6px 8px; white-space: nowrap; font-size: 13px; }
table.t td { border-bottom: 1px solid var(--bb-sunk); padding: 5px 8px; vertical-align: middle; }
/* .card{background:#FFFFFF;border:1px solid #DDD6C8;border-radius:16px;padding:18px} */
.card { background: var(--bb-sheet); border: 1px solid var(--bb-line); border-radius: 16px; padding: 18px; }
/* .eyebrow{font-family:'DM Mono',monospace;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#5C574D} */
.eyebrow { font-family: 'DM Mono', monospace; font-size: 12px; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); }
/* .banner{padding:10px 16px;font-size:14px;display:flex;gap:10px;align-items:center;flex-wrap:wrap} .b-o{background:#D2B347} */
.banner { padding: 10px 16px; font-size: 14px; display: flex; gap: 10px; align-items: center; flex-wrap: wrap; background: var(--bb-ochre); }
.btn:focus-visible, .inp:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* Missing in Rust: the reference's .off (color:#5C574D;opacity:.55). */
.btn:disabled, .inp:disabled, .off { color: var(--bb-stone); opacity: .55; cursor: default; }
</style>
