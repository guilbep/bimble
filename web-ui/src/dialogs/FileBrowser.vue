<script lang="ts">
export const hint = "files.hint";
</script>

<script setup lang="ts">
// Ticket FS1: the file dialog of the native host (the desktop build), drawn in the page. One dialog for every purpose
// (files.ts PURPOSES: open, import, link IFC / PDF / DXF, save as, export). It lists what the host's queries answer
// (fs_roots, fs_home, fs_list: ccore/host/fs.h, the path rule) and answers an absolute path to files.ts pickFile.
// Names and dates only: the host never sends a file's bytes here.
import { computed, onMounted, ref } from "vue";
import { query } from "../bimble";
import { fileRequest, PURPOSES, type Purpose } from "../files";
import { t } from "../i18n";

interface Entry { name: string; kind: "dir" | "file" | "link" | "other"; size: number; mtime: number; target?: "dir" | "file" | null; outside?: boolean }
interface Listing { dir: string; parent: string | null; entries: Entry[]; hidden: number; skipped: number; truncated: boolean }

const req = fileRequest.value;
const purpose: Purpose = req?.purpose ?? "open";
const saving = !!PURPOSES[purpose].save;
const roots = ref<{ kind: "home" | "root"; path: string }[]>([]);
const listing = ref<Listing | null>(null);
const where = ref("");
const name = ref(req?.name ?? "");
const error = ref<string | null>(null);
const msg = (e: unknown) => (e instanceof Error ? e.message : String(e));

const isDir = (e: Entry) => e.kind === "dir" || (e.kind === "link" && e.target === "dir");
const entries = computed(() => {
  const l = listing.value?.entries ?? [];
  return [...l.filter(isDir), ...l.filter((e) => !isDir(e))];
});
const join = (dir: string, n: string) => (dir.endsWith("/") ? dir + n : `${dir}/${n}`);

function list(dir: string): void {
  try {
    listing.value = query<Listing>("fs_list", { dir, ext: PURPOSES[purpose].ext, purpose });
    where.value = listing.value.dir;
    error.value = null;
  } catch (e) {
    error.value = msg(e);
  }
}

onMounted(() => {
  try {
    roots.value = query<{ kind: "home" | "root"; path: string }[]>("fs_roots");
    const h = query<{ path: string | null; last: string | null }>("fs_home", { purpose });
    const start = h.last ?? h.path ?? roots.value[0]?.path;
    if (start) list(start);
    if (h.last && error.value && h.path) list(h.path); // the last folder is gone: home
  } catch (e) {
    error.value = msg(e);
  }
});

const picked = ref<string | null>(null);
function click(e: Entry): void {
  if (e.outside) return;
  if (isDir(e)) list(join(listing.value!.dir, e.name));
  else {
    picked.value = e.name;
    if (saving) name.value = e.name;
  }
}
function choose(n: string | null = saving ? name.value.trim() : picked.value): void {
  if (!n || !listing.value) return;
  let file = n;
  // A typed name without its extension takes the purpose's first one.
  if (saving && !PURPOSES[purpose].ext.some((x) => file.toLowerCase().endsWith(x))) file += PURPOSES[purpose].ext[0];
  req?.resolve(file.startsWith("/") || /^[A-Za-z]:[\\/]/.test(file) ? file : join(listing.value.dir, file));
}
const exists = computed(() => saving && !!name.value.trim() && (listing.value?.entries ?? []).some((e) => e.name === name.value.trim()));
const go = computed(() => t(saving ? (purpose === "save_as" ? "files.go.save" : "files.go.export") : purpose.startsWith("link") ? "files.go.link" : purpose === "import_ifc" ? "files.go.import" : "files.go.open"));
const size = (n: number) => (n < 1024 ? `${n} B` : n < 1048576 ? `${(n / 1024).toFixed(1)} KB` : `${(n / 1048576).toFixed(1)} MB`);
const date = (s: number) => new Date(s * 1000).toLocaleString();
</script>

