# ADR 0085 — Tessellation by deflection: the reader's detail, and booleans that take nothing

- Status: Accepted, 27 Nov 2026
- Decided by: Pierre delegated the choices on 27 Nov 2026 (ticket TESS1: "performance is
  everything"; stop producing triangles nobody sees, at the source); the choices below by Claude,
  the scope cut by the orchestrator after the cause table (three fixes; no collinear-point or
  internal-face removal; standard as the default for links and read-only opens, the import exact).
- Reversal cost: low (one reader option; `exact` is Rust's reader byte for byte; the link cache
  names its detail, so a change converts again).

## Context / Problem

A comparison on the owner's 17 IFC files (`docs/research/2026-11-27-bonsai-vs-bimble-loading.md`)
found Bimble's triangles at 1.2 to 1.8 times IfcOpenShell's on slabs, walls, columns, beams,
fittings, plates and proxies. The C reader is a port of Rust's (v0.19.0, frozen): arcs and
circles as chords within 0.5 mm and at most 15 degrees each, at least 8 for any sweep; booleans
and openings through manifold.

Measured (synthetic files `ccore/bench/tess1_synth.py`, per element against IfcOpenShell
0.9.0.post1 with `ccore/bench/tess1_compare.py`; private files #5, #7, #9, #10 element by element;
the five largest by body kind):

| body kind | C before (= Rust) | IfcOpenShell, Bonsai's 0.05 m / 0.5 rad | IfcOpenShell defaults 0.001 m / 0.5 rad |
|---|---|---|---|
| round column r 10 / 150 / 300 / 1500 / 5000 mm | 92 / 152 / 216 / 484 / 888 | 48 / 48 / 48 / 48 / 88 | 48 / 108 / 152 / 344 / 628 |
| profile with arcs (stadium, D slab) | 292 / 292 | 60 / 40 | 204 / 208 |
| I-shape with fillets | 172 | 108 | 108 |
| swept disk, bar r 6 straight / bent | 92 / 524 | 28 / 156 | 100 / 516 |
| rectangles, faceted breps, face sets, half-space clippings, openings through | equal | equal | equal |
| openings that take nothing (a local file: 80 hosts) | 36,613 (22,687 uncut) | 21,770 | |

Causes: (a) the 0.5 mm chord rule (a 300 mm column 55 chords); (d) the kernel re-splits a host
an opening does not touch (a slab of 20 faceted pieces: 968 triangles, 1,569 after; OCC merges to
800). Not causes: caps (earcut, n − 2), extrusion sides (one quad per edge), clippings and real
openings (manifold's output is already merged into coplanar polygons: a merge pass saves 0.1 to
0.4 %), given meshes (equal to IfcOpenShell), copies (shared). Comparison artefacts: Bonsai leaves
elements with over 30 openings uncut; the "proxies 1.74×" of file #9 were arcs in extruded profiles.

The owner's folder, though, is mostly given meshes: the five largest files (94.9 M of 110.5 M
triangles) hold 32.7 M in IfcFaceBasedSurfaceModel (#14), 19.7 M in IfcPolygonalFaceSet (#15),
18.8 M, 7.6 M and 7.3 M in IfcFacetedBrep (#16, #11, #17). An exact coplanar merge of those
given meshes would remove 0.0 to 3.5 % (measured per kind and per shared shape): not worth its
own pass. Their reduction is the tile pyramid's coarse levels (ticket TILE1).

## Decision

1. **A reader option, three details** (`cc_ifc_read_with`, `cc_ifc_detail`, `src/ifc_read.h`):
   - **exact**: Rust's reader. Every Rust golden is read at exact and keeps its bytes.
   - **standard**, the default of `cc_ifc_read` and `cc_ifc_read_progress` (links, read-only
     opens): chords within **2 mm** and at most **18 degrees**; at least 8 a full turn, in
     proportion for an arc; at most 1024. 20 chords a circle is the most a crease allows: scene.c
     draws an edge where neighbour faces turn by 20 degrees or more, so 18 degrees keeps a
     cylinder without lines along its side, as before. The largest distance from the true curve
     is 2 mm. The count depends only on the radius, the sweep and the detail: copies stay copies
     (`shared_shapes`, `share_copies`: as many found, checked on every golden input).
   - **light**: 10 mm and 30 degrees (12 chords a circle, facet edges drawn), for a folder where
     speed matters more than round outlines.
   - The import (`cc_ifc_import`) stays exact: its meshes become model data.
2. **A boolean that takes nothing keeps its operand** (standard and light): when a kernel result
   (an opening cut, an `IfcBooleanResult`) has its operand's volume within 1 mm³ + 1e-9 and area
   within 1 mm² + 1e-9, the operand is drawn as the file gives it. A difference or an intersection
   only removes material and a union only adds, so an equal volume is the same solid.
3. **Kernel results cleaned** (standard and light, `cc_boolean_merge_coplanar`): repeated-point
   triangles dropped; coplanar regions (every point within 0.01 mm, the precision exporters write,
   of the plane of the region's largest triangle) triangulated again by earcut keeping every
   boundary point, so a closed solid stays closed and no point moves; only the outermost boolean
   and a host after its openings; a region is left as the kernel gave it when its boundary
   pinches, earcut would drop a point, the area would move or nothing is saved.
4. **Every part is a setting** (Pierre, 27 Nov: "provide good default but allow a lot of
   latitude"): user settings `link_detail` = `exact` | `standard` (default) | `light` | `custom`,
   with `link_chord_mm` (0.01 to 1000, default 2) and `link_max_deg` (1 to 90, default 18) for
   custom; exact turns everything of this ADR off (Rust's chords, the kernel's results as given).
   A link's Reload may name its own (`manage_links` `reload` with `detail`, and `chord_mm`,
   `max_deg` for custom; `null`: the settings' again; kept for the session, not in the `.bimble`).
   The link cache names the detail it was read at (format `bimble-link-cache 7`, SRCE `"detail"`,
   and the two numbers for custom); a cache of another detail is converted again. This commit is
   last and separate (it touches `session/link_cache.*`, ticket TILE1's): merged after TILE1.

### Alternatives

- **IfcOpenShell's Bonsai settings** (13 chords a circle up to r 1.6 m): within 1.1× of it on
  every class, but every facet edge of every column and pipe drawn (27.7 degrees > 20), and a
  300 mm column off by 8.7 mm. Kept as light (30 degrees), not the default.
- **5 mm instead of 2 mm**: 0.3 % (#5) to 4.5 % (#9) fewer triangles on files #1-#10 than 2 mm;
  the angular bound decides most circles. 2 mm is under a 0.13 mm pen at 1:20.
- **A coarser crease angle** (draw no edge up to 30 degrees): changes every drawing and plan; not
  this ticket's.
- **Collinear boundary points removed** after the merge: at most 0.5 % (#7: 1,415 points); needs
  both regions' agreement to stay closed. Not done.
- **Hidden faces between an element's parts** (coincident, opposite): #5 32, #7 56, #9 379 pairs
  in 0.3 to 1.4 M triangles. Not done.
- **A union of an element's items before its openings**: no gain measured (#7: 33,290 vs 33,294).

## Evidence

Files #1-#10 (IfcOpenShell-Bonsai 5,562,686 triangles): before 6,427,100; standard 5,850,803
(−9.0 %); light 5,554,792 (−13.6 %). Per class at standard / light against IfcOpenShell: proxies
1.08 / 0.99, columns 1.03 / 1.00, beams 1.02 / 0.99, doors 1.41 / 0.90, slabs 1.26 / 1.21, plates
1.09 / 1.00, wall standard cases 1.18 / 1.08, flow fittings 1.53 / 0.92 (before: 1.30, 1.08, 1.05,
1.67, 1.38, 1.19, 1.35, 1.83). The whole local folder (17 files): 110,460,314 triangles before,
108,516,368 at standard (−1.76 %), 105,891,179 at light (−4.14 %); its bodies with a circle profile
(pipes, round columns) 5.29 M -> 4.26 M -> 2.53 M; its given meshes (99 M) unchanged. The public
corpus (617 STEP files, `ccore/results/ifc_corpus.md` "After TESS1"): the same outcomes and
elements, no crash, 32,276,755 -> 30,569,953 triangles (−5.3 %), no file with more.

Shape: the box of every golden input within 2 mm (light 10 mm) of exact; volumes within 2 %
(light 5 %): a round column r 250 h 3200 is −0.26 % from πr²h at exact, −1.05 % at standard
(quantities of a read IFC come from its mesh, as in Rust); an I-shape's +0.19 % (its fillets).
Goldens: `golden/ifc_c/standard/` (14 of 50 inputs read otherwise at standard), DEVIATIONS.md
section 40.

## Revisit when

- Pierre looks at a folder at light and finds it good enough: make it the default for folders.
- The tile pyramid (TILE1) simplifies given meshes: the detail could follow the distance.
