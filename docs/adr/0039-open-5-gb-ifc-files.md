# ADR 0039 — Open 5 GB IFC files

- Status: Accepted for the desktop (9 Nov 2026); the browser part is proposed (not built)
- Decided by: Pierre set the target: open a 5 GB IFC (Trimble Connect's upload limit), stretch
  goal 7 GB (a file IfcConvert ran out of memory on with 64 GB of RAM). Claude did the
  implementation and the measurements.
- Reversal cost: low

## Context / Problem

No real IFC file this large is public. Bimble refused any file over 2 GiB (`MAX_FILE_BYTES`),
before reading a byte. The reader already streamed the file in 64 MB windows (ADR 0033), so
the file's text was never whole in memory. The cap was the only thing stopping a 5 GB file.
Without it, two unbounded cases needed a bound:
- a file without line breaks made the displayed-bytes scan hold the whole file (`read_until`);
- a file without a `;` made the parse window grow to the whole file.

## Decision

1. **A synthetic test file**, `scripts/repeat_ifc.py`. It repeats a public model N times on a
   square grid, 200 m apart. It streams, one statement at a time, and never holds the output.
   - For each copy k ≥ 1:
     - every `#n` outside strings becomes `#(n + k·stride)`;
     - every IfcRoot gets a fresh GlobalId (22 characters in IFC base64), hashed from
       (k, old GlobalId) so that runs are reproducible;
     - placements relative to nothing (the site's) get a new parent placement at the copy's
       offset.
   - **One IfcProject**: copy 0's. Later copies drop theirs and point at it, so their sites
     aggregate under it. That is how viewers and IfcOpenShell expect a file: one project, and
     units and contexts from one place.
   - Python rather than a hidden `bimble` subcommand: it is test tooling, not product code,
     and it sits next to the other scripts. It writes about 9 MB/s, so a 5 GB file takes
     10 minutes.
   - Generated files are never committed.
2. **The 2 GiB cap rises to 64 GiB**, about 10× the largest IFC reported. A streamed file
   costs time for its size, not memory. Memory is bounded by:
   - the displayed bytes (`Limits`, and the viewer's estimate and budget, ADR 0033);
   - **a cap on one statement, 1 GiB**, in `step`. A real statement is at most tens of MB. A
     file without a `;`, or a header without `DATA;`, is refused once a statement passes
     the cap.
   - The displayed-bytes scan keeps at most 256 bytes of a line, so a file without line
     breaks costs no more memory.
3. **The parsed file is freed on all cores**, in batches of 4096 entities (`Drop for
   Entities`). A profile of the 5 GB file under memory pressure: freeing it on one thread
   took 15.5 s of the main thread's 36 s.
4. The viewer's open path is unchanged. The ADR 0033 estimates (below) let the 5 and 7 GB
   files open without asking on this machine.

## Measurements

The machine is an M4 Pro with 24 GB of RAM, not the 48 GB the brief assumed. A watchdog
stopped any run past 20 GB.

**The machine was shared and overloaded during all of this.** Load average reached 176, 27 of
28 GB of swap were in use, and the disk was 99 % full. Wall times are noisy by ±30 % or more.
Instructions retired and the triangle counts are exact. "Peak" is the peak memory footprint
from `/usr/bin/time -l`, GPU buffers included.

Sixty5 (343 MB, public): one copy loads in 1.9 s, 0.59 GB peak, 1.65 M triangles.

| file | before | after: load | peak | elements | triangles |
|---|---|---|---|---|---|
| Sixty5 ×5, 1.76 GB | 7.5–7.7 s, 1.8 GB | 6.3–7.2 s | 1.8–2.2 GB | (not printed) | |
| Sixty5 ×15, 5.32 GB | refused: "file too large" | 24–27 s (44–63 s under swap) | 5.3 GB | 244 500 | 24.80 M |
| Sixty5 ×20, 7.12 GB | refused | 125 s (load average 176) | 6.55 GB | 326 000 | 33.07 M |

- The ×20 file has exactly 20× the triangles.
- The ×15 file is 27 triangles short of 15× (24 799 788 against 24 799 815). The likely cause
  is tiny triangles that collapse at the copies' larger coordinates. Not investigated.
- The ×2 render shows two separate towers and the ×15 render shows 15.
- IfcOpenShell opens a 2-copy Schependomlaan:
  - 1 project, 2 sites, 2 buildings;
  - every GlobalId is valid;
  - all are unique except the one duplicate the source file already has;
  - the second site's placement chain carries the 200 m offset.
- `scripts/check_ifc.py` needs a `.bimble` source, so it does not apply. The check script used
  is not in the repo (`scratchpad/validate.py`).

**ADR 0033's estimate on the 5 GB file.** Displayed bytes are 1.15 GB of 5.32 GB: Sixty5 is
78 % property sets.
- Read: 1.25 × (64 MB + 5 × 1.15 GB) = 7.3 GB.
- Show: 1.25 × (532 MB + 1.93 × 1.15 GB + 225 B × 4.8 M stored triangles) = 4.8 GB.
- The estimate is 7.3 GB against 5.3 GB measured: 37 % over, which is safe. The budget is
  80 % of 24 GiB = 20.6 GB, so the viewer opens the file without asking. The 7 GB file is
  estimated at 9.7 GB against 6.55 GB measured.

**Frames on the 5 GB file** (`bimble bench --quick --frames 10`, 1080p, MSAA 4, median of a
whole-model orbit; 24.8 M triangles drawn, 120 MB of geometry on the GPU, 3190 draws):

| option | whole model | p95 | moving | close up |
|---|---|---|---|---|
| none (quieter run) | 22.4 ms | 85 ms | 4.7 ms | 24.8 ms |
| none (load average ~170) | 35.4 ms | 123 ms | 6.6 ms | 21.5 ms |
| `--proxies 3` | **6.6 ms** (42 draws) | 20 ms | 5.1 ms | 14.8 ms |
| `--occlusion` | 29.1 ms | 104 ms | 5.9 ms | 32.0 ms |
| `--gpu-occlusion` | 26.3 ms | 185 ms | 5.8 ms | 21.9 ms |
| `--gpu-cull` | 20.5 ms | 61 ms | 4.1 ms | 30.7 ms |

88 % of the drawn triangles are hidden: the towers hide each other.

## Is Nanite-like work needed at 5 GB?

Not on this evidence.
- The whole 5 GB model draws at about 45 fps with no option.
- Far cells drawn as their proxies (ADR 0036 #9, already built) bring it to 6.6 ms (150 fps).
- Moving frames stay under 7 ms.
- Occlusion and GPU culling are within the noise here.
- Memory is no constraint on the GPU: 120 MB, because the copies are mapped items.

The caveat is that this file is one tower repeated. A real 5 GB model with unshared,
geometry-heavy elements would store many more triangles. ADR 0036's "revisit when" (over
100 M drawn triangles, or over 50 ms frames on a weak GPU) still holds and has not been
reached.

## What is left

- **A geometry-heavy 5 GB file.** Sixty5 is 22 % displayed bytes. In a file that is mostly
  geometry, displayed bytes would be near 5 GB. The read estimate would then be about 31 GB
  and the viewer would ask before reading on a 24 GB machine; parsing alone would need about
  20 GB. That file is not tested: there was no disk space for Schependomlaan ×100, which is
  39 % displayed.
  The next step for such files would be to parse less:
  - an index of statement offsets, with entities parsed when meshing asks for them;
  - or a compact arena instead of boxed `Value` trees. That would also make freeing instant.
- **Load time.** On 5 GB, the main thread still spends:
  - ~5 s on the displayed-bytes scan (serial; the parser could count it instead, when the
    viewer has no question to ask);
  - ~3 s finding statement boundaries (serial, `complete_chunks`);
  - ~1.5 s building the id hash map.
  Sixty5's ids are too sparse for the dense table.
- **Measure again on an idle machine.** The numbers above were taken under heavy load.

## The browser (wasm32): not built here

A tab holds at most 4 GB (wasm32), and today a picked file is read whole into memory
(`Source::Bytes`). That alone fails above 4 GB, and the 5 GB file's parse is 5+ GB. Opening it
in a browser would need, in order of cost:
1. **Streaming the File.** Read `Blob.slice()` windows in a worker with `FileReaderSync` and
   feed `parse_read_skipping`, which already takes any `Read`. This removes the text from
   memory, but not the parse: about 5 × displayed bytes.
2. **Parsing less.** An offset index, with geometry parsed per element, per storey or per
   cell, the way the census already works.
3. **A prepared format.** Mesh once, natively or on a server, into stored triangles (shapes
   once, copies as placements, the ADR 0036 #8 codec) with the #9 proxies per cell. Load the
   proxies first and the cells on demand. For this file that is about 1 GB of mesh before
   compression.
4. **wasm64 (memory64)** would lift the 4 GB limit. It ships in recent Chrome and Firefox, but
   wgpu and wasm-bindgen support is not there yet.

## Evidence

- Tests:
  - `a_file_past_4_gib_is_read`: a sparse 5 GiB file with a model at its start. The old code
    fails it with TooLarge.
  - `a_statement_past_the_cap_is_refused`
  - `displayed_bytes_leave_out_property_sets`, extended: the streamed scan matches the
    in-memory scan on lines longer than its 256-byte head and than a 1 MB read.
- Reproduce:
  - `python3 scripts/repeat_ifc.py sixty5.ifc out.ifc --size-gb 5`
  - `/usr/bin/time -l bimble render out.ifc out.png`
  - `bimble bench --quick --frames 10 [--proxies 3 | --occlusion | --gpu-occlusion | --gpu-cull] out.ifc`

## Revisit when

- A real multi-GB file arrives: compare it with the estimate, and check its displayed-bytes
  share.
- Browser users need files over ~1 GB (displayed): build steps 1–3 above.
