# Size of the wasm builds (ticket W1)

ADR 0069 rule 3: a viewer wasm of at most 400 KB, selected by files. `build.sh wasm-view` makes
`out/wasm/ccore-view.wasm` and fails over 400,000 bytes. `build.sh wasm` (the editor) is unchanged.

## Measured

25 Nov 2026, 16 GB aarch64, clang 23.1.3, no wasm-opt. Bytes of the file; gzip is `gzip -9`.

| Build | Files | Opt | Raw bytes | Gzip bytes |
|---|---|---|---:|---:|
| `wasm` (editor), before W1 | all | -O2 | 585,204 | 243,338 |
| `wasm` (editor), after W1 | all | -O2 | 585,323 | 243,536 |
| view, -O2 | without `VIEW_DENY` | -O2 | 439,691 | 190,774 |
| **`wasm-view`** (kept) | without `VIEW_DENY` | **-Os** | **344,455** | **166,318** |
| view, -Oz | without `VIEW_DENY` | -Oz | 321,489 | 160,332 |
| editor, -Os (for scale) | all | -Os | 455,960 | 209,926 |
| editor, -Oz (for scale) | all | -Oz | 419,985 | 199,563 |

The editor grew by 119 bytes: `session/view_only.c` (the guard, empty outside the view build) and
`src/material_class.c` (moved out of `ifc_in.c`, so the display reader links without the import).

`VIEW_DENY` (build.sh): `src/verbs.c`, `src/verb_table.c`, `src/ifc_in.c` (the IFC import),
`src/ifc_out.c` (the IFC writer). At -O2 that cut is 146 KB, not enough: -Os gives the other 95 KB.
The kept files still reference a few denied symbols; `wasm_stub.c` defines them under
`CC_VIEW_ONLY` (an empty verb table, refusals). `session/view_only.c` answers "this build is
view-only" to the calls that need them, before they run.

### Speed of -Os

