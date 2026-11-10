# ADR 0042 — Convert every element; a binary `.bimble`

- Status: Accepted (10 Nov 2026)
- Decided by: Pierre ("ok do it!", "no json!? bson? protobuf? sqlite? what do we do?"); choices by Claude
- Reversal cost: medium (files already written in version 2 stay readable only with a reader of it)

## Context / Problem

Converting an IFC to `.bimble` (ADR 0032) kept only the five classes edited by dimensions. On the
17 private models of 10 Nov, 9 had none of them and could not be converted, and the others lost
their doors, pipes and fittings. And the file was JSON text: 3.4× the IFC on the 709 MB model
(2.4 GB), because imported meshes are long lists of floats with 17 digits, the survey origin
(≈ 2.5·10⁹ mm) folded into every vertex, pretty-printed one number per line.

## Decision

1. **Every product with a body is converted.** One that isn't a class Bimble edits by dimensions
   becomes `IfcBuildingElementProxy` with its own class name kept in a new optional element field,
   `ifc_entity` ("IfcDoor", "IfcPipeSegment"…), shown everywhere a class is shown. Its geometry
   is an `ExtrudedPolygon` when it is an extrusion, else a `Mesh`. It can be selected, moved,
   given properties and synced, not edited by dimensions. Export writes it as
   `IfcBuildingElementProxy` with the class in `ObjectType` (`.USERDEFINED.`): the attribute lists
   of other classes differ, so writing `IFCDOOR(…)` with these attributes would be invalid IFC.
   Importing it again reads the class back from `ObjectType` when that is an IFC class name.
   File → "Convert to .bimble (editable)…" says how many elements become editable by dimensions.
2. **Meshes relative to their own first point** (to the mm), set as the element's placement, not
   to the survey origin: shorter numbers, and two copies of a shape get the same positions.
3. **`.bimble` version 2 is binary:** the line `bimble\0`, then gzip (fast level) of the same
   model as CBOR (RFC 8949). Same serde types, so canonical as before: sorted maps, floats exact
   (CBOR writes the shortest float that keeps the value), the same model gives the same bytes.
   Version 1 JSON is still read, and saving to a path ending in `.json` writes it, to read, diff
   or check with other tools (CI's IfcOpenShell checks read it). Hashes and the collaboration store (ADR 0038) don't depend on the file encoding. What
   is decompressed is capped like the file (a small gzip can unpack huge).
4. **Each shape once.** An element whose body is one IfcMappedItem, uncut, moved and turned only
   (a Placement can't scale or mirror), shares its map's mesh with the other copies: in memory
   (`Arc`), in the file (a table of mesh arrays ahead of the model, elements refer to them by
   index), and in the scene (placed instances, as the IFC reader does, ADR 0034). JSON and hashes
   still see each element's mesh in full, so hashes don't change. `canonical_model` fixes mesh
   arrays in place instead of sending them through JSON, which kept copies apart and cost most
   of the open time.

## Alternatives considered

- **BSON:** made for MongoDB documents; numbers are no smaller than CBOR's, and it has no
  canonical form.
- **Protocol Buffers:** needs a schema and generated code beside the serde types, and has no
  tagged enums (the `Generator` is `#[serde(tag = "kind")]`); its encoding isn't canonical either.
- **postcard / bincode:** smallest and fastest, but they don't describe their own structure, so
  they can't read tagged enums or skipped optional fields; adopting them means rewriting the types.
- **SQLite (zipped or not):** random access we don't use (the model is loaded whole), a C
  dependency that complicates the browser build, and bytes that aren't canonical.
- **gzip of the JSON:** most of the size gain with no new dependency, but still text to parse.
  CBOR is the standard binary form of the same data, one small pure-Rust dependency (ciborium).

## Evidence

The 17 private models (sizes only), this machine, one run each; "before" is the same code
without step 4, JSON is version 1 of the same conversion:

| IFC | JSON | binary | + shapes once | open: JSON → binary + shapes once |
|---:|---:|---:|---:|---|
| 24 MB | 129 MB | 10 MB | 3 MB | 0.7 s → 0.3 s |
| 191 MB | 910 MB | 67 MB | 7 MB | 7.2 s, 3.3 GB → 1.9 s, 1.2 GB |
| 395 MB (sprinklers) | 4.2 GB | 398 MB | 20 MB | 40.6 s, 13.6 GB → 3.8 s, 1.2 GB |
| 676 MB | 2.4 GB | 186 MB | 73 MB | 18.6 s, 8.2 GB → 4.8 s, 1.9 GB |
| 723 MB | 2.2 GB | 229 MB | 43 MB | 17.5 s, 7.6 GB → 3.6 s, 1.3 GB |
| 832 MB | — | — | 52 MB | 2.8 s, 1.2 GB |

- All 17 convert (8 did before); the binary file is 5.5–46× smaller than the IFC (median 17×).
- Converting the sprinkler model: 124 s and 9.0 GB peak before step 4, 7.8 s and 2.1 GB after.
- Without step 4, CBOR + gzip was ~10× smaller than JSON but opened no faster (0.83 s vs 0.69 s
  on the 24 MB model): the time was in building the scene and in the JSON trip of
  `canonical`, not in parsing.

## Revisit when

- Mirrored or scaled copies matter (they are stored in full today): a placement with a full
  transform.
- The history store (ADR 0038) keeps one object per element with its mesh in full: shapes as
  their own objects if a converted model's history gets too large.
- Opening times matter more than size: positions as packed bytes, or a format without field names.
