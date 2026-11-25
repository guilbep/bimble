# C core for the Vue lab: spec and tickets

25 Nov 2026. Study only: no code was built or run. Every claim cites `file:line` in this worktree
(branch `rewrite/c-core`). **Inference** marks what was not read in code.
Short paths: `core/` = `crates/bimble-core/src/`, `api/` = `crates/bimble-api/src/`,
`geom/` = `crates/bimble-geom/src/`, `ifc/` = `crates/bimble-ifc/src/`,
`viewer/` = `crates/bimble-viewer/src/`, `ts` = `web-ui/src/bimble.ts`.

Scope: a C core behind the unchanged `Wasm` interface (`ts:200-213`), for the lab only. No egui, PDF,
map projections, sheets, schedules, collaboration. Rust stays the oracle.

## 1. Minimum lab surface

Transport (`viewer/serve.rs:188-298`): `POST /<op>/<name|->?t=<token>`, body = JSON text; 200 = the
answer, 400 = error text the lab throws (`ts:284`). `GET /change?since=&epoch=&t=` answers
`{"epoch","seq","change"}` (`serve.rs:235-255`). 403 on a bad token, `Origin` or `Host`
(`serve.rs:206-223`); body over 1 MiB is 413 (`serve.rs:14`, `:225-227`). Ops: `viewer/calls.rs:27-41`.

After each change the lab runs `state`, `actions`, `history`, `visibility`, `open_views`, `status`,
`placing`, and, when `rev` or `file` changed, `storeys`, `materials`, `project` (`ts:528-530`,
`ts:636-646`). It polls `pointer` (`ts:566`) and `status` (`ts:536`). The first refresh counts only
when `state.file` is not null **and** `actions` is not empty (`ts:648`).

Every query through `tryQuery` falls back on an error (`ts:624-630`), so a 400 "unknown query" is
a valid stub for those. "Stub" below is the explicit value to return instead.

| Call | Shape (Rust producer) | C core |
|---|---|---|
| `/change` payload | `{selection[], rev, unsaved, file, view[8], tab, placing}` (`calls.rs:134-167`) | Real. `view` = eight zeros but the tab count. Publish once at start: the twin starts at `(0,"null")` (`viewer/twin.rs:120`) and the lab ignores null (`ts:325`) |
| `query state` | `{file, editable, unsaved, schema, version, user}` (`calls.rs:239-258`) | Real; `user: null` |
| `query actions` | `[{name,label,tab,enabled,pressed,shortcut}]`, 54 entries (`calls.rs:325-339`, `viewer/ribbon.rs:130`) | Fixed table of the 54 names; enabled: `save`, `undo`, `redo`, `place_*`, `export_ifc4`, `home`. Others `enabled:false` |
| `query history` | `{undo[],redo[]}`, newest first, max 50 (`calls.rs:346-359`) | Real |
| `query visibility` | `calls.rs:442-460` | Stub: four `{ids:[],count:0}`, `section_box:{on:false,box:null,origin_mm}` |
| `query open_views` | `{views,active,tabs,current,paper}` (`calls.rs:362-397`) | One 3D view, id `derived("bimble view 3d")` (`core/view.rs:584`), always current. No plans |
| `query status` | `Status` (`viewer/lib.rs:4712-4725`) | Real counts; `fps:null, idle:true, app_bytes:0` |
| `query placing` | `Editor::placing_json` (`viewer/edit.rs:387-408`) | Real (needed for c) |
| `query storeys` / `materials` | `api/query.rs:76-104` | Real |
| `query project` | `api/query.rs:195-204` | Real (needs blake3 for `site_id`, `building_id`) |
| `query elements` | filter `{class,storey,limit}`, limit 1000, cap 10 000 (`api/query.rs:12-14`, `118-127`) | Real |
| `query element` | `ElementDetail` (`api/query.rs:54-65`, `130-149`) | Real |
| `query pointer` | `{world_mm,text}` or null (`calls.rs:403-406`) | Stub `null` |
| `collab`, `recent_files`, `links`, `measurements`, `schedule_list`, `sheets`, `views`, `schedules`, `standards`, `recent_actions` | `calls.rs:261-441` | Stub: `[]`; `collab` = the lab's `NO_COLLAB` (`ts:747`); `standards` = `null` |
| `palette_match` | `calls.rs:313-321` | Cut: 400 (the palette shows no filter result) |
| `select` | body `[ids]`, unknown id = error (`calls.rs:550-572`) | Real |
| `apply` | body `[Command]` (`calls.rs:84-88`); the lab sends `rename`, `set_project` (`ts:588`, `ts:598`) | Real |
| `undo`, `redo` | `calls.rs:89-93` | Real |
| `verb` | 14 verbs (`api/dispatch.rs:25-77`, `405-447`) | `place`, `move`, `delete`, `set_generator`, `set_material`, `set_note`. Others: 400 "not in the C core" |
| `set_placing` | patch `{kind,storey,material,sizes,…}` (`calls.rs:190-202`, `edit.rs:413`) | Real |
| `action` | by name, error when not enabled (`calls.rs:538-547`) | `save`, `undo`, `redo`, `place_<kind>`, `export_ifc4`, `home` |
| `tab` | `calls.rs:205-232` | Accept the 3D tab; else error |
| `dismiss_belongs`, `standards_action` | `calls.rs:108-118` | No-op 200 |

