# ADR 0050 — Two web builds: a small viewer and the full editor

- Status: Accepted (20 Nov 2026)
- Decided by: Pierre ("two different web builds: the viewer, IFC and `.bimbleview` only, and the
  editor, which may have DXF/PDF and way more MB"). Measurements by Claude.
- Reversal cost: low (cargo features and a build script)
- Amends: ADR 0020 (one web build), ADR 0040 (`.bimbleview` for sharing)

## Context / Problem

The browser build grew from 14.1 MB (4.3 MB gzipped, v0.5.0) to 22.2 MB (6.9 MB gzipped), measured
on 20 Nov (`docs/research/2026-11-20-wasm-size.md`). Most people open Bimble in the browser to view
IFC exports (ROADMAP principle 9). With the CPU slowed 4x and slow 4G, Lighthouse stopped the page
after 45 s at 7.8 MB of 23 MB, served without compression: the viewer did not start.

## Decision

- Two web builds from the same crates:
  - Viewer: open IFC, `.bimble` and `.bimbleview`, look, measure, section. No reference plans (DXF,
    PDF). A further feature leaves it only after a measurement.
  - Editor: everything, as today. The Vue lab (`web-ui/`, ADR 0048) uses it.
- Both builds: no name section in release. A size-optimized wasm profile, kept only if the
  measurement shows it smaller.
- Each size change is measured before and after. Page bytes are budgeted in `web-ui/perf/`.

## Alternatives considered

- One build, loading reference plans lazily as a second wasm module: smaller first load, but two
  modules to keep in step. Not now.
- One build, smaller only through compiler settings: not enough alone (estimate 15 to 30 %).

## Revisit when

The viewer build is measured on a slow connection and an old laptop.
