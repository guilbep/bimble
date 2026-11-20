# ADR 0056 — Levels and grids: levels beyond storeys, a project zero, grids in the model

- Status: Proposed
- Decided by: not yet: proposed from the wireframes v01, for Pierre
- Reversal cost: medium (a level kind, a grid map in the model, IFC export of both)
- Extends: ADR 0018 (grid axes on plans), ADR 0021 (views per storey), ADR 0045 (coordinates)

## Context / Problem

Storeys exist: `Storey { id, name, elevation }` in mm (`model.rs:188`), with `AddStorey` and
`RemoveStorey` Commands and a `storeys` query. Grid axes exist only for drawing: they are read
from an IFC's `IfcGrid` (`read.rs:1266`) into `GridAxis` (`scene.rs:175`), or made through the
columns (ADR 0018). Nobody can draw or edit a grid, and a `.bimble` model holds none.

What is new in wireframe A7:
- **Project zero**: one absolute height (+432.80 m) and every level as "project zero + level".
- **Levels that are not storeys** (top of parapet 442.10, bottom of foundation 429.10). A tick
  "Storey" makes a level an `IfcBuildingStorey` with its plan and ceiling plan.
- **Storey height** computed, **elevation preview**, and "changing a level moves the elements on
  it".
- A **Grids** tab: grids drawn and edited in the model.

## Decision

1. **Project zero** is `Georef.height_m` (ADR 0045, `georef.rs:26`). No new field.
2. **Levels**: `Storey` gains `is_storey: bool` (default true, so existing files keep their
   hash). A level with `is_storey = false` is a datum: views snap and dimension to it, and
   no element belongs to it. Ticking "Storey" adds the plan and ceiling plan views (ADR 0021),
   unticking keeps them and asks once (ADR 0028) only when elements are on it.
3. **Changing a level's height moves its elements** in one Command, because placements are
   relative to their storey (`model.rs:195`): only the storey's elevation changes.
4. **Grids**: `Model.grids`, a map GlobalId → `{ name, axes: [{ id, tag, start_mm, end_mm }] }`,
   axes in plan, world mm, with stable GlobalIds per axis. Plans draw them as today (ADR 0018).
   An IFC's grids import into this map, so the column-based fallback stays for models without.
5. **UI**: Setting out → Levels opens the page with Levels and Grids tabs. The Levels table has
   Name, Level (m), Elevation (m), Storey height, Storey, Plan view, and "Add a level". The
   properties panel shows the level's elevation, element count and IFC class. Changes apply at
   once (ADR 0052).

## API needed

- Commands: `SetStorey(Storey)` (rename, move, storey flag), `AddGrid`, `SetGrid`, `RemoveGrid`.
  `AddStorey` / `RemoveStorey` exist (`command.rs:11`).
- Verbs: `level_add {name, level_mm, storey: bool}`, `level_set {id, name?, level_mm?, storey?}`,
  `level_remove {id}` (refused while elements are on it), `grid_add {name, axes: [{tag, start,
  end}]}`, `grid_axis_set {grid, axis, tag?, start?, end?}`, `grid_remove {id}`.
- Queries: `storeys` (exists, `query.rs:76`) gains `is_storey`, `height_mm` (to the next storey)
  and `elevation_m` (project zero + level). New `grids` → `[{id, name, axes}]`.
- IFC: storeys export as `IfcBuildingStorey` (exists). A non-storey level has no standard IFC
  place: export it as a Bimble property set `Bimble_Level` on the building, or skip it. Grids
  export as `IfcGrid` with `IfcGridAxis` and `IfcPolyline` curves, contained in the ground storey.

## Alternatives considered

- Levels as views only (annotation): then "move a level moves its elements" needs a second link.
- Grids kept read-only from IFC: the engineer then sets out columns without axes in a new model.
- Absolute elevations per storey (no project zero): every storey changes when the survey height
  is corrected.

## Evidence

- `docs/design/wireframes-v01/en/LevelsGrids.html`: Project zero +432.80 m, six levels
  (two without storey or plan), "A level ticked “Storey” becomes an IfcBuildingStorey and gets
  its plan and ceiling plan", "Changing a level moves the elements on it".
- `crates/bimble-core/src/model.rs:188-193`, `crates/bimble-core/src/command.rs:11`.
- `crates/bimble-geom/src/scene.rs:175`, `crates/bimble-ifc/src/read.rs:1266`: grids read only.

## Questions for Pierre

- Do non-storey levels go to IFC at all? Other tools ignore a Bimble property set.
- Curved and radial grids now, or straight axes only?

## Revisit when

A model needs grids per storey, or radial grids.
