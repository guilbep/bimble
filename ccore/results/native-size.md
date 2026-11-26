# The native binary's size: what it is made of and what each option gives (ticket SIZE1)

Question: the release binary `bimble-ccore` (Linux, aarch64) is 3,674,408 bytes stripped. How
much is the boolean kernel (manifold, C++) and the C++ runtime, how much is our C, and what does
each build option give, in bytes and in speed?

Measured 25 Nov 2026 on the dev machine (8-core aarch64, 16 GB, clang 23.1.3, GNU ld 2.46, GCC
16.1.1 for manifold v3.5.3). Other agents were running (load average 8 to 15): the read timings
carry noise of about ±10 %; sizes are exact.

## Result in short

- **Before: 3,674,408 bytes** = our C 1,149,247 (31 %), manifold + Clipper2 1,132,825 (31 %), the
  C++ runtime (libstdc++, libgcc, unwinder; static) 1,072,593 (29 %), linker-made (ELF headers,
  dynamic tables, `.eh_frame_hdr`, padding) 319,251 (9 %), C start files 492.
- **Applied (pure win): one section per function and datum, and `--gc-sections`** at the release
  link, with manifold built the same way (`deps.sh`): **2,232,352 bytes (−39 %)**; the read
  benchmark unchanged (median 560.6 ms against 562.9 ms), the scene and every mesh identical, the
  gate green. Most of the gain is code nobody calls: manifold 1,132,825 → 406,830 bytes, the C++
  runtime 1,072,593 → 558,799, linker-made 319,251 → 165,626, our C 1,149,247 → 1,100,613.
- The release archive (CI content: no `web/ccore/`): **2,406,647 → 1,438,737 bytes (−40 %)**:
  −413,154 for the Roboto files (SIZE1 part 1), then −554,756 for `--gc-sections`.
- Not applied, for the owner: dynamic libstdc++ (−1.1 MB more, but a runtime dependency), the
  kernel at `-Os` (−131 KB more, kernel speed not measured on a boolean-heavy model), no unwind
  tables in our C (−66 KB, loses backtraces). LTO is larger (+590 KB) and slower to link: no.

## Method

A scratch script (not committed): every `src/`, `session/`, `host/` file compiled
with the release flags of `build.sh release` (clang -O2, `-ffp-contract=off`, the kernel's
`-DCC_HAVE_MANIFOLD`) plus the variant's flags, 4 at a time; `bimble-ccore` linked as the release
does (manifold archives, `-Wl,-Bstatic -lstdc++`, `-static-libgcc`, `-s`) plus the variant's link
flags; `tools/ifc_dump` linked from the same objects. Size: the stripped file. Breakdown: the GNU
ld map of the link (`-Wl,-Map`), input sections of the loaded output sections summed per input
file (as `tools/weight.mjs`; the remainder to the file size is "linker-made").

Read benchmark (the `results/ifc_read.md` method on a synthetic model, no client file):
`bench/large_model.py 60000` (59,832 elements: columns, beams, slabs, walls, footings, 1,656
proxies with two voids each), exported to IFC4 by the C core (`export_ifc4`): 64,946,697 bytes.
`ifc_dump read` of it, `read_ms` (the read call alone), 9 rounds with the variants interleaved
(A B2 C E G D, A B2 …), median and minimum. Every variant gives the same `scene.txt` and
`meshes.txt` (sha1 4b6d095f69…, e37637ae56…).

manifold variants: built from the pinned tag with deps.sh's cmake options plus
`-DCMAKE_{C,CXX}_FLAGS="-ffunction-sections -fdata-sections"` (Release: manifold's CMake adds
`-O3`), and `MinSizeRel` for `-Os` (also `-Oz`: GCC 16 on aarch64 gives the same bytes as `-Os`).
Each build 20–26 s with `-j 3`.

## Table

"ours" = our C objects; "kernel" = libmanifold, libmanifoldc, libClipper2; "C++ rt" = libstdc++,
libgcc, libgcc_eh. Build time: compile of every source (4 jobs) / link. Read: median (min) of 9.

