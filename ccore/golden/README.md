# C core golden files

Rust is the oracle: each C slice must print these bytes (`docs/research/c-core-spec.md` §4).
Producer: `crates/bimble-api/examples/ccore_golden.rs`. `floats/` belongs to another ticket and is
not made or checked here.

## Regenerate and check

- `cargo run -p bimble-api --example ccore_golden -- --generate-frame`: makes `frame.json`. Its
  ids are random, so run it once and commit the file. It refuses to overwrite an existing one.
- `cargo run -p bimble-api --example ccore_golden -- --generate-frame2`: makes `frame2.json`
  (random ids too: run once, commit). It refuses to overwrite an existing one.
- `cargo run -p bimble-api --example ccore_golden`: derives every other file from the committed
  `frame.json` and `frame2.json` (loaded with Rust's own loader) and writes them. Reruns give the
  same bytes.
- `cargo run -p bimble-api --example ccore_golden -- --check`: derives the files again in memory
  and compares them byte for byte with this directory. Exit 1 lists each file that differs, is
  missing, or is here but no longer produced (`frame.json`, `frame2.json`, `README.md` and
  `floats/` excepted). `ifc/fixtures/*.ifc` are inputs too: hand-written, committed, checked
  like the outputs (the dumper does not write them; see `ifc/fixtures/` below).

All text files are UTF-8 with `\n` line ends and a final `\n`. "Hex" is lower case.

## Masking of new GlobalIds

Used in `verbs.txt`, `verbs_after*.json`, `ifc/frame_reimport.json` and `ifc/import/`.

- A **token** is a maximal run of characters from the IFC alphabet `[0-9A-Za-z_$]`.
- A token that is a valid GlobalId (22 characters, first one in `0123`) and is not an id of
  `frame.json` (project, storeys, materials, elements) becomes `<new-N>`. The all-zero id
  `0000000000000000000000` is never masked (the transcripts use it as an unknown id).
- `N` counts from 1 by first appearance. `verbs.txt`, `verbs_after_undo.json` and
  `verbs_after.json` share one numbering, in that order of reading (all new ids appear first in
  `verbs.txt`). `ifc/frame_reimport.json` has its own numbering.
- Model files: Rust sorts maps by GlobalId, so random ids give a random order. Before masking,
  each new id is replaced by `GlobalId::from_u128(N)` (`"000000000000000000000" + alphabet[N]`
  for N < 64), the text is loaded and written again (sorted, canonical), then those ids become
  `<new-N>`. So new objects come first in their map, in `N` order. A C core whose id generator
  gives `from_u128(1)`, `from_u128(2)`… in creation order (`CCORE_SEED`) only has to replace them.

## frame.json

The demo concrete frame (`bimble_core::demo::concrete_frame`, default spec: 72 elements,
3 storeys, 1 material), as `bimble_core::file::to_string` writes it: JSON text form, version 1,
pretty with 2-space indent, no final `\n`. Load then save must give the same bytes (the dumper
asserts it on every run).

## guid.txt

One record per line:

- `derived <seed-hex> <id> <u128-hex>`: `GlobalId::derived(seed)`; `seed-hex` is the seed's UTF-8
  bytes in hex, `-` for the empty seed; `u128-hex` is 32 hex digits, the 16 raw bytes big-endian.
  24 seeds: `"bimble view 3d"`, empty, ASCII, accents, CJK, emoji, tab and newline, and 1024, 1025
  and 3000 bytes (blake3 chunk edges).
- `from_u128 <u128-hex> <id>`: 9 edge values (0, 1, 63, 64, max, 2^127…).
- `parse <id> <u128-hex>`: every id of `frame.json` (project, storeys, materials, elements, each
  in sorted order); parse then format gives the same text.
- `invalid <input-hex> <error text>`: inputs the parser refuses, with Rust's message.

## mesh/

`Scene::from_model(frame)`.

- `scene.txt`: `origin` and the scene origin as 3 f64 hex `to_bits` (16 digits); `elements <n>`;
  then one line per element in scene order (model order, sorted by GlobalId):
  `<k> <GlobalId> <IfcClass> <category index> <category> v=<vertices> i=<triangles> e=<edges>
  n=<edge normals>`. `k` is 3 digits; the category index is the position in
  `MaterialCategory::ALL` (0 Concrete … 7 Other). Then `warning <text>` lines, if any (none now).
- `<k>.txt` (`000.txt` … `071.txt`): the element's `local_mesh()` in the format of
  `spikes/c-slab/expected/README.md`: `v` = 3 f32 hex, `i` = 3 u32 per triangle, `e` = 6 f32 hex,
  `n` = 8 i8 edge normals. An element drawn as an instance of a shared shape (the scene builder
  shares identical meshes; all 72 here) starts with a `t` line: 12 f32 hex, the `Placement32`
  columns `x`, `y`, `z`, `t`; the `v` lines are then the shared shape, in its own coordinates.

## query.txt

`bimble_api::dispatch::query` on the frame. One record per call:

```
>>> query <name> <args-json>
<status> <answer>
<blank line>
```

`args-json` is compact JSON with sorted keys. `status` is `200` with the answer as compact JSON
(what the viewer's HTTP query returns), or `400` with the error text. Calls: `storeys`,
`materials`, `project`, `elements` with no filter, `class` IfcBeam, `storey` Niveau 1, `limit` 3,
and all three; `element` for a footing, a column, two beams (x and y) and a slab; then errors:
query `nope`, an unknown id, a malformed id, an unknown class, an unknown argument.

## verbs.txt, verbs_after_undo.json, verbs_after.json

`dispatch::run` then `History::apply` on a copy of the frame, as the viewer's `verb` op does. One
record per step:

```
>>> <op> <name> <args-json>
<status> <answer>
undo <JSON array of the undo labels, newest first>
redo <JSON array of the redo labels, newest first>
elements <element count>
<blank line>
```

For `verb`: `200` with the commands from `dispatch::run` as compact JSON, or `400` with the error
text of `run` or of `History::apply`. For `undo` / `redo`: name `-`, args `null`, answer `200 true`
or `200 false`. Ids are masked (see above), arguments included.

Steps: `place` a column, a beam, a wall (Niveau 1, default sizes and material) and a slab (material
`C35/45`, a library grade, so a new material is added); `move` the new column and a frame column;
`set_generator` (beam height 600); `set_material` (wall and a frame column to C35/45);
`set_note` (text with spaces to trim); `delete` the slab; `undo` ×3; `redo` ×1. Then: `place` an
unknown kind, `place` a beam without `b`, `move` the unknown all-zero id (accepted: no command, but
History still pushes a `"nothing"` step and clears redo), `set_generator` with a negative width,
the unknown verb `fly`.

`verbs_after_undo.json` is the model after the three undos; `verbs_after.json` the model at the
end. Both as `file::to_string`, masked as above.

## ifc/

- `frame.ifc`: `bimble_ifc::export(frame, "frame.ifc", "2026-01-01T00:00:00")`, IFC4. The header's
  `'bimble 0.18.1'` (the Rust version) is replaced by `'bimble <version>'`; the C side does the
  same with its own version.
- `frame_reimport.json`: `bimble_ifc::import::import_str(frame.ifc)` (the masked text above),
  then `file::to_string`, masked as above. The import keeps element and storey ids, makes a new
  material id (`<new-1>`), drops the material's values and names the project "Imported model".

## frame2.json

A second synthetic model, for what `frame.json` lacks. Built through `Command`s and
`History::apply` on an empty model (`frame2()` in the dumper), written with `file::to_string`.
Load then save must give the same bytes (asserted on every run). It has:

- Project name with `"` and `\`; project information (`SetProject`): 6 entries, values with
  `"`, `\`, a newline, a tab, accents and CJK.
- 2 storeys: `Rez-de-chaussée` (0.0) and `Étage "1"` (3150.5).
- 3 materials: `C30/37` and `S355` from the library (all 4 props), and a timber
  `Bois "local" \ épicéa` with density and strength only (no modulus, no source).
- 11 elements:

| Name | Class (`ifc_entity`) | Generator | Notes |
|---|---|---|---|
| `F1` | IfcFooting | pad_footing | origin z −600 |
| `C1` | IfcColumn | rect_column | height 2899.9; 6 properties |
| `HEB "200"` | IfcColumn | rect_column | steel |
| `Poutre \ "B1" bois` | IfcBeam | rect_beam | timber, along x; renamed (`Rename`) |
| `W1` | IfcWall | wall | axis-aligned |
| `W2<tab>rotated 30°` | IfcWall | wall | `x_axis` = (cos 30°, sin 30°, 0); 2 properties |
| `D1` | IfcSlab | slab | rectangle |
| `D2 (L)` | IfcSlab | extruded_polygon | non-convex L, 6 points |
| `D3 trémie` | IfcSlab | extruded_polygon | one clockwise void |
| `Gousset` | IfcBuildingElementProxy (`IfcPlate`) | extruded_polygon | triangle; `z_axis` = −y |
| `Garde-corps` | IfcBuildingElementProxy (`IfcRailing`) | mesh | tetrahedron, 4 vertices; 1 property |

Property keys (`SetProperty`) include `Remarque "chantier"`, `Chemin\Clé`, `日本語/名前`; values
include `"`, `\`, `—` and an emoji. No openings, views, sheets, schedules or georef.

## frame2/

Derived from `frame2.json`, in the formats above:

- `frame2/mesh/`: as `mesh/` (`scene.txt`, `000.txt` … `010.txt`). Here the rectangular
  extrusions (footing, columns, beam, walls, slab `D1`) are instances (a `t` line); the
  extruded polygons and the mesh are not. `scene.txt`'s class is `ifc_entity` when there is one.
- `frame2/query.txt`: as `query.txt`. Calls: `storeys`, `materials`, `project`, `elements` with
  no filter, `class` for each of the 6 classes, `storey` for each storey, `limit` 2, then class
  IfcSlab + upper storey + limit 2; then `element` for every element, in id order.

## ifc/ exports (IFC4 and IFC2x3)

`bimble_ifc::export_as(model, <file name>, "2026-01-01T00:00:00", schema)`, the version masked as
in `frame.ifc`. In IFC2x3 the version is also in `IFCAPPLICATION(#n,'<version>','Bimble','bimble')`.

- `frame2.ifc`: `frame2.json`, IFC4, file name `frame2.ifc`.
- `frame.ifc2x3.ifc`, `frame2.ifc2x3.ifc`: `Schema::Ifc2x3` (`ifc2x3::from_ifc4` of the IFC4 text),
  file names `frame.ifc2x3.ifc`, `frame2.ifc2x3.ifc`.

## ifc/fixtures/

Eight small hand-written IFC4 files, synthetic, for what the six crate fixtures
(`crates/bimble-ifc/tests/fixtures/`) do not cover. Each has a project, site, building and one
storey (`IfcRelAggregates`), its elements contained in it, one `IfcMaterial`; GlobalIds are
`2T2c<Tag>…<NN>`. They are inputs: edit one, run the dumper, commit both.

| File | Element(s) | Body |
|---|---|---|
| `ishape_beam` | IfcBeam `HEA 200`, storey at 3000, along x | `IfcIShapeProfileDef` (fillet 18) extruded 5000; `Pset_BeamCommon` (3 values) |
| `circle_column` | IfcColumn | `IfcCircleProfileDef` r 250, extruded 3200 |
| `rect_hollow` | IfcColumn | `IfcRectangleHollowProfileDef` 200×100×8, fillets 8/12 |
| `triangulated` | IfcSlab | `IfcTriangulatedFaceSet`, closed box 4000×3000×200; `Pset_SlabCommon` |
| `faceted_brep` | IfcWall | `IfcFacetedBrep`: 6 `IfcPolyLoop` faces, sloped top |
| `mapped_item` | 3 IfcColumn, 2 IfcBuildingElementProxy | 2 `IfcRepresentationMap`s: an L extrusion (2 columns share one `IfcMappedItem`, one turned; 1 has a translating operator) and an `IfcTriangulatedFaceSet` tetrahedron (2 copies, one turned) |
| `indexed_arc` | IfcSlab | `IfcArbitraryClosedProfileDef` over `IfcIndexedPolyCurve`: `IfcLineIndex`, `IfcArcIndex` (half circle), `IfcLineIndex` |
| `opening` | IfcWall | rectangle extrusion; `IfcOpeningElement` 1200×1400 by `IfcRelVoidsElement`; `Pset_WallCommon` |

## ifc/import/

`bimble_ifc::import::import_str(text)` on: the six crate fixtures (`booleans`, `chain18`, `curves`,
`cyclic_map`, `mirror`, `tilt`), the eight `ifc/fixtures/`, `ifc/frame2.ifc` (`frame2`) and
`ifc/frame2.ifc2x3.ifc` (`frame2.ifc2x3`). `frame.ifc` is `ifc/frame_reimport.json` already.

- `<name>.json`: the model, `file::to_string`, masked as above. The known ids are every token of
  the source IFC text that is a valid GlobalId (plus the all-zero id): the import keeps element,
  storey, opening and project ids. What it derives (`GlobalId::derived`: materials
  `material/<name>`, the synthetic storey, `material/none`…) becomes `<new-N>`, numbered per file.
- `<name>.report.txt`: `ImportReport::lines()`, one per line (ids in them masked with the same
  numbering). If `import_str` fails: one line `error <text>` and no model file.
- `<name>.size.txt` instead of `<name>.json` when the model text is over 1 MiB (only `chain18`, a
  nested-map bomb stopped at the import limits: 45 MB). Lines: `bytes <length of the masked text>`,
  `elements <n>`, then per element in id order `<id> <IfcClass> <generator kind> v=<mesh
  positions> i=<mesh triangles>` (0 0 for a non-mesh generator).

## ifc/read/

The display reader: what the viewer draws when it opens an IFC read-only,
`bimble_ifc::read::read_str(text)` (default limits `Limits::for_text`), a `bimble_geom::Scene`.
Same inputs as `ifc/import/`, plus `frame` (`ifc/frame.ifc`) and `frame.ifc2x3`. One folder each.

- `scene.txt`:
  - `schema <json string>`: `Scene::schema` (`"IFC4"`, `"IFC2X3"`).
  - `origin` and 3 f64 hex, as in `mesh/scene.txt`.
  - `georef <compact JSON>`: `Scene::georef` through serde (`{}` when default).
  - `storeys <n>`, then per storey, lowest first: `storey <elevation f64 hex> <id json> <name json>`.
  - `grids <n>`, then `grid <x0> <y0> <x1> <y1> <tag json>` (f64 hex, plan mm).
  - `elements <n> triangles <total> meshes yes|no`. `no` when over 20 000 triangles: then no
    `<k>.txt` (none today; `chain18` shows no element).
  - Per element in scene order (file order of the products): `<k> <GlobalId> <IfcClass>
    <category index> <category> v= i= e= n= name=<json> storey=<json> material=<json>`. The
    GlobalId is the file's text as is (not checked, not masked); `storey` is the storey's name.
  - `warning <text>` lines: the reader's warnings, sorted, `(×N)` when repeated.
  - If `read_str` fails: one line `error <text>`, nothing else.
- `<k>.txt`: as `mesh/<k>.txt`. An element that is a copy of a mapped shape (ADR 0034) starts
  with the `t` line and gives the shared shape.

The display parses no property sets (`NOT_DISPLAYED` in `read.rs`), so there are no properties
here; `import/` has them.
