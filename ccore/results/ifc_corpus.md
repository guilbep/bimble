# The C IFC reader on the public IFC corpora (ticket IFC1)

Question: "let's be fully IFC compatible… 100 % compatibility". How far is the C display reader
(`cc_ifc_read`, the twin of Rust's `bimble_ifc::read::read_str`) from that on every public IFC
sample and certification file we can find? What does it refuse, crash on, or skip?

## Result in short (25 Nov 2026)

**Update, ticket IFC2 (same day): the C reader now reads 7 of these gaps that Rust (frozen) still
has; see "After IFC2" below.** **Ticket IFC3: the importer at the reader's level (no silent drop),
profile slopes, centre-line, composite and mirrored profiles, geometric sets, nesting 64; see
"After IFC3".** **Ticket TESS1: arcs by deflection and booleans that take nothing (the reader's
standard detail): the same 617 files and elements, 5.3 % fewer triangles; see "After TESS1".** The numbers in this section and the tables are IFC1's.

- **617 STEP files (IFC2X3 283, IFC4 83, IFC4X3 249, other 2), 1.24 GB, 19.2 M entities: the C
  reader reads all 617 (ok), Rust reads all 617.** No refusal, no crash, no timeout, no file over
  the memory cap.
- **ASan+UBSan (and LeakSanitizer) over all 655 files, `halt_on_error`: no report.** The 38
  non-STEP files (34 ifcZIP/zip, 3 ifcXML, 1 other) are refused by both readers with
  `parse: line 1: not a STEP file (missing ISO-10303-21 header)`: ifcZIP and ifcXML are not
  supported (the message does not say so; see the to-do list).
- **C == Rust on 616 of 617 files**, byte for byte (scene.txt and every element's mesh hash). The
  one difference (`engine_web-ifc/tests/ifcfiles/public/Viadotto Acerno.ifc`) is the warning count
  of `unsupported placement IFCLINEARPLACEMENT` (C ×5873, Rust ×3320): the deviation documented in
  `ifc_read.md` (a placement whose chain warned is not cached in C, so it counts once per product;
  Rust counts once per worker). Geometry identical (5900 elements, 2,414,913 triangles).
- **Geometry: 128,478 products with a body, 125,538 drawn (97.7 %).** Of the 2,940 drawn empty,
  1,407 are IFC4X3 alignments (IfcAlignment*, no body to draw), the rest are the skipped types of
  the to-do list or degenerate bodies (IfcOpenShell draws nothing for them either, checked on
  `IfcSampleFiles/Ifc2x3_SampleCastle.ifc`).
- **IfcOpenShell 0.9.0 on 40 files: 15,679 products with geometry, C 15,409; equal on 32 files.**
  C draws fewer on 6 (each one a warned gap: IfcAdvancedBrep, IfcRevolvedAreaSolid,
  IfcLShapeProfileDef, IfcSectionedSolidHorizontal, and "representation nesting too deep" on two
  IfcOpenShell regression files) and more on 2 (INDOT road files that IfcOpenShell draws empty).
- **The to-do list for "100 % IFC" is short and concrete** (ranked below): IfcAdvancedBrep (9
  files, 2,119 items), IfcLinearPlacement (8 files, 6,252 placements), IfcSectionedSolidHorizontal
  (5 files), IfcRevolvedAreaSolid (5), IfcGeometricSet (4), IfcIndexedPolyCurve as a body item (3),
  the parametric profiles (L, T, U, C, Z, asymmetric I, trapezium, ellipse, rounded rectangle,
  composite, centre line), CSG primitives (IfcCsgSolid, IfcBlock, IfcSphere), the reference swept
  solids, IfcExtrudedAreaSolidTapered, IfcSurfaceCurveSweptAreaSolid, IfcGridPlacement. One
  **silent** gap: products whose `Representation` is a plain `IfcProductRepresentation` (allowed by
  IFC2X3) are not read at all, with no warning (3 files, 147 IfcProductRepresentation instances;
  IfcOpenShell draws 52 products of `files/acad2010_objects.ifc`, C none).
- **Speed: C 19.0 s, Rust 20.1 s for the 617 read calls.** C is slower on single large swept-disk
  rebar shapes: `files/468--reinforcingBar--segfault--augmented.ifc` 1.44 s vs Rust 0.37 s, `424--…`
  0.80 vs 0.19 s (3 runs each, stable; one element, IfcSweptDiskSolid, ~0.5 M triangles). Not
  investigated here.
- **Not run: 173 files of `Community-Sample-Test-Files` are git-lfs pointers** and git-lfs is not
  installed on the machine; only its 27 ifcZIP files (stored without LFS) were run. With git-lfs
  installed, `corpus.sh fetch` pulls them and `corpus.sh run` adds them.

## The corpora

Cloned shallow at the pinned commit into `$BIMBLE_IFC_CORPUS` (default `~/.cache/bimble-ifc-corpus`,
outside the repo; never copied into it). Licence as the repository states it.

| corpus | URL | commit | licence (as stated) |
|---|---|---|---|
| Certification-datasets | https://github.com/buildingSMART/Certification-datasets | 5030f652e85d | CC BY 4.0 (LICENSE, © buildingSMART International) |
| files | https://github.com/IfcOpenShell/files | 0e6e21bc314c | none stated |
| IfcSampleFiles | https://github.com/youshengCode/IfcSampleFiles | 7e6927982d7e | none stated |
| IFC4.x-specification-models (found) | https://github.com/buildingSMART/IFC4.x-specification-models | ef22440ec501 | README: all rights reserved until formal release, then CC BY-ND 4.0 |
| IFC-Rail-Sample-Files (found) | https://github.com/bSI-RailwayRoom/IFC-Rail-Sample-Files | b5a0a289e10f | none stated |
| IFC-infra-unit-test (found) | https://github.com/bSI-InfraRoom/IFC-infra-unit-test | bc13603cc489 | CC BY 4.0 (LICENSE.txt, © original authors) |
| Community-Sample-Test-Files (found) | https://github.com/buildingsmart-community/Community-Sample-Test-Files | 31eb1dd0a040 | README: CC BY 4.0 |
| bim-whale-ifc-samples (found) | https://github.com/andrewisen/bim-whale-ifc-samples | 595fa90e3af7 | MIT |
| IFC-files (found) | https://github.com/opensourceBIM/IFC-files | cb96c50f5112 | README: CC BY-ND 4.0 |
| ifc-sample-files (found) | https://github.com/flinker-app/ifc-sample-files | 157dda49b68e | none stated (README lists the sources) |
| engine_web-ifc `tests/ifcfiles/public` (found) | https://github.com/ThatOpen/engine_web-ifc | f25c51bcedff | repository MPL-2.0 |

Full commits are in `bench/corpus.sh`. Looked at and not added: the IfcOpenShell main repository
(its test files are mostly the `files` repo), `buildingSMART/IFC4.x-development` (578 MB, the
specification sources), the Open IFC Model Repository (Auckland) and KIT samples (web pages, no
git or single direct download), BIMData's list (links to the repositories above).

## Method

`ccore/bench/corpus.sh` (fetch, asan, run, ios, report; the logic in `bench/corpus.py`). A file
is a candidate when it starts with `ISO-10303-21` or is named like IFC (`.ifc`, `.ifc_`,
`.ifczip`, `.ifcxml`, `.zip`, ...); git-lfs pointers are counted and skipped. Per file, one at a
time, each process under `timeout 120` and `systemd-run --user --scope -p MemoryMax=6G
-p MemorySwapMax=0`:

1. C: `out/native/ifc_dump read` (clang -O2, manifold kernel, 8 threads). Outcome: ok, refused
   (`error …` in scene.txt), crash (signal or a non-zero exit), timeout (124), over memory
   (cgroup kill, or "out of memory").
2. Rust: `ccore_golden --dump-read` (release, rayon). The binary in the shared target was built
   from the 13:14 sources; the crates changed since only in other golden modes of
   `ccore_golden.rs`, and it gives the `same_level` read golden (the last read.rs change) on six
   runs out of six.
3. C == Rust: scene.txt and meshes.txt (one FNV-1a hash per element mesh) byte for byte, as R1.
4. ASan+UBSan: `bench/corpus.sh asan` (clang -O1 -g, `-fsanitize=address,undefined`, the same
   sources), `ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`, `UBSAN_OPTIONS=halt_on_error=1`; any
   `AddressSanitizer`, `LeakSanitizer` or `runtime error:` line is a finding (the check was tried
   on a program with a heap overflow: caught).
5. IfcOpenShell 0.9.0.post1 (a venv in the corpus folder): `bench/corpus_ios.py` counts the
   products its iterator triangulates to at least one face, without the classes the C reader
   skips. 40 files, spread over schema and size, among the files with at least one product with a
   body in a schema IfcOpenShell opens (it refuses IFC4X3_RC2/RC3/RC4 and IFC4X2: 192 files).
   `scripts/compare_ifcopenshell.py` (per-element volumes) needs the Rust `bimble info` output
   and was not used.

Counts the reader did not report before, added for this run (C only, not in the golden text):
`cc_ifc_scene.products` (products with an IfcProductDefinitionShape and a placement, not a
skipped class: the scene's candidates), `.entities` and `.empty_classes` (the products drawn
empty, per IFC type); `ifc_dump read` writes them to `counts.txt`. The skipped representation
items were already reported: the reader's warnings (`unsupported geometry X (skipped)`, with
"(×N)" the occurrences).

Machine: Apple M1 Pro (8 cores), Asahi Linux aarch64, 15.2 GiB; clang 23; load average 7–10
(other builders running). Wall time for the whole run (C, Rust and ASan on 828 files): about 4
minutes.

## The to-do list for "100 % IFC" (C and Rust alike: the scenes are identical)

Ranked by files, then by items. "Files" and "items" are from the warnings of this corpus run;
"products" where the reader's empty-product count or IfcOpenShell shows the effect.

| # | gap | files | items | where it shows | note |
|---|---|---|---|---|---|
| 1 | IfcAdvancedBrep (B-spline faces) skipped | 9 | 2,119 | 4 corpora; DigitalHub: 111 doors and windows empty | the largest gap on real Revit IFC4 exports |
| 2 | IfcLinearPlacement used as identity | 8 | 6,252 | IFC4X3 rail and bridge files | needs the alignment curves evaluated |
| 3 | IfcSectionedSolidHorizontal skipped | 5 | 697 | Viadotto Acerno: IfcOpenShell 6,534 products, C 5,900 | IFC4X3 infrastructure |
| 4 | IfcRevolvedAreaSolid skipped | 5 | 12 | spec model beam-revolved-solid | |
| 5 | IfcGeometricSet as a body skipped | 4 | 243 | web-ifc ISSUE_034 | curves/points, drawable as edges |
| 6 | IfcIndexedPolyCurve as a body item skipped | 3 | 715 | IfcSampleFiles Ifc4_Revit_ARC | |
| 7 | Parametric profiles skipped: L (112), rounded rectangle (25), T (22), ellipse (6), U (4), trapezium (3), composite (3), C, Z, centre line (2 each), asymmetric I (1) | 1–3 each | 183 | ISSUE_021 Mini Project; IfcOpenShell `files/Ifc*ProfileDef.ifc` | the cheapest batch: closed-form outlines |
| 8 | Products whose Representation is a plain IfcProductRepresentation: not read, **no warning** | 3 | 147 representations | `files/acad2010_objects.ifc`: IfcOpenShell 52 products, C 0 | IFC2X3 allows the supertype; the only silent gap found |
| 9 | "representation nesting too deep" (the recursion bound) | 3 | 6 | `files/588--…`, `files/599--…`: one product each, IfcOpenShell draws it | the bound is a safety rule; raise it or make the boolean chain iterative |
| 10 | Reference swept solids (Fixed, Inclined), IfcSurfaceCurveSweptAreaSolid, IfcExtrudedAreaSolidTapered | 1–3 each | 15 | IFC4X3 rail, web-ifc ISSUE_171 | |
| 11 | CSG: IfcCsgSolid, IfcBlock, IfcSphere | 1–3 | 5 | `files/IfcCsgPrimitive.ifc`, spec bath-csg-solid | |
| 12 | IfcGridPlacement used as identity | 1 | 25 | spec grid-placement | |
| 13 | Approximations, drawn but not exact: openings not cut (host not a closed solid), boolean cut failed (NotManifold), polygonal half-space cut by its plane alone, trimmed curves drawn untrimmed, hollow swept disk drawn solid, IfcEdgeLoop face loops skipped | 1–6 each | | see the warnings table | |
| 14 | ifcZIP and ifcXML not read; the error says "not a STEP file", not "ifcZIP is not supported" | 38 files | | Community-Sample-Test-Files (27 ifcZIP), IfcOpenShell `files` | ifcZIP needs only inflate (the core has one) and a zip directory reader |
| 15 | Speed: swept-disk rebar shapes 3.9–4.3× Rust's time | 2 | | `files/468--…`, `files/424--…` | one element each |

Out of scope of this count: IfcAnnotation, IfcGrid, spaces and openings (skipped classes by
design), IfcTextLiteralWithExtent (text, 1 file), IFC4X3 alignments (no body: IfcAlignment* are
1,407 of the 2,940 products drawn empty).

## After IFC2 (25 Nov 2026): the first gaps closed, C past Rust

Ticket IFC2 closed the cheapest gaps of the to-do list in the C reader (`ccore/src/ifc_geom.h`,
`ccore/src/ifc_read.c`); Rust is frozen at v0.19.0 and still has them. The reference is
IfcOpenShell and the specification. What the C reader draws that Rust does not is recorded in
`ccore/golden/DEVIATIONS.md` section 10; each new type has a synthetic fixture in
`ccore/golden/ifc_c/` and a hand-computed volume and box in `ccore/test/ifc_read_test.c`.

| item | to-do # | done |
|---|---|---|
| plain `IfcProductRepresentation` read like `IfcProductDefinitionShape` | 8 | yes |
| parametric profiles L, T, U, C, Z, asymmetric I, trapezium, ellipse, rounded rectangle (fillets and edge radii as arcs; slopes not drawn) | 7 | yes (not: composite, centre line) |
| `IfcRevolvedAreaSolid` | 4 | yes |
| swept-disk rebar speed | 15 | yes: 3.6x, the same bytes |
| `IfcExtrudedAreaSolidTapered` | 10 | yes |
| `IfcCsgSolid`, `IfcBlock`, `IfcRectangularPyramid`, `IfcRightCircularCylinder`, `IfcRightCircularCone`, `IfcSphere` | 11 | yes |
| "unsupported format: ifcZIP / ifcXML / ifcJSON" instead of "not a STEP file" | 14 | yes (the message; reading them is not done) |

### Before and after, file by file (C, `CORPUS_TAG=ifc2-before` / `ifc2-after`)

Both runs: `bench/corpus.sh run` with the scene hash (scene.txt and every element's mesh hash)
that IFC2 added to each record; "before" is the IFC1 commit's `ifc_dump` (`C_DUMP=`).

- **828 files; 591 STEP files give the same scene, byte for byte.** 26 STEP files changed, and
  each of them contains a newly supported type (checked by the script: a STEP file that changed
  without one would be listed; none is). 37 non-STEP files changed only in their refusal
  message: 34 ifcZIP (`unsupported format: ifcZIP (a zip archive; unzip it and open the .ifc
  inside)`), 3 ifcXML. The 38th non-STEP file (neither zip nor XML) keeps `not a STEP file`.
- The 26 (elements before -> after, the type that explains it):

  | file | elements | triangles | new type(s) in the file |
  |---|---|---|---|
  | `IFC4.x-specification-models/models/advanced-geometric-shape/bath-csg-solid/bath-csg-solid.ifc` | 0 -> 1 | 0 -> 220 | IFCROUNDEDRECTANGLEPROFILEDEF, IFCCSGSOLID, IFCBLOCK |
  | `IFC4.x-specification-models/models/basic-geometric-shape/csg-primitive/csg-primitive.ifc` | 0 -> 1 | 0 -> 12 | IFCCSGSOLID, IFCBLOCK |
  | `IFC4.x-specification-models/models/building-elements/beam-parametric-cross-section/beam-parametric-cross-section.ifc` | 9 -> 18 | 1548 -> 2376 | IFCTSHAPEPROFILEDEF |
  | `IFC4.x-specification-models/models/building-elements/beam-revolved-solid/beam-revolved-solid.ifc` | 0 -> 1 | 0 -> 5892 | IFCREVOLVEDAREASOLID |
  | `IFC4.x-specification-models/models/building-elements/beam-varying-extrusion-paths/beam-varying-extrusion-paths.ifc` | 1 -> 2 | 172 -> 1576 | IFCREVOLVEDAREASOLID |
  | `engine_web-ifc/tests/ifcfiles/public/ISSUE_021_Mini Project.ifc` | 2524 -> 2636 | 156946 -> 167546 | IFCLSHAPEPROFILEDEF, IFCROUNDEDRECTANGLEPROFILEDEF, IFCREVOLVEDAREASOLID |
  | `files/1030--proxy--no-geometry--accept-errors.ifc` | 0 -> 1 | 0 -> 4154 | IFCREVOLVEDAREASOLID |
  | `files/10336--buildingElementProxy--wrong-geometry.ifc` | 0 -> 0 | 0 -> 0 | IFCREVOLVEDAREASOLID |
  | `files/598--discreteAccessory--segfault--augmented.ifc` | 0 -> 1 | 0 -> 76 | IFCLSHAPEPROFILEDEF |
  | `files/IfcCShapeProfileDef.ifc` | 0 -> 2 | 0 -> 344 | IFCCSHAPEPROFILEDEF |
  | `files/IfcCsgPrimitive.ifc` | 0 -> 1 | 0 -> 9356 | IFCBLOCK, IFCSPHERE, IFCRIGHTCIRCULARCYLINDER, IFCRIGHTCIRCULARCONE, IFCRECTANGULARPYRAMID |
  | `files/IfcEllipseProfileDef.ifc` | 0 -> 2 | 0 -> 184 | IFCELLIPSEPROFILEDEF |
  | `files/IfcIShapeProfileDef.ifc` | 2 -> 3 | 216 -> 388 | IFCASYMMETRICISHAPEPROFILEDEF |
  | `files/IfcLShapeProfileDef.ifc` | 0 -> 4 | 0 -> 272 | IFCLSHAPEPROFILEDEF |
  | `files/IfcRectangleProfileDef.ifc` | 3 -> 4 | 76 -> 216 | IFCROUNDEDRECTANGLEPROFILEDEF |
  | `files/IfcTShapeProfileDef.ifc` | 0 -> 4 | 0 -> 496 | IFCTSHAPEPROFILEDEF |
  | `files/IfcTrapeziumProfileDef.ifc` | 0 -> 3 | 0 -> 36 | IFCTRAPEZIUMPROFILEDEF |
  | `files/IfcUShapeProfileDef.ifc` | 0 -> 4 | 0 -> 368 | IFCUSHAPEPROFILEDEF |
  | `files/IfcZShapeProfileDef.ifc` | 0 -> 2 | 0 -> 184 | IFCZSHAPEPROFILEDEF |
  | `files/acad2010_objects.ifc` | 0 -> 52 | 0 -> 8522 | IFCPRODUCTREPRESENTATION |
  | `files/acad2010_walls.ifc` | 0 -> 8 | 0 -> 168 | IFCPRODUCTREPRESENTATION |
  | `files/basic_shape_CSG.ifc` | 0 -> 1 | 0 -> 12 | IFCCSGSOLID, IFCBLOCK |
  | `files/building_service_element_air-terminal.ifc` | 1 -> 2 | 312 -> 344 | IFCEXTRUDEDAREASOLIDTAPERED |
  | `files/ellipses.ifc` | 0 -> 4 | 0 -> 1408 | IFCELLIPSEPROFILEDEF |
  | `files/nested_mapped_item.ifc` | 0 -> 53 | 0 -> 9402 | IFCPRODUCTREPRESENTATION |
  | `files/standard_case_element_beam.ifc` | 9 -> 18 | 1548 -> 2376 | IFCTSHAPEPROFILEDEF |

- `files/10336--…`: still drawn empty: its revolved solids sweep an `IfcCenterLineProfileDef`
  (not supported), so the warning moved from "unsupported geometry IFCREVOLVEDAREASOLID" to
  "unsupported profile IFCCENTERLINEPROFILEDEF" (×2 -> ×6).
- Products with a body drawn: **125,538 -> 125,817** of 128,591 (before: 128,478) (the 52 + 8 + 53 products of
  the three `IfcProductRepresentation` files now count as products with a body).

### IfcOpenShell agreement

- **The 40-file sample (products with geometry): IfcOpenShell 15,679; C 15,409 -> 15,414; equal
  files 32 -> 34.** Fixed: `files/IfcLShapeProfileDef.ifc` (0 -> 4 of 4) and
  `beam-varying-extrusion-paths` (1 -> 2 of 2: a revolved solid). Still fewer: the two
  "representation nesting too deep" files, `files/338--…` (IfcAdvancedBrep) and Viadotto Acerno
  (IfcSectionedSolidHorizontal, IfcLinearPlacement).
- `ISSUE_021_Mini Project.ifc` (L, rounded rectangle, revolved; not in the sample): IfcOpenShell
  2,636 products with geometry; C 2,524 -> 2,636.
- **Per product, volume (1 %) and box (1 mm), C against IfcOpenShell 0.9** (`files/` and the
  specification models that use the new types; per-element meshes, so files under 20,000
  triangles): equal on L, T, U, C, Z, asymmetric I, trapezium, ellipse (2 files), rounded
  rectangle, `IfcProductRepresentation` (acad2010 objects 52/52, walls 8/8, nested mapped item
  51/51 drawn by both), `standard_case_element_beam` 18/18, `beam-parametric-cross-section`
  18/18, `beam-revolved-solid`, `beam-varying-extrusion-paths`, `bath-csg-solid`,
  `csg-primitive`, `basic_shape_CSG`. Differences, each explained:
  - L, T, U files: 2 of 4 products each, 1.7–3 % more volume in C: those have a leg, web or
    flange slope, which IfcOpenShell draws and C does not (documented; the I-shape ignores its
    slope too). Boxes equal.
  - `IfcRectangleProfileDef.ifc`: one `IfcRectangleHollowProfileDef` with radii (Rust's
    behaviour, unchanged: drawn sharp). The rounded rectangle itself agrees (24,561 / 24,558).
  - `IfcCsgPrimitive.ifc`: boxes equal (0.2 mm); volume C 40.9e9, IfcOpenShell 36.1e9 mm³. By
    subtree: the hollow block (24.83e9) and sphere ∩ block (3.790e9 / 3.791e9) agree;
    IfcOpenShell draws nothing for the union of four `IfcRightCircularCone`s (C 4.49e9) and
    its roof (pyramid ∪ cylinder minus pyramid minus cylinder ∩ block) has an exact-rational
    volume (7.4773e9) that no cylinder could give: it leaves the cylinders out.
  - `building_service_element_air-terminal.ifc`: the tapered air terminal is drawn by C only
    (IfcOpenShell: none); its volume is the hand value: a 24×24 in, 2 in wall hollow square to
    12×12, 1 in wall, over 4 in: 4/3 (576 + 144 + 288) − 4/3 (400 + 100 + 200) = 410.67 in³ =
    6,729,621 mm³ (C 6,729,621).
  - `files/1030--proxy--no-geometry--accept-errors.ifc` (a revolved solid): C draws it,
    IfcOpenShell does not.
- The synthetic fixtures: IfcOpenShell agrees with the hand numbers on every profile (exact on
  the sharp ones), on both revolved solids (box to 0.001 mm), on `csg_block`; on `tapered` it
  puts the end profile at twice the depth (its 2D operator gets a z), on `csg_round` it draws
  the sphere alone.

### Swept-disk rebar: 3.6x faster, the same bytes

Profiled with gprof (`clang -O2 -pg`, one thread): half of `files/468--…`'s read went to
`cc_sin`/`cc_cos` (the exact double-double ones), called once per ring point of `tube()` for
the same n angles on every ring (498,388 calls); then a third of the rest to `cc_sqrt` (a
digit-by-digit square root, 1.08 M calls). Fixes: the ring's cos/sin once per tube (the same
values, so the same bits); `cc_sqrt` uses the IEEE 754 instruction (`fsqrt`, `sqrtsd`, correctly
rounded like the digit version) under clang and GCC on aarch64 and x86-64; tcc and wasm keep the
digit version. Read call, mean of 3 runs (`ifc_dump read`, 8 threads):

| file | before | after | Rust (IFC1) |
|---|---|---|---|
| `files/468--reinforcingBar--segfault--augmented.ifc` | 1,410 ms | 390 ms | 373 ms |
| `files/424--reinforcingBar--segfault--augmented.ifc` | 760 ms | 190 ms | 190 ms |
| all 617 STEP files (read calls, sum) | 16.5 s | 13.2 s | |

`scene.txt` and every mesh hash identical before and after on both files (and on the 591
unchanged files above). What is left is the shared mesh builder (`cc_scene_mesh_build`: the
weld hash and crease edges), which Rust also pays.

### ASan+UBSan

`bench/corpus.sh asan` then `CORPUS_TAG=ifc2-asan bench/corpus.sh run` (clang -O1 -g,
ASan+UBSan+LeakSanitizer, `halt_on_error`) on the IFC2 sources: 655 files (617 STEP, 38 not
STEP), **no report**; every outcome equal to the -O2 build's (617 ok, 38 refused). That run's C
scenes are the "after" of the file list above (it includes the trapezium centring, made after the
first "after" run). The sum of the read calls varies with the machine's load (other builders):
13.2 s and 15.9 s in the two "after" runs, 16.5 s before.

### Not in IFC2: approach and size for the three large gaps

- **IfcAdvancedBrep** (9 files, 2,119 items; DigitalHub doors and windows). Faces are
  `IfcAdvancedFace` on `IfcBSplineSurfaceWithKnots` (rational too), `IfcPlane`,
  `IfcCylindricalSurface`, bounded by `IfcEdgeLoop`s of `IfcOrientedEdge` / `IfcEdgeCurve` over
  B-spline curves, polylines and circles. Approach: evaluate curves and surfaces with de Boor
  (rational weights); per face, the boundary in the surface's (u, v) space (point inversion by
  Newton on the B-spline, closed form on planes and cylinders), a (u, v) grid at the arc
  tolerance clipped by that boundary and triangulated (earcut has no interior points: a
  grid-and-clip or a small constrained Delaunay), then lifted to 3D; planar faces through
  `add_polygon`. Size: about 900 lines (curves and surfaces 300, inversion and trimming 300,
  triangulation 300) plus fixtures; 3–4 builder days. Check per face area against IfcOpenShell
  (OpenCascade).
