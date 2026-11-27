# ADR 0079 — Link IFC and link a folder: a cache beside each IFC, conversions in turn

- Status: Accepted, 26 Nov 2026
- Decided by: Pierre delegated the decision on 26 Nov 2026 ("don't wait for me to validate the
  ADR, just do your best, I'll review once it's implemented"); the choices below by Claude on
  ticket LINKF, to be reviewed by Pierre on the implementation.
- Reversal cost: low to medium (a cache file next to each IFC, readable by any `.bimbleview`
  reader; a new action; a new platform hook set; the `.bimble` format is unchanged)

## Context / Problem

Pierre wants to "create a new bimble edit file" and load all the IFC of a consultant folder "as
'links' equivalent to bimble-view all at the same time": a local folder of 17 IFC files, 4.18 GB in
total, the largest 0.87 GB, the median 71 MB, six over 256 MiB. Six of them were refused at once:
`<name>.ifc: larger than 268435456 bytes`. The session checked every file it opened against
`CC_MAX_FILE_BYTES` (256 MiB), the `.bimble` limit (its JSON DOM), although the STEP reader's own
limit is 64 GiB (Rust `read.rs`). He gave Revit's "Link IFC" as the model: a positioning option, a
pinned link, Manage Links → Reload, and helper files written next to the IFC (`<file>.ifc.RVT`,
the intermediate model, and a conversion report).

## The measurements (native build, one file at a time, ascending size, 26 Nov 2026)

`bimble-ccore link-convert --cache-dir <scratch>` (the link's conversion; nothing written beside
the files), 8 threads, the dev machine (16 GB aarch64, 3 to 7 GB available while two other tickets
ran browsers; each file run only with 3 × its size + 2 GB available, under `ulimit -v`). Peak = the
process's maximum resident memory (getrusage). The 17 conversions add up to 25.3 s.

| # | size | read | peak | peak / byte | elements | triangles (drawn) | shapes | cache |
|---|---|---|---|---|---|---|---|---|
| 1 | 0.20 MB | 11 ms | 5 MB | 26 | 17 | 524 | 14 | 7 kB |
| 2 | 1.56 MB | 34 ms | 8 MB | 5.1 | 511 | 6,132 | 51 | 82 kB |
| 3 | 18.7 MB | 140 ms | 39 MB | 2.1 | 4,528 | 76,392 | 991 | 0.75 MB |
| 4 | 25.3 MB | 268 ms | 131 MB | 5.2 | 783 | 1,102,656 | 56 | 2.9 MB |
| 5 | 43.7 MB | 558 ms | 151 MB | 3.4 | 6,510 | 327,060 | 951 | 2.6 MB |
| 6 | 53.5 MB | 414 ms | 369 MB | 6.9 | 12 | 465,748 | 12 | 4.3 MB |
| 7 | 59.4 MB | 530 ms | 186 MB | 3.1 | 5,965 | 627,224 | 1,138 | 3.7 MB |
| 8 | 60.0 MB | 556 ms | 598 MB | 10.0 | 46 | 888,593 | 20 | 7.5 MB |
| 9 | 71.3 MB | 480 ms | 305 MB | 4.3 | 719 | 1,370,970 | 123 | 2.6 MB |
| 10 | 159 MB | 1.03 s | 704 MB | 4.4 | 8,059 | 1,561,801 | 1,624 | 9.3 MB |
| 11 | 201 MB | 0.78 s | 450 MB | 2.2 | 29,935 | 8,838,551 | 749 | 6.3 MB |
| 12 | 336 MB | 2.10 s | 1.13 GB | 3.4 | 30,295 | 5,430,874 | 1,980 | 21.1 MB |
| 13 | 394 MB | 2.13 s | 1.28 GB | 3.2 | 21,192 | 3,738,159 | 354 | 19.1 MB |
| 14 | 415 MB | 3.10 s | 1.52 GB | 3.7 | 32,766 | 35,798,718 | 1,195 | 19.3 MB |
| 15 | 709 MB | 4.52 s | 2.57 GB | 3.6 | 26,814 | 20,144,585 | 1,160 | 72.2 MB |
| 16 | 758 MB | 3.94 s | 2.23 GB | 2.9 | 48,794 | 20,987,851 | 2,860 | 30.3 MB |
| 17 | 872 MB | 4.74 s | 2.91 GB | 3.3 | 35,243 | 9,094,476 | 1,984 | 29.6 MB |

Files #1 to #14 loaded together from their caches (`link-convert --keep`, every cache matched):
**0.57 s, 473 MB resident, 141,338 elements, 9,258 shapes, 60.2 million drawn triangles**; the 14
caches weigh 96 MB on disk for 1.84 GB of IFC (5 %). BLAKE3 (the portable code the core carries):
756 MB/s on one core here, so a full hash of the 0.87 GB file costs 1.2 s plus reading it, against
0.02 to 0.1 s to load a cache.

## Decisions

1. **A limit per format** (DEVIATIONS 37): an IFC is checked against the STEP reader's limit
   (64 GiB native, 1 GiB in a 32-bit build), a `.bimbleview` against its reader's (8 GiB native, as
   Rust's `file.rs`), a `.bimble` keeps 256 MiB. Alternative: raise `CC_MAX_FILE_BYTES` for all;
   refused: the JSON DOM of a `.bimble` is the reason for that cap.
2. **"Too large for this machine" before memory runs out**: the carrier says how much memory is
   free (`cc_platform.mem_available`, Linux `MemAvailable`; the core stays pure); a read needs
   4 × the file + min(512 MiB, 6 × the file) for an IFC (the table: files over 100 MB peaked at 2.2
   to 3.7 times their size, small ones with heavy meshes up to 10 times), 4 × + 64 MiB otherwise;
   more than is free: refused with the numbers ("reading 0.87 GB needs about 4.0 GB of memory,
   3.1 GB is free"). Alternative: try and let malloc fail; refused: on Linux the OOM killer or swap
   answers first (a session was killed this way on 25 Nov).
3. **The cache beside the IFC, Revit's way**: `<file>.ifc.bimbleview` (the reader's scene as a
   view file, copies shared, not gzipped: read without inflating) and `<file>.ifc.log.txt` (plain
   text: schema, counts per IFC class, products with and without a body, the reader's warnings,
   times, sizes). Alternative: one cache folder per user; kept only as the fallback when the IFC's
   folder cannot be written (`cc_platform.cache_dir`), so the project's folder carries its caches
   to the next machine as Revit's `.RVT` does.
4. **The cache's source record**: a section `SRCE` appended to the view file (skipped by any
   reader that does not know it): the IFC's size, modified time and the BLAKE3 of its first MiB,
   last MiB and size. Alternative: a full BLAKE3; refused: 1.2 s plus a full read of 0.87 GB on every
   open, where the ends catch a re-export (header timestamp, the last entities) for 2 MiB read.
   When the IFC cannot be read, size and time alone decide, and the panel says so.
