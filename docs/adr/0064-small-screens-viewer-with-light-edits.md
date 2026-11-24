# ADR 0064 — Small screens: a viewer with light edits below 1 024 px, a bottom bar and sheets on a phone

- Status: Proposed
- Decided by: Pierre chose the scope, the navigation and the sizes (24 Nov 2026). The breakpoints,
  the note field, the tablet scope and the laptop panel are open: see "Open questions"
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

1. **Three layouts, by window width.** Phone below 600 px. Tablet from 600 to 1 023 px. Desktop
   from 1 024 px: the shell of ADR 0037 and 0051, unchanged. The layout follows the window, not
   the device: a narrow desktop window gets the phone layout.
2. **Phone and tablet are a viewer with light edits** (Pierre). View, switch views and sheets,
   cut, measure, hide, isolate, read properties. Light edits: rename an element, change its
   material, write a note. No modelling, annotation, sheet layout or graphic standards: those
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
- `Laptop1280`: the 14 tabs fit on one row at 1 280 px. At 720 px high, three rows of view
  properties show under the navigator and the status hint is cut short.

## API needed

- No new Command for rename and material (`Rename`, `SetElementMaterial`).
- A note on an element is new. Either a Command `SetNote {"id":"<GlobalId>","text":"…"}` stored as
  an `IfcPropertySingleValue` in a `Pset_Bimble…` set, or nothing until notes are decided. Pierre
  chooses.
- No query for the layout: Vue reads the window width.
- Performance on a phone is not measured. No claim is made. The budgets of
  `web-ui/perf/budgets.txt` are desktop numbers.

## Open questions (for Pierre)

1. The breakpoints 600 and 1 024 px are a proposal. A tablet in landscape (1 180 px) gets the
   desktop shell with them.
2. The "Note" field: build it now, or leave it out of the first phone version?
3. Does the tablet get any modelling tool, or the same scope as the phone?
4. At 1 280 × 720, should the properties panel scroll, or the navigator collapse?
5. Is "Save" enough on a phone, or does a shared model also need Synchronize there (ADR 0060)?

## Revisit when

A phone or a tablet is measured on a real model, or an office asks to model on a tablet.
