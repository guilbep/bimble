<script lang="ts">
export const hint = "sharedfolder.hint";
</script>

<script setup lang="ts">
// Screen C1, Shared folder (wireframes v01 fr/SharedFolder.html, ADR 0038).
// Rust today: the `shared_folder` action picks a folder and joins it in one step (collab.rs; remote::join
// decides first person / joining itself), desktop only. The folder, the case and the email have no query yet.
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import { actions, runAction, state } from "../bimble";
import { t } from "../i18n";
import type * as copy from "../i18n/parts/sharedfolder";
import { currentDialog } from ".";

const tx = (k: keyof typeof copy.fr) => t(k);
const on = (name: string) => actions.value.some((a) => a.name === name && a.enabled);
const name = computed(() => state.value.user?.name ?? "");

function run(action: string) {
  currentDialog.value = null;
  const err = runAction(action);
  if (err) console.warn(`shared folder ${action}: ${err}`);
}

// The reference dims the viewport only (its .overlay sits in .vp): cover the viewport's box.
const box = ref<Record<string, string>>({ inset: "0" });
let ro: ResizeObserver | null = null;
onMounted(() => {
  const vp = document.querySelector<HTMLElement>('[data-bb="viewport"]');
  if (!vp) return;
  const place = () => {
    const r = vp.getBoundingClientRect();
    if (r.width) box.value = { left: `${r.left}px`, top: `${r.top}px`, width: `${r.width}px`, height: `${r.height}px` };
  };
  place();
  ro = new ResizeObserver(place);
  ro.observe(vp);
});
onBeforeUnmount(() => ro?.disconnect());
</script>

<template>
  <div class="overlay sf-ov" :style="box" @click.self="currentDialog = null">
    <div class="bb-dialog sf" role="dialog" :aria-label="tx('sharedfolder.title')" style="width:620px" data-bb="shared-folder">
      <button class="ib x" type="button" :aria-label="tx('sharedfolder.close')" data-bb="shared-folder-close" @click="currentDialog = null">
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg>
      </button>
      <div class="eyebrow">{{ tx('sharedfolder.eyebrow') }}</div>
      <h2 style="font-size:22px" data-bb="shared-folder-title">{{ tx('sharedfolder.title') }}</h2>
      <p style="margin:0">{{ tx('sharedfolder.intro') }}</p>
      <div class="fld">
        <label class="lbl" for="sf-folder">{{ tx('sharedfolder.folder') }}</label>
        <div style="display:flex;gap:8px">
          <input id="sf-folder" data-bb="shared-folder-path" class="inp mono" style="flex:1" value="" disabled data-missing="query('collab').folder: the shared folder in use (remote path)">
          <button class="btn s" type="button" data-bb="shared-folder-browse" disabled data-missing="a folder picker that returns a path + set_shared_folder(path); today shared_folder picks and joins at once">{{ tx('sharedfolder.browse') }}</button>
        </div>
      </div>
      <fieldset data-bb="shared-folder-case" style="border:0;padding:0;margin:0;display:flex;flex-direction:column;gap:8px" data-missing="query('collab').case: first / joining (remote::join decides it, no query)">
        <legend class="lbl" style="margin-bottom:6px">{{ tx('sharedfolder.youAre') }}</legend>
        <label class="card" style="padding:12px;display:flex;gap:10px"><input type="radio" name="sf-w" disabled> <span><b>{{ tx('sharedfolder.first') }}</b><br><span class="note">{{ tx('sharedfolder.firstNote') }}</span></span></label>
        <label class="card" style="padding:12px;display:flex;gap:10px"><input type="radio" name="sf-w" disabled> <span><b>{{ tx('sharedfolder.join') }}</b><br><span class="note">{{ tx('sharedfolder.joinNote') }}</span></span></label>
      </fieldset>
      <div class="row" style="font-size:14px">
        <span class="k">{{ tx('sharedfolder.author') }}</span>
        <!-- Settings → You: the name is in query("state").user; the email is not. -->
        <span>{{ name }}<span data-missing="query('state').user.email"></span> <a href="#" data-bb="shared-folder-author" @click.prevent="run('settings')">{{ tx('sharedfolder.change') }}</a></span>
      </div>
      <div style="display:flex;gap:10px;justify-content:flex-end">
        <!-- ADR 0052: no Cancel (×, Esc, click outside close). Sharing is the egui `shared_folder` action. -->
        <button class="btn p" type="button" :disabled="!on('shared_folder')" data-bb="shared-folder-share" @click="run('shared_folder')">{{ tx('sharedfolder.share') }}</button>
      </div>
    </div>
  </div>