| variant | bytes | gzip -9 | ours | kernel | C++ rt | linker-made | compile / link | read ms |
|---|---:|---:|---:|---:|---:|---:|---|---|
| A: release before (O2, static libstdc++, stripped) | 3,674,408 | 1,617,670 | 1,149,247 | 1,132,825 | 1,072,593 | 319,251 | 5.4 s / 0.22 s | 562.9 (521.9) |
| not stripped (A without `-s`) | 4,460,176 | 1,782,719 | | | | | 3.3 s / 0.18 s | – |
| B: A + function sections, `--gc-sections` (manifold as imported) | 3,018,864 | 1,437,079 | | | | | 5.8 s / 0.26 s | – |
| **B2: B with manifold in function sections (applied)** | **2,232,352** | **1,064,123** | 1,100,613 | 406,830 | 558,799 | 165,626 | 3.8 s / 0.22 s | 560.6 (515.5) |
| B2 linked by lld | 2,237,720 | 1,073,640 | | | | | 3.6 s / 0.14 s | – |
| C: B2 with the kernel at `-Os` (`-Oz` the same) | 2,101,288 | 975,915 | 1,100,753 | 246,187 | 564,085 | 189,779 | 3.9 s / 0.22 s | 538.9 (497.0) |
| C0: the kernel at `-Os`, no gc | 3,215,672 | 1,369,674 | | | | | 4.2 s / 0.41 s | – |
| D: B2 + ThinLTO (lld) | 2,824,224 | 1,271,258 | | | | | 2.2 s / 4.8 s | 561.2 (505.5) |
| D2: B2 + ThinLTO (GNU ld, LLVMgold) | 2,822,168 | 1,264,408 | | | | | 2.4 s / 5.0 s | – |
| E: A with libstdc++ dynamic | 2,428,960 | 1,228,778 | | | | | 4.7 s / 0.17 s | 538.9 (484.1) |
| G: B2 + no unwind tables in our C | 2,166,816 | 1,041,588 | | | | | 4.1 s / 0.22 s | 546.5 (526.2) |
| H: C + G + libstdc++ dynamic (all size options) | 1,379,784 | 723,305 | | | | | 3.7 s / 0.20 s | – |

Compile times are noisy (other jobs); the read medians differ by less than the noise.

## Each option

- **Stripping** (`-s`, already in the release): 4,460,176 → 3,674,408 (−786 KB). Kept.
- **`-ffunction-sections -fdata-sections` + `-Wl,--gc-sections`**: on our C only, with manifold
  as imported (one section per file), 3,674,408 → 3,018,864; with manifold also in function
  sections, → 2,232,352. Same speed, same scenes, release_test 60/60 and the gate green:
  **applied** (`build.sh release` on Linux, `deps.sh`). The CI's manifold cache key hashes
  deps.sh, so CI rebuilds manifold once. macOS: `-Wl,-dead_strip` is the equivalent; not applied
  (no Mac to measure it on).
- **`-Os`/`-Oz` for the kernel only**: −131 KB more (manifold 406,830 → 246,187). The read
  benchmark is not slower (538.9 ms), but it calls the kernel for 3,312 voids only; a
  boolean-heavy model (3D printing, many openings) is not measured. For the owner.
- **LTO**: larger, not smaller (2.82 MB against 2.23 MB: cross-file inlining at -O2), the link
  20× slower (5 s), no speed gain on the read. Not applied. Kernel LTO would need GCC's LTO
  plugin in a clang link (manifold is built by GCC): not tried.
- **`-static-libstdc++` vs dynamic**: dynamic saves 1,245,448 bytes on A (and about 0.56 MB on
  B2, after gc). But the program then needs the system's libstdc++ with the GLIBCXX version of
  the build machine (CI builds on Ubuntu 22.04: fine there and newer; an older or musl system
  fails to start). The archive promises "Nothing is installed": not applied, for the owner.
- **No unwind tables in our C** (`-fno-asynchronous-unwind-tables -fno-unwind-tables`): −66 KB.
  A C++ exception cannot cross our C frames either way (nothing catches it there), but crash
  backtraces through C frames get worse. Not applied.

## Does the desktop archive need `ccore.wasm` and `ccore-view.wasm`?

No. The archive's page is opened as `/?native=1&gl=1&port=…&token=…` (host/main.c): it talks to
the host over HTTP; `src/ccore.ts` loads `ccore/ccore.wasm` only with `?ccore=1`, and
`bimble.ts` loads `ccore/ccore-view.wasm` only in a `VITE_CCORE=1` build with `?view=1`. The CI
archive has neither: `web/` is Vite's build of `web-ui/`, and `public/ccore/` is git-ignored,
filled only by `copy-ccore.mjs` (the lab tests, `npm run ccore:wasm`). A local `build.sh release`
after a lab test ships `web/ccore/ccore.wasm` and `frame.json` (970,240 bytes, 383,268 in the
archive's gzip) and, after `copy-ccore.mjs --view`, `ccore-view.wasm` too. **Proposal (not
applied):** `build.sh release` removes `web/ccore/` as it removes `web/fonts/`, so the archive is
the same wherever it is built. What breaks: `?ccore=1` (the wasm lab) served by the desktop host,
a developer's convenience only.