- **IfcLinearPlacement and alignment geometry** (8 files, 6,252 placements). An
  `IfcAxis2PlacementLinear` is a point at a distance along an alignment curve
  (`IfcPointByDistanceExpression`: distance along, lateral, vertical, longitudinal offsets) plus
  an orientation. Approach: one evaluator "position and frame at distance s" for
  `IfcCompositeCurve` / `IfcGradientCurve` / `IfcSegmentedReferenceCurve` made of
  `IfcCurveSegment`s: lines and circles in closed form, clothoids by the Fresnel series, the
  IFC4X3 polynomial and trigonometric spirals (cubic, Bloss, Helmert, cosine, sine, Viennese
  bend) by Gauss–Legendre integration of their curvature; the vertical as a second curve over
  the horizontal length, cant as a roll. Size: 700–900 lines; 3–4 days; the bSI rail unit
  tests (`IFC-Rail-Sample-Files/1_Alignment with Cant`) and IfcOpenShell 0.8+ as references.
- **IfcSectionedSolidHorizontal** (5 files, 697 items; Viadotto Acerno: 634 products). Needs the
  evaluator above for its directrix; then the cross sections (often `IfcOpenCrossProfileDef`:
  widths and slopes to a polyline, to add) at their `IfcAxis2PlacementLinear` stations, sampled
  along the directrix at the arc tolerance, profiles interpolated point to point between
  stations, each section kept vertical (horizontal frame: no tilt with the gradient), rings
  joined like the revolved solid, caps at the ends. Size: 300–400 lines after the evaluator;
  1–2 days. The same evaluator also gives `IfcFixedReferenceSweptAreaSolid`,
  `IfcDirectrixCurveSweptAreaSolid` and `IfcInclinedReferenceSweptAreaSolid`.