5. **A cache is untrusted**: read as any `.bimbleview` (bounded), its `SRCE` checked as JSON; a
   damaged or foreign one is read again from the IFC and replaced (atomic write: temp, rename).
6. **Only files of 1 MiB and more get a cache and a thread** (`CC_LINK_CACHE_MIN`): smaller ones
   read in under 40 ms (#1, #2), as fast as a cache; the golden folders stay clean. Alternative: a
   cache for every IFC; refused for those two reasons.
7. **One conversion at a time, the smallest first**, on the carrier's thread (`cc_platform.spawn`;
   the native host ticks the queue every 100 ms of its poll loop and publishes each change on the
   change long-poll). The reader is already multi-threaded inside (`cc_jobs`), and two at once
   would add their peaks. Progress 0 to 100 % from the reader (`cc_ifc_read_progress`: the STEP
   parse to 30 %, then per batch of 1024 elements), cancellable between batches. Alternative: steps
   on the host's thread (as PERF2 opens); refused: a 3 s STEP parse cannot be cut without
   rewriting the parser.
8. **After a conversion the IFC's data is freed**: the session keeps the view file only (the scene
   is read back from the bytes just written). Alternative: keep the read scene; refused: its STEP
   strings and per-element info cost more than the light form.
9. **`link_folder {path, recursive?}`**: every `*.ifc` of the folder (and each `*.bimbleview`
   that is not an IFC's cache), sorted by name, at most 64 links in all; the folder must be under a
   root the host lists (the dialog's path rule). **One undo step** for the whole folder, though a
   link is not in the model's history (DEVIATIONS 29): the session keeps link_folder's own undo and
   redo steps beside the history, undone only when it is the last step. Alternative: links as model
   Commands; refused for now: it would change LNK1's file semantics and the command log.
10. **Positioning and pin (Revit's options)**: each link has an origin rule and a manual offset and
    rotation about the host's Z, applied after the rule: `file` (the default: both georeferenced,
    with a map conversion in the same or an unnamed system, the link goes through the survey
    coordinates, h = R(-a_h)(R(a_l) w + S_l - S_h); else it stays), `origin` (the identity),
    `base_point` (the link's base point, the origin of its IfcBuilding's placement, on the host's,
    its world origin per ADR 0075), `survey_point` (the link's survey point, its world origin for an
    IFC, on the host's survey point). A pin (default pinned: `place` refused while pinned) and a
    colour override (palette 0..11). Saved in the `.bimble` `links` section, each left out at its
    default. The core applies the placement wherever a link's geometry is used (`cc_link_scene`
    answers the placed scene: measure, Box to selection, the measure boxes, paper); the page gets
    the read scene and the transform in the LNKS trailer, version 2 (version 1 when nothing is
    placed or coloured). Alternative: send the placed scene in the trailer; refused: moving a link
    would rebuild its buffers in the page instead of changing one matrix. The base point is the
    building's origin, not Revit's project base point (an IFC has none): said in the panel's
    vocabulary only.
11. **Unload / Load** keep the link and free its memory; **Reload all** reads each link again (its
    cache unless the IFC changed).
12. **Several chosen files in one go** (the owner: "is there a way to select like 4 files at once?"):
    `link_model {paths: [...]}` beside `{path}`: one call, one undo step, the same queue; a path
    already linked is skipped with a note, one outside the dialog's roots (`cc_platform.path_allowed`),
    beyond 64 links or unreadable gets a "refused" row with its reason; the answer lists them. The
    native dialog's Link IFC… selects several files (click, Ctrl/Cmd-click, Shift-click, Ctrl A; a
    checkbox per row under 480 px; "N files, size") and still offers "Link this folder"; several files
    dropped on the page are linked in one call (natively staged, 256 MB each at most: a larger one is
    told to use the dialog). Alternative: `link_folder` taking a list; refused: a folder and a
    selection are different answers of the dialog, and `link_model` already names "link these files".
13. **Responsiveness (ADR 0074)**: the rows of Link this folder showed 392 to 500 ms after the click
    while the host answered in 4 ms: each change the host published during a conversion (its start,
    each 5 %) made the page run its full refresh (18 synchronous queries, a 66 to 93 ms long task) and
    no frame was painted meanwhile. Only a finished conversion now publishes a change; the panel polls
    its rows while one runs. After: 16 to 57 ms (load average 3.4 to 5). The eye's row answers within
    200 ms; the viewport follows when /mesh is fetched again (51 to 131 ms measured): a per-batch
    visibility flag in the draw path is left for after ticket 3D2.

## Consequences

- The six files over 256 MiB are no longer refused for their size. By decision 2, #15 to #17 need
  3.35, 3.55 and 4.0 GB free; they peaked at 2.57, 2.23 and 2.91 GB. With two browsers running
  (3 to 4 GB free) the largest is refused with the numbers, which is the intended answer.
- The whole folder (4.18 GB of IFC): converted one file at a time in 25 s, peak 2.9 GB (#17);
  the 17 caches weigh 232 MB on disk (5.5 % of the IFC). Loaded together: measured for #1 to #14
  (96 MB of caches): 0.57 s, 473 MB resident; for all 17, by the same ratio (not measured: the run
  was not allowed here), about 1.1 GB resident and 1.4 s. The 5 GB case is a conversion-time
  problem (one 0.9 GB file needs about 3 GB once), not a viewing one: once converted, the folder
  is light.
- Open: Windows' `spawn` and `mem_available` (the Windows host converts in the call and does not
  check the free memory); the grid axes and levels of a placed link (snapping, `views` JSON) are
  still the link's own, unplaced.

## Amendment (27 Nov 2026, ticket SIZE2): the cache small again, the estimate refit, the IFC mapped

The owner's folder (17 IFC files, 3.9 GB): the caches beside the IFCs weighed 1.6 GB (41 %; this ADR measured 232 MB,
5.5 %, before M1LINKS added `PROP` and LNK3D `HLOD`). One cache of 441 MB for a 709 MB IFC: 72 MB of geometry and
strings, 227 MB of baked proxies (5.04 M triangles, three times the model's unique triangles they stand for), 142 MB of
property sets, nothing compressed. And "too large for this machine" refused the 17th file with 2.1 GB free. Choices by
Claude on ticket SIZE2 under the same delegation.

14. **Cache format 5** (`bimble-link-cache 5`: older caches are converted again):
    - `HLOD` (ADR 0078 decision 26): one gzip member of the BPRX, within a budget of 15 % of the geometry section (`ELEM`)
      bounded by 256 KiB and 32 MiB, the coarsest cells kept, 4,096 triangles at most a cell. Inflated when served, at
      most 64 times its stored size and 64 MiB.
    - `PRPZ` in place of `PROP` (ADR 0081 amendment): each distinct property set once, the elements pointing to set ids,
      the sets' texts in gzip blocks of 64 KiB; still read per element from the file (a few small reads, one block
      inflated to exactly its declared size, at most 1 MiB).
    - The geometry (`ELEM`) stays as it was (the mesh codec, read at open): compressing it would add an inflate of every
      byte to every open, and it is already about a third of raw f32 (ADR 0078 decision 23 serves it as it is).
    - The report (`<file>.ifc.log.txt`) states each section's bytes, the proxies' budget and what it left out, and the
      property sets' bytes as read.
    **Alternative:** compress the whole cache (gzip view file): every open inflates everything, and `HLOD` / `PROP` could
    no longer be read lazily from the file.