Per flow: (a) boot = `/change`, `state`, `actions` and the stubs. (b) open = the same plus
`storeys`, `materials`, `project`, `elements`, `element`, `select`, the mesh (§2). The model is the
host's argument, as the twin's (`web-ui/e2e/native-place.mjs:24`); `open` is disabled.
(c) = `placing`, `set_placing`, `verb place`, `undo`, `redo`, `history`. (d) = `action save`,
`action export_ifc4`.

Two deviations to accept. An IFC opened by Rust is a read-only scene (`viewer/lib.rs:494-498`,
`calls.rs:65-71`); the C core always imports it (the ADR 0032 path, `viewer/lib.rs:526-538`), so
`editable` is true. `version` is the C core's own.

## 2. Viewport

Today's native mode draws RGBA frames from `GET /frame` and posts `orbit`, `pan`, `zoom`
(`ts:353-483`, `viewer/twin.rs:263-323`). It sends no click, so there is no picking in that mode.
A C host that answered `/frame` would need a software rasteriser. Rejected.

Recommended: mesh buffers drawn in the page by WebGL2, as `spikes/c-slab/web/glue.js` does (flat
normal from `dFdx/dFdy`, `glue.js:33`; the same idea as Rust, `crates/bimble-render/src/lib.rs:33-39`).
This needs one additive branch in `start()` (`ts:497-500`): with `&gl=1`, call a new
`glViewport(base, token)` (new file `web-ui/src/glviewport.ts`) instead of `streamViewport`. The
`Wasm` interface does not change. Until that ticket lands the host answers 500 on `/frame`, as the
default handler does (`serve.rs:40`); the lab retries quietly (`ts:466-470`).

`GET /mesh?t=` → `application/octet-stream`, little-endian, header `X-Rev`. Fetched when `rev`
changes.

| Block | Content |
|---|---|
| Header | `"BMSH"`, u32 version, u32 rev, u32 nE, nV, nI, nS; f64[3] `origin_mm` (`geom/scene.rs:211`) |
| Element table, nE rows, model order (sorted by GlobalId, `scene.rs:267`) | char[22] GlobalId, u8 `MaterialCategory` index (`core/model.rs:49-58`), u8 pad; u32 first vertex, vertex count, first index, index count, first segment, segment count |
| Positions | f32[3] × nV, mm relative to the origin (`geom/lib.rs:26-30`) |
| Indices | u32 × nI |
| Edges | f32[6] × nS, then i8[8] × nS face normals (`geom/lib.rs:31-37`) |

- Picking: in JS, ray against triangles; the element is the table row whose index range holds the
  triangle. Rust does a CPU ray pick giving an element index (`render/src/lib.rs:3118-3125`). Then
  `select([id])` through the existing call.
- Placing by click: JS intersects the ray with the storey plane and calls `verb place`
  (`api/dispatch.rs:206-219`), as `native-place.mjs:98-104` does.
- Edges: crease edges at 20° (`geom/lib.rs:282`, `scene.rs:584`), drawn as `gl.LINES`. The
  hidden-edge test with normals is only needed under a section box: skip it.
- Palette, sRGB (`core/style.rs:335-410`, `:302`): background 247,247,245; edges 26,26,28; selection
  215,172,182; ambient 0.62; fills concrete 214,214,210, steel 150,170,196, timber 226,196,150,
  masonry 206,150,128, insulation 236,220,120, earth 176,160,132, glass 176,214,226, other
  238,238,236. Colour is by material family, not by IFC class. No hatches.

