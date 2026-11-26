# ADR 0077 — Concrete: footings, raft, openings and sleeves from the ribbon

- Status: Accepted, 26 Nov 2026. Delegated by Pierre (owner): "let's start to implement all of
  them!!", "< 200ms response time", "don't wait for me to validate the ADR, just do your best, I'll
  review once it's implemented". To be reviewed on the implementation (ticket CONC1).
- Concerns: `ccore/src/verbs_concrete.c`, `ccore/src/command.c` (`set_openings`),
  `ccore/src/ifc_out.c`, `ccore/session/concrete.c`, `web-ui/src/shell/PlaceSide.vue`, the
  Concrete tab (`web-ui/src/shell/ribbon-tabs.ts`); `ccore/golden/DEVIATIONS.md` section 34.
- Reversal cost: medium (files written with openings, rafts or strip footings keep the data).

## Context

Four tools of the lab's Concrete tab did nothing: Footing, Raft, Opening, Sleeve. Rust v0.19.0
had the `pad_footing` generator and openings as data (`model.rs` `Opening`: an optional GlobalId,
a placement and a generator in the host's frame, exported as `IfcOpeningElement` +
`IfcRelVoidsElement`), and no tool, verb or command that makes any of them. Stairs and Ramp are
another ticket (CONC3) and stay "not available yet".

## Decision

Each choice, then the alternative rejected.

1. **Verbs first.** `footing_add`, `raft_add`, `opening_add`, `opening_set`, `opening_remove` are C
   additions of the verb table (agents and the page use the same rule); the place tool runs them.
   Rejected: tool-only code in the session (an agent could not make a footing).
2. **Pad footing** = `IfcFooting` with Rust's `pad_footing` generator (width x, depth y, height =
   the depth below the top). Under a column (a click inside its footprint) it is centred on the
   column, top at its base, on its storey; elsewhere its top is at the active level.
   Rejected: a footing on the storey below the column (the fixture does it); the column's storey
   keeps the footing with what it carries and needs no level to exist.
