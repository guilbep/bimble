# ADR 0044 — Hide and isolate; Home and who you are

- Status: Accepted (10 Nov 2026)
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
   IFC, the files opened last (desktop, 12, missing ones greyed). Opening one asks about unsaved
   work like Open….
7. **Who you are = Settings → You** (name, e-mail): the author of commits, the owner of borrowed
   elements. Signing in waits for a server (ADR 0038's HTTP remote).

## Alternatives considered

- Isolate in view as "hide all the others": a list as long as the model, and new elements
  would stay hidden; an isolation list keeps new ones out until unhidden, which is Revit's way.
- Previews of recent files: a thumbnail per file needs a GPU read-back and an image cache;
  later, if Home is used.

## Revisit when

- A server exists: sign-in replaces Settings → You.
- Sizes or properties should be set on several elements at once.
