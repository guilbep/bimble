# ADR 0016 — Linked models and comparison by geometry

- Status: Accepted (5 Nov 2026)
- Decided by: Pierre ("charger plusieurs IFC dans un même modèle pour les superposer. L'IFC de l'archi
  plus le nôtre, et on voit les différences"); design and evidence by Claude
- Reversal cost: low (renderer layers are additive; matching is one function)

## Problem
Engineers check their structural model against the architect's. The two files share coordinates
but not GlobalIds, element splits or exact thicknesses.

## Decision
- **Layers in the renderer:** `render_layers` draws up to 4 scenes (the model and linked models),
  each with its own uniforms: translation into the model's coordinates (from the scenes' world
  origins, so site coordinates line up), edge colour, section box shifted to the layer.
- **Coincident faces:** linked layers are pushed back by 2·10⁻⁵ in depth and drawn first, the
  model last: where both models have the same face or edge, the model's is shown, and the linked
  model appears exactly where it differs. (Without it the shared slab was a patchwork of both.)
- **Modes:** solid (reference colour), edges only, hidden. Only the model is selectable.
- **Comparison by space, not id** (`bimble_geom::compare`): an element is matched when its
  bounding box grown by 20 mm is ≥ 80 % covered (summed intersections) by the other model's grown
  boxes; a uniform 2 m grid finds candidates, huge boxes go to a list checked against all. Ours
  without counterpart: `only_here` (red); theirs: `only_in_reference` (green). All colours are
  style values.
- Viewer "Link model…", `bimble view ours.ifc --link theirs.ifc`.

## Alternatives considered
- **Match by GlobalId:** fails across offices (ids differ); fine for two versions of our own
  model, which is the versioning diff (ADR 0004), not this.
- **Exact geometric difference** (mesh booleans A−B): exact but slow and noisy for small offsets.
  Later, for a "show me the difference volume" view.
- **Transparency** for the reference: needs sorting or order-independent transparency; the depth
  push gives a clear picture without it.

## Evidence
- `compare` tests: walls 180 vs 200 mm, 10 mm off match; an extra wall/column doesn't; a slab in one
  piece vs three matches both ways; site coordinates (different scene origins); 1 km boxes.
- `linked_layers_are_drawn_at_their_offset_in_their_colours`: reference fill and edges at the offset;
  edges-only draws no fill; only-here colour.
- Viewer, demo 3×2 bays linked to 4×2 bays: 21 elements only in the linked one (extra bay, and its
  larger slabs), 0 only here; reversed: 21 only here. Coincident geometry shows the model cleanly.
  0 CPU ticks idle.

## Revisit when
- Alignment differs (shared coordinates missing): a manual offset/rotation for the link.
- Many false "differences" in practice: per-class tolerances, or the exact difference volume.
