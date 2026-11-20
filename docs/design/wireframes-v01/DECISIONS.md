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
