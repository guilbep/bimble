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

## bin/ (binary .bimble, version 2)

From `frame.json` and `frame2.json` (loaded with Rust's loader):

- `bin/frame.bimble`, `bin/frame2.bimble`: `bimble_core::file::to_bytes`, what Rust's `save`
  writes (ADR 0042): the magic `bimble\0`, then gzip (flate2, fast level) of the CBOR.
- `bin/frame.cbor`, `bin/frame2.cbor`: that gzip inflated: the CBOR (ciborium) of the file
  struct: `format`, `version` (2), `positions` and `indices` (each mesh array once), `model`
  (meshes as indices into those tables).

The C core must load the `.bimble` files to the same model (its v1 text equals the `.json`)
and write the `.cbor` bytes exactly (`cc_model_to_cbor`). Its gzip stream may differ from
flate2's: compare after inflating. The dumper asserts that `to_bytes` loads back to the same
model. Suite: `test/file2_test.c`.

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

Nine small hand-written IFC4 files, synthetic, for what the six crate fixtures
(`crates/bimble-ifc/tests/fixtures/`) do not cover. Each has a project, site, building and one
storey (`IfcRelAggregates`, `same_level`: eight), its elements contained in it, one `IfcMaterial`; GlobalIds are
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
| `same_level` | IfcColumn | rectangle extrusion; eight storeys at elevation 0, ids sparse (#100000 + k×7919) and not in name order: the read keeps file order (Rust's entities are a hash map then) |

## ifc/import/

`bimble_ifc::import::import_str(text)` on: the six crate fixtures (`booleans`, `chain18`, `curves`,
`cyclic_map`, `mirror`, `tilt`), the nine `ifc/fixtures/`, `ifc/frame2.ifc` (`frame2`) and
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

## verbs_all.txt, frame2/verbs_all.txt (every verb)

All 14 verbs of `dispatch::list`, for the C tickets after the first six. Same record format,
`dispatch::run` + `History::apply` steps, and masking as `verbs.txt`, with two changes:

- Known ids (written as they are) also include the model's default view ids
  (`GlobalId::derived("bimble view 3d")`, `"bimble view plan down|up <storey id>"`) and the
  `project` query's site and building ids (`derived("<project id>/site")`, `.../building`):
  they are derived, so the C core must give the same text. New element, material and sheet ids
  are `<new-N>` as before.
- A step can be `query <name> {}` (`dispatch::query`, the answer as compact JSON, masked); it
  prints the history and element lines like the others. `sheets` is not queried: it lists sheets
  in id order and new ids are random.

`verbs_all.txt` runs on a copy of `frame.json`, `frame2/verbs_all.txt` on a copy of
`frame2.json`. Each new verb kind is followed by `undo` and `redo` of its first call. Arguments
are compact JSON with sorted keys (as `serde_json` prints a `Value`).

| Verb | frame (`verbs_all.txt`) | frame2 (`frame2/verbs_all.txt`) |
|---|---|---|
| `place` | (in `verbs.txt`) | a sloped wall, a slab with a timber material by name, a column with `sizes` in S355; refused: wall size −1 |
| `move` | (in `verbs.txt`) | the 30° wall and the L slab; the plate and the mesh down 50.5; refused: dz −2e7 |
| `set_generator` | (in `verbs.txt`) | wall thickness; polygon `height`; the new wall's length and height; refused: `width` on a polygon, height 1 on a mesh, no size |
| `set_material` | (in `verbs.txt`) | polygons to S355; wall and slab to library `C40/50` (adds it); the same again (no command); refused: unknown material |
| `set_note` | (in `verbs.txt`) | a note with accents and quotes, trimmed; blank (removes); another; refused: unknown id |
| `delete` | (in `verbs.txt`) | a polygon slab and the mesh, undo, redo, undo; refused: `ids` not a list |
| `copy` | a column and a beam by (7000, 0, 0); a copy and a slab by (0, −250.5, 3000) (names `(2) (2)`); a copy again (`(2) (3)`); the unknown all-zero id (no command); refused: dx 1e8, missing `d` | the 30° wall, the L slab and the mesh by (0, 10000, 0); the void slab and a copy up 3150.5; refused: dz 2e7 |
| `rotate` | 30° about the centre of the origins; −45.5° about (1000, 2000); 90° (exact cos/sin); 360° (no command); refused: 400°, unknown id | 30° (wall, L slab); 17.25° about (0, 0) (the tilted plate, the mesh); −90° on the 30° wall; refused: −361° |
| `mirror` | about x = 3000 (dir (0, 1)); about the diagonal (1, 1); refused: zero dir, unknown id | about y = 0 (the 30° wall, the void slab); through (500, 500) along (3, 4); refused: point 1e8 |
| `set_project_field` | add `Maître d'ouvrage` (key `maitre_d_ouvrage`); change it through another spelling of the label; add `Phase`; the same again (no command); remove (empty value); refused: a label with no letters, missing `value` | change `phase`; remove `client`; add `Bureau d'études` (CJK value); the same again; remove `phase`; refused: `!!!`, missing `value` |
| `set_view` | 3D view: scale 50 + true north (`add_view`: a default view's first change), scale 200 (`set_view`), `crop: null` on no crop (no command); the lowest storey's plan: scale 20 + crop (`add_view`), cut/bottom/depth offsets, `crop: null`, bottom −0.0 (written 0.0); refused: crop on 3D, crop min > max, unknown view, range on 3D, scale 0, planes out of order, unknown field | the same calls; the plan is the upper storey's |
| `set_schedule` | `Concrete columns`: group_by_level off (adds it to the stored list), columns by an upper-case name; `Steel beams`: `family: null`, then timber; refused: unknown name, unknown column, no columns | the same with `Timber beams` and `Steel columns` |
| `add_sheet` | defaults (`S-101`, `New sheet`, A1); number, name with `"` `\` `é`, A3, portrait; name only (next number `S-102`); refused: paper A5, a used number, a blank number | the same |
| `set_sheet` | name + revision; paper A0 + landscape; the same again (no command); empty revision (removes); refused: a used number, unknown sheet, 201-character revision | the same |

Then queries `project`, `views`, `schedules`.

`verbs_all_after.json` and `frame2/verbs_all_after.json`: the model at the end
(`file::to_string`, masked and renumbered as `verbs_after.json`), with views, sheets and
schedules. The C core loads none of these yet (it refuses views, sheets and schedules): they are
for the tickets that port the verbs.

## verbs_list.json, verbs_schema/

- `verbs_list.json`: `dispatch::list()`, the `verbs` query (the viewer's `verbs` call, MCP's
  `list_verbs`): an array of `{name, summary}`, in the order of `VERBS`.
- `verbs_schema/<verb>.json`: `dispatch::describe(<verb>)`, the JSON schema (schemars, draft
  2020-12) of the verb's arguments, one file per listed verb. `move` and `copy` share one schema.

Both are `serde_json::to_string_pretty` (2-space indent; object keys sorted, as `serde_json::Value`
keeps them) plus a final `\n`.

## view/ (.bimbleview, the viewer export)

From `frame.json` and `frame2.json`: `bimble_geom::viewfile::write(Scene::from_model(m), gzip)`,
what the viewer's share and `bimble pack` write (ADR 0040).

- `view/<name>.bimbleview`: `gzip = true`, the file as written: gzip (flate2, level 6, mtime 0,
  OS byte 255) of the stream below.
- `view/<name>.bimbleview.raw`: `gzip = false`, the stream itself (what the gzip inflates to; the
  dumper asserts it, and that `read_bytes` reads the file back).

As for `bin/`, a C writer must give the `.raw` bytes exactly; its gzip may differ, compare after
inflating.

Format (`viewfile.rs`, version 1). Little-endian; varint = unsigned LEB128; str = varint byte
length + UTF-8; f64 and f32 are written with `-0.0` as `0.0`.

```
"BIMBLEVW"  u32 version (1)
sections, each: [u8; 4] kind, u64 payload length, payload
  HEAD  varint elements, varint shared shapes, varint stored triangles; f64 × 3 scene origin (mm)
  META  varint n, str × n: the distinct class, storey, material and family names, sorted (bytes)
        storeys: varint n, (str name, f64 elevation, str id) × n
        grids: varint n, (str tag, f64 × 4 start x y, end x y) × n
        densities: varint n, (str material, f64) × n
        warnings: varint n, str × n
  GEOR  str: Scene::georef as JSON; only when not the default (absent here; see geo/)
  ELEM  (repeated) varint n, (varint length, codec mesh) × n: the shared shapes first used in
        this chunk, numbered on from the previous chunks; then varint m, elements × m:
        str GlobalId, str name, varint class, storey, material, family (indices into META's table)
        then u8 0 + varint length + codec mesh (own geometry), or
             u8 1 + varint shape index + f32 × 12 placement (columns x, y, z, t)
```

Sections in that order: HEAD, META, GEOR (if any), ELEM. Shapes are numbered by the first element
that uses them (scene order). A chunk is closed once its shapes and elements reach 1 MiB
(`CHUNK_BYTES`), so both frames have one ELEM. Family names are `MaterialCategory::label()`.

The codec mesh (`bimble_geom::codec::encode`, ADR 0036):

```
"BMG1"  u8 quantized (1) or raw (0)
varint positions, varint indices, varint edges, varint edge normals (min(normals, edges))
quantized: f32 × 3 lower corner; positions as 3 varint zigzag deltas of round((c − lo) × 16)
           (1/16 mm steps), per axis from the previous vertex (start 0)
raw:       positions as f32 × 3
indices: varint zigzag delta from the previous index (start 0)
edge endpoints (2 per edge): an endpoint equal (bits) to a vertex: varint zigzag(index delta) << 1
           (delta from the last such index, the first vertex with those bits); else byte 1, then
           the point coded like a position (its own previous point, start 0)
normals: varint n, i8 × 3 × n (the distinct ones, first use order); then one varint table index
         per normal (x, y, z of each edge's [i8; 4] normals, w dropped)
```

A mesh is raw when a coordinate is not finite, it spans over 2^32 steps, or a vertex would come
back more than 0.05 mm away.

## geo/ (georeference, ADR 0045)

Three synthetic models made from `frame.json` by `geo_models` in the dumper:
- `geo`: `georef` {rotation_deg 12.5, crs "EPSG:2056", east_m 2600123.456, north_m 1199876.5,
  height_m 432.1, map_rotation_deg -3.25}: an IFC4 map conversion to LV95 and a turned building;
- `turned`: {rotation_deg 30} only, no survey system;
- `survey`: every element moved by (2 600 000 000, 1 200 000 000) mm, {rotation_deg -20, crs
  "EPSG:2056"}: a model drawn in LV95 coordinates, as Revit exports are.

Per model `<name>`:
- `<name>.json`, `<name>.bimble`, `<name>.cbor`: the text (v1), the binary file and its CBOR (v2),
  as `bin/`;
- `<name>.ifc`, `<name>.ifc2x3.ifc`: `export_as` IFC4 and IFC2x3, version masked as `ifc/`;
- `import/<name>[.ifc2x3].json`, `.report.txt`: `import_str` of each export, masked as
  `ifc/import/`;
- `read/<name>[.ifc2x3]/scene.txt`: `read_str` of each export, the `ifc/read/` scene text only
  (no per-element meshes: `ifc/read/frame/` has them);
- `<name>.bimbleview.raw`: `viewfile::write(Scene::from_model(m), false)`, with its GEOR section;
- `<name>.coords.txt`: `georef <serde_json>`, then `north rotation_deg <hex> map_rotation_deg <hex>
  plan_turn <project hex> <true hex> has_map <bool>`, `survey_plan_to_world <deg hex> <x hex>
  <y hex>`, then for twelve world points (mm; moved by the survey offset for `survey`):
  `point <x> <y> <z>` (Rust `{:?}`), `survey <hex × 3>` (`Georef::survey`, m), `text <…>` and
  `text_wgs84 <…>` (the status bar's text, a copy of bimble-viewer `coords::text` in the dumper:
  the survey system, then WGS84), `wgs84 <lat> <lon>` (`in_system(Wgs84)`, proj4rs, `{:.9}`, or
  `none`) and `approx <lat> <lon>` (`Georef::wgs84`, swisstopo's formulas, `{:.9}`, or `none`).
  Hex values are f64 bits. The C core matches everything byte for byte except `wgs84`, compared
  within 2e-9 degrees (DEVIATIONS.md 5: its own exp, log, tan, asin; measured 4.4e-10).

## schedule/ (quantities and schedule tables)

`bimble_drawing::schedule` on `Scene::from_model(frame)`, `Scene::from_model(frame2)` and the
display read (`read_str`) of seven `ifc/fixtures/` (all but `opening` and `same_level`: the C core
cuts no openings). One folder each: `frame/`, `frame2/`, `ifc-<fixture>/`. C side:
`src/quantity.c`, suite `test/quantity_test.c` (it produces and compares every file here, and
fails if the folder holds a file it did not compare).

- `<slug>.json`: `table_json(schedule(scene, def), scene, 50)`, `serde_json::to_string`
  (compact, keys sorted) plus a final `\n`. What the viewer's `schedule_rows` call answers.
- `<slug>.csv`: `to_csv(schedule(scene, def))`, what `schedule_csv` wraps in `{"csv"}`.
- `measures.txt`: one line per scene element, scene order: `<k> <GlobalId> volume= area=
  length= width= height= mass= centre=<x>,<y> section=<json string>`; numbers are `{:?}` of
  `measure()`'s `volume_m3`, `area_m2`, `length`, `width`, `height` (mm) and `centre` (world mm);
  `mass` is `volume_m3 × density(scene, material)` or `-` when the density is unknown;
  `<k> <GlobalId> none` when `measure` gives none.

Schedules (`<slug>`: the name in lower case, spaces as `-`): for both frames every effective
schedule (`schedule::effective`: the ten built-in ones, neither frame stores one), then two that
no built-in one covers: `every-field` (all 13 fields, every class, not grouped, totals) and
`sums-only` (Volume, Area, Mass, Length of `IfcColumn` and `ifcbeam`: grouped, every column
summed so no label, no total). The IFC fixtures get only these two.

Cells are written as Rust's `format!("{:.N}")` writes them: exact digits, ties to even on the
binary value, the sign of `-0.0` kept (`cc_fmt_fixed`). Sums run in row order, as Rust's.

## mcp/ (the MCP server's transcripts)

Made by `ccore/tools/mcp_golden.sh` (not by the dumper; `--check` skips `mcp/`), which builds
`bimble` and runs `node test/mcp_test.mjs --generate rust <bimble>`. The server runs as
`bimble mcp model.json` on a fresh copy of `frame.json` in a temporary folder.

- `requests.jsonl`: the request lines, one per line, sent at once. A line `#bytes N` is sent as
  N bytes of `x`; `#ping ID N` as a ping with that id, padded to exactly N bytes (the 1 MiB
  limit is 1048576 bytes). Covered: initialize, a notification, ping, tools/list, list_verbs,
  describe_verb (two verbs and an unknown one), undo and redo with nothing to do, each of the 8
  queries, query errors (unknown id, bad id, unknown argument, unknown query), `move` a frame
  column, `delete` a frame beam, `place` a column, `delete` of no ids (a "nothing" step), verb
  errors (missing `b`, a value out of range, unknown verb, no name), undo and redo through all of
  it, an unknown tool, arguments that are not an object, unknown arguments, an unknown method,
  no method, ids of every JSON type (duplicate keys: the last one), lines that are not a request
  (no answer), JSON that does not parse, lines over the limit and one exactly at it, then `save`.
- `responses.jsonl`: Rust's answer lines, in order. New GlobalIds masked as above, except that the
  known ids are every GlobalId of `frame.json` and `query.txt` (the project's derived building
  and site ids), one numbering for the file; `serverInfo.version` becomes `<version>`.
- `saved.json`: the model file after `save` (the new column is undone first, so it holds only
  the frame's ids): `file::to_string`, Rust's bytes.
- `additions_requests.jsonl`, `additions_responses.jsonl`: the C additions (`rename`, `dry_run`,
  `if_rev`), answered by the C server (`tools/mcp_golden.sh additions`); `DEVIATIONS.md` §3.

`node test/mcp_test.mjs` runs `out/tcc/bimble-ccore mcp` (`CCORE_SEED=1`) on both and compares
every answer line, masked the same way, and the saved file.

## drawing/ (drawings on paper, round 3 tickets D1–D6)

Made in one Rust build so D2–D6 need none. Fixed inputs everywhere: project `"frame"`, date
`"2026-01-01"`, the template with the model's project information (`with_project`, as the CLI).
The ground plan of `frame` is storey `Niveau 1`, of `frame2` `Rez-de-chaussée`; the minimal
template is the default one with axes `off`, every annotation switch off and no legend.

| File | What | Ticket |
|---|---|---|
| `cut/<model>.txt` | `storey`, scene `origin`, `level`, `cut` (scene mm, storey level + 1200), then per scene element `element <id> <class> loops L open O up U` and one line per `cut::section` loop, open chain and `facing_up_region` loop: `loop\|open\|up <n> x y x y …`, `{:?}` | D1 |
| `cut/<model>.more.txt` | per scene element `element <id> <class>`, its sections at every storey's cut (level + 1200) and at its mid height (`at <h> loops L open O` and the rings as above), then each edge `clip <i> <seen> x y x y` (or `none`): `clip_z` from the ground level − 1 to its cut, `edge_seen_from_above` (1 when the mesh has no normals for it) | D1 |
| `svg/sample.svg`, `sample-roboto.svg` | `to_svg()` of a sheet built in the dumper (`drawing_sample_sheet`): holes, fills, hatches (0°, 90°, ±angles, offsets, NaN offset, zero spacing), an empty area, dashes, numbers that round at the third decimal, the three anchors, turned and bold text, text to escape; Helvetica and Roboto | D1 |
| `roboto.txt` | per face: `face`, `ascent`, `descent`, `cap_height`, `bbox`, `missing` (advance of glyph 0), `chars N`, then `<hex code point> <glyph id> <advance>` for U+0020–U+024F and 20 punctuation marks; f32 `{:?}`, 1/1000 em | D1, D6 |
| `plan/<model>/cut.svg`, `frame.txt` | a sheet of the minimal plan's size: per scene element one `Area` of its section loops through `PlanInfo.frame.to_paper(scene + origin, z = cut)`, pen 0.35 black, no fill or hatch; its open chains as `Lines` (pen 0.18 red, dash 1 0.5). `frame.txt`: sheet size and the `ViewFrame` fields, `{:?}` | D1 |
| `style/default.json`, `pens.txt`, `template.json` | `GraphicStyle::default().to_json()`; `role_pen` of each role at each of `STANDARD_SCALES`; `PlanTemplate::default().to_json()` | D2 |
| `plan/<model>/min.svg`, `min.info.txt` | `plan()` of the ground storey, `bare`, minimal template; the `PlanInfo` as `{:#?}` | D2 |
| `plan/<model>/full.svg`, `bare.svg`, `a1_50.svg`, `*.info.txt`, `errors.txt` | default template: auto paper at 1:100, `bare`, A1 at 1:50; error texts (scale 0, unknown storey, A4 at 1:1) | D3 |
| `model.json` | `frame2.json` plus, by Commands, a plan with its own view range and one dimension and one text, a ceiling plan, sections north (1:50) and east (depth 5 m), a cropped plan, a plan hiding `IfcSlab`, sheet `S-101` (A3: the ranged plan 1:100, the section 1:50, a text, field `revision`) and `S-102` (A4 portrait placing the 3D view: a note). Fixed ids `GlobalId::derived("ccore drawing <name>")` | D4, D5 |
| `view/<nn>.min.svg`, `<nn>.svg`, `.info.txt`, `index.txt` | each effective view of `model.json` (number = its place), minimal and default template, `section_marks` of all views; `index.txt`: number, id, name, and the error of a view with no drawing (3D, an empty ceiling plan) | D4 |
| `sheet/<number>.svg`, `.info.txt` | `sheetdraw::draw` of each sheet; `SheetInfo` as `{:#?}` | D5 |
| `pdf/<name>-<font>.pdf`, `.content` | `to_pdf()` of the sample, the frame's full plan and `S-101`, in Roboto and Helvetica; `.content` is the page's content stream (object 4). The producer's version is masked by as many `x` (same length, offsets stay right). Roboto PDFs embed both font files (about 410 KB): only `sample-roboto.pdf` is kept whole | D6, L1 |
| `fmt.txt` | f64 bits (hex), then Rust's `{}`, `{:.1}` and `{:.3}`: fixed values (ties, -0, extremes) and 1500 pseudo-random ones | D1 |

`ccore/test/draw_test.c` (suite `draw`) checks `cut/`, `svg/`, `plan/*/cut.svg`, `roboto.txt` (against
the table `tools/roboto_table.sh` generates from it) and `fmt.txt` byte for byte.

## history/ (the history store, ticket H1, ADR 0017)

`ccore_golden` builds a history with `bimble_core::repo` from `frame.json` in a temp folder, with
fixed authors (`alice@example.org`, `bob@example.org`) and times (2026-01-01 + n hours): on `main`
"first version", "rename a beam", "move a column", "add a sheet"; a branch `variant` from "move a
column" with "place on the map" (a georeference, a project field, two copies of one mesh: stored
once, referred to by hash, ADR 0042); then `main` merges `variant` ("Merge variant"). Commits are
numbered 1–6 in that order (5 is `variant`, 6 the merge).

- `frame.bimble-history/`: the folder as Rust wrote it, every file (objects gzipped, so binary).
- `log.txt`: `log <rev>` then `<hash> <time> <author> <parents, comma-separated, or -> <message>`
  per commit of `Repo::log(rev, 100)`, for `HEAD`, `variant`, `HEAD~2`, `main~1`; then `log HEAD
  limit 2`: `<hash> <message>`.
- `branches.txt`: `HEAD ref: <branch>`, then `<name> <hash>` per branch (`Repo::branches`).
- `resolve.txt`: `<rev> -> <hash>` or `<rev> error <RepoError>` (`Repo::resolve`): HEAD, empty,
  branches, `~`, `~n`, full hashes, prefixes (upper case too), a tree's prefix (not a commit),
  unknown names, bad counts.
- `model_at_<n>.json`: `file::to_string(Repo::model_at(commit n))`.
- `diff_<a>_<b>.txt`: `version::diff(model a, model b)`, one `Change` (its `Display`) per line
  group; `diff_6_extra.txt`: model 6 against `extra.json` (a generator of another kind, a mesh
  point, the schedules, a sheet field).
- `status.txt`: `Repo::status(working.json)` (`working.json` is model 6 with a renamed project and
  storey, a removed beam, a changed project field).
- `errors.txt`: `<case> <text>`: commit 2 as plain JSON with byte 10 flipped (`commit_info`, `log`),
  the same object plain and intact (read: an older history), a missing element object
  (`model_at HEAD`), unknown revisions, a non-hash, a format line `bimble-history 2`, no history.
  The temp folder's path reads `<dir>`.

## measure.txt

The measure tool's text (ticket T1; `bimble_api::measure`, the viewer's `format_mm` and
`measurements_json`). Tab-separated lines: `format_mm <x> <text>` for 34 fixed f32 lengths (ties,
the 100 mm switch, thousands, signs, saturation, infinities, NaN; `x` as Rust's `{:?}`); `pair <a>
<b> <distance>` for three point pairs (f32, the distance as glam's `Vec3::length` of b - a);
`measurements <json>`: the `measurements` query for those pairs. Read by `ccore/test/measure_test.c`.

`ccore/test/draw_annot_test.c` (suite `draw_annot`) checks `plan/<model>/full.svg`, `bare.svg`,
`a1_50.svg`, their `.info.txt` and `errors.txt` byte for byte. The frame's note says
`Bimble 0.18.1` (Rust's version): the test gives the plan that version (`cc_plan_options.version`,
an input like the project and the date; the C core's own is `CC_IFC_VERSION`).
`ccore/test/draw_view_test.c` (suite `draw_view`) checks every `view/<nn>.min.svg`, `<nn>.svg`, their
`.info.txt` and `index.txt` byte for byte: `plan()` of each effective view of `model.json` with the
section marks of all views (the effective views as the session lists them, `cc_sv_list`).
`ccore/test/draw_sheet_test.c` (suite `draw_sheet`) checks `sheet/<number>.svg` and `.info.txt` byte
for byte: `cc_sheet_draw` of each sheet of `model.json` with the dumper's `sheet_options` and Rust's
version; and the session's `sheet_svg` of `S-101` against `S-101.svg` (the file named `frame.json`, a
clock at 2026-01-01, the version notes cut out: DEVIATIONS.md 11).