## After IFC3 (25 Nov 2026): the importer at the reader's level, slopes, more profiles, depth 64

Ticket IFC3: the second round in the C core (`ccore/src/ifc_geom.h`, `ifc_read.c`, `ifc_in.c`);
Rust (frozen, v0.19.0) still has every gap. What C does past Rust: `ccore/golden/DEVIATIONS.md`
section 10, "Ticket IFC3". Each new type has a synthetic fixture in `ccore/golden/ifc_c/fixtures/`,
a C-own read golden (`golden/ifc_c/read/`) and, new, a C-own import golden
(`golden/ifc_c/import/<name>.json`, `.report.txt`), with hand-computed volumes and boxes in
`ccore/test/ifc_read_test.c` and `ifc_in_test.c` (`own_imports`). Every Rust-origin golden is
unchanged (the suites compare them: 0 failed).

| item | done |
|---|---|
| 1. the importer takes a plain `IfcProductRepresentation` for every class (it took it only for the five classes it parametrises; IfcMember, proxies… were dropped without a word) | yes |
| 2. importer and reader parity (table below); an extrusion is parametrised only when it is the whole Body | yes |
| 3. slopes: I and U FlangeSlope, T WebSlope and FlangeSlope, L LegSlope; `IfcCenterLineProfileDef`, `IfcCompositeProfileDef`, `IfcMirroredProfileDef` (`IfcDerivedProfileDef` was there) | yes |
| 4. `IfcGeometricSet` / `IfcGeometricCurveSet` read item by item; curves, points, texts in a body: "no body by design" | yes |
| 5. representation nesting 32 -> 64 levels (the corpus needs 49) | yes |

