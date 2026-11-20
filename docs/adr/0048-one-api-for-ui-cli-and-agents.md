# ADR 0048 — One API for the UI, the CLI and agents; UI iteration on egui first, a Vue spike measured

- Status: Proposed (20 Nov 2026)
- Decided by: not yet. Pierre asked for fast UI iteration and an AI-native API. Options, scores and
  measurements by Claude in `docs/research/2026-11-20-ui-architecture.md`.
- Reversal cost: low for the API (it moves code that exists), medium for a second UI
- Touches: ADR 0020 (it rejected a second code base for the browser), ROADMAP open decision 3
  (native window or Tauri + web)

## Context / Problem

Pierre wants to iterate on the UI quickly for the next weeks, and wants Bimble to be AI-native: an
agent creates and edits models from text, voice or a PDF plan. The proposal was two web UIs (egui,
Vite + Vue) over one API. Today the high-level verbs ("place a slab") live in the viewer
(`edit.rs`). There is no query API, no schema and no MCP entry point.

## Decision (proposed)

1. A crate `bimble-api`: verbs moved out of the viewer (material by name), queries, JSON schemas,
   size budgets at the boundary. The egui viewer, the CLI, wasm-bindgen and MCP call it.
2. `bimble mcp` over stdio with few tools: list verbs, describe a verb, apply, query, undo, render a
   view to PNG. No code execution: parameters are data.
3. UI iteration stays on egui for now: screens designed on a canvas first, a flag that opens the app
   in a given state, so each 2-3 s native build does not cost a manual replay.
4. A time-boxed spike: a Vue page with one panel (properties + chat) around the existing egui canvas,
   web only. Its edit loop is measured against the egui numbers. An ADR decides after it whether
   Vue becomes the UI (with Tauri on the desktop) or not.

## Alternatives considered

The research note scores four more options:
- B: a Vue shell around the egui canvas.
- C: Vue everywhere, with an egui-free viewport and Tauri.
- D: two full UIs side by side.
- E: chat first.

D is rejected, because every feature is built twice. C is not rejected. It needs the spike's numbers
and a measurement of Tauri's web view on an old machine (ROADMAP principle 1).

## Evidence

- Edit loop on egui, one string changed in `ribbon.rs`, Apple M4 Pro: native dev 2-3 s, web dev
  5-7 s, native release 12 s (2 runs each).
- Viewer: about 13.7k of its 19k lines are egui, about 5.3k are logic or mixed (estimate).
- Other tools converge on MCP (Revit preview, Archicad 30 beta, Bonsai, ifc-lite, Pascal Editor).
  The Archicad Tapir MCP uses five meta-tools over 800+ commands. A 50-tool IFC MCP costs about 40k
  tokens of context (MCP4IFC). Sources in `docs/research/2026-11-20-state-of-the-art.md`.

## Revisit when

- The Vue spike is measured.
- An agent drives `bimble mcp` on a real task (for example a storey of slabs and columns traced over
  a reference PDF) and the failures are known.