`test/wasm_view_run.mjs`, suite `time`: a synthetic model (frame.json and 3000 elements placed
with the editor's verbs), opened with `host_open` and meshed with `host_mesh`, mean of 5 after one
warm-up, Node 26. One run:

| File | Bytes | Editor (-O2) ms | View (-Os) ms |
|---|---:|---:|---:|
| big.json | 2,054,957 | 81.9 | 102.1 |
| big.ifc (IFC4 export) | 2,928,208 | 101.9 | 120.7 |

The view is 18 to 25 % slower to open. A try with -O2 for the hot files (ifc_read, scene, earcut,
step, strtod, mesh) and -Os for the rest: 376,157 bytes, 7 to 11 % slower than the editor (not kept:
it needs one compile per file instead of one clang call).

## Bytes per object file

`build.sh wasm-size [view]`: the same build in two steps (one object per file, then a link with a
map, `out/wsize/map.txt`), the map summed per object. Code and data (rodata and data, not bss).
`<internal>` is the merged string literals. The sum is less than the file: the rest is the type,
function, export and element sections.

### Editor (`build.sh wasm`, -O2)

full: 585323 bytes, gzip -9 242955 bytes
|---|---:|---:|---:|
| ifc_in.o             |   90865 |   90281 |     584 |
| ifc_read.o           |   87689 |   87121 |     568 |
| calls.o              |   38888 |   37261 |    1627 |
| verbs.o              |   37173 |   35830 |    1343 |
| view.o               |   33977 |   33433 |     544 |
| command.o            |   32817 |   32545 |     272 |
| scene.o              |   29617 |   29337 |     280 |
| ifc_out.o            |   27690 |   27501 |     189 |
| <internal>           |   27329 |       0 |   27329 |
| file.o               |   25512 |   25304 |     208 |
| verb_table.o         |   17693 |    2112 |   15581 |
| guid.o               |   15775 |   15433 |     342 |
| step.o               |   15261 |   15181 |      80 |
| strtod.o             |   14982 |    3810 |   11172 |
| viewfile.o           |   13701 |   13661 |      40 |
| fmt.o                |   13693 |    3005 |   10688 |
| query.o              |   12859 |   12588 |     271 |
| model.o              |   12712 |   12583 |     129 |
| wasm_api.o           |   12227 |   12135 |      92 |
| inflate.o            |   11706 |   10476 |    1230 |
| earcut.o             |   10001 |   10001 |       0 |
| visibility.o         |    9786 |    9774 |      12 |
| tool.o               |    8739 |    8707 |      32 |
| relation.o           |    8625 |    8521 |     104 |
| meshcodec.o          |    8439 |    8439 |       0 |
| json_r.o             |    6902 |    6902 |       0 |
| annot.o              |    6848 |    6792 |      56 |
| boolean.o            |    6410 |    6410 |       0 |
| session.o            |    6361 |    6361 |       0 |
| ifc_math.o           |    6336 |    6336 |       0 |
| views.o              |    6258 |    6242 |      16 |
| cbor.o               |    4428 |    4428 |       0 |
| links.o              |    4235 |    4203 |      32 |
| mesh.o               |    2446 |    2434 |      12 |
| wasm_stub.o          |    1912 |    1912 |       0 |
| history.o            |    1846 |    1846 |       0 |
| json_w.o             |    1813 |    1796 |      17 |
| buf.o                |    1633 |    1633 |       0 |
| material_class.o     |    1554 |    1242 |     312 |
| math.o               |    1430 |    1430 |       0 |
| library.o            |    1210 |     570 |     640 |
| arena.o              |     731 |     731 |       0 |
| jobs.o               |     199 |     199 |       0 |
| platform.o           |     124 |     124 |       0 |
| mesh_model.o         |      67 |      67 |       0 |
| view_only.o          |       5 |       5 |       0 |

### Viewer (`build.sh wasm-view`, -Os)

view: 344455 bytes, gzip -9 166312 bytes
|---|---:|---:|---:|
| ifc_read.o           |   67116 |   66500 |     616 |
| calls.o              |   35002 |   33199 |    1803 |
| command.o            |   26542 |   26246 |     296 |
| view.o               |   23616 |   23052 |     564 |
| <internal>           |   21802 |       0 |   21802 |
| scene.o              |   21411 |   21131 |     280 |
| file.o               |   20669 |   20461 |     208 |
| guid.o               |   14850 |   14508 |     342 |
| strtod.o             |   14671 |    3499 |   11172 |
| fmt.o                |   13519 |    2831 |   10688 |
| step.o               |   12146 |   12066 |      80 |
| query.o              |   10381 |   10046 |     335 |
| viewfile.o           |    9904 |    9864 |      40 |
| earcut.o             |    8316 |    8316 |       0 |
| wasm_api.o           |    7726 |    7634 |      92 |
| visibility.o         |    7617 |    7585 |      32 |
| model.o              |    7559 |    7406 |     153 |
| tool.o               |    7069 |    7037 |      32 |
| relation.o           |    6592 |    6488 |     104 |
| inflate.o            |    6374 |    6168 |     206 |
| meshcodec.o          |    5846 |    5846 |       0 |
| ifc_math.o           |    5789 |    5789 |       0 |
| annot.o              |    5624 |    5568 |      56 |
| views.o              |    5502 |    5454 |      48 |
| session.o            |    5306 |    5306 |       0 |
| boolean.o            |    4692 |    4692 |       0 |
| json_r.o             |    4549 |    4549 |       0 |
| links.o              |    3797 |    3717 |      80 |
| cbor.o               |    3535 |    3535 |       0 |
| mesh.o               |    2429 |    2417 |      12 |
| history.o            |    1703 |    1703 |       0 |
| json_w.o             |    1586 |    1569 |      17 |
| math.o               |    1294 |    1294 |       0 |
| material_class.o     |    1292 |     980 |     312 |
| wasm_stub.o          |    1250 |    1202 |      48 |
| library.o            |    1018 |     378 |     640 |
| buf.o                |     635 |     635 |       0 |
| arena.o              |     530 |     530 |       0 |
| view_only.o          |     246 |     186 |      60 |
| platform.o           |     124 |     124 |       0 |
| mesh_model.o         |      67 |      67 |       0 |
| jobs.o               |      61 |      61 |       0 |

## What the view build leaves out

- Verbs and the verb table: `verb` answers "this build is view-only"; the `verbs` query answers
  `[]` and `describe` "no verb with this name".
- Edits: `apply`, `undo`, `redo`, `set_placing`, the place tool and the annotate tool (`tool`
  pointer, click, cancel, annotate). The section box tool (`box_*`) stays.
- Actions: `new`, `import_ifc`, `convert_open`, `save`, `save_as`, `export_ifc4`,
  `export_ifc2x3`, `share_for_viewing`, `undo`, `redo`, `place_*`. `open` (a `.bimble` or `.json`
  as a model, an IFC or a `.bimbleview` read-only), `close`, `link_model`, `manage_links` and the
  visibility actions stay.
- The handle API: `open_ifc` (the import), `apply`, `verb`, `undo`, `redo`, `save`, `export_ifc`.
  `open_text`, `query`, `history`, `mesh` and `close` stay.

Everything else answers byte for byte as the editor build: `test/wasm_view_run.mjs` compares the
handle queries of the goldens, `mesh`, and on every golden file (`.json`, `.ifc`, `.bimbleview`)
the session's read queries, `select`, the visibility actions, the section box, the plan tabs and
their clip, `host_mesh` and `change_json`.

## The next cuts, if the budget gets tight

- `command.c` (32 KB at -O2): the view keeps it for `cc_str_trim` (view.c) and the history. Moving
  `cc_str_trim` out would let the history and the command parser go.
- `calls.c` keeps the code of the refused calls (37 KB at -O2): the guard refuses them at run time,
  the linker cannot drop them. Splitting the edit calls into their own file would.
- `file.c`'s writer and `viewfile.c`'s writer (save and share are refused).