### Importer and display reader: what each takes (parity audit)

The importer (`cc_ifc_import`) shares the reader's geometry (`ifc_geom.h`: `profile()`,
`item()`). Its rule for an element: (a) its Body is one `IfcExtrudedAreaSolid` (through mapped
items of one item each) that extrudes straight up a profile it can read: a parametric element
(`wall`, `rect_column`, `rect_beam`, `pad_footing`, `slab`) or an `extruded_polygon` (any outline,
holes included); (b) else its Body is one rigid `IfcMappedItem` without openings: a placed copy of
the map's shared mesh; (c) else a `mesh` of everything `item()` draws; (d) nothing drawn: the named
line `skipped: <class>: a body Bimble can't read yet` (or `no shape in the file`, `no 3D body,
only <ids>`). Every new type follows that rule, so nothing the reader draws is dropped:

| reader accepts | importer, before IFC3 | importer, after |
|---|---|---|
| plain `IfcProductRepresentation` | IfcColumn, IfcBeam, IfcSlab, IfcWall, IfcFooting only; **other classes dropped silently** | every class (`product_rep_member`) |
| L, T, U, C, Z, asymmetric I, trapezium, ellipse, rounded rectangle profiles (IFC2) | `extruded_polygon` (shared `profile()`) | the same; the sloped ones too |
| I, U, T, L slopes (IFC3) | drawn without the slope | `extruded_polygon` with the slope |
| `IfcCenterLineProfileDef`, `IfcMirroredProfileDef` (IFC3) | none: "a body Bimble can't read yet" | `extruded_polygon` (the closed centre line: with its hole) |
| `IfcCompositeProfileDef` (IFC3) | none | `mesh` (several regions are not one extrusion; one profile: that profile) |
| `IfcRevolvedAreaSolid`, `IfcExtrudedAreaSolidTapered`, `IfcCsgSolid` and the five primitives (IFC2) | `mesh` | `mesh` |
| booleans, face sets, breps, surface models, swept disks (Rust's set) | as Rust | as Rust |
| an extrusion beside other items in one Body | **the first extrusion only, the rest dropped silently** (Rust too) | `mesh` of the whole Body (`multi_item`) |
| curves, points, texts in a Body (IFC3: "no body by design") | "a body Bimble can't read yet" | the same line (and the reader's warning) |
| 33..64 levels of nesting (IFC3) | none | `mesh` (`deep_nesting`) |

On the corpus (`ifc_dump import`, the 616 STEP files under 60 MB, before = the IFC2 build, after
= IFC3): **114,723 -> 114,797 elements imported** (the reader draws 114,708 elements on these
files); 63 files changed. The largest change is the whole-Body rule: **12,825 elements had been
parametrised from their first extrusion only** and are now meshes of their whole Body (doors and
windows of frames and panes, MEP fittings, furniture, and 60 walls, 13 slabs, 2 columns:
`DigitalHub_FM-ARC_v2.ifc` has 53 walls of four extrusions each, of which the importer kept one).
Kinds after: mesh 63,423 -> 76,261, extruded_polygon 42,801 -> 30,112, wall 5,215 -> 5,155, slab
1,196 -> 1,183, rect_column 466 -> 464 (beams and footings unchanged). +74 elements: the
`IfcProductRepresentation` products of `acad2010_objects.ifc` (23 -> 52) and
`nested_mapped_item.ifc` (23 -> 53), and the new types. The reader draws more than the importer
imports on 8 files only, none for a geometry reason: invalid or duplicate GlobalIds (the
IfcOpenShell "augmented" files `458`, `471`, `474`, `478` use `xxxx…`, `aaaa…`;
`ifcopenshell_halfspaces.ifc` has ids out of the base-64 range; `construction2.ifc` and `640--…`
one each): the importer skips such an element by name (`invalid GlobalId`, `duplicate
GlobalId`), as Rust does.

### Slopes: reference points and IfcOpenShell

The IFC figures do not dimension where the nominal thickness of a sloped face holds; the reference
points are IfcOpenShell's, read in its source (v0.8.0, `src/ifcgeom/mapping/IfcIShapeProfileDef.cpp`
and the U, T, L ones; 0.9 is the venv here): I at the flange's edge, U at half FlangeWidth from the
web's back, L at the box's centre lines, T's web at mid-depth and T's flange a quarter FlangeWidth
from the centre. The slope is the face's angle exactly. One difference: IfcOpenShell's T underside
ends at x = WebThickness/2 with a run of x/2 - WebThickness (it mixes the half and the whole web),
so its face is a little less steep than FlangeSlope; C keeps the slope.

Per product volume, C against IfcOpenShell 0.9 (before -> after):

| file / fixture | before | after |
|---|---|---|
| `files/IfcLShapeProfileDef.ifc` (2 sloped of 4) | +1.70 %, +1.68 % | +0.000 %, +0.003 % |
| `files/IfcUShapeProfileDef.ifc` (2 sloped of 4) | +1.58 %, +2.24 % | +0.000 %, +0.012 % |
| `files/IfcTShapeProfileDef.ifc` (2 sloped of 4) | +2.84 %, +2.94 % | +0.22 %, +0.21 % |
| `ishape_slope` (hand 13,411,040.8 mm³) | -13.5 % | 0.000 % |
| `ushape_slope` (hand 3,270,207.2) | +1.77 % | 0.000 % |
| `ushape_slope_fillet` (hand 3,295,600.4; arcs as chords) | +1.95 % | -0.015 % |
| `lshape_slope` (hand 1,795,060.3) | +5.85 % | 0.000 % |
| `tshape_slope` (hand 3,998,768.5; IfcOpenShell 3,965,280.7, its T rule) | +3.40 % | +0.84 % |

The I-shape corpus file has no slope (IFC2X3); unchanged. Every corpus profile file is within
0.5 % of IfcOpenShell.

Other new types against IfcOpenShell 0.9: `IfcCompositeProfileDef` agrees region by region
(`ISSUE_044`: the new element 431,487.4 / 431,487.5 mm³; `793--wall--segfault.ifc` -0.02 %;
`files/IfcCompositeProfileDef.ifc` +4.5 %, all of it its derived T scaled by 0.3, where
IfcOpenShell's area is 0.018 of the parent's instead of 0.3² = 0.09; the other three regions
agree to 0.06 %). `IfcCenterLineProfileDef`: IfcOpenShell 0.8 maps it to nothing; 0.9 fills the
curve's area (`files/10336--…`: a 10 mm strip on a half ellipse 50 x 25, C 120,811 mm³ against
121,100 by hand, IfcOpenShell 194,244, the half-ellipse's area). `IfcMirroredProfileDef`:
IfcOpenShell mirrors inside the parent's Position (the one corpus file with it,
`UT_SAS_4/IFC reference files/UT_SAS_2.ifc`, IFC4X3_RC3, uses it as the section of an
IfcSectionedSolidHorizontal: no change there). Curves in a body: neither draws
them. The deep files: `599--beam--segfault.ifc` +0.002 %, `588--…` -1.3 % (its augmented
faceted breps cut 49 times).

### Before and after, file by file (C, `CORPUS_TAG=ifc3-before` / `ifc3-after`)