## 3. Data model in C

Structs mirror `core/model.rs`, in one arena per model.

| C struct | Fields (Rust) |
|---|---|
| `GlobalId` | `char[22]`; order = `memcmp` (derived `Ord` on `[u8;22]`, `core/guid.rs:8-10`). First char in `0123` (`guid.rs:68`) |
| `Storey` | id, name, elevation mm (`model.rs:188-193`) |
| `Material` | id, name, category, props: three optional f64 and a source (`model.rs:173-185`, `core/library.rs:16-29`) |
| `Placement` | origin, z_axis, x_axis; unit and perpendicular within 1e-6 (`model.rs:198-223`) |
| `Generator` | tagged union: rect_column, rect_beam, slab, wall, pad_footing, extruded_polygon, mesh (`model.rs:256-299`); validation `model.rs:395-487` |
| `Element` | id, class, name, storey, material, placement, generator, properties (sorted key/value), `ifc_entity` (`model.rs:502-523`) |
| `Model` | project_id, project_name, storeys, materials, elements (arrays sorted by id), project map (`model.rs:547-568`) |

A file with `openings`, `views`, `sheets`, `schedules` or `georef` is refused with a clear error:
no silent loss.

**GlobalId.** `new()` is a random UUID v4 (`guid.rs:14-16`): C uses `getrandom`, or a counter when
`CCORE_SEED` is set (tests). `derived()` is the first 16 bytes of blake3 (`guid.rs:19-24`): vendor
the portable blake3 C file. Ids are never regenerated on edit.

**Commands** (`core/command.rs:10-70`), first: `add_element`, `remove_element`, `set_generator`,
`set_placement`, `rename`, `set_element_material`, `set_property`, `add_material`,
`remove_material`, `add_storey`, `remove_storey`, `set_project`. Each validates, makes the value
canonical, mutates and returns its inverse (`command.rs:156-259`, `347-351`). `rename` and
`set_property` have user-input checks that inverses skip (`command.rs:139-152`).

**History** (`command.rs:489-563`): `apply(cmds)` is one transaction; on an error it applies the
inverses in reverse and keeps nothing (`:499-509`). It pushes a step (label from `describe_group`,
`:465`, and the reversed inverses) and clears redo (`:511-516`). `undo`/`redo` pop one step, apply
it, and push the reversed result on the other stack (`:548-563`).

**Canonical JSON.** "Sorted keys" is only half true in Rust:
- Struct fields are written in declaration order (serde derive), e.g. `ElementDetail`
  (`api/query.rs:54-65`). Tags come first: `"kind"` (`model.rs:254-255`), `"op"` (`command.rs:8-9`).
- Maps are `BTreeMap`: keys in byte order (`model.rs:550-552`).
- `serde_json::json!` objects and `generator_params` are sorted by key, because `preserve_order` is
  off (`Cargo.toml:22`). So `change_json` keys are alphabetical.
- Empty optional fields are left out (`model.rs:513-522`, `553-567`).

So the C writer emits keys in the order its caller gives, and each producer copies the order of its
Rust twin. The golden catches a mistake.

