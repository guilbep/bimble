# ADR 0064 — Small screens: a viewer with light edits below 1 024 px, a bottom bar and sheets on a phone

- Status: Accepted
- Decided by: Pierre chose the scope, the navigation and the sizes, then the breakpoints, the note,
  the tablet scope and Synchronize (24 Nov 2026). The laptop panel is open: see "Open questions"
- Reversal cost: low for the layout (Vue only), medium once offices rely on editing from a phone
- Extends: ADR 0037 and 0051 (top rows and ribbon), ADR 0040 (prepared viewing format), ADR 0048
  (Vue lab over `bimble-api`), ADR 0049 (brand), ADR 0052 (changes apply at once)

## Context / Problem

The wireframes v01 are drawn at 1440 × 900 only, and the lab has no rule for a narrow window. On
an iPhone 13 mini (375 × 812) the desktop shell wraps: the quick-access row takes three lines, the
14 tabs take four, the ribbon takes three more, and the model starts below the fold. The File menu
opens wider than the screen. Pierre, 24 Nov: "It's unusable."

ADR 0040 names who opens a model on a small screen: "a client, a site manager, a colleague with a
tablet". They look at a model. They do not model on a phone.

## Decision

1. **Three layouts, by window width** (Pierre: 600 and 1 024 px). Phone below 600 px. Tablet from
   600 to 1 023 px. Desktop from 1 024 px: the shell of ADR 0037 and 0051, unchanged. The layout
   follows the window, not the device: a narrow desktop window gets the phone layout. A tablet in
   landscape (1 180 px) gets the desktop shell. A phone on its side is wide but short: under
   500 px of height it keeps the phone's panel width (286 px).
2. **Phone and tablet are a viewer with light edits** (Pierre). View, switch views and sheets,
   cut, measure, hide, isolate, read properties. Light edits: rename an element, change its
   material, write a note. The tablet has the same scope as the phone (Pierre). No modelling, annotation, sheet layout or graphic standards: those
   screens say they need a window at least 1 024 px wide.
3. **Phone: the model keeps the screen; a bottom bar and sheets replace the ribbon** (Pierre).
   - Top bar, 52 px: the "b" icon, the file name with the view under it, the save state chip.
   - Bottom bar, five actions: Views, Objects, Cut, Measure, More. The active one is an ink pill.
   - A panel is a sheet that rises from the bottom, with a grab handle. Views, the selection,
     properties, Cut and Measure each have one. Properties and Views cover most of the screen;
     the others stay short so the model shows above them.
   - More holds File (Open, Share for viewing), Project (information, history as read-only,
     sub-projects) and the display toggles (edges, hatching, orthographic).
4. **Phone in landscape:** the bar becomes a rail on the left, the sheet a panel on the right.
5. **Tablet:** the top bar, one row of viewer tools (Cut, Measure, Hide, Isolate, Fit all, Edges,
   Hatching), the model navigator on the left, properties docked under the model.
6. **Touch.** Targets are at least 44 × 44 px. Nothing depends on hover. One finger orbits, two
   fingers zoom and pan, a tap selects, a long press opens the actions of the element.
7. **One primary action per screen**, as on desktop (ADR 0049). On a phone it is "Save", shown
   when there is something to save.
8. **Save and Synchronize are both in More** (Pierre). A conflict is not resolved on a small
   screen: the sheet says to finish on a screen at least 1 024 px wide (ADR 0060).
9. **A note is a property of the element** (Pierre: build it now). The Command is
   `SetProperty {"op":"set_property","id":"<GlobalId>","key":"Note","value":"…"}`; without
   `value` it removes the property. One line, at most 2 000 characters. The verb `set_note
   {"id","text"}` trims the text and removes the note when it is empty. The key is the bare name
   "Note": the IFC export writes it as an `IfcPropertySingleValue` in `Bimble_Properties` and the
   import brings it back under the same name (ADR 0032). `SetProperty` is general on purpose: one
   Command for any property, not one per field.
10. **Changes apply at once** (ADR 0052), on a phone too: a field is sent on Enter or when it
    loses focus. The properties sheet has no "Save" of its own; "Save" saves the model.

## Alternatives considered

- Viewer only on a phone: simpler, no save state. Pierre chose light edits.
- Everything reflowed behind menus: 14 tabs of tools nobody can aim at with a finger.
- One menu button over a full-screen model: hides Cut and Measure, the two tools a site visit needs.
- A swipeable tab strip with a second strip of tools: still spends two rows on tools that do not
  work on a phone.