Before: the IFC2 build (HEAD at the ticket's start, `C_DUMP=`); after: the IFC3 build.
**828 files; 811 give the same scene, byte for byte; 17 changed, each explained** (checked by a
script: the file holds a new type, or warned "representation nesting too deep" before):

| file | elements | triangles | why |
|---|---|---|---|
| `IfcSampleFiles/Ifc4_Revit_ARC.ifc` | 442 -> 442 | 351933 -> 351933 | warning only: IFCINDEXEDPOLYCURVE now "no body by design" |
| `IfcSampleFiles/Ifc4_Revit_ARC_FireRatingAdded.ifc` | 442 -> 442 | 351933 -> 351933 | the same |
| `IfcSampleFiles/Ifc4_Revit_MEP.ifc` | 6787 -> 6787 | 997812 -> 997812 | the same |
| `engine_web-ifc/tests/ifcfiles/public/FM_ARC_DigitalHub.ifc` | 529 -> 529 | 21744 -> 21744 | warning only: IFCPOLYLINE in a body |
| `engine_web-ifc/tests/ifcfiles/public/ISSUE_034_HouseZ.ifc` | 228 -> 228 | 242972 -> 242972 | warning only: IFCGEOMETRICSET of curves and texts |
| `engine_web-ifc/tests/ifcfiles/public/ISSUE_044_test_IFCCOMPOSITEPROFILEDEF.ifc` | 6 -> 7 | 104 -> 128 | IFCCOMPOSITEPROFILEDEF |
| `engine_web-ifc/tests/ifcfiles/public/S_Office_Integrated Design Archi.ifc` | 3403 -> 3407 | 1165570 -> 1166908 | nesting bound (42 deep) |
| `files/10336--buildingElementProxy--wrong-geometry.ifc` | 0 -> 6 | 0 -> 2752 | IFCCENTERLINEPROFILEDEF; IFCGEOMETRICSET now names its surfaces |
| `files/452--line-segment--curved.ifc` | 0 -> 0 | 0 -> 0 | warning only: IFCGEOMETRICSET of curves |
| `files/452--line-segment-straight.ifc` | 0 -> 0 | 0 -> 0 | the same |
| `files/588--buildingElementPart--timeout--augmented.ifc` | 0 -> 1 | 0 -> 26 | nesting bound (49 deep) |
| `files/599--beam--segfault.ifc` | 0 -> 1 | 0 -> 1546 | nesting bound (39 deep) |
| `files/793--wall--segfault.ifc` | 0 -> 1 | 0 -> 14188 | IFCCOMPOSITEPROFILEDEF |
| `files/IfcCompositeProfileDef.ifc` | 0 -> 1 | 0 -> 808 | IFCCOMPOSITEPROFILEDEF (an L with LegSlope inside) |
| `files/IfcLShapeProfileDef.ifc` | 4 -> 4 | 272 -> 272 | LegSlope |
| `files/IfcTShapeProfileDef.ifc` | 4 -> 4 | 496 -> 496 | WebSlope, FlangeSlope |
| `files/IfcUShapeProfileDef.ifc` | 4 -> 4 | 368 -> 368 | FlangeSlope |

- Elements drawn (617 STEP files): **125,817 -> 125,832** (+15).
- The unsupported list loses `IFCGEOMETRICSET` (4 files), `IFCINDEXEDPOLYCURVE` (3),
  `IFCPOLYLINE` (1), `IFCTEXTLITERALWITHEXTENT` (1), `IFCCOMPOSITEPROFILEDEF` (3),
  `IFCCENTERLINEPROFILEDEF` (1) and "representation nesting too deep" (3); it gains
  `IFCSURFACEOFLINEAREXTRUSION` and `IFCSURFACEOFREVOLUTION` (1 file each, `10336--…`: they were
  hidden inside its geometric sets). Left: IfcAdvancedBrep (9), IfcSectionedSolidHorizontal (5),
  IfcFixedReferenceSweptAreaSolid (3), IfcSurfaceCurveSweptAreaSolid (2),
  IfcInclinedReferenceSweptAreaSolid (1), the two surfaces (1).

### IfcOpenShell agreement (the 40-file sample, products with geometry)

**IfcOpenShell 15,679; C 15,414 -> 15,416; equal files 34 -> 36** (`588--…` and `599--…`, the
nesting bound). Still fewer: `338--…` (IfcAdvancedBrep), Viadotto Acerno
(IfcSectionedSolidHorizontal, IfcLinearPlacement); still more: the two INDOT files IfcOpenShell
draws empty.

### Nesting bound: 64

The deepest item nesting in the 617 STEP files (a script over the item graph: boolean operands,
CSG roots, mapped items, geometric sets): 49 (`files/588--…`), then 42 (`S_Office_Integrated
Design Archi.ifc`), 39 (`files/599--…`), 31, 30; all others under 30. The bound is now 64
(`MAX_NESTING`, `ifc_geom.h`); the budget on visited items and triangles still caps the total
work (a binary tree 64 deep visits at most `visits_left`, 5 M or a tenth of the displayed bytes).
The recursion's stack: the wasm module has 64 KB (`__stack_pointer` 65,536); per level item()
and boolean_result() now take 128 + 80 bytes on wasm32 (`-fstack-usage`, `-O2`) against 416 + 720
before (the leaves, the mapped item's frames and the boolean arithmetic moved to NOINLINE
functions), so 64 levels use 13 KB where 32 used 36 KB. Checked: `open_ifc` in `ccore.wasm` on
`deep_nesting.ifc` (64), `deep_nesting_over.ifc` (65), `588--…` and `599--…`: no trap.
Fixtures: `deep_nesting` (64 nested `IfcBooleanResult`, drawn) and `deep_nesting_over` (65:
warned, the proxy skipped by the importer with its named line).

### ASan+UBSan

`bench/corpus.sh asan` then `CORPUS_TAG=ifc3-asan bench/corpus.sh run` (clang -O1 -g,
ASan+UBSan+LeakSanitizer, `halt_on_error`) on the IFC3 sources: 655 files (617 STEP, 38 not
STEP; 173 git-lfs pointers skipped as before), **no report**; every outcome equal to the -O2
build's (617 ok, 38 refused), and the -O2 scenes of that run equal the "after" run's on all 828
records. `ccore/ci/run.sh full`: 37 suites, 0 failed (the clang ASan+UBSan unit run included).

## After TESS1 (27 Nov 2026): arcs by deflection, booleans that take nothing

The reader's default detail is now standard (ADR 0085, `golden/DEVIATIONS.md` section 40: chords
within 2 mm and 18 degrees; a boolean or an opening that takes nothing keeps its operand; kernel
results cleaned); exact is Rust's reader. Before: the build at the ticket's start (`C_DUMP=`, the
exact rule); after: the TESS1 build at its default, `CORPUS_TAG=tess1-before` / `tess1-after`,
each file under the shared measure lock with a 6 GB cgroup cap.

**828 records (617 STEP files read, 38 refused, 173 git-lfs pointers skipped, as before); 405 files
give the same scene, byte for byte; 212 changed, every one in its triangles only.**

| | before | after |
|---|---|---|
| outcomes (ok / refused / skipped) | 617 / 38 / 173 | 617 / 38 / 173 |
| crashes, timeouts, memory kills | 0 | 0 |
| products with a body | 128,591 | 128,591 |
| elements drawn | 125,832 | 125,832 |
| triangles | 32,276,755 | 30,569,953 (−5.3 %) |
| files with more triangles than before | | 0 |
| files whose warnings changed | | 0 |
| read time, sum of the 617 read calls | 15.5 s | 13.8 s |

Largest reductions (triangles before -> after):

| file | triangles |
|---|---|
| `IfcSampleFiles/Ifc4_Revit_STR.ifc` | 1,411,324 -> 1,056,164 |
| `files/468--reinforcingBar--segfault--augmented.ifc` | 496,132 -> 142,692 |
| `files/424--reinforcingBar--segfault--augmented.ifc` | 283,472 -> 81,648 |
| `IFC-infra-unit-test/SpatialStructure-6/SpatialStructure-6-3.ifc` | 558,348 -> 471,404 |
| `files/330--reinforcingBar--segfault--augmented.ifc` | 224,104 -> 151,720 |
| `ifc-sample-files/DigitalHub_v2/DigitalHub_FM-SAN_v2.ifc` | 1,473,764 -> 1,413,428 |
| `engine_web-ifc/tests/ifcfiles/public/ISSUE_068_ARK_NUS_skolebygg.ifc` | 1,403,447 -> 1,357,422 |
| `files/IfcReinforcingBar.ifc` | 263,256 -> 219,264 |

A first after-run showed one file with more triangles (`files/Allplan 2017 Test
Bauteil-Oberflächen.ifc`, 19,124 -> 19,132: an operand kept where the kernel's result of the same
solid was lighter); the reader now keeps the lighter of the two (19,116), and the run above is the
one after that fix. Five public files have products with two `Body` representations (741 products
in 5 files, `Ifc4_Revit_MEP.ifc` 627): C draws both, as before; none has `Body` and `Facetation`
together.

## Tables (generated: `ccore/bench/corpus.sh report`)

### Files by corpus

| corpus | STEP files | IFC2X3 | IFC4 | IFC4X3 | other schema | zip | xml | json | other | LFS pointer (not fetched) | MB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| bim-whale-ifc-samples | 6 | 6 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 100 |
| Certification-datasets | 35 | 10 | 15 | 10 | 0 | 0 | 0 | 0 | 0 | 0 | 16 |
| Community-Sample-Test-Files | 0 | 0 | 0 | 0 | 0 | 27 | 0 | 0 | 0 | 173 | 25 |
| engine_web-ifc | 29 | 19 | 7 | 2 | 1 | 2 | 0 | 0 | 0 | 0 | 481 |
| files | 271 | 228 | 42 | 1 | 0 | 4 | 3 | 0 | 1 | 0 | 122 |
| IFC-files | 8 | 8 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 134 |
| IFC-infra-unit-test | 62 | 0 | 3 | 59 | 0 | 0 | 0 | 0 | 0 | 0 | 68 |
| IFC-Rail-Sample-Files | 131 | 0 | 0 | 131 | 0 | 0 | 0 | 0 | 0 | 0 | 15 |
| ifc-sample-files | 11 | 6 | 4 | 0 | 1 | 1 | 0 | 0 | 0 | 0 | 189 |
| IFC4.x-specification-models | 46 | 0 | 0 | 46 | 0 | 0 | 0 | 0 | 0 | 0 | 2 |
| IfcSampleFiles | 18 | 6 | 12 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 184 |
| all | 617 | 283 | 83 | 249 | 2 | 34 | 3 | 0 | 1 | 173 | 1336 |

Schemas (FILE_SCHEMA): IFC2X3 283, IFC4 83, IFC4X2 2, IFC4X3_ADD2 57, IFC4X3_RC2 2, IFC4X3_RC3 72, IFC4X3_RC4 115, IFC4x3 1, IFC4x3_RC3 1, ifc4x3_add2 1

### Outcome by schema (STEP files)


C reader (clang -O2):

| schema | files | ok | refused | crash | timeout | over memory |
|---|---|---|---|---|---|---|
| IFC2X3 | 283 | 283 | 0 | 0 | 0 | 0 |
| IFC4 | 83 | 83 | 0 | 0 | 0 | 0 |
| IFC4X3 | 249 | 249 | 0 | 0 | 0 | 0 |
| other | 2 | 2 | 0 | 0 | 0 | 0 |
| all | 617 | 617 | 0 | 0 | 0 | 0 |

Rust read_str:

| schema | files | ok | refused | crash | timeout | over memory |
|---|---|---|---|---|---|---|
| IFC2X3 | 283 | 283 | 0 | 0 | 0 | 0 |
| IFC4 | 83 | 83 | 0 | 0 | 0 | 0 |
| IFC4X3 | 249 | 249 | 0 | 0 | 0 | 0 |
| other | 2 | 2 | 0 | 0 | 0 | 0 |
| all | 617 | 617 | 0 | 0 | 0 | 0 |

C reader, ASan+UBSan:

| schema | files | ok | refused | crash | timeout | over memory |
|---|---|---|---|---|---|---|
| IFC2X3 | 283 | 283 | 0 | 0 | 0 | 0 |
| IFC4 | 83 | 83 | 0 | 0 | 0 | 0 |
| IFC4X3 | 249 | 249 | 0 | 0 | 0 | 0 |
| other | 2 | 2 | 0 | 0 | 0 | 0 |
| all | 617 | 617 | 0 | 0 | 0 | 0 |

Non-STEP files (ifczip, ifcxml, zip, ...), C reader: refused 38; Rust: refused 38

### C vs Rust

Both ok: 617; identical (scene.txt and every mesh hash): 616; different: 1.
Outcome classes differ on 0 file(s).

| file | C | Rust | differs in | C elements / tris | Rust elements / tris |
|---|---|---|---|---|---|
| `engine_web-ifc/tests/ifcfiles/public/Viadotto Acerno.ifc` | ok  | ok  | scene.txt | 5900 / 2414913 | 5900 / 2414913 |

### Geometry (C, files read ok: 617)

Products with a body (IfcProductDefinitionShape and a placement, not a skipped class): 128478; drawn (non-empty mesh): 125538; drawn empty (no geometry produced): 2940 (2.3 %). Triangles: 32200963.

| schema | files ok | products with a body | drawn | empty | empty % |
|---|---|---|---|---|---|
| IFC2X3 | 283 | 93381 | 93020 | 361 | 0.4 |
| IFC4 | 83 | 20167 | 19742 | 425 | 2.1 |
| IFC4X3 | 249 | 14600 | 12446 | 2154 | 14.8 |
| other | 2 | 330 | 330 | 0 | 0.0 |

Products with a body drawn empty, by IFC type (ranked by products):

| IFC type | files | products | example file |
|---|---|---|---|
| IFCALIGNMENTSEGMENT | 64 | 1137 | `IFC-Rail-Sample-Files/1_Alignment with Cant (AWC)/UT_AWC_0_(Synthetic_Cases)/Horizontal/SyntheticTestcases/Bloss/TS1_Bloss_100.0_inf_300_1_Meter/GENERATED__Bloss_100.0_inf_300_1_Meter.ifc` |
| IFCBEAM | 7 | 675 | `IFC-Rail-Sample-Files/3_Swept Area Solid (SAS)/UT_SAS_4/IFC reference files/UT_SAS_2.ifc` |
| IFCBUILDINGELEMENTPROXY | 28 | 210 | `IFC-Rail-Sample-Files/3_Swept Area Solid (SAS)/UT_SAS_1/IFC reference files/RWR_Rail_Phase2_UT_SAS_1.ifc` |
| IFCDOOR | 2 | 111 | `engine_web-ifc/tests/ifcfiles/public/FM_ARC_DigitalHub.ifc` |
| IFCALIGNMENT | 77 | 106 | `IFC-Rail-Sample-Files/1_Alignment with Cant (AWC)/UT_AWC_0_(Synthetic_Cases)/Horizontal/SyntheticTestcases/Bloss/TS1_Bloss_100.0_inf_300_1_Meter/GENERATED__Bloss_100.0_inf_300_1_Meter.ifc` |
| IFCWINDOW | 2 | 99 | `engine_web-ifc/tests/ifcfiles/public/FM_ARC_DigitalHub.ifc` |
| IFCALIGNMENTHORIZONTAL | 67 | 93 | `IFC-Rail-Sample-Files/1_Alignment with Cant (AWC)/UT_AWC_0_(Synthetic_Cases)/Horizontal/SyntheticTestcases/Bloss/TS1_Bloss_100.0_inf_300_1_Meter/GENERATED__Bloss_100.0_inf_300_1_Meter.ifc` |
| IFCWALLSTANDARDCASE | 2 | 88 | `IfcSampleFiles/Ifc2x3_SampleCastle.ifc` |
| IFCWALL | 10 | 62 | `IfcSampleFiles/Ifc2x3_SampleCastle.ifc` |
| IFCALIGNMENTVERTICAL | 26 | 51 | `IFC-Rail-Sample-Files/1_Alignment with Cant (AWC)/UT_AWC_0_(Synthetic_Cases)/Horizontal/SyntheticTestcases/Viennese Bend/TS1_Viennese Bend_100.0_inf_300_1_Meter/GENERATED__VienneseBend_100.0_inf_300_1_Meter.ifc` |
| IFCSTRUCTURALCURVECONNECTION | 1 | 40 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_171_IfcSurfaceCurveSweptAreaSolid.ifc` |
| IFCCOURSE | 1 | 38 | `engine_web-ifc/tests/ifcfiles/public/Viadotto Acerno.ifc` |
| IFCSLAB | 3 | 28 | `IFC-Rail-Sample-Files/3_Swept Area Solid (SAS)/UT_SAS_4/IFC reference files/UT_SAS_2.ifc` |
| IFCDISCRETEACCESSORY | 2 | 26 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_021_Mini Project.ifc` |
| IFCRAILING | 5 | 23 | `IFC-Rail-Sample-Files/3_Swept Area Solid (SAS)/UT_SAS_4/IFC reference files/UT_SAS_2.ifc` |
| IFCALIGNMENTCANT | 16 | 20 | `IFC-Rail-Sample-Files/1_Alignment with Cant (AWC)/UT_AWC_0_(Synthetic_Cases)/Horizontal/SyntheticTestcases/Viennese Bend/TS1_Viennese Bend_100.0_inf_300_1_Meter/GENERATED__VienneseBend_100.0_inf_300_1_Meter.ifc` |
| IFCSTRUCTURALSURFACEMEMBER | 1 | 20 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_171_IfcSurfaceCurveSweptAreaSolid.ifc` |
| IFCMARINEFACILITY | 3 | 19 | `IFC-infra-unit-test/SpatialStructure-5/SpatialStructure-5-2.ifc` |
| IFCCOVERING | 1 | 15 | `engine_web-ifc/tests/ifcfiles/public/S_Office_Integrated Design Archi.ifc` |
| IFCSTAIRFLIGHT | 2 | 10 | `engine_web-ifc/tests/ifcfiles/public/FM_ARC_DigitalHub.ifc` |
| IFCBEAMSTANDARDCASE | 1 | 9 | `files/standard_case_element_beam.ifc` |
| IFCPROXY | 2 | 7 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_034_HouseZ.ifc` |
| IFCBUILDINGELEMENTPART | 3 | 6 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_044_test_IFCCOMPOSITEPROFILEDEF.ifc` |
| IFCLIGHTFIXTURE | 1 | 6 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_159_kleine_Wohnung_R22.ifc` |
| IFCPIPESEGMENT | 2 | 6 | `IFC-Rail-Sample-Files/2_Linear Placement (LP)/UT_LP_6/IFC reference files/Draft/UT_LP_6_Case1.ifc` |
| IFCSTRUCTURALCURVEMEMBER | 1 | 6 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_171_IfcSurfaceCurveSweptAreaSolid.ifc` |
| IFCBUILTELEMENT | 4 | 4 | `IFC-infra-unit-test/FixedReferenceSweptAreaSolid-1/FixedReferenceSweptAreaSolid-1.ifc` |
| IFCFACILITYPART | 2 | 4 | `IFC-infra-unit-test/SpatialStructure-6/SpatialStructure-6-2.ifc` |
| IFCSURFACEFEATURE | 1 | 3 | `engine_web-ifc/tests/ifcfiles/public/Viadotto Acerno.ifc` |
| IFCCOLUMN | 1 | 2 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_021_Mini Project.ifc` |
| IFCFLOWSEGMENT | 2 | 2 | `files/452--line-segment--curved.ifc` |
| IFCMEMBER | 1 | 2 | `engine_web-ifc/tests/ifcfiles/public/FM_ARC_DigitalHub.ifc` |
| IFCPAVEMENT | 1 | 2 | `engine_web-ifc/tests/ifcfiles/public/Viadotto Acerno.ifc` |
| IFCROOF | 2 | 2 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_159_kleine_Wohnung_R22.ifc` |
| IFCSANITARYTERMINAL | 2 | 2 | `IFC4.x-specification-models/models/advanced-geometric-shape/basin-advanced-brep/basin-advanced-brep.ifc` |
| IFCAIRTERMINAL | 1 | 1 | `files/building_service_element_air-terminal.ifc` |
| IFCDISTRIBUTIONCHAMBERELEMENT | 1 | 1 | `IFC-infra-unit-test/DrainageSystem-2/DrainageSystem-2.IFC` |
| IFCDUCTFITTING | 1 | 1 | `files/1269--Project_IfcDuctFitting.ifc` |
| IFCFOOTING | 1 | 1 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_171_IfcSurfaceCurveSweptAreaSolid.ifc` |
| IFCSOLIDSTRATUM | 1 | 1 | `IFC-infra-unit-test/Georeferencing-1/UT_GeoRef_1.ifc` |
| IFCWALLELEMENTEDCASE | 1 | 1 | `IfcSampleFiles/Ifc4_WallElementedCase.ifc` |

### Reader warnings, ranked by files (C; occurrences are the reader's counts)

