# ADR 0044 — Hide and isolate; Home and who you are

- Status: Accepted (10 Nov 2026); amended 11 Nov 2026 (tabs, materials, drawings)
- Decided by: the office (issues #54, #52); Pierre ("go ahead"; sign-in: a local name and e-mail
  until there is a server); details by Claude
- Reversal cost: low (view visibility gains two optional fields; settings gain three)

## Context / Problem

The office hides and isolates elements all day in Revit: for now (temporary hide/isolate) or
in a view, with Reveal hidden to get them back (#54). And it wants a start panel with recent
projects and a sign-in, reachable from a house button (#52). Bimble had one selected element,
visibility only by class, storey and material, and opened to an empty window.

## Decision

1. **A selection of several elements**: Ctrl/Cmd+click adds an element or takes it out; a plain
   click selects one. Move, Copy and Delete (and the Delete key) act on all of them, as one
   undoable step; sizes and names are the clicked one's.
2. **Hide / Isolate for now**: per session, by GlobalId (survives edits that rebuild the
   scene), cleared on another view or model; a status chip "For now: N hidden ×" (ADR 0028).
3. **Hide / Isolate in view**: saved in the view's `Visibility` as `hidden_elements` and
   `isolated_elements` (GlobalIds, sorted; written only when not empty, so older views and
   their hashes are unchanged). The "Hidden" chip counts them; its × shows everything.
4. **Reveal hidden** draws what is hidden by element, in the comparison colour, pickable;
   **Unhide** shows the selected ones again (in the view and for now). A slot of its own would
   change the graphic style's format: later if the colour confuses.
5. Commands in View → Visibility and on a **right click** on the view (not while measuring or
   placing); right-clicking an element outside the selection selects it first (Revit).
6. **Home**: shown when nothing is open and from the house button; who you are, Open, Import
   IFC, the files opened last (desktop, 12, missing ones greyed), each with a preview: the first
   whole picture after opening, read back from the GPU, shrunk to 192 × 120 and kept as a PNG
   next to the settings (named by a hash of the path). Opening one asks about unsaved
   work like Open….
7. **Who you are = Settings → You** (name, e-mail): the author of commits, the owner of borrowed
   elements. Signing in waits for a server (ADR 0038's HTTP remote).

## Amendment, 11 Nov 2026 (Pierre)

8. **Tabs**: a right click on a tab closes it, the other tabs, or all of them; every tab has its
   ×, the last one too. Nothing left open shows **Home**, the model still loaded; choosing a
   view, sheet or schedule leaves Home. On Home there are no tabs: leaving it brings back the
   view's.
9. **A family split by material** (the "Other" family is large): in the Visibility panel each
   family opens into the material names found, each hidden on its own (the view's
   `hidden_materials`, optional) and each with a family drop-down. A material put in another
   family is saved in the **graphic standards** (`material_families`, material name → family;
   Pierre: shared by the office, for every model), drawn with that family's colour and hatches
   in the viewer, drawings and schedules; the standards panel lists the moves, each with ×.
   Putting a material back in the family it came with forgets the move. In a `.bimble` model
   the Materials page still sets the material's own family (model data); when the standards
   move it, the page says "Drawn as ‹family› (graphic standards)": the standards win on
   screen and paper. A shared `.bimbleview` carries the families as imported, not this
   office's moves. The moves are applied before the upload that loading or an edit makes
   anyway: no second upload when nothing moves.
10. **The family's colour opens the standards**: a click on a family's swatch in the Visibility
    panel opens the Standards panel on "‹family› on screen".
11. **Drawings leave out what a view hides by element and by material**: sheets and PDFs
    applied only families, classes and storeys; hidden, isolated (#54) and hidden materials
    were still drawn. One set per list per drawing.
12. Sizes read in the unit that fits (12 kB, 99 MB, 3.4 GB) instead of always GB.
13. A `.bimbleview` says what it leaves out (property sets and quantities, types, views,
    sheets, schedules, annotations, reference plans, linked models, history): when shared, and
    on the selected element of one (ADR 0040).

Not done: material moves on linked models (they keep their own families); the command line
(`bimble render --style`) ignores the moves.

## Alternatives considered

- Other families fixed in Bimble (metal, plaster, membranes…), or families each office
  defines (name, colour, hatch, matching names): asked, Pierre chose the split by material.
- Moves saved in the model: each project would choose again; the standards are the office's.

- Isolate in view as "hide all the others": a list as long as the model, and new elements
  would stay hidden; an isolation list keeps new ones out until unhidden, which is Revit's way.
- Previews from the file alone (rendering it again off screen): the model would be read twice;
  the picture already on screen costs one read-back.

## Revisit when

- A server exists: sign-in replaces Settings → You.
- Sizes or properties should be set on several elements at once.