3. **Strip footing** = `IfcFooting` with the `wall` generator (a box along an axis: length,
   thickness = width, height = depth). One click on a wall places it along the whole wall, top at
   the wall's base; elsewhere two clicks. Rejected: a new generator kind (every table of kinds,
   the file and the IFC reader would change for the same box) and the beam generator (its
   profile is centred on the axis: the top would not be at the click's level).
4. **PredefinedType as an element property** `Bimble/PredefinedType` (`STRIP_FOOTING`,
   `BASESLAB`): the IFC export writes it as the entity's PredefinedType when it is one of the
   class's enumeration (IFC4 and IFC2x3), and as a `Bimble` property set, so an import brings it
   back through the property sets. Rejected for now: a new element field `predefined_type` (every
   reader, writer, command, diff and merge would change; revisit when more types need it).
5. **Raft** = `IfcSlab` from the polygon slab's clicks (`verbs::new_slab`), default 400 mm, on the
   lowest storey, top at its elevation, named R1, R2…, BASESLAB. Quantities list it with the
   slabs (its own "foundations" schedule is not served yet). Rejected: an `IfcFooting` raft
   (IFC4 names a raft a slab of type BASESLAB).
6. **Openings stay Rust's data** (in the host, in its frame): they follow the host when it moves
   and go with it when it is deleted (one undo step brings both back). A new Command
   `set_openings {id, openings}` (the C core's own, after `set_material`) replaces the list in one
   history step; the session journal writes it, and `add_element` now writes and reads an
   element's openings (before, the journal dropped them). Each new opening gets its own GlobalId.
   Rejected: openings as elements with a relation (ADR 0070's table): Rust's file and IFC export
   already hold them in the host.
7. **An opening** is a box (`rect_column` generator) through the host's thickness plus 10 mm on
   each face: in a wall, width from the two clicks along it, height and sill from the panel
   (1200, 900); in a slab, the two clicked corners. Refused when it leaves the host (the wall's
   length and height, the slab's outline) or overlaps another opening (their boxes in the host's
   frame), with the reason. Moved and resized by `opening_set` (its GlobalId kept).
   Rejected: one click with a size typed first (two clicks show the size on the plan).
8. **A sleeve** is a round opening: an `extruded_polygon` of 32 corners on one radius around its
   origin, recognised by that shape (`cc_opening_is_sleeve`); exported with ObjectType `SLEEVE`
   and PredefinedType USERDEFINED (IFC4's `IfcOpeningElementTypeEnum` has no sleeve). The
   polygon's volume is 0.64 % under the cylinder's πr²t (16 r² sin(2π/32) t). In a wall it is
   centred on the wall's height. Rejected: a field on the opening (the file would change) and a
   circle profile in IFC (the C core has no circle generator; a later ticket can add one).
9. **The cut.** Native: manifold, as Rust. Wasm (no kernel): a wall opening is a box in a
   rectangular host (SLAB1's case a, exact); a slab opening or sleeve is a convex prism through
   the whole slab (case b). A sleeve in a wall (a prism across the wall) is not one of the cases:
   drawn uncut in wasm with the existing warning.
10. **Hosts** must be an `IfcWall` with the wall generator or an `IfcSlab` with the slab generator
    (an imported mesh wall cannot take an opening yet).

11. **The preview of an opening or a sleeve** (ticket WAVE1B, the same delegation) is the void's
    solid as the ghost the other place tools draw: the dry run's `set_openings` made into one
    solid in the host's frame (`cc_conc_opening_ghost`). Rejected: the host's cut drawn live (a
    boolean per pointer move; the 200 ms budget of ADR 0074).
12. **Selecting an opening** keeps the element selection on its host and adds the opening as page
    state (`selectedOpening`): a click in 3D or in a plan asks `query opening_at {ray}` (the opening
    whose box, grown by 30 mm, the ray enters before the first surface the measure tool's pick
    meets), and the host's Properties lists its openings (`query openings {host}`); the selected
    one's box is drawn in the selection colour and its fields (width, height, sill; length;
    diameter; the centre X, Y) run `opening_set`. Rejected: openings as selectable ids of the
    selection (every selection consumer would have to tell an opening from an element).

13. **A sleeve's quantities are the exact cylinder** (WAVE1B B1): the value is carried by the
    opening's own parameter, the radius of its 32-gon (`cc_opening_is_sleeve`), never read from
    the mesh: the host's net volume is the mesh's plus (polygon area - pi r^2) x the host's
    thickness (`cc_sleeve_volume_fix_m3`, added by `cc_measure`), and the Quantities page lists
    each opening and sleeve in its own row (`query opening_rows`: pi r^2 t; a box w x h x t).
    Decision 8's polygon stays the drawn and cut solid. Rejected: a circle generator (every table
    of kinds, the file and the readers would change for one number).
14. **The sleeve's plan symbol**: a round hole (24 corners or more) of a slab seen in a plan gets
    a cross of two diameters at 45 degrees with the Annotate "Openings" toggle. A wall's sleeve is
    not drawn in plan (the cut plane rarely meets it). Rejected: a symbol for every round hole
    whatever the toggle (Rust's plans would change for imported round holes).
15. **IFC: `IfcCircleProfileDef`** of the sleeve's radius for its body (IFC2x3: the profile placed,
    as its rectangles), ObjectType `SLEEVE` kept. Supersedes decision 8's "a circle profile in IFC"
    rejection: the writer needs no circle generator for it.
16. **Wasm: a sleeve in a wall is cut** (decision 9 left it uncut): without a kernel the wall is
    seen as a slab of its face (x, z) extruded through its thickness, and its openings across it
    (boxes and 32-gons) are the "holes" case of `prism_cut.h`, mapped back.

17. **History: openings merge and diff by their GlobalIds** (WAVE1B B4): a three-way merge takes
    an element's `openings` opening by opening (an opening added on one branch and another on the
    other branch are both kept; one added and the host moved merged as before), the same opening
    changed differently on both is a conflict named `openings.<id>.<field>`; the diff names an
    opening by its GlobalId (`openings.<id>.generator.width`). A list with an opening without a
    GlobalId is taken whole, as Rust does. Rejected: openings as objects of their own in the
    commit tree (a file and repo format change for the same result).

18. **Import** (WAVE1B B3): an `IfcOpeningElement` voiding a host comes back as an opening of the
    host in parameters when its Body is one `IfcExtrudedAreaSolid` along its own +z of an
    `IfcRectangleProfileDef` (a box: `rect_column`) or an `IfcCircleProfileDef` (a sleeve: the
    32-gon `opening_add` draws), its profile at its origin, whatever host it is in; anything else
    stays today's mesh in the host's frame (cut, listed as "other", not editable by its
    parameters). The entity's PredefinedType `STRIP_FOOTING` (IfcFooting) or `BASESLAB` (IfcSlab)
    becomes the property `Bimble/PredefinedType` when the file has no `Bimble` one, and a strip
    footing read as a pad (its rectangle extruded) becomes the wall generator along its long side.
    Export -> import gives the same footings, rafts, hosts and openings (placements and sizes within
    1e-6 mm, GlobalIds kept). Rejected: checking that the box goes through the host first (the cut
    is the same solid either way; a recess keeps its depth).

19. **A foundations list** (WAVE1B B5): the Quantities page shows, apart from the schedules, the
    footings (pad, strip) and the rafts (IfcSlab BASESLAB) measured as the schedules measure
    (`query foundation_rows`, `src/quantity_rows.h`), with their total. The built-in schedules
    keep Rust's list (its "Slabs" still holds a raft, by class). Rejected: a new built-in schedule
    (Rust's schedule list, a golden, would change for every model) and a raft moved out of
    "Slabs" (a class filter that reads properties, for one kind).

## Response times (ADR 0074)

Measured by `ccore/test/lab_concrete.mjs` on the native host (tcc build), headless Chromium with
SwiftShader at 1280×800, the 72-element fixture: see the ticket's report for the numbers and the
load (`/proc/loadavg`) of the run.

The first footing click (213 ms in CONC1's run) profiled in WAVE1B (`WAVE1B_PROFILE=1 node
ccore/test/lab_concrete.mjs`, CDP Profiler at 0.1 ms, load 6.8, so the shares count more than the
milliseconds): 213 ms from the pointerdown to the frame; the core's click itself is 5 ms (curl on
the host) to 7.8 ms (in the page); the page then makes 28 synchronous calls to the core (74.5 ms in
all, 2 to 8 ms each: visibility_sets, view_clip, measurements, view_annotations…), 25 % of the
profile, and Chromium's own work (layout, paint, the SwiftShader upload) is another 22 %. Not fixed
here: the cure is one batched refresh call instead of 28 (the page's refresh, bimble.ts), a change
to every page surface; recorded as the next step of ADR 0074's slow list.

## Consequences

- `query actions` has 70 rows (`place_footing`, `place_strip_footing`, `place_raft`,
  `place_opening`, `place_sleeve`); `query placing` answers `concrete` (the sizes) while one of
  these kinds is chosen, and Rust's bytes otherwise.
- `query element` answers the element's real opening count (Rust's `e.openings.len()`; the C core
  answered 0).
- The IFC export writes openings (Rust's `lib.rs:498-524`; the C writer had left them out).

## Revisit when

- More PredefinedTypes are needed (then a model field), or round profiles are needed elsewhere
  (then a circle generator, and the sleeve becomes one).
- Stairs and Ramp (CONC3), rebar hosts (RBR1).
