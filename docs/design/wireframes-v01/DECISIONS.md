# Wireframes v01: decisions taken during the implementation

Pierre asked (20 Nov) to be left out until the end: the orchestrator takes each decision on its own
recommendation and records it here with the options that existed, for review at the end. "Pierre"
marks the decisions he took himself.

| # | Decision | Options | Chosen | Why |
|---|---|---|---|---|
| 1 | Record decisions 1, 2 and 4 of DESIGN-SPEC | Accepted from the design / Proposed, wait | Accepted (ADR 0051, 0052) | Pierre |
| 2 | Reference PNGs in the repo | commit 9.6 MB / render in the check | render in the check | Pierre |
| 3 | Branch and pull requests | one branch, one PR / one PR per screen | `wireframes-v01`, one PR | Pierre |
| 4 | Rust additions before screens | action bridge, verb bridge, read queries | all three | Pierre |
| 5 | Screen order | the brief's order (D1, B2, B1...) / code order | code order, one at a time | Pierre |
| 6 | Home shows the "bimble" wordmark in the Bimble Wordmark font | follow the reference (no wordmark) / add it | add it, listed as an allowed deviation | Pierre |
| 7 | How Vue reaches egui-only tools | draw them disabled / bridge `action(name)` | `action(name)` + `actions` query | Pierre (#4) |
| 8 | Active view row in the navigator | pink (ticket text) / not marked (reference) | not marked | pink is selection only (ADR 0049). The reference does not mark it |
| 9 | Default view names ("Ceiling Niveau 1") in a French UI | rename stored names / Rust reports `auto_name`, Vue writes the name in the UI language | `auto_name` | stored names are data a test parses. A language must not change model data. |
| 10 | When the check measures the lab | at `load` / after the model and the deep link are ready | after a ready flag | at `load` the wasm has not drawn real state, so every tool looked disabled |
| 11 | Status bar chips | include saved / not saved / view modes only | view modes only | the reference shows view modes; the quick-access row already shows the save state. |
| 12 | D1 primary button | none (ticket) / "Enregistrer" (reference HTML) | as the HTML | the HTML is the source of truth. The ticket was wrong |
| 13 | Hide and Isolate in the View ribbon | for now / in this view | for now (`hide_now`, `isolate_now`) | as the egui View tab. The in-view variants exist and stay reachable |
| 14 | Model / On paper switch | the existing `on_paper` action / a new `paper_view` action | new `paper_view` | `on_paper` is "Export view…", not the switch |
| 15 | Pointer coordinates | pushed by the change callback / polled | polled every 100 ms while the pointer is over the view | a push on every mouse move refreshes the whole page. One poll costs ~4 µs |
| 16 | Thousands separator in counts | thin space / plain space | plain space | the reference HTML uses a plain space |
| 17 | User trigram | a Settings field / derived from the name | derived (first letters, 3 at most) | Settings has no trigram field. No new setting without need |
| 18 | Clearing "hidden for now" and "hidden in view" from a chip | existing actions / new `clear_temporary`, `show_all_in_view` | new actions | the existing ones did not clear the chip: reveal is a toggle, and unhide needs a selection |
| 19 | Screens marked "new" | build them / ADR drafts only | ADR drafts 0053-0063, Proposed | the brief says to stop for Pierre's decision before building them |
| 20 | The 11 ADR drafts (0053-0063) | build the new screens now / draft, then wait | drafted as Proposed, each lists its open questions for Pierre | the brief |
| 21 | ADR 0053: sub-projects and ADR 0038's "borrow a layer" | keep layer locks / sub-projects never lock | never lock (the draft replaces that 0038 bullet) | the wireframe's decision 3; Pierre to confirm |
| 22 | ADR 0055 and 0062: the wireframes show Cancel / Save buttons | keep them / follow ADR 0052 | follow ADR 0052: no Cancel or Save; an export keeps one "Export" action | ADR 0052, accepted today |
| 23 | ADR 0061: three rights View/Edit/Admin vs ADR 0038's three levels | wireframe / 0038 | the wireframe's, flagged | Pierre to confirm |
| 24 | How each screen state is reached for the check | test-only hooks / deep links in the URL | deep links (`tab`, `view`, `select`, `page`, `dialog`, `panel`, `paper`, `action`) | they are also a real feature: a link opens a precise state |
| 25 | Where screens live | edit App.vue per screen / `src/pages`, `src/dialogs`, `src/panels` registered by `import.meta.glob` | the folders | screens run in parallel without editing one shared file |
| 26 | Parallel screen work (Pierre: "batches A, then B, then C, with /wtx") | wtx sessions (needs `wtx init`, one tmux agent each) / git worktrees run by the orchestrator, by batch | git worktrees by batch; the wasm built once and copied, no Rust build in screen worktrees | same isolation, reports stay checked by the orchestrator; the Rust build was the slow part |
| 27 | Menus over a backdrop | dim like dialogs / no dim, as the reference | no dim for menus, still closed by a click outside | the reference draws no dimmed backdrop for the File menu |
| 28 | File menu "Exporter la vue…" | disabled / the egui `on_paper` action (Export view, PDF/SVG) | `on_paper` | that action is exactly this export |
| 29 | Menu labels and shortcuts | the egui action labels (English) / the reference copy | the reference copy | the egui labels are English and worded for egui |
| 30 | Home covers the whole window (no tabs, ribbon, left panel in the reference) | page slot only / App.vue hides the bands / fixed overlay | fixed overlay now, App.vue full-window pages in the batch A polish | keeps every Home box exact today; the overlay is replaced by the clean fix |
| 31 | Home wordmark placement | eyebrow word in the font / above the eyebrow / beside the disc / quick-access bar | above the eyebrow, out of flow, 28 px | no reference box moves; the eyebrow is too small to read as a name |
| 32 | Home title without a user name | sample name / "Bonjour" alone | "Bonjour {name}", empty name when not set | real data; FR then fits one line instead of two |
| 33 | Home recent files on the web | sample rows / empty list | empty list, `data-missing="recent-files"` | no Rust query lists them; never fake data |
| 34 | When a project field is sent | blur and Enter handlers / the native `change` event | `change` (once, only when the value changed) | two handlers sent two commands and empty undo steps |
| 35 | An emptied project field | send "" / remove the key | remove the key | as the egui form (ADR 0030) |
| 36 | Project field keys | the reference's `{address}`, `{city}` / Rust's known keys | Rust's keys | real data; the reference keys are samples |
| 37 | Error band on Project information | none (reference has none) / the reference's ochre banner style with "Erreur" | the ochre banner | ADR 0049: errors are ink on ochre with the word |
| 38 | Status bar and left panel per screen | one shell for all / screens export `hint`, `side`, `layout` | exports, read by the shell | each reference screen changes them; no shell edit per screen |
| 39 | Translation parts | one list in i18n/index.ts / glob of `parts/*.ts` | glob; `t()` then takes any string, a missing key logs in dev | the list made every parallel ticket conflict; each part still types `en` from `fr` |
| 40 | Home quick-access row | the full row / the reference's short row (logo, File, command field, user) | short row on full-window pages | the reference Home has no save, undo or filename: no model work happens there |
| 41 | The check and hidden elements | hidden = present at size 0 / hidden = absent | absent (`checkVisibility()`) | a hidden element is not on screen; the self-check still gives 0 |
| 42 | Home status bar with a model open | the reference's "Aucun modèle ouvert" / the real state | real state, listed | the lab opens a model; the text would be false |
| 43 | Batch C beside batch B | wait for B / run C dialogs and pages beside B1 | beside B1 (B2 waits for B1) | different files; parts no longer conflict; B1 alone owns bimble.ts |
| 44 | History: where the session's undo list goes | the "New version" card / the changes pane | the changes pane, one row per undo label | it is the real list of changes; no fake element/field cells |
| 45 | History rows | click undoes up to a row / display only | display only | no Rust call undoes to a given step |
| 46 | Ribbon "Historique" | opens the egui panel / opens this page | unchanged for now (deep link `?page=history`), ribbon follow-up | Ribbon files were not this ticket's |
| 47 | History on the web | bridge the version store now / leave it drawn with data-missing | leave it: egui opens the store from the model's folder, the web has no folder | a bridge would still show nothing in the web lab; revisit with a web storage (ADR 0038) |
| 48 | Shared folder dialog dim | the whole window / the viewport only, as the reference | viewport only (component overlay) | the reference's overlay sits in the viewport |
| 49 | Shared folder "Annuler" | keep / × close (ADR 0052) | × close | ADR 0052 |
| 50 | Placing panel place | right panel / left panel properties, as the reference | left panel, slab only for now | the reference |
| 51 | Deep link `view=plan:N` | stored views / the views the viewer offers | the views the viewer offers | frame.bimble's plans are default views, not stored |
| 52 | Borrowed alert copy without data | sample names (Ana, B12) / the reference sentences with "—" and data-missing | "—" + data-missing | never fake data |
| 53 | Borrowed alert wording is gendered ("elle", "her work") | keep / neutral rewrite | keep now, neutral rewrite when real owners show (open question) | Rust has no such data; Pierre to confirm the wording |
| 54 | Borrowed alert "Annuler" | remove (ADR 0052) / keep | keep: it cancels the blocked change, not a form | it is a real choice, not a dialog Cancel |
| 55 | Landmarks of one screen matched on others | generic selectors everywhere / an `only` list per screen-specific landmark | `only` list | `.props .pn` exists on every reference screen; it belongs to B1 only |
| 56 | Placing hint copy | Rust hint (English) / UI keys by kind and click | UI keys; only the slab second click is from the reference, others follow its pattern | Rust hint is English only |