15. **The estimate refit** (`ifc_read.h`): 3 × the file + min(640 MiB, 7 × the file) + 32 MiB, above every peak of the
    table above (by 5.7 % for the 60 MB file of heavy meshes, 13 % for the 0.87 GB one: 3.3 GB asked for 2.91 GB taken;
    the old rule asked 4.0 GB); `test/memfit_test.c` holds the table. **Alternative:** a ratio alone (3.5 ×): the small
    files of heavy meshes peaked at 10 times their size.
16. **A refused link** has "Convert now" in the Links panel (the link read again), and the host queues it again once by
    itself when the memory it needs is free (checked every 2 s while nothing converts). **Alternative:** retry on every
    tick: a file that does not fit would be refused again and again.
17. **The native carrier maps the IFC** (`cc_platform.map_file`, mmap read-only, the core gets a pointer and a length as
    before): the text is the page cache's pages, which the kernel can drop and read again, not the heap's. A file whose
    size is a multiple of the page is read as before (no zero byte after it). The 270 MiB synthetic IFC of
    `test/link_large_test.mjs` (283 MB): the conversion's anonymous peak 1.32 → 1.05 GB (getrusage's peak counts the
    mapped pages too: 1.32 GB both). Freeing the STEP entities batch by batch is not done: they are most of what is left.
