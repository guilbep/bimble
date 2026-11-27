# Rust (v0.19.0) against the C port on a real folder of IFC files

- Date: 27 Nov 2026, measured 09:36–10:12 on one machine, the same files, the same hour.
- Ticket: RUSTCMP. Question (Pierre): "I feel like our port to C is not working as well as the Rust
  implementation": memory, the light view files (`.bimbleview`), load times, 3D frames.
- Data: a local folder of 17 IFC files, 3.9 GB (4,178 MB), called #1…#17 by ascending size
  (#1: 0.2 MB … #17: 872 MB). Private: no names, no paths here.
- Status: measurements, not committed. Nothing in the C tree was changed for them.

## Summary

| measure | Rust v0.19.0 | C native (host / SDL viewer spike) | C web page (real GPU) | better | why |
|---|---|---|---|---|---|
| A. read one IFC for display: counts | 252,189 elements, 110,460,314 triangles, 15,262 shapes | **identical, file by file** | (same core) | same | the C reader is a port |
| A. read time, 17 files | 25.9 s of reading | 28.6 s of reading | – | same (±10 %) | |
| A. what the app does per IFC | read only: 26.3 s | link conversion: **50.5 s** | – | Rust | C also builds HLOD proxies (17.4 s), reads the property sets and writes the cache: work Rust never does |
| A. peak RSS of one read (#15, 709 MB) | 2,409 MB | 2,991 MB | – | Rust (C +1 % to +58 % above 100 MB) | the property-set pass and the HLOD build |
| B. view file, 17 files | 113 MB gzipped (232 MB raw) | caches **1,716 MB** raw | – | Rust | C's geometry part is **byte-identical** to Rust's file (232 MB); C adds 619 MB of baked proxies and 865 MB of property sets, nothing compressed |
| C. the folder: load to first frame | 3.2 s (17 view files) / 28.5 s (17 IFCs) | 2.7 s (17 caches) / 20.8 s (17 IFCs) | not re-measured | C native | |
| C. the folder: memory | 1.08 GB at rest, 1.47 GB peak (one process, view files) | host 2.17 GB at rest + the page; SDL viewer 1.45 GB | owner's tab: 3.8 GB (16 links), not re-measured | **Rust** (≈1.1 GB against ≈6 GB host + tab) | the page receives 820 MB of mesh + 619 MB of proxies and keeps them; the host keeps the 820 MB mesh it served |
| C. the folder: GPU memory | 858 MB geometry (964 MB allocated) | 984 MB | – | Rust (−13 %) | |
| C. the folder: frame, all exact, line edges | 155 ms median (MSAA 4) | 205 ms | – | Rust | C native's line-edge pass is slower; without edges both 62–63 ms |
| C. the folder: frame, fast settings | 23 ms (proxies 1 px + screen edges, no MSAA) | 21 ms (HLOD + screen edges + 1 px cut) | – | same | both need proxies; both build them at open (11–17 s) |
| D. #11 (201 MB, 8.8 M triangles): full frame | 10.9 ms (1920×1080, MSAA 4) | 10.2 ms (lines), 5.1 ms (screen edges) | 21.6 ms WebGL2, 24.5 ms WebGPU, at **984×547** | native (both) | the page draws 4× fewer pixels and is still 2× slower |
| D. #11: GPU memory | 15.5 MB | 43 MB | 88–93 MB | Rust | |
| D. imported model (1,749 elements, 2.2 M triangles): load | 0.18–0.22 s | 1.2 s (1.0 s meshing) | 2.0 s to first frame | Rust | C meshes the model 5× slower and shares 4× fewer shapes |
| D. imported model: GPU memory | 13 MB | 108 MB | 133–145 MB | **Rust (8–11×)** | Rust stores 0.30 M triangles (1,472 copies of shared shapes); C 2.0 M (797 copies) |
| E. binary | 44 MB (31.5 MB stripped) | 3.5 MB host + 2.9 MB web; SDL viewer 1.75 MB + SDL3 3.3 MB | – | C | |
| E. start | 66–104 ms to the first headless frame | host serving in 8–24 ms; SDL viewer first frame 25–31 ms | not measured | C | |

**The three causes that matter most:**

1. **The browser page, not the C core.** The same core drives a native viewer that matches Rust's
   renderer frame for frame (folder: 62 against 63 ms without edges, 66 against 65 ms with screen
   edges; #11: 10.2 against 10.9 ms). The page draws the same model 2× slower at a quarter of the
   pixels, and for the folder it needs the host (2.2 GB) **plus** the tab (3.8 GB observed) where Rust
   needs one process of 1.1 GB.
2. **What C sends and keeps for links.** The mesh the page asks is the shared-shape form (BMSH
   version 3, not expanded: 820,564,052 bytes for 17 links; the expanded form would be 4.39 GB), but
   it is uncompressed and the host keeps its copy after serving it (+1.05 GB RSS). Then the page also
   downloads every link's baked proxies (619 MB). Rust holds the scenes once (1.08 GB) and has no
   proxies for links at all.
3. **The caches' extra sections.** 1.6 GB of caches are 232 MB of the very same geometry Rust writes
   (byte for byte), plus proxies stored at about 45 bytes per triangle (619 MB, 2.7× the geometry)
   and property sets as one JSON text per element, the shared sets repeated (865 MB, 3.7× the
   geometry; gzip shrinks them 31×). Building the proxies doubles the conversion time.

## Method

Machine: Apple M1 Pro (8 cores, 16 GB), Asahi Linux aarch64, kernel 7.2, Mesa 26.3 "Borealis"
Vulkan, adapter "Apple M1 Pro (G13S C0)" (printed by every GPU run: no llvmpipe, no SwiftShader).
Headless only. Two other agents worked at the same time (a browser and IfcOpenShell): the 1-minute
load was 3–9 during the runs, recorded per row in the raw logs; available memory 5–8.6 GB.

Builds:
- Rust: branch `cmp/rust` (v0.19.0, `oracle/rust` + reference dumpers; the workspace says 0.18.1),
  `cargo build --release -p bimble-cli --bin bimble --example folder`. Commits `d890122` and `fa1c174` add
  `crates/bimble-cli/examples/folder.rs` (a folder of links in the v0.19.0 renderer, headless),
  make `bimble view --link` repeatable and raise the renderer's `MAX_LAYERS` from 4 to 32 (v0.19.0
  draws at most the model and 3 links: `crates/bimble-render/src/lib.rs:416`). The example uses
  mimalloc, as the `bimble` binary does.
- C: `git archive` of the integration checkout at `000c3c6` (`next` = preview.5 + a research note)
  into a scratch folder, `ccore/build.sh native` (clang -O2, manifold) → `bimble-ccore`; the NAT1
  spike's `ccore/native/` copied beside it and built against today's core (`bimble-view-native`).
  The page: the preview.5 release's `web/` copied, served by that host.

Every run: `free` and `/proc/loadavg` first; peak RSS from `getrusage` (a 15-line stand-in for
`/usr/bin/time -v`, which is not installed); memory capped at MemAvailable − 1.5 GB (`ulimit -v`,
or a `systemd-run --user --scope -p MemoryMax=… -p MemorySwapMax=0` for GPU runs: Vulkan maps
address space that `ulimit -v` counts); files #12–#17 only with ≥ 3 × their size + 2 GB available;
one run at a time; heavy runs under the shared measurement lock once it existed.

Commands (`<folder>` = the private folder, `$C` = the C build, `$R` = Rust's `target/release`):

```sh
# A, C: each IFC through a link's conversion, nothing written beside the IFC
$C/bimble-ccore link-convert --cache-dir <scratch>/ccache <folder>/#n.ifc
# A, Rust: the read a link does (bimble_viewer::load_scene), counts, nothing drawn
$R/examples/folder --read-only <folder>/#n.ifc
# B, Rust: the view file as shipped (gzip)
$R/bimble pack <folder>/#n.ifc <scratch>/rview/n#n.bimbleview
# B, both: section table (magic BIMBLEVW, version 1, [kind, u64 length, payload]…), gzip -6 per part
# C, Rust: 17 view files (or IFCs) read one after the other as links, one GpuScene each, drawn
#    together (render_layers), 240 frames of a 360° orbit at 1920×1080, each waited for
$R/examples/folder --frames 240 --keep-cpu [--samples 1] [--proxies 1] [--gpu-cull] [--occlusion] \
    [--screen-edges] [--no-edges] [--close] n1.bimbleview … n17.bimbleview
# C, native viewer: the 17 caches (or IFCs), same orbit, size and fit rule
$C/bimble-view-native --offscreen 1920x1080 --frames 240 --orbit [--hlod] [--edges screen|none] \
    [--cut 1] [--mode instanced] [--dist 0.2] #1.cache … #17.cache
# C, host: a copy of the owner's project file (links by absolute path, caches read beside the IFCs)
BIMBLE_TOKEN=… $C/bimble-ccore open --no-browser --port P <project>.bimble
#    then POST /query/links_panel until 17 × "ready", GET /mesh?shapes=1, GET /mesh?link_proxies=k
#    (k = 0…16), GET /mesh (version 1); VmRSS / VmHWM from /proc after each
# D, Rust: the shipped benchmark
$R/bimble bench --quick --frames 240 [--samples 1|--proxies 1|--gpu-cull|--occlusion] --json <file>
# D, web: tools/frames.mjs with FRAMES_EXTRA_MODEL=<file>, --builds native --models local
#    --renderer webgl2|webgpu, Chromium flags of DEV.md §6 (ANGLE on Vulkan on the M1 Pro, checked)
```

The camera of the folder runs is the C native viewer's (`ccore/native/view.c` `cam_fit`): the bounds
of the models within 20 km of the model nearest to all others (one of the 17 lies far away), yaw
−55°, pitch 28°, fov 35°. Rust's example fits the same way.

## A. Reading one IFC

C: `link-convert` (what linking an IFC does: read, property sets, proxies, cache written); "read" is
the read alone from its report. Rust: `examples/folder --read-only` (the read a link does). Peak RSS
of the process. Counts are identical in every row (elements, triangles, shapes after sharing).

| # | IFC MB | elements | triangles | shapes | C total s (read ms, HLOD ms) | C peak MB | Rust s (read ms) | Rust peak MB |
|---|---|---|---|---|---|---|---|---|
| #1 | 0.2 | 17 | 524 | 14 | 0.01 (4, 0) | 13 | 0.02 (17) | 71 |
| #2 | 1.6 | 511 | 6,132 | 51 | 0.03 (21, 1) | 13 | 0.02 (20) | 75 |
| #3 | 18.7 | 4,528 | 76,392 | 991 | 0.16 (130, 11) | 88 | 0.11 (105) | 91 |
| #4 | 25.3 | 783 | 1,102,656 | 56 | 0.34 (184, 110) | 140 | 0.17 (164) | 211 |
| #5 | 43.7 | 6,510 | 327,060 | 951 | 0.69 (521, 114) | 174 | 0.45 (440) | 212 |
| #6 | 53.5 | 12 | 465,748 | 12 | 0.58 (411, 100) | 360 | 0.42 (412) | 481 |
| #7 | 59.4 | 5,965 | 627,224 | 1,138 | 0.88 (585, 218) | 204 | 0.80 (790) | 256 |
| #8 | 60.0 | 46 | 888,593 | 20 | 1.02 (474, 437) | 594 | 0.50 (490) | 619 |
| #9 | 71.3 | 719 | 1,370,970 | 123 | 0.92 (455, 418) | 290 | 0.52 (509) | 403 |
| #10 | 159.1 | 8,059 | 1,561,801 | 1,624 | 1.70 (1,063, 489) | 674 | 1.11 (1,088) | 670 |
| #11 | 200.7 | 29,935 | 8,838,551 | 749 | 2.20 (1,265, 771) | 672 | 1.21 (1,189) | 426 |
| #12 | 335.8 | 30,295 | 5,430,874 | 1,980 | 4.29 (2,598, 1,302) | 1,071 | 2.13 (2,100) | 989 |
| #13 | 394.1 | 21,192 | 3,738,159 | 354 | 3.80 (2,717, 690) | 1,370 | 2.51 (2,483) | 1,120 |
| #14 | 415.1 | 32,766 | 35,798,718 | 1,195 | 6.95 (3,605, 2,887) | 1,669 | 2.76 (2,737) | 1,368 |
| #15 | 709.0 | 26,814 | 20,144,585 | 1,160 | 10.66 (4,403, 4,909) | 2,991 | 4.16 (4,069) | 2,409 |
| #16 | 758.4 | 48,794 | 20,987,851 | 2,860 | 8.71 (4,856, 3,230) | 2,195 | 4.05 (3,999) | 1,823 |
| #17 | 872.5 | 35,243 | 9,094,476 | 1,984 | 7.58 (5,304, 1,714) | 2,769 | 5.33 (5,257) | 2,410 |
| all | 4,178 | 252,189 | 110,460,314 | 15,262 | **50.5** (28,596, 17,401) | max 2,991 | **26.3** (25,869) | max 2,410 |

Verdict: the readers are the same (counts identical; read times within 10 %, the order of the runs
and the other agents' load can explain that much). C's link conversion takes 1.9× Rust's read
because it does more: HLOD proxies (17.4 s of the 50.5 s, up to 46 % on #15), the property-set pass
and the cache write. Its peak RSS is higher above 100 MB (+1 % to +58 %); below, C's process is
smaller (13 MB against 71 MB at the start). Limits: one run per file; the IFCs were in the page
cache for both; Rust's peak RSS with the system allocator was 11–20 % lower on the files over 200 MB
(mimalloc keeps more: 1,995 MB against 2,409 MB on #15); the table uses mimalloc, as `bimble` ships; the C "read" is the report's own timer.

What it would take in C: build the proxies later (in the background after the first picture, or
only when the GPU is short, as Rust does), or not for small files; read property sets lazily from
the IFC on the first click (they are only for the properties panel).

## B. The light view file

Rust's `.bimbleview` (`crates/bimble-geom/src/viewfile.rs`): "BIMBLEVW", version 1, sections HEAD,
META, GEOR, ELEM (meshes in the BMG1 codec), the whole stream gzipped (level 6) by default. C's link
cache: the same sections, byte-identical (checked with `cmp` on #3, #11 and #15 against Rust's file
inflated), plus SRCE (the IFC it came from, ~220 bytes), HLOD (baked proxies, BPRX) and PROP
(property sets: per element its sets as JSON text), uncompressed. The 17 caches in this table are
today's `link-convert` output (the folder's own caches have the same sizes; #1 has none there: under
the cache's minimum size).

| # | IFC MB | Rust raw MB | Rust gzip MB | C cache MB | = geometry MB | + HLOD MB | + PROP MB |
|---|---|---|---|---|---|---|---|
| #1 | 0.2 | 0.01 | 0.00 | 0.07 | 0.01 | 0.00 | 0.06 |
| #2 | 1.6 | 0.08 | 0.01 | 0.99 | 0.08 | 0.02 | 0.89 |
| #3 | 18.7 | 0.75 | 0.11 | 12.65 | 0.75 | 0.48 | 11.42 |
| #4 | 25.3 | 2.93 | 1.60 | 9.79 | 2.93 | 3.23 | 3.62 |
| #5 | 43.7 | 2.63 | 1.06 | 22.68 | 2.63 | 3.85 | 16.20 |
| #6 | 53.5 | 4.30 | 1.10 | 8.27 | 4.30 | 3.95 | 0.02 |
| #7 | 59.4 | 3.67 | 1.74 | 27.32 | 3.67 | 9.89 | 13.75 |
| #8 | 60.0 | 7.47 | 4.08 | 12.38 | 7.47 | 4.68 | 0.23 |
| #9 | 71.3 | 2.64 | 1.11 | 19.03 | 2.64 | 15.61 | 0.77 |
| #10 | 159.1 | 9.30 | 3.80 | 39.14 | 9.30 | 18.35 | 11.50 |
| #11 | 200.7 | 6.27 | 1.31 | 112.85 | 6.27 | 23.63 | 82.95 |
| #12 | 335.8 | 21.09 | 10.09 | 144.07 | 21.09 | 59.87 | 63.11 |
| #13 | 394.1 | 19.06 | 10.19 | 161.74 | 19.06 | 26.27 | 116.41 |
| #14 | 415.1 | 19.26 | 10.87 | 182.34 | 19.26 | 57.16 | 105.91 |
| #15 | 709.0 | 72.17 | 31.50 | 441.15 | 72.17 | 227.18 | 141.80 |
| #16 | 758.4 | 30.31 | 17.12 | 316.87 | 30.31 | 109.50 | 177.06 |
| #17 | 872.5 | 29.61 | 17.22 | 204.31 | 29.61 | 55.52 | 119.18 |
| all | 4,178 | **231.5** | **112.9** | **1,715.6** | **231.5** | **619.2** | **864.9** |

gzip -6 of each part (the 16 caches of the folder): geometry 231.5 → 112.5 MB (2.1×, as Rust's),
HLOD 619.2 → 206.8 MB (3.0×), PROP 864.8 → 28.0 MB (**31×**), the whole caches 1,716 → 347 MB.

Why the caches are big, in order: (1) the property sets: one JSON text per element with every set
written out again for each element that shares it, uncompressed (#11: 83 MB of sets for 6 MB of
geometry; 2.8 MB gzipped); (2) the proxies: BPRX stores unindexed positions, edges and normals, about
45 bytes per proxy triangle (#15: 5.04 M triangles, 227 MB); for files of few shared shapes they are
larger than the model itself (#9: 15.6 MB of proxies for 2.6 MB of geometry); (3) no compression.

What Rust keeps instead: no property sets anywhere (the reader skips them: `read.rs` NOT_DISPLAYED,
also said in `ccore/src/ifc_psets.h`); proxies only in memory, built at open for the main model when
the GPU is slow and the setting is on (`crates/bimble-viewer/src/lib.rs:2373-2386`), never for links.
Built for the whole folder in the example: 11.0–16.4 s, +530 MB of GPU memory, +210 MB of RSS at rest.

Reading the other's file (a small and a large one): Rust read C's caches of #3 and #15 (`bimble info`,
the example: 445 ms for #15's 441 MB cache, peak 949 MB because Rust reads the whole file into memory,
the 369 MB of sections it skips included); C read Rust's gzipped files of #3 and #15
(`link-convert`: 4 ms and 660 ms, the same counts). Both directions work.

What it would take in C: gzip (or a per-section deflate) for PROP and HLOD (−80 % of the bytes);
property sets stored once per distinct set with an index per element; proxies only when the model's
geometry is larger than a threshold, or quantized and indexed.

## C. All 17 together

### C core host, no page (the owner's project file, 17 links from their caches)

| step | time | host RSS (peak) |
|---|---|---|
| links ready (17 × "ready", from the caches beside the IFCs) | 3.2 s | 1,147 MB (1,333) |
| GET /mesh?shapes=1 (what the page asks: `web-ui/src/glviewport.ts:791`): BMSH version 3, each shape once | 820,564,052 bytes, built in 529–585 ms, served in 0.9 s | 2,221 MB (2,731) |
| GET /mesh?link_proxies=k, k = 0…16 (what the page asks next) | 619,199,704 bytes (= the caches' HLOD sections; #1 has no cache there, so no baked proxies) | 2,173 MB |
| at rest | | 2,173 MB (2,731) |
| GET /mesh (version 1, expanded; the page does not ask it) | 4,393,669,048 bytes built in 6.2 s; asked a second time the host was killed by the 6.8 GB cap | – |

The owner's 728,838,144 bytes for 16 links was the version 3 form (shapes once), not the expanded
one. It is 3.5× the geometry sections (232 MB) because it is the GPU-ready layout (f32 positions,
u32 indices, edge segments and normals, a 48 + 52-byte row per element) where the cache holds the
BMG1 codec. It is close to what Rust puts on the GPU (858 MB). The cost is that it exists three times:
in the host after serving (≈1 GB that stays), in the page's buffers, and on the GPU.

### Rust renderer and C native viewer, headless, 1920×1080, 240 frames of a 360° orbit

Rust rows with the system allocator are marked (s); frame times do not depend on it.

| run | files | load → first frame | RSS at rest / peak | GPU | frame median / p95 ms |
|---|---|---|---|---|---|
| Rust, MSAA 4, line edges (the viewer's look) | 17 view files | 3.2 s (read 1.5, upload 1.3) | 1,081 / 1,472 MB | 858 MB geometry, 964 MB allocated | **155 / 169** |
| Rust, the same, close (1/5 of the distance) | | | | | 6.6 / 139 |
| Rust, MSAA 1, line edges | 17 view files | 3.3 s | 1,082 / 1,519 MB | 858 MB | 152 / 171 |
| Rust, MSAA 1, no edges (s) | 17 view files | 3.0 s | 1,150 / 1,325 MB | 858 MB | 63 / 75 |
| Rust, MSAA 1, screen edges (s) | 17 view files | 3.1 s | 1,152 / 1,324 MB | 858 MB | 65 / 75 |
| Rust, MSAA 4, proxies 1 px | 17 view files | 20.0 s (proxies built 16.4 s) | 1,294 / 1,857 MB | 1,495 MB allocated | 42 / 68 |
| Rust, MSAA 1, screen edges, proxies 1 px (s) | 17 view files | 14.6 s | 1,504 / 1,660 MB | 1,436 MB allocated | **23 / 35** |
| Rust, MSAA 4, gpu-cull (s) | 17 view files | 3.3 s | 1,151 / 1,316 MB | 858 MB | 131 / 165 (\*) |
| Rust, MSAA 4, occlusion (s) | 17 view files | 3.2 s | 1,152 / 1,323 MB | 858 MB | 148 / 155 (\*\*) |
| Rust, MSAA 4, line edges | **17 IFCs** | 28.5 s (read 26.5 s) | 1,722 / **3,584** MB | 858 MB | 143 / 163 |
| C native, GPU cull, line edges | 17 caches | 2.7 s | – / 1,451 MB | 984 MB | **205 / 225** |
| C native, the same, close (0.2 × distance) | | | | | 160 / 184 (70.7 M of 110 M triangles drawn) |
| C native, instanced (no GPU cull) | 17 caches | 2.6 s | – / 1,452 MB | 969 MB | 161 / 176 |
| C native, no edges | 17 caches | 2.4 s | – / 1,451 MB | 984 MB | 62 / 72 |
| C native, screen edges | 17 caches | 2.4 s | – / 1,452 MB | 984 MB | 66 / 80 |
| C native, 1 px cut | 17 caches | 2.5 s | – / 1,451 MB | 984 MB | 100 / 118 |
| C native, HLOD (proxies built at open: 17.0 s) | 17 caches | 19.5 s | – / 2,832 MB | 1,624 MB | 46 / 61 |
| C native, HLOD, screen edges, 1 px cut | 17 caches | 18.9 s | – / 2,831 MB | 1,624 MB | **21 / 31** |
| C native, GPU cull, line edges | **17 IFCs** | 20.8 s (core open 19.2 s) | – / **3,686** MB | 973 MB | 184 / 216 |

(\*) In v0.19.0 GPU culling (and GPU occlusion queries) apply to the main model only
(`crates/bimble-render/src/lib.rs:2685-2698`, the `DrawOptions` comments); here the main model is #1
(17 elements), so the 16 links get none of it. The viewer behaves the same way with links.
(\*\*) CPU occlusion runs per layer, each link hidden only by its own large elements (`lib.rs:703`):
−4 % here.

C native "RSS at rest" is not printed by the spike (its peak is). The C native viewer frees each
session after reading its mesh, so its peak is a lower bound for what the host + page need.

### The web page on the folder

Not re-measured: the host alone holds 2.2 GB, the owner's tab held 3.8 GB with 16 links this
morning, and available memory was 5–8.6 GB with two other agents running (my watchdog stopped one
small web run at 2.3 GB available). From the host's side, the page receives 820 MB of mesh and
619 MB of proxies: 1.44 GB of buffers before any copy of its own.

Verdict on C: Rust is better on memory, by far, because it is one process holding each scene once
(1.1 GB at rest), where C is a host (2.2 GB) and a browser tab (3.8 GB observed). C native and Rust
are the same on the GPU work when edges are screen-space or off; with line edges Rust is 25 % faster
(155 against 205 ms). Both need proxies to be usable on the whole folder (21–23 ms), and both build
them for 11–17 s at open: C has them baked in its caches but the native spike does not read them
(it calls `cc_mesh_proxies`), and the page does (`link_proxies`), which this run did not time. C
native loads the folder faster (2.7 against 3.2 s from caches; 20.8 against 28.5 s from the IFCs).

What it would take in C: the host drops its copy of the served mesh (or streams it from the caches
per link); the page receives links' geometry in the codec form and expands it on the GPU side; the
page's buffers are not kept twice (ArrayBuffer and GPU). The structural fix is the native viewport
(NAT1 option d) for folders this size.

## D. One model alone

#11 (201 MB IFC, 29,935 elements, 8.8 M triangles). Native runs at 1920×1080; the page's canvas in
headless Chromium is **984×547** (frames.mjs, window 1280×800): a quarter of the pixels. "Full
frame" = every triangle; the page's orbit frames are budgeted moving frames (about 50,000
triangles), not comparable, so its still frame without proxies is used.

| | Rust `bimble bench` | C native | web WebGL2 | web WebGPU |
|---|---|---|---|---|
| full frame median / p95 ms | 10.9 / 15.4 (MSAA 4); 12.0 / 17.0 (MSAA 1) | 10.2 / 14.0 (lines); 5.1 / 8.5 (screen edges); 4.8 / 7.2 (none) | 21.6 (still, all 8.8 M) | 24.5 |
| what the app shows while moving | 1.7 ms (elements < 3 px left out) | 4.6 ms (1 px cut) | 1.5 / 3.8 ms (budget) | 4.3 / 10.6 ms |
| with proxies | 12.5 ms (1 px) | 10.1 ms | 9.1 ms (still, 1.9 M triangles) | 13.9 ms |
| load → first frame | 1.3 s (read 1.14 s) | 0.64 s (see note) | 4.6 s (the host reads a copy without cache) | 4.5 s |
| RSS | 487 MB peak | 405 MB peak | page: 13 MB JS heap + 92 MB array buffers; host not measured | 17 MB + 92 MB |
| GPU | 15.5 MB geometry | 43 MB | 92.5 MB (with 23.6 MB of proxies) | 87.6 MB |

The imported model (`.bimble`, 1,749 elements, 2.2 M triangles):

| | Rust `bimble bench` | C native | web WebGL2 | web WebGPU |
|---|---|---|---|---|
| full frame median / p95 ms | 5.0 / 8.3 (MSAA 4); 4.8 / 8.0 (MSAA 1) | 4.6 / 6.2 (lines); 2.1 / 2.8 (screen); 1.8 / 2.5 (none) | 13.6 | 30.5 |
| with proxies | 2.4 ms (1 px) | 2.6 ms | 1.7 ms (still) | 7.9 ms (still) |
| load | 0.18–0.22 s (read and mesh) | 1.2 s (core open 0.09 s, **mesh 1.0 s**) | 2.0 s to first frame | 2.1 s |
| RSS | 226 MB peak | 417 MB peak | 4 MB JS heap + 141 MB array buffers | – |
| GPU | **13 MB** | 108 MB | 145 MB | 133 MB |
| stored triangles | 304,381 (1,472 copies of shared shapes) | 1,998,757 (797 copies) | – | – |

Verdict: Rust and C native are the same per frame (C native faster with screen edges, which Rust
v0.19.0 only does single-sampled); the page is 2× slower at a quarter of the pixels (WebGL2) and
WebGPU is not faster yet. Rust is better on GPU memory (3–8×) and on opening a `.bimble` model (5×):
the C scene of this model shares far fewer shapes (797 copies against Rust's 1,472) and meshes slower.
Note: C native opened #11 in 0.57–1.09 s; I did not check whether its session used the cache beside
the IFC (the link conversion reads it in 1.27 s).

What it would take in C: share the shapes of a model's elements as `Scene::from_model` does (ADR
0042: one mesh per shape, placed); find what costs 1.0 s in meshing this model (Rust: 0.2 s).

## E. Other things

- **Start.** Rust: `bimble --version` 0–10 ms; the headless renderer to its first frame (GPU device,
  pipelines, #1) 66–104 ms; a window (`bimble view`) not measured (no visible windows allowed). C:
  the host serves its page 8–24 ms after start; the SDL viewer's first frame on #1's cache 25–31 ms;
  the page to its first frame empty not measured.
- **Binaries.** Rust `bimble` 44.0 MB (31.5 MB stripped; viewer, CLI, wgpu, egui in one). C:
  `bimble-ccore` 3.5 MB (preview.5; 3.1 MB today's native build) + `web/` 2.9 MB, and a browser; the
  SDL viewer 1.75 MB + SDL3 3.3 MB (shared library).
- **Per frame, Rust and not the page:** CPU frustum culling of every layer's clusters and shape copies,
  the visible copies packed into the instance buffer each frame (`crates/bimble-render/src/lib.rs:965`,
  `:999`, the culls at `:2654-2676`); MSAA 4 (`:1260`, `:1312`); hatching in the face shader
  (`shader.wgsl`); line edges as their own pass (`edges.wgsl`); occlusion, GPU queries, GPU culling and
  GPU culling and queries for the main model only (`:2685-2698`), CPU occlusion per layer (`:703`); a redraw only on change. Links are drawn in one
  submit as layers with a depth push (`:416-419`).
- **Per frame, the page and not Rust:** no MSAA (`web-ui/src/glviewport.ts:736`); a triangle budget
  while moving (`gl/cull.ts:16` MIN_MOVING_TRIS, `:31` budgetCut): about 50,000 triangles drawn per
  moving frame on #11; links' baked proxies cut per frame (`glviewport.ts:1029-1110`); a state texture
  per element for colour and visibility (`:1367`); a still frame refined over several frames after a
  move.

## What was not measured, and why

- The web page with the 17 links (memory: see C).
- The Rust app with a window (`bimble view`): forbidden on the owner's desktop; the headless example
  draws with the same renderer, but the viewer's own costs (egui, picking trees, the link comparison
  `compare::match_models` it runs per link) are not in it.
- GPU memory of the C page on the folder; the C native viewer's RSS at rest.
- The time the page takes to fetch and upload 619 MB of link proxies.
- Each number is one run. The Rust folder row run twice (two allocators) moved by 3 %; MSAA 4 against
  MSAA 1 on #11 came out inverted (10.9 against 12.0 ms): noise of that size. The load average (3–9)
  came from other agents.
- Rust's tests were not run on `cmp/rust`; clippy (`-D warnings`) passes on the three changed crates.

## Raw data

Logs and the scripts that produced them were in a scratch folder (deleted after; they hold private
names). Rust's code: branch `cmp/rust`, commits `d890122`, `fa1c174` (`examples/folder.rs`, repeatable `--link`,
`MAX_LAYERS` 32).
