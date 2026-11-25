# The C IFC display reader on 17 real models (tickets R1, R2)

Question (R2): the C reader (`cc_ifc_read`) gave scenes byte-identical to Rust's `read_str` on 17
real models (R1), but single-threaded it took 3–4× Rust's time on the large ones and 1.5× its
memory. Can it match Rust (multi-threaded, rayon) in time and memory with the same bytes?

The models are not in the repo (client files): numbered #1–#17 by size, sizes and ratios only.

## Result in short (25 Nov 2026)

- **Read call on the largest model (#17, 873 MB): 14.7 s → 3.6 s (Rust 4.6 s); peak RSS 4.33 →
  2.62 GiB (Rust 2.89 GiB).** Every model is faster than before (0.21–0.32× the time on all
  models over 15 MB; 0.5–0.7× on the two small ones, a few ms) and as fast as Rust or faster
  (#14: 1.02×). Peak RSS at most Rust's on 15 of 17; #8 is 1.06× Rust (0.57 GiB, before 0.38),
  #1 is equal (5 MB).
- **All 17 scenes byte-identical to Rust** after the change (scene.txt and every element's mesh
  text hash), and identical on 1 and 8 threads (test/ifc_read_test.c, test/step_test.c).
- Threads: 8 (all cores of the 8-core aarch64 machine; `CC_THREADS` sets it). Other agents were
  running: timings carry some noise.

## Method

`ccore/tools/ifc_dump read` (clang -O2, manifold kernel) and Rust's `ccore_golden --dump-read`
(release, rayon), one model at a time, each process capped at 7 GB (systemd-run, no swap).
"op" is the read call alone (`read_ms`); "wall" is the whole process (reading the file, the call,
writing the dump); RSS is the process peak (the 873 MB text included). Before = the R1 build,
after = this branch. Rust = the R1 runs. `-DCC_IFC_PROFILE` prints time and memory per phase.

## Table

"GB" is GiB (kB / 1024²). Ratios are of the read call ("op") and of peak RSS.

| # | size MB | Rust s (wall / op) | Rust GB | C before s (wall / op) | C before GB | C after s (wall / op) | C after GB | op after / before | op after / Rust | GB after / Rust | scene identical |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.2 | 0.0 / 0.00 | 0.00 | 0.0 / 0.00 | 0.00 | 0.0 / 0.00 | 0.00 | 0.74 | 0.63 | 1.01 | yes |
| 2 | 1.6 | 0.0 / 0.01 | 0.01 | 0.0 / 0.01 | 0.01 | 0.0 / 0.01 | 0.01 | 0.52 | 0.88 | 0.88 | yes |
| 3 | 18.7 | 0.2 / 0.08 | 0.04 | 0.2 / 0.20 | 0.11 | 0.1 / 0.05 | 0.03 | 0.25 | 0.58 | 0.89 | yes |
| 4 | 25.3 | 0.7 / 0.17 | 0.14 | 0.8 / 0.47 | 0.15 | 0.5 / 0.15 | 0.12 | 0.32 | 0.90 | 0.84 | yes |
| 5 | 43.7 | 0.7 / 0.41 | 0.15 | 1.3 / 1.15 | 0.23 | 0.5 / 0.36 | 0.13 | 0.31 | 0.87 | 0.90 | yes |
| 6 | 53.5 | 0.7 / 0.35 | 0.41 | 1.2 / 0.99 | 0.33 | 0.5 / 0.31 | 0.34 | 0.31 | 0.87 | 0.82 | yes |
| 7 | 59.4 | 1.2 / 0.80 | 0.19 | 1.7 / 1.52 | 0.31 | 0.6 / 0.41 | 0.17 | 0.27 | 0.52 | 0.90 | yes |
| 8 | 60.0 | 1.0 / 0.51 | 0.54 | 1.6 / 1.33 | 0.38 | 0.6 / 0.39 | 0.57 | 0.29 | 0.76 | 1.06 | yes |
| 9 | 71.3 | 1.1 / 0.45 | 0.29 | 1.6 / 1.21 | 0.38 | 0.8 / 0.39 | 0.27 | 0.32 | 0.86 | 0.91 | yes |
| 10 | 159.1 | 1.9 / 0.92 | 0.64 | 3.0 / 2.42 | 0.81 | 1.3 / 0.78 | 0.61 | 0.32 | 0.85 | 0.96 | yes |
| 11 | 200.7 | 5.8 / 1.47 | 0.49 | 4.8 / 2.38 | 0.91 | 2.9 / 0.51 | 0.41 | 0.21 | 0.35 | 0.82 | yes |
| 12 | 335.8 | 5.1 / 1.78 | 1.07 | 8.1 / 6.51 | 1.66 | 3.2 / 1.64 | 0.97 | 0.25 | 0.92 | 0.91 | yes |
| 13 | 394.1 | 5.4 / 2.96 | 1.26 | 8.0 / 6.76 | 2.00 | 2.8 / 1.67 | 1.19 | 0.25 | 0.56 | 0.94 | yes |
| 14 | 415.1 | 26.4 / 2.61 | 1.64 | 22.6 / 8.90 | 2.11 | 16.2 / 2.65 | 1.31 | 0.30 | 1.02 | 0.80 | yes |
| 15 | 709.0 | 14.8 / 3.79 | 2.56 | 18.9 / 12.67 | 3.52 | 9.0 / 2.88 | 2.40 | 0.23 | 0.76 | 0.94 | yes |
| 16 | 758.4 | 14.9 / 3.60 | 2.24 | 18.6 / 12.46 | 3.67 | 9.0 / 2.95 | 2.02 | 0.24 | 0.82 | 0.90 | yes |
| 17 | 872.5 | 9.3 / 4.56 | 2.89 | 17.2 / 14.70 | 4.33 | 6.0 / 3.62 | 2.62 | 0.25 | 0.79 | 0.91 | yes |

## Where the time and the memory went (#17, read call)

| phase | before ms | after ms |
|---|---|---|
| STEP parse (split, chunks, merge) | 4,504 | 1,163 |
| Parsed::new (containers, voids, materials, products) | 1,791 | 421 |
| displayed bytes, shapes (Parsed::shapes) | 1,703 | 312 |
| elements, batches of 1024 (Parsed::piece) | 3,976 | 980 |
| origin | 107 | 109 |
| mesh builds (MeshBuilder::build) | 2,209 | 393 |
| share_copies | 70 | 71 |

Peak RSS before: text 0.83 GB + parse tree 2.0 GB (14.2 M entities: properties and quantities
parsed too) + side tables 1.1 GB (one zeroed slot per entity of the file, for containers,
materials, openings, placements) + world triangles 0.45 GB. After: text 0.83 + parse tree 1.37
(12.4 M entities) + side tables ~0 + world triangles 0.45.

## What changed (one commit each)

1. `src/jobs` (L0): run N tasks on pthreads (serial in wasm), each task its own result slot,
   merged in order. STEP: the 1 MiB chunks of step.rs parsed on all cores, one parser and arena
   per worker, merged in file order; names interned; no copy of the entity array; the statement
   split by memchr; `cc_step_parse_skipping` (read.rs does not parse property and quantity
   types).
2. The reader: the shared shapes, each batch of 1024 elements, the final mesh builds, the
   displayed-bytes scan and the product scan as jobs (one reader, mesh arena and scratch arena
   per worker). The triangle and item budget is shared with relaxed atomics, as read.rs; a batch
   that runs out is meshed again on one thread, as read.rs, so the cut is the same on any thread
   count. The placement cache is a hash map per reader; libc calloc natively (the wasm shim's
   calloc zeroed every side table page by page).
3. Side tables sparse: containers and materials a hash map from entity index, openings sorted
   (host, opening) pairs.
4. A worker's scratch arena drops a block over 1 MiB after its job.
5. Crease edges sorted as contiguous records: the merge passes read random 104-byte edge uses
   (813 → 83 ms on the largest shape of #14).

One behaviour differs from Rust on purpose: a placement whose chain warned ("unsupported
placement ...") is not cached in a reader, so the warning counts once per product on it. Rust
counts it once per worker that met it first, which depends on the thread timing. None of the 17
models has such a warning.

## What is left

- #14 (35.8 M triangles drawn) is at Rust's time: one shape of 648 k triangles (meshing 0.4 s
  and its crease-edge hash 0.4 s) runs on one thread while the others wait.
- #8 (46 elements, 0.9 M triangles): peak RSS 0.57 GiB (before 0.38, Rust 0.54): 8 large shapes
  meshed at once. A memory-aware schedule would cut it; not done.
- The statement split before the parallel parse is serial (0.36 s on #17), as in Rust.
- Rust's import and the C import are not threaded (not in this ticket).