18. Not done: freeing other links' detail before a large conversion (the detail is the page's, in another process; the
    host holds only the light scenes).

Synthetic set (17 links, 272 MB of IFC): the caches 55.4 → 20.2 MB, 20.3 % → 7.4 % of the IFC bytes (`ELEM` 9.37 MB,
`HLOD` 8.18 → 1.12 MB, `PROP` 37.8 → `PRPZ` 9.71 MB: the synthetic elements' sets are each their own, so only the
gzip blocks shrink them). A local folder of 17 IFC files (4.18 GB): its caches 1.716 GB (`ELEM` 231 MB, `HLOD` 619 MB,
`PROP` 865 MB) → 281 MB, 6.7 % of the IFC (`ELEM` 231 MB, `HLOD` 14.8 MB, `PRPZ` 34.5 MB); the 17 converted one after
the other in 50 s (the machine loaded by two other measuring agents), loaded together from their caches in 0.93 s.

## Amendment (27 Nov 2026, ticket SIZE3): one copy in the host, a link ready before its proxies, a refused link retried when idle

After SIZE2 the host still held, for the local folder's 17 links from their caches, each cache's geometry bytes and
every decoded mesh: 1.15 GB, as much as Rust's whole app with its renderer. Choices by Claude on ticket SIZE3 under the
same delegation.

