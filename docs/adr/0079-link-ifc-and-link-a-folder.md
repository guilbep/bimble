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
