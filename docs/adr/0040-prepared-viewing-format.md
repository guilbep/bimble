# ADR 0040 — A prepared viewing format, `.bimbleview`: share a model to be explored

- Status: Accepted (9 Nov 2026), as an option next to opening the IFC
- Decided by: Pierre ("your decision, but keep it as an option; people want to share a model for
  others to explore, mainly for the browser and low-RAM machines; put the compression where it's
  best for the end user"); the format and what is in it by Claude, on the numbers below
- Reversal cost: low now; medium once files are shared (readers must keep reading version 1)

## Context / Problem

Opening an IFC means parsing all of its text: 0.5–7 s and 0.5–2.6 GB at peak for 49–872 MB files
on an M4 Pro, more on an old laptop, and a 4 GB ceiling in a browser tab. Someone who only wants
to look at a model (a client, a site manager, a colleague with a tablet) pays all of that every
time, and must first download the IFC (9–194 MB even gzipped). What the viewer shows is much
smaller: the `Scene` (ADR 0034), whose meshes the codec (ADR 0036 #8) packs 3.5–4.4×.

## Decision

**A file holding the scene, nothing to parse:** File → "Share for viewing…" (desktop: a save
dialog; browser: a download) and `bimble pack <in.ifc|in.bimble> <out.bimbleview>` write it; the
viewer opens it like an IFC (Open, drag and drop, the web file picker, `bimble view`, `bimble
render`, Link), straight to a `Scene`, read-only. The IFC stays what Bimble opens by default;
this is an extra, for sharing. Not offered when only part of a too-large model is shown (ADR
0033): it would be shared as if it were the whole.

**Extension `.bimbleview`.** Starts with "bimble" so people know what opens it. It is not
`.bimble` (an editable model, text, ADR 0012) and not `.bimble.gz` (which would read as a
compressed editable model). One word, no dot inside: file pickers and mail filters handle it.

**What is in it:** per element its GlobalId, IFC class, name, storey, material and material
family; its own mesh, or a shared shape (stored once, in the chunk that first uses it) and its
f32 placement; the origin, storeys (name, elevation, GlobalId), grid axes, material densities and
the loader's warnings. All meshes go through `bimble_geom::codec` (within 0.05 mm, measured
≤ 0.031 mm). That is the `Scene` exactly: what the viewer shows and selects today.

**Not in it (measured, see below):**
- *Property sets and quantities.* They are 78 % (Sixty5) and 44 % (Schependomlaan) of the IFC
  text: 46 and 3.5 MB gzipped, 22× and 4× the whole view file. The viewer shows none today (the
  display reader skips them; the properties panel lists the scene's fields). Left out of
  version 1; a `PSET` section can be added later without a new version (unknown sections are
  skipped), with a compact table (names once, values per element), when the viewer shows them.
- *The proxies (ADR 0036 #9).* Encoded with the codec they weigh more than the scene itself
  (Sixty5: 5.8 MB of proxies for a 4.4 MB scene; Schependomlaan 0.7 for 1.5) and take 0.08–0.6 s
  to build, only on a machine that misses its frame-rate target. Downloading them costs more than
  building them; also a later section if a slow machine's numbers ask.

**Container** (`bimble_geom::viewfile`; little-endian; varint as in the codec):

```text
"BIMBLEVW"  u32 version (1)
sections: [u8; 4] kind, u64 length, payload
  HEAD  elements, shared shapes, stored triangles (budget before reading); origin f64 × 3
  META  sorted table of the distinct classes, storeys, materials, families; storeys; grids;
        densities; warnings
  ELEM  repeated, ~1 MB each: the shapes first used here (codec), then elements
        (GlobalId, name, four table indices, own mesh or shape index + placement f32 × 12)
```

- *Streaming.* The reader takes `impl Read` and decodes section by section; it never holds the
  file twice (from bytes in a browser, the bytes and the scene being built; from a gzipped
  stream, one section at a time). A chunk is complete in itself (its shapes come before the
  elements that use them), so a viewer can draw the first chunks before the rest arrives; the
  viewer does not yet (left below).
- *Canonical.* The same scene gives the same bytes: sorted table, shapes numbered by first use,
  chunks cut by size, f64/f32 bits exact with -0.0 written as 0.0, gzip at level 6 with no time
  stamp (deterministic for a given `flate2`). Test `round_trip` writes twice and compares.
- *Versions.* A different version is the error "a .bimbleview file of format version N; this
  Bimble reads version 1 (a newer Bimble made it: update Bimble)". New section kinds are skipped,
  so additions do not need a new version; a new version is only for changes old readers would
  misread.
- *Gzip, in the file, by default* (`--no-gzip` to leave it out). HTTP compression does not reach
  this use: `python3 -m http.server` never compresses, most static hosts compress only text types,
  and a file sent by mail, a shared drive or a USB key gets none. Gzip halves the file (1.7–2.3×
  on the seven models) and costs nothing to read (Sixty5: 47 ms gzipped against 59 raw, the
  difference is noise; Schependomlaan 17 against 12). Readers recognise gzip by its first two
  bytes. Not chunk by chunk: one stream is simpler and still streams (`GzDecoder` over a reader).
- *Untrusted input.* Every count is checked against the bytes left before anything is
  allocated, every index against what was read; floats must be finite; text must be UTF-8; a
  section over 1 GiB is refused and a lying length allocates nothing (the payload grows with
  what arrives); a gzipped file may expand at most 64× (the bytes compress about 2×); the element
  and shape counts of HEAD must match at the end, so a file cut between chunks is "cut short".
  No recursion, no panics: test `corrupt_files_are_errors_not_panics` (prefixes, gzip prefixes,
  500 files with four flipped bytes, a huge section with nothing behind).

## Evidence

`bimble pack`, then `bimble render` (1600 × 1000) from the IFC and from the view file; M4 Pro,
release, `/usr/bin/time -l` peak memory footprint (Apple unified memory: GPU buffers included).
The five private models by size only. Other builds ran on the machine at the same time: times
vary up to 2× between runs (sizes are exact).

| model | IFC | IFC gzipped | view raw | **view** | load IFC → view | peak, render IFC → view | pack (read + write) |
|---|---|---|---|---|---|---|---|
| Schependomlaan 49 MB | 49.3 MB | 9.4 MB | 1.5 MB | **0.80 MB** | 458 → 22 ms | 500 → 341 MB | 0.4 + 0.08 s |
| Sixty5 343 MB | 343 MB | 62.3 MB | 4.4 MB | **2.07 MB** | 1.4 s → 34 ms | 803 → 404 MB | 1.6 + 0.17 s |
| 394 MB | 394 MB | 75.9 MB | 19.1 MB | **10.2 MB** | 3.7 s → 211 ms | 1509 → 615 MB | 2.9 + 0.8 s |
| 415 MB | 415 MB | 84.3 MB | 19.3 MB | **10.9 MB** | 3.8 s → 175 ms | 1662 → 673 MB | 2.5 + 1.1 s |
| 709 MB | 709 MB | 131 MB | 72.2 MB | **31.5 MB** | 3.8 s → 484 ms | 2633 → 1454 MB | 4.3 + 2.6 s |
| 758 MB | 758 MB | 165 MB | 30.3 MB | **17.1 MB** | 3.3 s → 195 ms | 2030 → 831 MB | 6.0 + 1.2 s |
| 872 MB | 872 MB | 194 MB | 29.6 MB | **17.2 MB** | 6.7 s → 205 ms | 2510 → 802 MB | 7.2 + 1.0 s |

So the view file is 28–166× smaller than the IFC and 4–30× smaller than the gzipped IFC; it
opens 16–33× faster (the rest of `render`, GPU start and upload, is the same either way: total
0.2–0.9 s against 0.9–7.2 s); peak memory falls by 0.16–1.7 GB, to what the scene and GPU
buffers take. Packing costs reading the IFC once plus 0.1–2.6 s of encoding (one thread).
The renders from both are the same picture (checked by eye on all seven; test
`packed_ifc_renders_the_same` counts differing pixels on the demo frame).

Browser (release web build, headless Chrome on the M4 Pro, `python3 -m http.server`; the file's
bytes dropped on the canvas, so the download is not counted; to the scene read, then to the next
frame drawn):

| model | IFC: read / drawn | view file: read / drawn | to download (no HTTP gzip) |
|---|---|---|---|
| Schependomlaan | 1.15 s / 1.32 s | 27 ms / 0.25 s | 49.3 → 0.80 MB |
| Sixty5 | 4.01 s / 4.35 s | 42 ms / 0.26 s | 343 → 2.07 MB |

(Headless Chrome's WebGPU drew the 3D view black in both cases, so "drawn" is the frame, not a
checked picture. The browser read 3 504 elements from the Schependomlaan IFC where the native
reader, which made the view file, read 3 511: a difference between the two IFC paths, not of this
format.)

## Alternatives considered

- *glTF / 3D Tiles.* Standard and streamable, but they carry no IFC class, storey or GlobalId
  without extensions, no edges with their face normals (the drawing look), and a second
  geometry path (accessors, buffer views) to keep in step with the scene. The view file is the
  scene, so it cannot drift from what the viewer shows. An export to them can come later.
- *Fragments (That Open), XKT (xeokit).* Formats of other viewers; same objection, plus their
  schemas.
- *The gzipped IFC.* 4–30× larger than the view file, and still parsed: no gain on open time or
  memory.
- *Deflate per chunk* (random access to chunks): not needed while chunks are read in order.
- *Property sets, proxies:* above.

## Revisit when

- The viewer shows property sets: add a `PSET` section (measure a compact table first).
- A slow machine or a browser spends more building proxies than downloading them would cost.
- Files over a few hundred MB of view file appear: draw chunks as they arrive (the format
  allows it) and fetch by URL (a `?model=` link) instead of only through the file picker.