19. **A link's cache is read in place** (`cc_viewfile_read_mapped`; the native carrier maps it, `map_file`): the strings
    point into the mapping, each mesh is decoded once into a scratch to check it (the same checks as before; the mapped
    reader and the reader give the same verdict on every prefix and on 3,000 damaged files) and only its counts, its
    place in the file and its box are kept. The mapping goes with the link. A gzipped cache, a carrier without
    `map_file` (the wasm module) or a cache whose size is a multiple of the page are read as before.
20. **`GET /mesh?link=k` is the cache's own bytes**: version 4's rows, shapes and placements from the counts, each block
    the file's BMG1 bytes (`cc_scene_bmsh4_blocks`), byte for byte what encoding the decoded scene gives (checked on a
    heavy view file). Copied once into the answer, freed after it is sent (SIZE2); a scatter-gather send from the mapping
    is left.
21. **A link's meshes are decoded only when a call needs its geometry** (`cc_link_scene`, `cc_link_raw_scene`: a
    measure whose ray passes near the link's box, Box to selection, a plan or a section through the link, PDF, the
    element's volume), into an arena of the link's: the only way to a link's geometry. The calls that need its rows
    only use `cc_link_rows` (the Links panel, `visibility_sets`, `link_draw`, the LNKS heads, ids, texts), where a link
    not decoded shows EMPTY meshes (counts 0, a zeroed shapes array of its own): its counts live in the view file only
    (`cc_link_triangles`, the served blocks), so geometry code handed the wrong scene sees nothing, never a NULL array;
    `cc_scene_world_mesh` and the BMSH writers refuse a mesh whose counts promise arrays it lacks. (A first version kept
    the counts with NULL arrays in the rows: the element query's volume and GET /mesh's version 1 trailer read them and
    the host crashed twice in the owner's dev build; `link_lazy_test` now calls every such entry point on a cold link
    and after a drop, also under ASan.) They are dropped again past a budget of 256 MB of
    decoded meshes across links (`link_mesh_budget`, `BIMBLE_LINK_MESH_BUDGET_MB`), least recently used first, never one
    used in the last 10 s (a measure over every link keeps them while it runs; `BIMBLE_LINK_KEEP_MS` for the tests), at
    the carrier's tick, and the freed memory given back to the system (platform `trim`: glibc's `malloc_trim`).
    Decoding and dropping are logged with their size and time. A call that needs ONE element's geometry (the element
    query's volume, Box to selection around a linked element) decodes that element's block alone (its own mesh or its
    shape, `cc_link_element_scene`), not the link. A link not loaded, or no longer there, answers its mesh and rows
    with 204 (a page's request races its unload or removal). **Alternative:** decode per element on demand (every
    consumer walks the scene's arrays directly: a rewrite of measure, plans, sections and paper).
22. **A link refused for memory is retried when the host is idle too** (`cc_links_wants_tick`): the carrier looked at its
    links only while one was queued or converting, so a refused last link stayed refused (decision 16 did not hold).
    `BIMBLE_MEM_AVAILABLE=@<file>` reads the free memory from a file at each call (`test/link_retry_test.mjs`).
23. **A link is ready before its proxies exist** (ticket SIZE3 E, cache format 6): the conversion writes the cache's
    geometry, names, source and property sets, its SRCE saying `"complete": false`, and the link is drawn; its proxies are
    baked afterwards by a job of their own, once no conversion is queued (the smallest link first): the cache read again
    (untrusted: every section checked), rebuilt as its sections, SRCE marked complete, the proxies (HLOD), then the
    property sets (last: read lazily), written by tmp + rename (a crash leaves the smaller, valid cache, which the next
    open serves and completes), its report appended ("indexed after the link was ready"), the link switched to it (the
    same rows: its placement, colour and selection stay). The Links panel says "shown, indexing…" and Cancel stops it
    (the cache stays incomplete). `query link_draw` gains `proxies`, a counter that moves when a link's proxies arrive:
    the page then asks that link's proxies and makes its batches again by their leaves (ADR 0078 amendment 5).
    **Alternative:** read the property sets later too (they need the IFC's entities: a second read of the file, as long
    as the first); bake the proxies only when the GPU is short, as Rust does (the page's first picture of a large folder
    is its links' proxies). Left: a proxy of a shared shape instanced, levels coarser than 4,096 triangles a cell, 16-bit
    positions in a cell's box (SIZE2's list).

Evidence for decision 23, the local folder converted fresh, the same hour as Rust v0.19.0's read of the same 17 files
(`examples/folder --read-only`, one process a file: 27.8 s of reading, peak 2.38 GB): the 17 links drawn (all ready)
after 35.5 s (load 9.2; 40.9 s at load 7.3 in another run), indexed after 49.7 s (55.9 s); before, nothing was drawn
before the end (50.5 s in the comparison of the day). The rest of the gap to Rust is the property sets' pass and the
cache's write (C's read alone took 28.6 s in that comparison).