<template>
  <div class="bb-dialog fb" role="dialog" :aria-label="t(`files.title.${purpose}`)" data-bb="file-browser" :data-purpose="purpose">
    <div style="display:flex;align-items:center">
      <h2 style="font-size:22px">{{ t(`files.title.${purpose}`) }}</h2><span style="flex:1"></span>
      <button class="ib" type="button" :aria-label="t('files.close')" data-bb="file-browser-close" @click="req?.resolve(null)"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button>
    </div>
    <div style="display:flex;gap:8px;align-items:center">
      <button class="btn s" type="button" :disabled="!listing?.parent" :aria-label="t('files.up')" :title="t('files.up')" data-bb="file-browser-up" @click="listing?.parent && list(listing.parent)"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 19V5"/><path d="m6 11 6-6 6 6"/></svg></button>
      <input v-model="where" class="inp mono" style="flex:1" :aria-label="t('files.folder')" data-bb="file-browser-path" @keydown.enter.prevent="list(where)">
    </div>
    <div class="body">
      <nav class="places" :aria-label="t('files.places')">
        <div class="lbl">{{ t('files.places') }}</div>
        <button v-for="r in roots" :key="r.path" type="button" class="place" :class="{ on: listing?.dir === r.path }" :title="r.path" data-bb="file-browser-root" @click="list(r.path)">
          {{ r.kind === 'home' ? t('files.home') : r.path.split('/').filter(Boolean).pop() ?? r.path }}
        </button>
      </nav>
      <div class="card list" role="listbox" :aria-label="t('files.folder')" data-bb="file-browser-list">
        <button v-for="e in entries" :key="e.name" type="button" role="option" class="ent" :class="{ on: picked === e.name, off: e.outside }" :aria-selected="picked === e.name" :disabled="e.outside"
          :title="e.outside ? t('files.outside') : e.name" data-bb="file-browser-entry" :data-name="e.name" :data-kind="e.kind" @click="click(e)" @dblclick="!isDir(e) && !e.outside && choose(saving ? e.name : e.name)">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
            <path v-if="isDir(e)" d="M3 7h6l2 2h10v10H3z"/><path v-else d="M6 3h8l4 4v14H6z"/>
            <path v-if="e.kind === 'link'" d="M10 15l4-4M14 15v-4h-4"/>
          </svg>
          <span class="nm">{{ e.name }}</span>
          <span class="mono meta">{{ isDir(e) ? '' : size(e.size) }}</span>
          <span class="mono meta">{{ date(e.mtime) }}</span>
        </button>
        <div v-if="listing && !entries.length" class="note" style="padding:8px">{{ t('files.empty') }}</div>
        <div v-if="listing?.truncated" class="note" style="padding:8px" data-bb="file-browser-truncated">{{ t('files.truncated', { n: listing.entries.length }) }}</div>
      </div>
    </div>
    <div v-if="saving" class="fld"><label class="lbl" for="fb-name">{{ t('files.name') }}</label>
      <input id="fb-name" v-model="name" class="inp mono" data-bb="file-browser-name" @keydown.enter.prevent="choose()"></div>
    <div style="display:flex;gap:10px;align-items:center;justify-content:flex-end">
      <span v-if="error" class="note err" role="alert" data-bb="file-browser-error">{{ error }}</span>
      <span v-else-if="exists" class="note" data-bb="file-browser-replace">{{ t('files.replace', { name: name.trim() }) }}</span>
      <span style="flex:1"></span>
      <button class="btn p" type="button" :disabled="saving ? !name.trim() : !picked" data-bb="file-browser-go" @click="choose()">{{ go }}</button>
    </div>
  </div>
</template>

<style scoped>
.fb { width: 820px; max-width: 100%; box-sizing: border-box; }
h2 { font-family: 'Sora', sans-serif; margin: 0; }
.lbl { font-size: 13px; color: var(--bb-stone); }
.body { display: grid; grid-template-columns: 170px minmax(0, 1fr); gap: 14px; min-height: 0; }
.places { display: flex; flex-direction: column; gap: 2px; }
.place { text-align: left; padding: 5px 10px; border-radius: 8px; border: 0; background: none; font: inherit; font-size: 13.5px; color: var(--bb-ink); cursor: pointer; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.place:hover { background: var(--bb-sunk); }
.place.on { background: var(--bb-pink); }
.card { background: var(--bb-sheet); border: 1px solid var(--bb-line); border-radius: 16px; }
.list { height: 360px; overflow: auto; padding: 6px; display: flex; flex-direction: column; gap: 1px; }
.ent { display: grid; grid-template-columns: 18px minmax(0, 1fr) 80px 150px; gap: 8px; align-items: center; padding: 4px 8px; border-radius: 8px; border: 0; background: none; font: inherit; font-size: 13.5px; color: var(--bb-ink); cursor: pointer; text-align: left; }
.ent:hover { background: var(--bb-sunk); }
.ent.on { background: var(--bb-pink); }
.ent.off { opacity: .55; cursor: default; }
.nm { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mono { font-family: 'DM Mono', monospace; }
.meta { font-size: 11.5px; color: var(--bb-stone); text-align: right; white-space: nowrap; }
.ent.on .meta { color: var(--bb-ink); }
.fld { display: flex; flex-direction: column; gap: 3px; }
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.inp.mono { font-family: 'DM Mono', monospace; font-size: 13px; }
.note { font-size: 13px; color: var(--bb-stone); }
.note.err { color: var(--bb-error); }
.btn { display: inline-flex; align-items: center; justify-content: center; gap: 6px; min-height: 32px; padding: 5px 16px; border-radius: var(--bb-radius-pill); font: inherit; font-weight: 600; color: var(--bb-ink); cursor: pointer; white-space: nowrap; border: 1px solid var(--bb-stone); background: none; }
.btn.p { background: var(--bb-clay); border: 0; }
.btn:disabled { opacity: .55; cursor: default; }
button:focus-visible, input:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
@media (max-width: 640px) { .body { grid-template-columns: 1fr; } .ent { grid-template-columns: 18px minmax(0, 1fr) 70px; } .ent .meta:last-child { display: none; } }
</style>
