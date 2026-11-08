# ADR 0033 — Models too large for the machine: a memory budget, and a choice before a cut

- Status: Accepted (8 Nov 2026)
- Decided by: Pierre (a warning when a model needs more than half, then 80 %, of the memory;
  offer one storey, IFC class or material); implementation by Claude
- Reversal cost: low

## Context / Problem

A 200 MB electrical IFC crashed the viewer (one 742 MB vertex buffer against wgpu's 256 MiB
limit, fixed by splitting). Behind it, the real limits: peak memory reached 7.4 GB for an 872 MB
file, and a fixed reader cap of 10 M triangles silently cut two real models (415 MB, 758 MB),
at a different place on each load (the parallel threads shared the budget). The 758 MB model
has 21.0 M triangles; half of it was never shown. Old machines and browsers (wasm: 4 GB) need
Bimble to know before it runs out, and to let the user decide what to drop.

## Decision

1. **A memory budget**: 80 % of the machine's memory (of 4 GB in a browser; 8 GB assumed where
   it can't be read). The viewer's triangle cap is what fits in it, not a constant.
2. **An estimate from what is known**, fitted on 17 IFC files (0.2–872 MB, peak footprint of
   `bimble render`, GPU included; within 16 % on every file over 25 MB):
   reading = 1.2 × (text + 7.5 × displayed bytes); showing = 1.2 × (480 MB + 6.2 × displayed
   bytes + 300 B × triangles). Displayed bytes come from a scan that parses nothing: all but
   property sets and quantities, which the display never parses. `scripts/calibrate_memory.sh`
   measures and fits again.
3. **Three outcomes**, in this order:
   - reading alone would not fit: ask before reading (read anyway / cancel);
   - read, then mesh up to what fits: when everything fits (the usual case), done — no extra cost;
   - otherwise a **census** (every element meshed once, only its triangle count kept: 2–6 s on
     the largest files) prices every choice exactly: storeys, IFC classes or materials, each
     enabled only if it fits; or all of it anyway.
4. **Parsed** (`bimble_ifc::read::Parsed`) owns the parsed file between reading and meshing, so
   the browser pauses there the same way (no threads).
5. **A partial model says so**: a "Showing part: …" chip (ADR 0028); its × opens the file again.
6. **The cut is deterministic**: meshing in file-order batches, the cap stops at the first
   element that doesn't fit. The reader's own cap (10 M, CLI and links) stays as a guard.
   Import (ADR 0032) never opens partially: a partial copy would silently drop elements.

## Alternatives considered

- Estimate triangles from entity counts: off 2–10× (one mapped item is 10 or 10 000 triangles).
- A fixed share of 50 %: Pierre's first proposal, raised to 80 % (a model is the main job).
- Cut at the budget in file order without asking: what the cap did; drops what matters as
  readily as what doesn't.

## Evidence

- 758 MB model, this M-series Mac (budget 20.6 GB): 21.0 M triangles shown in 10.2 s (was 10 M,
  cut). With an 8 GB budget: asks after 12.7 s; all 33 storeys, 7 of 8 classes, 68 of 69
  materials fit alone.
- Tests: `a_model_is_loaded_or_asked_about_by_its_cost`, `census_counts_what_a_filtered_scene_draws`,
  `the_triangle_cap_keeps_the_first_elements_in_file_order`.

## Revisit when

- Instancing or compact vertices land: 300 B/triangle changes; re-run the calibration.
- The budget becomes a setting, or degrading (LOD, culling) replaces choosing.
- Browser coefficients: calibrated on native memory, which overestimates the wasm heap (safe).