Evidence (`link_lazy_test.mjs`, a scratch script reading `/proc/<host>/status`, `frames.mjs`'s folder mode). A local
folder of 17 IFC files, from caches made by the same build, the SIZE2 host and this one the same hour: host at rest
after the 17 links are ready 1,145 MB RSS, 1,141 MB anonymous → 348 MB RSS, 121 MB anonymous (the rest the caches'
file pages, which the kernel may drop); after the page's requests (the lazy mesh, each link's mesh and proxies) 1,178 →
375 MB RSS (1,173 → 148 MB anonymous); in the browser run, after the open, 319 MB RSS, 92 MB anonymous. The slowest
link's mesh served in 890 → 137 ms. A measure over every link decodes all 17 (1.1 GB, 0.86 s; anonymous 1.72 GB at its
peak) and they are dropped down to the budget 10 s later (392 MB anonymous); a pointer whose ray meets no link's box
decodes none (2 ms). The synthetic set (17 links, converted by the same host): 102 → 66 MB anonymous at rest. A cold
link's decode, per link of the local folder: 0.7-307 MB in 0-333 ms (the largest: 307 MB in 220 ms, 87 MB in 333 ms):
what a measure pays when its ray meets that link's box; the element query and Box to selection decode one block.
Found, not caused here: a plan through the 17 links (`view_svg`) took 12.6 s and 6.5 GB (8.5 GB at its peak):
`paper_scene.c` expands every placed copy of every link (110 M triangles); a plan should cut the links' shapes once.