| warning | files | occurrences | example file |
|---|---|---|---|
| no length unit in project; assuming millimetres | 11 | 11 | `IFC-Rail-Sample-Files/4_Railway Spatial Structure (RSS)/UT_RSS_1/IFC reference files/UT_RSS_1_reference file.ifc` |
| unsupported geometry IFCADVANCEDBREP (skipped) | 9 | 2119 | `IFC4.x-specification-models/models/advanced-geometric-shape/basin-advanced-brep/basin-advanced-brep.ifc` |
| unsupported placement IFCLINEARPLACEMENT (used identity) | 8 | 6252 | `IFC-Rail-Sample-Files/2_Linear Placement (LP)/UT_LP_1/IFC reference files/UT_LP_1_GeometryGym.ifc` |
| IfcWall: 2 opening(s) not cut (geometry not a closed solid) | 6 | 16 | `IfcSampleFiles/Ifc4_SampleHouse.ifc` |
| unsupported geometry IFCSECTIONEDSOLIDHORIZONTAL (skipped) | 5 | 697 | `IFC-Rail-Sample-Files/3_Swept Area Solid (SAS)/UT_SAS_2/IFC files from implementers/ACCA/UT_SAS_2_ACCA.ifc` |
| unsupported geometry IFCREVOLVEDAREASOLID (skipped) | 5 | 12 | `IFC4.x-specification-models/models/building-elements/beam-revolved-solid/beam-revolved-solid.ifc` |
| IfcWallStandardCase: 1 opening(s) not cut (geometry not a closed solid) | 5 | 6 | `files/1116--wall--segfault.ifc` |
| unsupported geometry IFCGEOMETRICSET (skipped) | 4 | 243 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_034_HouseZ.ifc` |
| boolean cut failed, drawn uncut (not a closed solid: manifold3d status: NotManifold) | 4 | 155 | `engine_web-ifc/tests/ifcfiles/public/S_Office_Integrated Design Archi.ifc` |
| unsupported geometry IFCINDEXEDPOLYCURVE (skipped) | 3 | 715 | `IfcSampleFiles/Ifc4_Revit_ARC.ifc` |
| unsupported profile IFCLSHAPEPROFILEDEF (skipped) | 3 | 112 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_021_Mini Project.ifc` |
| unsupported profile IFCTSHAPEPROFILEDEF (skipped) | 3 | 22 | `IFC4.x-specification-models/models/building-elements/beam-parametric-cross-section/beam-parametric-cross-section.ifc` |
| representation nesting too deep | 3 | 6 | `engine_web-ifc/tests/ifcfiles/public/S_Office_Integrated Design Archi.ifc` |
| IfcWall: 1 opening(s) not cut (geometry not a closed solid) | 3 | 5 | `IfcSampleFiles/Ifc4_Revit_ARC.ifc` |
| unsupported geometry IFCFIXEDREFERENCESWEPTAREASOLID (skipped) | 3 | 5 | `IFC-Rail-Sample-Files/2_Linear Placement (LP)/UT_LP_6/IFC reference files/Draft/UT_LP_6_Case1.ifc` |
| IfcWall: 3 opening(s) not cut (geometry not a closed solid) | 3 | 4 | `IfcSampleFiles/Ifc4_SampleHouse.ifc` |
| IfcWall: 4 opening(s) not cut (geometry not a closed solid) | 3 | 3 | `IfcSampleFiles/Ifc4_Revit_ARC.ifc` |
| unsupported geometry IFCCSGSOLID (skipped) | 3 | 3 | `IFC4.x-specification-models/models/advanced-geometric-shape/bath-csg-solid/bath-csg-solid.ifc` |
| unsupported profile IFCCOMPOSITEPROFILEDEF (skipped) | 3 | 3 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_044_test_IFCCOMPOSITEPROFILEDEF.ifc` |
| unsupported profile IFCROUNDEDRECTANGLEPROFILEDEF (skipped) | 2 | 25 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_021_Mini Project.ifc` |
| IfcFurnishingElement: 1 opening(s) not cut (geometry not a closed solid) | 2 | 16 | `IfcSampleFiles/Ifc2x3_Duplex_Architecture.ifc` |
| unsupported geometry IFCSURFACECURVESWEPTAREASOLID (skipped) | 2 | 8 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_171_IfcSurfaceCurveSweptAreaSolid.ifc` |
| unsupported profile IFCELLIPSEPROFILEDEF (skipped) | 2 | 6 | `files/IfcEllipseProfileDef.ifc` |
| IfcWall: 6 opening(s) not cut (geometry not a closed solid) | 2 | 4 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_068_ARK_NUS_skolebygg.ifc` |
| polygonal half-space: boundary not a closed polygon (not a closed solid: manifold3d status: NotManifold); cut by its plane alone | 2 | 4 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_068_ARK_NUS_skolebygg.ifc` |
| IfcSolidstratum: 1 opening(s) not cut (geometry not a closed solid) | 2 | 2 | `IFC-infra-unit-test/Earthworks-12d-2/Earthworks-12d-2.ifc` |
| polygonal half-space: boundary not closed; cut by its plane alone | 1 | 28 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_129_N1540_17_EXE_MOD_448200_02_09_11SMC_IGC_V17.ifc` |
| unsupported geometry IFCPOLYLINE (skipped) | 1 | 25 | `engine_web-ifc/tests/ifcfiles/public/FM_ARC_DigitalHub.ifc` |
| unsupported placement IFCGRIDPLACEMENT (used identity) | 1 | 25 | `IFC4.x-specification-models/models/placements-and-gis-referencing/grid-placement/grid-placement.ifc` |
| unsupported geometry IFCTEXTLITERALWITHEXTENT (skipped) | 1 | 5 | `engine_web-ifc/tests/ifcfiles/public/ISSUE_034_HouseZ.ifc` |
| unsupported profile IFCUSHAPEPROFILEDEF (skipped) | 1 | 4 | `files/IfcUShapeProfileDef.ifc` |
| unsupported profile IFCTRAPEZIUMPROFILEDEF (skipped) | 1 | 3 | `files/IfcTrapeziumProfileDef.ifc` |
| IfcWall: 18 opening(s) not cut (geometry not a closed solid) | 1 | 2 | `ifc-sample-files/DigitalHub_v2/DigitalHub_FM-ARC_v2.ifc` |
| IfcWall: 7 opening(s) not cut (geometry not a closed solid) | 1 | 2 | `ifc-sample-files/DigitalHub_v2/DigitalHub_FM-ARC_v2.ifc` |
| trimmed IFCPOLYLINE drawn untrimmed | 1 | 2 | `files/trimmed_polyline.ifc` |
| unsupported face loop IFCEDGELOOP (skipped) | 1 | 2 | `files/faces.ifc` |
| unsupported profile IFCCENTERLINEPROFILEDEF (skipped) | 1 | 2 | `files/10336--buildingElementProxy--wrong-geometry.ifc` |
| unsupported profile IFCCSHAPEPROFILEDEF (skipped) | 1 | 2 | `files/IfcCShapeProfileDef.ifc` |
| unsupported profile IFCZSHAPEPROFILEDEF (skipped) | 1 | 2 | `files/IfcZShapeProfileDef.ifc` |
| 2 malformed entity instance(s) skipped | 1 | 1 | `IFC-Rail-Sample-Files/7_Domain Physical Elements (DPE)/UT_DPE_4/IFC reference files/Draft/UT_DPE_4_Alignment_Site_Test_Placement.ifc` |
| IfcBuildingElementProxy: 16 opening(s) not cut (geometry not a closed solid) | 1 | 1 | `files/345--buildingElementProxy--segfault--1--augmented.ifc` |
| IfcBuildingElementProxy: 24 opening(s) not cut (geometry not a closed solid) | 1 | 1 | `files/345--buildingElementProxy--segfault--2--augmented.ifc` |
| IfcCovering: 3 opening(s) not cut (geometry not a closed solid) | 1 | 1 | `files/tests/Covering_with_3_openings.ifc` |
| IfcWall: 5 opening(s) not cut (geometry not a closed solid) | 1 | 1 | `ifc-sample-files/DigitalHub_v2/DigitalHub_FM-ARC_v2.ifc` |
| IfcWallStandardCase: 2 opening(s) not cut (geometry not a closed solid) | 1 | 1 | `files/411--walls--segfault--accept-errors.ifc` |
| hollow swept disk drawn solid (inner radius ignored) | 1 | 1 | `files/1032-curve.ifc` |
| trimmed IFCTRIMMEDCURVE drawn untrimmed | 1 | 1 | `files/trimmed_polyline.ifc` |
| unsupported geometry IFCBLOCK (skipped) | 1 | 1 | `files/IfcCsgPrimitive.ifc` |
| unsupported geometry IFCEXTRUDEDAREASOLIDTAPERED (skipped) | 1 | 1 | `files/building_service_element_air-terminal.ifc` |
| unsupported geometry IFCINCLINEDREFERENCESWEPTAREASOLID (skipped) | 1 | 1 | `IFC-Rail-Sample-Files/3_Swept Area Solid (SAS)/UT_SAS_1/IFC reference files/RWR_Rail_Phase2_UT_SAS_1.ifc` |
| unsupported geometry IFCSPHERE (skipped) | 1 | 1 | `files/IfcCsgPrimitive.ifc` |
| unsupported profile IFCASYMMETRICISHAPEPROFILEDEF (skipped) | 1 | 1 | `files/IfcIShapeProfileDef.ifc` |

### Unsupported entity types (the to-do list), ranked by files

