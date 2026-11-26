<script lang="ts">
import HistorySide from "./HistorySide.vue";
export const hint = "history.hint";
export const side = HistorySide;
export const capability = "history"; // FAKE1
</script>

<script setup lang="ts">
// C6 History (wireframes v01 History.html; ADR 0038). Versions, the branch, the changes of a version and
// the changes not yet in a version come from the C core's history store (ticket H1:
// ccore/session/versions.h). H2a: Create version (action commit, the author from the settings: without a
// name the card sends the user to Settings), New variant (branch_create) and the branch switch
// (branch_switch, refused while changes are not in a version, as Rust's Switch) where the core says
// history_write (natively). Merging and compare are tickets H2b and later: drawn as the reference,
// disabled, data-missing. Without a history (the Rust viewer, the browser, a model with no history
// folder) the page says why; the first version starts one.
import { computed, onMounted, ref, watch } from "vue";
import { actionBody, loadRepoHistory, repoHistory, state, versionChanges, type VersionChange } from "../bimble";
import { t } from "../i18n";
import { act, send } from "../shell/send";
import { useCapability } from "../shell/capability";

onMounted(loadRepoHistory);
watch(() => [state.value.rev, state.value.file], loadRepoHistory);

const versions = computed(() => (repoHistory.value?.versions.history ? repoHistory.value.versions.versions : []));
const head = computed(() => (repoHistory.value?.versions.history ? repoHistory.value.versions.head : null));
const noHistory = computed(() => {
  const v = repoHistory.value?.versions;
  return v && !v.history ? v.reason : null;
});
const pending = computed(() => (repoHistory.value?.pending.history ? repoHistory.value.pending.changes : null));
const selected = ref<string | null>(null);
watch(versions, (v) => {
  if (!v.some((x) => x.hash === selected.value)) selected.value = v[0]?.hash ?? null;
});
const selectedVersion = computed(() => versions.value.find((v) => v.hash === selected.value) ?? null);
/** The selected version against its first parent; the first version has none. */
const changes = computed<VersionChange[] | null>(() => {
  const v = selectedVersion.value;
  if (!v || !v.parents[0]) return null;
  const r = versionChanges(v.parents[0], v.hash);
  return r.history ? r.changes : null;
});
interface Row { element: string; field: string; change: string }
const rows = computed<Row[]>(() => (changes.value ?? []).flatMap((c) => {
  const element = `${c.kind} ${c.name}`;
  if (c.change !== "modified") return [{ element, field: "—", change: t(c.change === "added" ? "history.added" : "history.removed") }];
  return (c.fields ?? []).map((f) => ({ element, field: f.path, change: `${JSON.stringify(f.before)} → ${JSON.stringify(f.after)}` }));
}));
// ---- H2a: commit, new branch, switch ----
const canWrite = useCapability("history_write");
const branches = computed(() => (repoHistory.value?.branches.history ? repoHistory.value.branches.branches : []));
/** settings.rs author(): "name <e-mail>"; null without a name (the card links to Settings). */
const author = computed(() => {
  const u = state.value.user;
  if (!u?.name) return null;
  return u.email ? `${u.name} <${u.email}>` : u.name;
});
const message = ref("");
const commitError = ref<string | null>(null);
/** Rust's Commit: a message, and changes to commit (or no history yet: the first version). */
const canCommit = computed(() => canWrite.value && author.value !== null && message.value.trim() !== "" &&
  (noHistory.value !== null || (pending.value?.length ?? 0) > 0));
function commit(): void {
  commitError.value = null;
  try {
    actionBody("commit", { message: message.value });
    message.value = "";
  } catch (e) { commitError.value = e instanceof Error ? e.message : String(e); }
  loadRepoHistory();
}
const branchName = ref("");
const branchOpen = ref(false);
const branchError = ref<string | null>(null);
function createBranch(): void {
  branchError.value = null;
  try {
    actionBody("branch_create", { name: branchName.value.trim() });
    branchName.value = "";
    branchOpen.value = false;
  } catch (e) { branchError.value = e instanceof Error ? e.message : String(e); }
  loadRepoHistory();
}
/** Rust disables Switch while changes are not committed ("Commit or undo the changes first"). */
const canSwitch = computed(() => canWrite.value && head.value !== null && (pending.value?.length ?? 1) === 0);
/** A switch is navigation (the model changes): a refusal goes to the shell's error state. */
function switchTo(name: string): void {
  if (!name || name === head.value?.branch) return;
  try { actionBody("branch_switch", { name }); } catch (e) { send({ do: "failed", message: e instanceof Error ? e.message : String(e) }); }
  loadRepoHistory();
}
// ---- end H2a ----
const when = (s: number) => new Date(s * 1000).toISOString().slice(0, 16).replace("T", " ");
const short = (h: string) => h.slice(0, 12);
</script>