**Floats.** serde_json prints f64 with `zmij` (`Cargo.lock:3635-3644`): shortest text that round
trips. **Inference** (to pin in ticket 1's golden): `300.0`, `0.1`, `1e-300`,
`2535844157.0496545` (values from `core/lib.rs:69`), exponent form outside about 1e-5..1e16. In
C: vendor Ryu's `d2d` (digits and exponent, 64-bit-only mode so tcc compiles it) and write two
formatters on top: the serde_json one, and Rust's `{:?}` one that the IFC writer uses
(`ifc/lib.rs:26-41`). A `%.17g` loop is not enough: it is wrong at binade edges and absent in
freestanding wasm. `-0.0`: Rust rewrites it to `0.0` when a command is applied and on load
(`core/version.rs:13-25`, `core/file.rs:94`); C does `x + 0.0` on every double at the same two
points (as `version.rs:54`). Reading: `strtod` natively (correctly rounded in glibc).

**Save.** Rust `save` writes gzip of CBOR (`core/file.rs:99-127`): equal bytes are out of reach.
The C core writes the JSON text form, version 1 (`file.rs:68-77`, pretty, 2 spaces), which Rust
still reads (`file.rs:79-95`, test `core/lib.rs:106`).

## 4. Golden strategy

One Rust example, `crates/bimble-api/examples/ccore_golden.rs` (this crate already dev-depends on
geom and ifc, `crates/bimble-api/Cargo.toml`; no wgpu). Pattern: `crates/bimble-geom/examples/slab_dump.rs`.
It is run once per slice and its output is committed under `ccore/golden/`. Only one builder runs
cargo at a time (16 GB).

| Slice | Rust output | C check |
|---|---|---|
| Floats | 2 000 f64 (edge cases and random bits), as serde_json and as `{:?}` | `cmp` |
| Model file | `file::to_string(concrete_frame)` → `frame.json` (ids are random, `core/demo.rs:45`: generated once, committed) | load, save, `cmp` |
| Meshes | per element of `Scene::from_model`: `v/i/e/n` hex lines, format of `spikes/c-slab/expected/README.md` | `cmp` |
| Queries, verbs | a transcript `op name body` → `status body`, through `dispatch::query`, `dispatch::run` and `History` | `cmp` after masking |
| IFC | `export(frame, "frame.ifc", fixed timestamp)`; `import_str` of it → `file::to_string` | `cmp` both ways |
| Viewer-level calls | recorded once from the headless Rust twin (`BIMBLE_SERVE`, `BIMBLE_HEADLESS=1`, `viewer/twin.rs:176-179`) with a Node script | `cmp` after masking |

Masking: a GlobalId absent from the fixture becomes `<new-N>` by first appearance, on both sides.
Volatile fields (`load_ms`, `text`, `version`, `epoch`, byte counts) are dropped.

Fixtures, all synthetic: the demo frame (72 elements, `core/lib.rs:41-46`); the six files of
`crates/bimble-ifc/tests/fixtures/` with their STEP dumps on `spike/compile-speed`
(`spikes/compile-speed/corpus/expected/`); the slab case of `spikes/c-slab/expected/`.

## 5. Tickets

C lives in `ccore/` (`src/`, `host/`, `vendor/`, `test/`, `golden/`, `build.sh`). Each done check
exits 0. "Read" lists what the builder reads first.

| # | Title | Owns | Read | Done check | Needs |
|---|---|---|---|---|---|
| 1 | Skeleton: `build.sh tcc\|native\|wasm`, test runner, arena, buffer, JSON writer, two float formatters (vendored Ryu) | `ccore/build.sh`, `src/{arena,buf,json_w,fmt}.c`, `vendor/ryu`, `test/run.sh` | `spikes/c-slab/build.sh`, `ifc/lib.rs:26-41` | `ccore/build.sh tcc && ccore/test/run.sh fmt json` (30 hand-checked floats) | — |
| 2 | Golden dumper in Rust, all slices of §4 except the twin recording | `crates/bimble-api/examples/ccore_golden.rs`, `ccore/golden/` | `slab_dump.rs`, `api/dispatch.rs` | `cargo run -q -p bimble-api --example ccore_golden -- --check` | — |
| 3 | JSON reader, model structs, validation, load and save of the text form | `src/{json_r,model,file}.c` | `core/model.rs`, `core/file.rs` | `ccore/test/run.sh file` (`cmp` with `frame.json`; bad files give an error, no crash) | 1, 2 |
| 4 | GlobalId: parse, random, blake3 `derived` | `src/guid.c`, `vendor/blake3` | `core/guid.rs` | `ccore/test/run.sh guid` (vectors from 2) | 1, 2 |
| 5 | Geometry: four rectangle generators, footing, placement matrix, scene origin, weld, crease edges | `src/geom.c` (from `spikes/c-slab/slab.c`) | `geom/lib.rs:144-290`, `geom/scene.rs:266-360`, `:409`, `:584` | `ccore/test/run.sh mesh` (72 elements, `cmp`) | 3 |
| 6 | Queries: storeys, materials, elements, element, project | `src/query.c` | `api/query.rs` | `ccore/test/run.sh query` | 3, 4 |
| 7 | Commands, History, labels; verbs place, move, delete, set_generator, set_material, set_note | `src/{command,history,verbs}.c` | `core/command.rs`, `api/verbs.rs:101-290`, `:386-440` | `ccore/test/run.sh verbs` (transcript; undo restores the file bytes) | 3, 4 |
| 8 | STEP parser and import of the subset Bimble exports (extruded rectangle and closed profile, `ifc/lib.rs:193-237`); anything else is an error line in the report | `src/{step,ifc_in}.c` (from `step_dump.c`) | `ifc/step.rs`, `ifc/import.rs:92-476`, `:697-960` | `ccore/test/run.sh step ifc_in` | 3, 4 |
| 9 | Native host: HTTP as `serve.rs`, change feed with epoch, selection, rev, `state`, `actions`, `history`, `placing`, `set_placing`, `open_views`, `status`, the stubs of §1 | `ccore/host/*.c`, `ccore/tools/record_twin.mjs` | `viewer/serve.rs`, `calls.rs`, `twin.rs`, `edit.rs:387-460` | `ccore/test/host.sh`: starts the host on `frame.bimble`, runs `node web-ui/e2e/native-place.mjs`, the 403/413 cases, and the recorded transcript | 6, 7 |
| 10 | Save and IFC4 export; actions `save`, `export_ifc4` | `src/ifc_out.c`, host actions | `ifc/lib.rs:279-687` | `ccore/test/run.sh ifc_out` (`cmp` with `frame.ifc`) | 3, 4; actions need 9 |
| 11 | `/mesh` and the WebGL2 viewport in the lab: orbit, pan, zoom, pick → `select`, click → `verb place` | host `/mesh`, `web-ui/src/glviewport.ts`, the branch in `bimble.ts` | `spikes/c-slab/web/glue.js`, `test/cdp.mjs`, `ts:353-500` | `node ccore/test/lab_pixels.mjs`: lab at `?native=1&gl=1` shows concrete grey; a click selects; `place` adds pixels | 5, 9 |
| 12 | Edit → pixels in the lab: edit a constant in `verbs.c`, `build.sh tcc`, restart the host on the same port and token, first changed frame. The host keeps a session file so the model and selection survive the restart | `ccore/bench/`, host session file, `ccore/results/bench.md` | `spikes/c-slab/bench/pixels_loop.mjs`, `ts:305-331` | `RUNS=10 ccore/bench/lab_loop.sh` writes medians and p95 | 11 |
| 13 | wasm32 build of the core (freestanding, libm shims), same goldens through Node | `src/wasm_main.c`, `test/wasm_run.mjs` | `spikes/c-slab/main_wasm.c` | `ccore/build.sh wasm && node ccore/test/wasm_run.mjs` | 5, 7 |
| 14 | ADR (Proposed, for Pierre) and journal entry with the numbers | `docs/adr/0067-*.md`, `docs/journal/` | `CLAUDE.md`, ADR 0066 | `test -f` both files | 12 |

Parallel waves: {1, 2} → {3, 4} → {5, 6, 7, 8} → {9, 10} → {11, 13} → 12 → 14.
Ticket 2's owner is the only one who runs cargo; a later slice that needs a new golden asks them.

## 6. Risks, and what to cut first

- **Float text.** Two formats, both must be exact. Ticket 1 carries it; if Ryu does not build under
  tcc, tcc loses its place in the loop. Check this first.
- **libm bits.** `hypot` in `place` (`api/verbs.rs:196`), `cosf` in crease edges: glibc matches
  natively (**inference**, as the slab spike found for `cosf`); freestanding wasm needs shims
  (`spikes/c-slab/main_wasm.c:28-35`). This is why ticket 13 is late.
- **Earcut.** The spike triangulates convex quads only (`spikes/c-slab/slab.c:40`). The demo and the
  four placed kinds are rectangles. A polygon slab or an `extruded_polygon` is an error until a
  full earcut port.
- **Restart loses state.** tcc gives 12 ms builds (`spikes/c-slab/results/bench.md`) but a new
  process; without the session file of ticket 12, each edit loses the undo stack.
- **The viewer-level goldens need the Rust twin built** (heavy). If it is not built, compare
  those calls by key set and types only.
- **Scope creep in `actions`**: 54 entries; a wrong `enabled` shows as a dead button, not a crash.
- **Sync XHR** to a single-threaded C host: `/change` long-polls while calls arrive
  (`ts:275-286`, `ts:312`). The host needs `poll()` over several sockets, not one blocking accept.

Cut order when time runs short: 13 (wasm), then 10's IFC export (keep save), then 8 (open only
`.bimble` text files), then picking by click in 11 (select from the navigator still works,
`web-ui/src/shell/Navigator.vue:118`). Never cut 1, 2, 9 or 12: they are the experiment.
