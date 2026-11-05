# ADR 0012 — Parallel loading: chunked STEP parse, per-element geometry on all cores, mimalloc

- Status: Accepted (5 Nov 2026)
- Decided by: Pierre ("we need fast, added complexity is okay"); measurements by Claude
- Reversal cost: low (rayon calls can be turned back into loops; the allocator is two lines)

## Problem
Loading was single-threaded. On a 50 MB IFC (51 301 elements), freeing the parsed file took as long
as parsing it (0.59 s each), because every keyword (entity, type, enum name) was its own allocation.
Building per CPU generation (x86-64-v3/AVX2) was measured first and gained nothing (±1 %, journal).

## Decision
- **STEP parse in chunks.** A serial pass finds statement starts using the parser's own
  `skip_ws`/`skip_statement` (the same boundaries as error recovery) and stops at `ENDSEC`. Chunks of
  ~1 MB are parsed with rayon and merged in file order, so later duplicate ids still win.
- **Interned keywords.** Entity, type and enum names are `step::Name` (`Arc<str>`, one per distinct
  keyword per chunk), which derefs to `str` and compares with `&str`.
- **Per-element geometry in parallel.** Each worker has its own `Reader` (placement cache, warnings);
  the triangle and item budgets are shared atomics; warnings are merged; results keep file order.
  `Scene::from_model` and `SceneBuilder::finish` (meshing, crease edges) are parallel too.
- **The parsed file is freed on a background thread** after the scene is built.
- **mimalloc** as the global allocator of the `bimble` binary only (libraries don't choose).

## Alternatives considered
- **Arena / zero-copy values** (borrowing strings from the file text): larger change to `Value`;
  revisit if parsing is still a bottleneck.
- **Sharing the placement cache between workers** (`DashMap` or precomputing): duplicates are
  cheap; not worth a lock.
- **Per-CPU-generation builds:** no measurable gain (see Problem).

## Evidence (2-core cloud VM, medians of 5 alternating runs, `bimble info`)
| File | before | after, 1 core | after, 2 cores |
|---|---:|---:|---:|
| Frame, 51 301 elements (50 MB) | 2 159 ms | 1 208 ms | 879 ms |
| 2 000 walls + 8 000 openings | 1 261 ms | 1 233 ms | 651 ms |
| Real file, 4 062 elements (21 MB) | 868 ms | 725 ms | 470 ms |
| Real file, 2 735 elements (7 MB) | 346 ms | 244 ms | 172 ms |
| Real file, 776 elements (2.5 MB) | 90 ms | 75 ms | 56 ms |

Native model (`bimble bench`, 51 301 elements): 649 → 409 ms. Scaling beyond 2 cores is not
measured yet. Output is identical to the serial reader (`info --elements-json`, summary and
warnings) on all fixtures, the synthetic files and the three real files, and stable run to run.
`chunked_parse_matches_serial` compares chunk sizes 1/7/64 bytes with a single chunk on the
fixtures and a sample with quotes in comments, `;` in strings, malformed and duplicate statements
and data after `ENDSEC`; it fails when the splitter ignores strings.

## Consequences
- When a budget runs out, which elements were cut short can vary between runs (threads race for
  the shared budget). Within budget, results are deterministic.
- Malformed files whose statements contain comments with quotes could resynchronise differently
  at a chunk boundary than a serial parse would; the instance is skipped and counted either way.

## Revisit when
- Pierre's old laptop benchmark shows the cores are not used, or memory peaks too high.
- A profile shows parse allocations dominate again (then: arena or borrowed strings).