<template>
  <div class="hi" data-bb="history">
    <section class="col">
      <div class="head">
        <h2 style="font-size:20px" data-bb="history-title">{{ t('history.title') }}</h2>
        <span style="flex:1" />
        <!-- H2a: the branch, and Switch (branch_switch); disabled while changes are not in a version (Rust's Switch). -->
        <label class="sel-inp" :class="{ off: !canSwitch }" style="width:170px" data-bb="history-branch" :title="canSwitch ? '' : t('history.switchHint')">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="6" cy="5" r="2" /><circle cx="6" cy="19" r="2" /><circle cx="18" cy="8" r="2" /><path d="M6 7v10M18 10c0 5-12 3-12 7" /></svg>
          <select class="sel" :aria-label="t('history.branch')" :disabled="!canSwitch" :value="head?.branch ?? ''" data-bb="history-switch" @change="switchTo(($event.target as HTMLSelectElement).value)">
            <option v-if="head && !head.branch" value="">{{ short(head.commit ?? '') }}</option>
            <option v-if="!head" value="">—</option>
            <option v-for="b in branches" :key="b.name" :value="b.name">{{ b.name }}</option>
          </select>
        </label>
        <button class="btn s" type="button" :disabled="!canWrite || !head?.commit" data-bb="history-new-variant" @click="branchOpen = !branchOpen; branchError = null">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 5v14M5 12h14" /></svg>{{ t('history.newVariant') }}
        </button>
      </div>
      <!-- H2a: the new branch's name (branch_create, then on it: the model stays). -->
      <div v-if="branchOpen" class="newb" data-bb="history-branch-form">
        <input v-model="branchName" class="inp" style="flex:1" :placeholder="t('history.branchPlaceholder')" :aria-label="t('history.branchName')" data-bb="history-branch-name" @keydown.enter="branchName.trim() && createBranch()">
        <button class="btn p" type="button" :disabled="!branchName.trim()" data-bb="history-branch-create" @click="createBranch">{{ t('history.createBranch') }}</button>
      </div>
      <p v-if="branchError" class="error" role="alert" data-bb="history-branch-error">{{ branchError }}</p>
      <!-- The version rows (query versions: Repo log from HEAD, newest first); the graph is not drawn. -->
      <p v-if="noHistory" class="note" style="margin:0" data-bb="history-none">{{ t('history.none', { reason: noHistory }) }}</p>
      <div class="versions" data-bb="history-versions">
        <button v-for="v in versions" :key="v.hash" type="button" class="ver" :class="{ on: v.hash === selected }" :aria-pressed="v.hash === selected" @click="selected = v.hash">
          <span class="mono">{{ short(v.hash) }}</span>
          <span class="msg">{{ v.message }}</span>
          <span class="who">{{ v.author }} · {{ when(v.time) }}</span>
        </button>
      </div>
      <div class="card" data-bb="history-new-version">
        <div class="eyebrow">{{ t('history.newVersion') }}</div>
        <div class="row"><span>{{ t('history.pending') }}</span><span class="chip c-o nx" :class="{ off: pending === null }" data-bb="history-pending">{{ t('history.elements', { n: pending === null ? '—' : pending.length }) }}</span></div>
        <!-- H2a: the author from the settings; without a name, Settings first (state.user is null). -->
        <div class="row" data-bb="history-author">
          <span>{{ t('history.author') }}</span>
          <span v-if="author" class="mono">{{ author }}</span>
          <a v-else href="#" data-bb="history-author-settings" @click.prevent="act('settings')">{{ t('history.setName') }}</a>
        </div>
        <input v-model="message" class="inp" :placeholder="t('history.messagePlaceholder')" :aria-label="t('history.message')" :disabled="!canWrite" data-bb="history-message" @keydown.enter="canCommit && commit()">
        <p v-if="commitError" class="error" role="alert" data-bb="history-commit-error">{{ commitError }}</p>
        <div style="display:flex;gap:10px;justify-content:flex-end">
          <button class="btn s" type="button" disabled data-missing="query: diff since the last version">{{ t('history.showChanges') }}</button>
          <button class="btn p" type="button" :disabled="!canCommit" data-bb="history-commit" @click="commit">{{ t('history.createVersion') }}</button>
        </div>
      </div>
    </section>
    <section class="col">
      <h2 style="font-size:20px" data-bb="history-version">{{ t('history.version') }} <span class="mono" :class="{ off: !selectedVersion }" style="font-size:14px">{{ selectedVersion ? short(selectedVersion.hash) : '—' }}</span></h2>
      <p class="note" style="margin:0" data-bb="history-meta">{{ selectedVersion && !selectedVersion.parents[0] ? t('history.first') : t('history.changes', { n: rows.length }) }}</p>
      <!-- The selected version against its first parent (query version_changes): element, field, before → after. -->
      <table v-if="rows.length" class="t" data-bb="history-changes"><!-- FAKE1: no empty table -->
        <thead><tr><th>{{ t('history.element') }}</th><th>{{ t('history.field') }}</th><th>{{ t('history.change') }}</th></tr></thead>
        <tbody>
          <tr v-for="(r, i) in rows" :key="i"><td>{{ r.element }}</td><td class="mono">{{ r.field }}</td><td>{{ r.change }}</td></tr>
        </tbody>
      </table>
      <div style="display:flex;gap:10px">
        <button class="btn s" type="button" disabled data-missing="verb: compare two versions in 3D" data-bb="history-compare">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="3" y="5" width="7" height="14" /><rect x="14" y="5" width="7" height="14" /><path d="M10 12h4" /></svg>{{ t('history.compare3d') }}
        </button>
        <button class="btn s" type="button" disabled data-missing="verb: merge a variant" data-bb="history-merge">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="6" cy="5" r="2" /><circle cx="6" cy="19" r="2" /><circle cx="18" cy="8" r="2" /><path d="M6 7v10M18 10c0 5-12 3-12 7" /></svg>{{ t('history.merge', { variant: '—' }) }}
        </button>
      </div>
    </section>
  </div>
</template>

<style scoped>
/* .page style="display:grid;grid-template-columns:minmax(0,1.2fr) minmax(0,1fr);gap:24px" (padding from .bb-page) */
.hi { display: grid; grid-template-columns: minmax(0, 1.2fr) minmax(0, 1fr); gap: 24px; align-content: start; }
/* style="display:flex;flex-direction:column;gap:10px;min-width:0" */
.col { display: flex; flex-direction: column; gap: 10px; min-width: 0; }
/* style="display:flex;align-items:center;gap:10px;flex-wrap:wrap" */
.head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
/* .page h2{font-family:'Sora',sans-serif;margin:0} */
h2 { font-family: 'Sora', sans-serif; margin: 0; }
.mono { font-family: 'DM Mono', monospace; }
/* .note{font-size:13px;color:#5C574D} */
.note { font-size: 13px; color: var(--bb-stone); }
.versions { display: flex; flex-direction: column; gap: 10px; }
.versions:empty { display: none; }
.ver { display: grid; grid-template-columns: auto 1fr; gap: 2px 10px; text-align: left; font: inherit; font-size: 13.5px; color: var(--bb-ink); background: var(--bb-sheet); border: 1px solid var(--bb-line); border-radius: 12px; padding: 8px 12px; cursor: pointer; }
.ver.on { border-color: var(--bb-ink); }
.ver .msg { font-weight: 600; min-width: 0; overflow-wrap: anywhere; }
.ver .who { grid-column: 2; color: var(--bb-stone); font-size: 13px; }
/* .card{background:#FFFFFF;border:1px solid #DDD6C8;border-radius:16px;padding:18px} + style="padding:14px;display:flex;flex-direction:column;gap:8px;margin-top:6px" */
.card { background: var(--bb-sheet); border: 1px solid var(--bb-line); border-radius: 16px; padding: 14px; display: flex; flex-direction: column; gap: 8px; margin-top: 6px; }
/* .eyebrow{font-family:'DM Mono',monospace;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#5C574D} */
.eyebrow { font-family: 'DM Mono', monospace; font-size: 12px; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
/* .chip{padding:2px 4px 2px 10px;border-radius:999px;font-size:13px;display:inline-flex;gap:4px;align-items:center;white-space:nowrap;color:#141414} .chip.nx{padding-right:10px} .c-o{background:#D2B347} */
.chip { padding: 2px 4px 2px 10px; border-radius: var(--bb-radius-pill); font-size: 13px; display: inline-flex; gap: 4px; align-items: center; white-space: nowrap; color: var(--bb-ink); }
/* Text on a fill is ink (ADR 0049): a disabled chip keeps ink at full opacity (stone or .55 on ochre is under 4.5:1). */
.chip.off { color: var(--bb-ink); opacity: 1; }
/* .sel-inp{display:inline-flex;align-items:center;justify-content:space-between;gap:6px;height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 6px 0 8px;font-size:13.5px;white-space:nowrap} */
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
/* .btn{...} .btn.p{background:#C06A41;border:0} .btn.s{background:#FFFFFF;border:1px solid #141414;font-weight:400} */
.inp:focus-visible, button:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* Missing in Rust: the reference's .off (color:#5C574D;opacity:.55). */
.off, button:disabled, input:disabled { color: var(--bb-stone); opacity: .55; cursor: default; }
/* H2a: the branch select inside the reference's .sel-inp; the new branch row (errors: shell.css .error). */
.sel-inp { display: inline-flex; align-items: center; gap: 6px; height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 6px 0 8px; font-size: 13.5px; white-space: nowrap; }
.sel { flex: 1; min-width: 0; border: 0; background: transparent; font: inherit; color: var(--bb-ink); }
.sel:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
.newb { display: flex; gap: 10px; align-items: center; }
/* table.t ... */
table.t { border-collapse: collapse; width: 100%; font-size: 13.5px; }
table.t th { text-align: left; font-weight: 600; color: var(--bb-stone); border-bottom: 1px solid var(--bb-stone); padding: 6px 8px; white-space: nowrap; font-size: 13px; }
table.t td { border-bottom: 1px solid var(--bb-sunk); padding: 5px 8px; vertical-align: middle; }
</style>
