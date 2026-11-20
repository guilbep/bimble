# web-ui: the Vue lab

A Vite + Vue + TypeScript page around the egui canvas (ADR 0048), with the brand of ADR 0049.
Vue only shows things. All logic stays in Rust: `src/bimble.ts` is the one typed client of the wasm exports.

## Run

1. Build the wasm: `scripts/build-web.sh` (from the repository root). It writes `web/dist/`.
2. In `web-ui/`: `npm install`, then `npm run wasm` (copies the wasm files into `public/bimble/`).
3. `npm run dev`, then open http://localhost:5173/.
   To open a model, put it in `web-ui/public/` and open http://localhost:5173/?url=/frame.bimble.

## Checks

`npm run typecheck` (vue-tsc) and `npm run build` (vite).

## Rules

- Raw colours live only in `src/brand/tokens.css`, vendored from the bimble-design skill (see `src/brand/README.md`). Components use its `var(--bb-...)` role variables.
- Components never import the wasm. They call `src/bimble.ts`.
- Wasm data is never deeply reactive (`shallowRef`).
- Fonts are self-hosted in `src/brand/` (vendored from the skill, see `src/brand/README.md`), no request to a font service.
- All copy comes from `src/i18n/fr.ts` and `src/i18n/en.ts` (same keys, checked by the typecheck), through `t(key)`. No literal copy in components.

## Deep links, pages, dialogs, panels

The address names a state of the lab (`src/deeplink.ts`, read once after the model loads):
`?url=/frame.bimble&tab=concrete&view=3d|plan:<storey name or index>|<GlobalId>&select=first|first:IfcBeam|<id,id>&page=<id>&dialog=<id>&panel=<id>&paper=1&action=<name,name>&edit=move-first-beam`.
`edit=move-first-beam` runs one real `move` verb (the first IfcBeam, 1 mm in x) first: the model is unsaved with one undo step, as the references show.
`view=plan:N` puts the plan tab first and the 3D tab after it (Rust tab calls: close and re-open 3D), as the references show.
Each wasm step waits for the next state change. `data-bb-ready` is set after the last step: the design check waits for it.
`design-check/screens.json` gives the deep link of each screen (`lab`).

To add a page, a dialog or a right panel, add one file. Do not edit `App.vue`:

- `src/pages/ProjectInfo.vue` → `?page=project-info`. It replaces the viewport (sheet, padding 24 32). `#bimble` stays in the DOM, hidden. Esc closes it.
- `src/dialogs/FileMenu.vue` → `?dialog=file-menu`. It shows over everything. Its root takes the class `bb-dialog` (cream card, on an ink 30 % backdrop) or `bb-menu` (sheet menu, no backdrop dim). Esc closes it. No OK/Cancel (ADR 0052).
- `src/panels/Standards.vue` → `?panel=standards`. The 400-wide right panel.

A page or a dialog can export values from a plain `<script lang="ts">` block next to `<script setup>`:

- `hint`: an i18n key. The status bar shows it while the page or dialog is current.
- `layout = "full"` (pages): the page fills the window under the quick-access row. The tabs row, ribbon, left panel and view tabs are hidden.
- `side` (pages): a component the left panel shows in place of the properties. Name its file `*Side.vue` (it is not a page).

Copy for a screen goes in a new `src/i18n/parts/<screen>.ts` (`fr`, and `en` typed from `fr`). It registers itself.

The id is the file name in kebab-case. `currentPage`, `currentDialog` and `currentPanel` (in each `index.ts`) open and close them.
`src/pages/Blank.vue` and `src/dialogs/Blank.vue` are examples.