- A separate "mobile app": a second code base. The lab is one Vue app over one API (ADR 0048).

## Evidence

- Pierre's two screenshots of the lab on an iPhone 13 mini, 24 Nov 2026, 18:05 (not committed):
  tabs on four lines, ribbon on three, File menu cut at the right edge.
- `web-ui/src`: no `@media` rule and no layout that reads the window width (`grep`, 24 Nov;
  `innerWidth` appears only to place two dialogs). `web-ui/index.html:5` sets
  `width=device-width`, so the phone lays the desktop shell out at 375 px.
- `docs/adr/0040-prepared-viewing-format.md:12`: the small-screen audience.
- `crates/bimble-core/src/command.rs:31,36`: `Rename` and `SetElementMaterial` exist. The light
  edits need no new Command, except the note.
- `crates/bimble-viewer/src/layout.rs:408`: the 3D view already reads a pinch (`zoom_delta`).
  One-finger orbit, two-finger pan and long press on the wasm canvas are not tested on a phone.
- `docs/design/wireframes-v01/{fr,en}/`: `SmallRules`, `PhoneViewer`, `PhoneViews`, `PhoneSelect`,
  `PhoneProps`, `PhoneCut`, `PhoneMeasure`, `PhoneMore` (375 × 812), `PhoneLandscape` (812 × 375),
  `Tablet` (820 × 1180), `Laptop1280` (1280 × 720). Rendered in Chrome at those sizes in French
  and English: no page is wider than its screen.
- `web-ui/e2e/small.mjs` (Chrome, software rendering, the demo frame) at 375 × 812, 812 × 375,
  820 × 1180 and 1280 × 720: 44 checks pass. Nothing is wider than the window; the view keeps
  375 × 688, 457 × 328 and 431 × 1123 px with a sheet open; a rename and a note typed in the sheet
  are read back from Rust and two undos remove them; at 1 280 px the desktop frame shows.
- Desktop unchanged: `design-check/compare.mjs --baseline` passes in French and English.
- `Laptop1280`: the 14 tabs fit on one row at 1 280 px. At 720 px high, three rows of view
  properties show under the navigator and the status hint is cut short.

## As built in the lab (24 Nov 2026)

- `web-ui/src/small/`: `screen.ts` (the size classes), `SmallTop.vue`, `SmallBar.vue` (bar or
  rail), `SmallSheet.vue` (the five sheets), `SmallFloat.vue` (the view's name over the model).
  `App.vue` shows them below 1 024 px instead of the desktop bands. The canvas is not re-created.
- Different from the reference pages, to be decided or built:
  - **Tablet**: it has the phone's rail and a 320 px panel, not the row of tools, the navigator
    and the docked properties of `Tablet.html`.
  - **Cut**: the sheet turns the section box on and off and sets it around the selection; top and
    bottom are shown, not stepped. No verb sets a face of the box, and there is no cut plane.
  - **Measure**: the sheet starts the viewer's tool and lists the kept measurements. ΔX, ΔY, ΔZ
    are not in `query("measurements")`.
  - **More**: Save, Synchronize, Open, Undo, Redo, Edges, Hatching. Share for viewing, project
    information, history and sub-projects are not there yet.
  - **"3D home" and "Fit all"** are the viewer's own buttons in the canvas (about 64 × 22 px):
    under the 44 px rule. The long press is not built.
- **Synchronize does nothing in a browser today**: `crates/bimble-viewer/src/collab.rs:136`
  answers "Working together needs the desktop version." on wasm. The button is there and follows
  `query("actions")`; the sheet says why it is off.

## API

- Rename and material: `Rename`, `SetElementMaterial` (unchanged).
- New: Command `SetProperty`, verb `set_note`, both above. `query("element").properties` already
  returns the note.
- No query for the layout: Vue reads the window width.
- Performance on a phone is not measured. No claim is made. The budgets of
  `web-ui/perf/budgets.txt` are desktop numbers.

## Open questions (for Pierre)

1. At 1 280 × 720, should the properties panel scroll, or the navigator collapse?
2. Should the tablet get the layout of `Tablet.html` (tools row, navigator, docked properties), or
   keep the phone's rail and panel?
3. How should a branch be tested on a phone? GitHub Pages publishes releases only
   (`.github/workflows/pages.yml`); a pull request has no preview.

## Revisit when

A phone or a tablet is measured on a real model, or an office asks to model on a tablet.
