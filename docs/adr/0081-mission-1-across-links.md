# ADR 0081 — Mission 1 across linked models

- Status: Accepted, 26 Nov 2026
- Decided by: Pierre delegated the design choices on 26 Nov 2026 ("our goal is to display the whole
  ifc folder as links in the viewer"; Mission 1 as published on bimble.ch); the choices below by
  Claude on ticket M1LINKS, to be reviewed by Pierre on the implementation.
- Reversal cost: low to medium (a cache section and a cache format number; an optional `notes` key in
  a link entry of the `.bimble`, left out when empty; a hidden action; no change to a file without
  links or notes)

## Context

Mission 1 (model viewing): fast loading of IFC files, several files on top of each other, section box
and cut planes, automatic floor plans, show or hide any element, measure, read the properties, add
notes, export views to PDF. LINKF (ADR 0079) links a folder of IFC files into a new `.bimble`. On a
project made of links alone (File > New, then Link this folder) the floor plans were the empty host's
only ("Level 0"), the section box and a plan's clip fitted the empty host (no box), a linked element
could not be hidden, its property sets were not read (audit finding C1), there were no notes.

## Decisions (each with the alternative not taken)

1. **Levels from links, read only.** Every loaded link's storeys at their placed elevation, merged
   when within 200 mm (`CC_LINK_LEVEL_TOL_MM`) of a host level or of an earlier link's (names from the
   first link, links in link order); never written into the model. Alternative: adopt them as
   storeys (Commands); left for an explicit "adopt levels" later, so a folder of links leaves the
   model untouched. 200 mm: consultants' levels differ by a floor build-up (the fixtures: 3000 and
   3150.5); alternative: exact elevations, refused (two plans 150 mm apart).
2. **A floor plan per level from links, automatic.** Listed after the file's views (no ceiling
   plan), id derived from the level's key as any default plan, marked "from links" in the Navigator.
   Alternative: an action "Create floor plans from links" storing views; refused: the model would
   carry views of levels it does not have (the view's storey must be a model storey).
3. **The paper and the clips see the links.** The paper's made model takes the link levels as
   storeys; Fit, the first section box, a plan's and a section's clip fit the visible links too
   (Rust's box when there are no links, byte for byte). Alternative: fit the host only; refused: a
   project of links alone had no box.
4. **Hiding linked elements: rules in the view, ids for the mesh.** The Navigator's eye on a link, a
   link's storey, a class or an element is a rule in the active view (`hidden_links`,
   `hidden_storeys` as `<link>/<storey>`, `hidden_classes`, `hidden_elements`), one undo step (hidden
   action `view_rule`); Isolate a class writes the other classes, not ids. At most 10,000 element
   ids per view list. Alternative: per-link lists of ids in a new view field; refused: thousands of
   ids per view, a model format change.
5. **The viewport drops hidden linked elements through the `/mesh` trailer.** A link's hidden
   elements are left out of its BMSH and a change of what is hidden bumps `links_gen` (the page
   fetches `/mesh` again). Alternative: a per-element flag in the draw path; not here (ticket 3D2
   edits it): left for 3D2's follow-up, which avoids the refetch.
6. **A plan from links keeps its hides in the session** (it is not a stored view). Alternative:
   store it; impossible without its level in the model.
7. **Property and quantity sets: a second, narrow parse.** `cc_step_parse_only` keeps only
   `IfcRelDefinesByProperties`, `IfcPropertySet`, `IfcElementQuantity` and their properties and
   quantities; one JSON entry per element. A small IFC (under 1 MiB, no cache) keeps the table in
   memory; a cache carries it as its last section `PROP` (format `bimble-link-cache 3`), which the
   reader leaves on disk: two `read_range` per element asked. Alternative: keep every set in memory;
   refused for the large folder (memory stays the light scene's). Types' sets (`IfcRelDefinesByType`)
   are not read yet.
8. **Notes live with their link.** A note (link name, element GlobalId, anchor in the link's own mm,
   text) is saved in the link's entry of the `.bimble` (`notes`, left out when empty), drawn as a text
   on plans and sections on paper (a plan: those between its bottom and top), listed in the
   Navigator, orphan when its element is gone. No undo step: the links section is the session's, as
   LINKF's placement and colour (DEVIATIONS 29, 39). Alternative: a model field changed by Commands;
   refused for now: a new Command op and its history format for a link's annotation.

## What is not done (said in the UI or below)

- Notes are not drawn in the 3D viewport nor on the live plan (they are in the Navigator, in the
  properties of their element and on paper): the viewport overlay is in `glviewport.ts`, the draw
  path ticket 3D2 edits.
- A plan from links keeps no stored hides (decision 6); "adopt levels" is not built.
- Sections through links use VIEWSHEET's section view (its box fits the links); their cut faces on
  paper take the hatch of the element's family; a linked element whose material is unknown takes the
  neutral hatch (Other).
- PDF: the same drawing as on screen for plans and sections; the 3D view has no paper drawing
  (ADR 0076). Hidden linked elements by a rule on a link's storey are left out on paper too.
- The wasm build has no files: a cache's property sets cannot be read there ("not available yet
  (no file access)" in the properties).