</template>

<style>
/* The overlay below dims the viewport only, as the reference: keep the window backdrop clear (it still closes on a click). */
.bb-app > .bb-backdrop:has(> .sf-ov) { background: none; }
</style>

<style scoped>
/* .overlay{position:absolute;left:0;right:0;top:0;bottom:0;background:rgba(20,20,20,.30);display:flex;align-items:center;justify-content:center;padding:24px} (fixed here: over the viewport's box) */
.overlay { position: fixed; box-sizing: border-box; background: color-mix(in srgb, var(--bb-ink) 30%, transparent); display: flex; align-items: center; justify-content: center; padding: 24px; }
.sf { position: relative; max-width: 100%; box-sizing: border-box; font-size: 14px; line-height: 1.35; color: var(--bb-ink); }
.sf * { box-sizing: border-box; }
/* .dialog h2{font-family:'Sora',sans-serif;margin:0} */
.sf h2 { font-family: 'Sora', sans-serif; margin: 0; }
/* × (ADR 0052); the reference's close glyph, as Export.html. */
.x { position: absolute; top: 16px; right: 16px; }
/* .ib{width:32px;height:32px;border-radius:999px;border:0;background:none;display:inline-flex;align-items:center;justify-content:center;color:#141414;cursor:pointer;padding:0;flex:none} .ib:hover{background:#EDE7DB} */
.ib { width: 32px; height: 32px; border-radius: 999px; border: 0; background: none; display: inline-flex; align-items: center; justify-content: center; color: var(--bb-ink); cursor: pointer; padding: 0; flex: none; }
.ib:hover { background: var(--bb-sunk); }
/* .eyebrow{font-family:'DM Mono',monospace;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#5C574D} */
.eyebrow { font-family: 'DM Mono', monospace; font-size: 12px; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); }
/* .fld{display:flex;flex-direction:column;gap:3px} .lbl{font-size:13px;color:#5C574D} */
.fld { display: flex; flex-direction: column; gap: 3px; }
.lbl { font-size: 13px; color: var(--bb-stone); }
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} .inp.mono{font-family:'DM Mono',monospace;font-size:13px} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.inp.mono { font-family: 'DM Mono', monospace; font-size: 13px; }
/* .btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;min-height:32px;padding:5px 16px;border-radius:999px;font:inherit;font-weight:600;color:#141414;cursor:pointer;text-decoration:none;white-space:nowrap} */
.btn { display: inline-flex; align-items: center; justify-content: center; gap: 6px; min-height: 32px; padding: 5px 16px; border-radius: 999px; font: inherit; font-weight: 600; color: var(--bb-ink); cursor: pointer; text-decoration: none; white-space: nowrap; }
/* .btn.p{background:#C06A41;border:0} .btn.s{background:#FFFFFF;border:1px solid #141414;font-weight:400} */
.btn.p { background: var(--bb-clay); border: 0; }
.btn.s { background: var(--bb-sheet); border: 1px solid var(--bb-ink); font-weight: 400; }
/* Unavailable: .btn.d{background:#EDE7DB;border:0;color:#5C574D;font-weight:400;cursor:default} */
.btn:disabled { background: var(--bb-sunk); border: 0; color: var(--bb-stone); font-weight: 400; cursor: default; }
.ib:focus-visible, .btn:focus-visible, a:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* .card{background:#FFFFFF;border:1px solid #DDD6C8;border-radius:16px;padding:18px} .note{font-size:13px;color:#5C574D} */
.card { background: var(--bb-sheet); border: 1px solid var(--bb-line); border-radius: 16px; padding: 18px; }
.note { font-size: 13px; color: var(--bb-stone); }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} .row .k{color:#5C574D;white-space:nowrap} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.row .k { color: var(--bb-stone); white-space: nowrap; }
/* a{color:#A35A37}a:hover{color:#141414} */
a { color: var(--bb-clay-text); }
a:hover { color: var(--bb-ink); }
</style>