| kind | IFC type | files | occurrences | corpora |
|---|---|---|---|---|
| geometry | IFCADVANCEDBREP | 9 | 2119 | 4 |
| placement | IFCLINEARPLACEMENT | 8 | 6252 | 4 |
| geometry | IFCSECTIONEDSOLIDHORIZONTAL | 5 | 697 | 3 |
| geometry | IFCREVOLVEDAREASOLID | 5 | 12 | 3 |
| geometry | IFCGEOMETRICSET | 4 | 243 | 2 |
| geometry | IFCINDEXEDPOLYCURVE | 3 | 715 | 1 |
| profile | IFCLSHAPEPROFILEDEF | 3 | 112 | 2 |
| profile | IFCTSHAPEPROFILEDEF | 3 | 22 | 2 |
| geometry | IFCFIXEDREFERENCESWEPTAREASOLID | 3 | 5 | 3 |
| geometry | IFCCSGSOLID | 3 | 3 | 2 |
| profile | IFCCOMPOSITEPROFILEDEF | 3 | 3 | 2 |
| profile | IFCROUNDEDRECTANGLEPROFILEDEF | 2 | 25 | 2 |
| geometry | IFCSURFACECURVESWEPTAREASOLID | 2 | 8 | 2 |
| profile | IFCELLIPSEPROFILEDEF | 2 | 6 | 1 |
| placement | IFCGRIDPLACEMENT | 1 | 25 | 1 |
| geometry | IFCPOLYLINE | 1 | 25 | 1 |
| geometry | IFCTEXTLITERALWITHEXTENT | 1 | 5 | 1 |
| profile | IFCUSHAPEPROFILEDEF | 1 | 4 | 1 |
| profile | IFCTRAPEZIUMPROFILEDEF | 1 | 3 | 1 |
| profile | IFCCENTERLINEPROFILEDEF | 1 | 2 | 1 |
| profile | IFCCSHAPEPROFILEDEF | 1 | 2 | 1 |
| profile | IFCZSHAPEPROFILEDEF | 1 | 2 | 1 |
| face loop | IFCEDGELOOP | 1 | 2 | 1 |
| geometry | IFCINCLINEDREFERENCESWEPTAREASOLID | 1 | 1 | 1 |
| geometry | IFCBLOCK | 1 | 1 | 1 |
| geometry | IFCSPHERE | 1 | 1 | 1 |
| profile | IFCASYMMETRICISHAPEPROFILEDEF | 1 | 1 | 1 |
| geometry | IFCEXTRUDEDAREASOLIDTAPERED | 1 | 1 | 1 |

### Crashes, timeouts, memory, sanitizer reports

None.

### Refusals (C), by message

| message | files | example |
|---|---|---|

### Timings (read call, ms; wall in s includes process start and the dump)

- c: 617 files, read total 19.0 s, wall total 38.7 s
- rust: 617 files, read total 20.1 s, wall total 45.4 s

Slowest 10 (C read call):

| file | MB | schema | C read ms | Rust read ms | C peak RSS MB | elements | triangles |
|---|---|---|---|---|---|---|---|
| `engine_web-ifc/tests/ifcfiles/public/ISSUE_098_R8_F1_MAB_AR_M3_XX_XXX_MO_7000.ifc` | 72.9 | IFC2X3 | 1947.5 | 2760.9 | 265 | 11124 | 1609354 |
| `files/468--reinforcingBar--segfault--augmented.ifc` | 0.1 | IFC2X3 | 1432.7 | 373.3 | 248 | 1 | 496132 |
| `bim-whale-ifc-samples/BasicHouse/IFC/BasicHouse.ifc` | 52.7 | IFC2X3 | 956.9 | 642.6 | 324 | 125 | 621127 |
| `engine_web-ifc/tests/ifcfiles/public/ISSUE_068_ARK_NUS_skolebygg.ifc` | 57.3 | IFC2X3 | 846.1 | 1283.2 | 190 | 4459 | 1403447 |
| `files/424--reinforcingBar--segfault--augmented.ifc` | 0.0 | IFC2X3 | 828.2 | 190.6 | 141 | 1 | 283472 |
| `IfcSampleFiles/Ifc4_Revit_STR.ifc` | 11.3 | IFC4 | 795.3 | 393.5 | 151 | 4133 | 1411324 |
| `engine_web-ifc/tests/ifcfiles/public/ISSUE_102_M3D-CON-CD.ifc` | 27.4 | IFC2X3 | 794.2 | 671.6 | 292 | 1616 | 2288290 |
| `files/706--buildingElementProxy--segfault--accept-errors.ifc` | 36.3 | IFC2X3 | 520.1 | 1070.7 | 247 | 1 | 248723 |
| `engine_web-ifc/tests/ifcfiles/public/advanced_model.ifc` | 35.3 | IFC2X3 | 353.4 | 273.2 | 138 | 6401 | 1322307 |
| `ifc-sample-files/LTU_A-House/LTU_A-House_Air.ifc` | 25.4 | IFC2X3 | 331.7 | 342.5 | 156 | 16570 | 959322 |

### IfcOpenShell vs C: products with geometry (sample)

| file | schema | IfcOpenShell products with geometry | C drawn | C products with a body | IfcOpenShell s |
|---|---|---|---|---|---|
| `files/1032-curve.ifc` | IFC2X3 | 1 | 1 | 1 | 0.25 |
| `files/678--wall--segfault--augmented.ifc` | IFC2X3 | 1 | 1 | 1 | 0.27 |
| `files/IfcLShapeProfileDef.ifc` | IFC2X3 | 4 | 0 | 4 | 0.22 |
| `files/561--wall--segfault--augmented.ifc` | IFC2X3 | 1 | 1 | 1 | 0.24 |
| `files/trimmed_polyline.ifc` | IFC2X3 | 5 | 5 | 5 | 0.36 |
| `files/726--reinforcingBar--segfault--1--augmented.ifc` | IFC2X3 | 1 | 1 | 1 | 0.24 |
| `files/395--beam--segfault--2--augmented.ifc` | IFC2X3 | 1 | 1 | 1 | 0.23 |
| `files/336--beam--segfault--2--augmented.ifc` | IFC2X3 | 1 | 1 | 1 | 0.25 |
| `files/654--wrong-layerset-slicing--augmented.ifc` | IFC2X3 | 2 | 2 | 2 | 0.23 |
| `files/341--slab--segfault--augmented.ifc` | IFC2X3 | 1 | 1 | 1 | 0.21 |
| `files/726--reinforcingBar--segfault--16--augmented.ifc` | IFC2X3 | 1 | 1 | 1 | 0.26 |
| `files/331--slab--segfault--augmented.ifc` | IFC2X3 | 1 | 1 | 1 | 0.38 |
| `files/786--reinforcingBar--segfault--augmented.ifc` | IFC2X3 | 1 | 1 | 1 | 0.24 |
| `files/702--slab--segfault--augmented.ifc` | IFC2X3 | 1 | 1 | 1 | 1.41 |
| `files/288--wall--segfault--augmented.ifc` | IFC2X3 | 1 | 1 | 1 | 0.19 |
| `engine_web-ifc/tests/ifcfiles/public/Sample_entities.ifc` | IFC2X3 | 2 | 2 | 2 | 0.22 |
| `files/599--beam--segfault.ifc` | IFC2X3 | 1 | 0 | 1 | 2.98 |
| `files/468--reinforcingBar--segfault--augmented.ifc` | IFC2X3 | 1 | 1 | 1 | 1.11 |
| `files/588--buildingElementPart--timeout--augmented.ifc` | IFC2X3 | 1 | 0 | 1 | 0.31 |
| `files/994--slab--segfault--augmented.ifc` | IFC2X3 | 1 | 1 | 1 | 2.21 |
| `IFC-files/ifcopenshell/IfcTextureTest.ifc` | IFC2X3 | 1 | 1 | 1 | 0.22 |
| `files/377--slab--segfault--accept-errors.ifc` | IFC2X3 | 1 | 1 | 1 | 1.39 |
| `IfcSampleFiles/Ifc2s3_Duplex_Electrical.ifc` | IFC2X3 | 103 | 103 | 103 | 0.75 |
| `IFC-files/HHS Office/construction2.ifc` | IFC2X3 | 339 | 339 | 339 | 1.08 |
| `engine_web-ifc/tests/ifcfiles/public/dental_clinic.ifc` | IFC2X3 | 2586 | 2586 | 2586 | 3.23 |
| `engine_web-ifc/tests/ifcfiles/public/ISSUE_102_M3D-CON-CD.ifc` | IFC2X3 | 1616 | 1616 | 1616 | 109.56 |
| `engine_web-ifc/tests/ifcfiles/public/ISSUE_068_ARK_NUS_skolebygg.ifc` | IFC2X3 | 4459 | 4459 | 4459 | 15.51 |
| `files/5473--pipeSegment--wrong-geometry.ifc` | IFC4 | 1 | 1 | 1 | 0.64 |
| `files/basic_shape_SurfaceModel.ifc` | IFC4 | 1 | 1 | 1 | 0.22 |
| `Certification-datasets/IFC 4.0.2.1 (IFC 4 ADD2 TC1)/ISO Spec - ReferenceView_V1.2/basin-tessellation.ifc` | IFC4 | 1 | 1 | 1 | 0.24 |
| `IfcSampleFiles/Ifc4_BasinFacetedBrep.ifc` | IFC4 | 1 | 1 | 1 | 0.29 |
| `IFC-infra-unit-test/RumbleStrip-INDOT/Twin-Branch-Surface-Features.ifc` | IFC4 | 0 | 4 | 4 | 0.21 |
| `files/338--buildingElementProxy--segfault--accept-errors.ifc` | IFC4 | 1 | 0 | 1 | 3.15 |
| `files/695--boiler--timeout.ifc_` | IFC4 | 1 | 1 | 1 | 16.83 |
| `IFC-infra-unit-test/SectionedSurface-INDOT/PR_Twin_Branch_SectionedSurface.ifc` | IFC4 | 0 | 368 | 368 | 0.73 |
| `IFC4.x-specification-models/models/tessellated-shape-with-style/tessellation-with-individual-colors/tessellation-with-individual-colors.ifc` | IFC4X3 | 1 | 1 | 1 | 0.22 |
| `IFC4.x-specification-models/models/building-elements/beam-varying-extrusion-paths/beam-varying-extrusion-paths.ifc` | IFC4X3 | 2 | 1 | 2 | 0.22 |
| `IFC4.x-specification-models/models/mapped-geometric-shape/mapped-shape-with-transformation/mapped-shape-with-transformation.ifc` | IFC4X3 | 1 | 1 | 1 | 0.22 |
| `IFC4.x-specification-models/models/advanced-geometric-shape/basin-faceted-brep/basin-faceted-brep.ifc` | IFC4X3 | 1 | 1 | 1 | 0.3 |
| `engine_web-ifc/tests/ifcfiles/public/Viadotto Acerno.ifc` | IFC4X3 | 6534 | 5900 | 6585 | 26.68 |

Sum over 40 files: IfcOpenShell 15679, C 15409; files where C draws fewer: 6, more: 2, equal: 32.
