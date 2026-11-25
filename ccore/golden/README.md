# C core golden files

Rust is the oracle: each C slice must print these bytes (`docs/research/c-core-spec.md` §4).
Producer: `crates/bimble-api/examples/ccore_golden.rs`. `floats/` belongs to another ticket and is
not made or checked here.

## Regenerate and check

- `cargo run -p bimble-api --example ccore_golden -- --generate-frame`: makes `frame.json`. Its
  ids are random, so run it once and commit the file. It refuses to overwrite an existing one.
- `cargo run -p bimble-api --example ccore_golden`: derives every other file from the committed
  `frame.json` (loaded with Rust's own loader) and writes them. Reruns give the same bytes.
- `cargo run -p bimble-api --example ccore_golden -- --check`: derives the files again in memory
  and compares them byte for byte with this directory. Exit 1 lists each file that differs, is
  missing, or is here but no longer produced (`frame.json`, `README.md` and `floats/` excepted).

All text files are UTF-8 with `\n` line ends and a final `\n`. "Hex" is lower case.

## Masking of new GlobalIds

Used in `verbs.txt`, `verbs_after*.json` and `ifc/frame_reimport.json`.

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
