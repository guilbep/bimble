# ADR 0048 — One API for the UI, the CLI and agents; Vue as the UI prototyping lab, egui follows

- Status: Accepted as a direction (20 Nov 2026). The tickets that build it are not yet approved.
- Decided by: Pierre ("have the native UI fall behind and have the Vue UI for fast prototyping, and
  once the UI is stabilized we redo the egui one"). Options, scores and measurements by Claude in
  `docs/research/2026-11-20-ui-architecture.md`.
- Reversal cost: low for the API (it moves code that exists), medium for a second UI
- Touches: ADR 0020 and ROADMAP open decision 3. ADR 0020 rejected a JavaScript viewer, which writes
  plans, styles and editing twice. A Vue UI over the same core duplicates only the UI. Open decision 3
  is a native window or Tauri + web.

## Context / Problem

Pierre wants to iterate on the UI quickly for the next weeks, and wants Bimble to be AI-native: an
agent creates and edits models from text, voice or a PDF plan. The proposal was two web UIs (egui,
Vite + Vue) over one API. Today the high-level verbs ("place a slab") live in the viewer
(`edit.rs`). There is no query API, no schema and no MCP entry point.

## Decision

1. A crate `bimble-api`: verbs moved out of the viewer (material by name), queries, JSON schemas,
   size budgets at the boundary. The egui viewer, the CLI, wasm-bindgen and MCP call it.
2. `bimble mcp` over stdio with few tools: list verbs, describe a verb, apply, query, undo, render a
   view to PNG. No code execution: parameters are data.
3. Vue (Vite) is the UI prototyping lab, in the browser. It owns panels, properties, schedules,
   Home and chat. The 3D and paper views stay the egui canvas in a "viewport only" mode, with their
   in-view tools (view cube, measure, section handles, sheet tools). One wasm instance, one model.
4. The rule that keeps the port cheap: all logic is in Rust behind `bimble-api`. A Vue component
   holds layout and panel state only. Every action it offers is an API call.
5. The egui UI lags on purpose. It gets fixes, not new screens. A screen is "stable" when Pierre says
   so. Then it gets a short spec (screenshot, states, actions) and is ported to egui.
6. When most screens are stable, an ADR chooses: port them all to egui, or ship the Vue UI on the
   desktop too (Tauri), measured on an old machine first (ROADMAP principle 1).

## Alternatives considered

The research note scores four more options:
- B: a Vue shell around the egui canvas.
- C: Vue everywhere, with an egui-free viewport and Tauri.
- D: two full UIs side by side.
- E: chat first.

Chosen: B, with the drift made deliberate (egui lags, then follows). D is rejected, because every
feature is built twice at parity. A stays for the native app's fixes. C is decided later (point 6).

## Risks

- A layout easy in CSS is hard in egui. Mitigation: port one screen early, to learn the cost.
- The office tests 0.15 on the desktop (issue #67). Their requests land in egui (fixes) or in Vue
  (new screens). Each request says which.
- Logic written in TypeScript by mistake. Mitigation: review checks that Vue only calls the API.

## Evidence

- Edit loop on egui, one string changed in `ribbon.rs`, Apple M4 Pro: native dev 2-3 s, web dev
  5-7 s, native release 12 s (2 runs each).
- Viewer: about 13.7k of its 19k lines are egui, about 5.3k are logic or mixed (estimate).
- Other tools converge on MCP (Revit preview, Archicad 30 beta, Bonsai, ifc-lite, Pascal Editor).
  The Archicad Tapir MCP uses five meta-tools over 800+ commands. A 50-tool IFC MCP costs about 40k
  tokens of context (MCP4IFC). Sources in `docs/research/2026-11-20-state-of-the-art.md`.

## Revisit when

- The first Vue screen is built: measure its edit loop against the egui numbers above.
- The first screen is ported to egui: measure the port cost.
- An agent drives `bimble mcp` on a real task (for example a storey of slabs and columns traced over
  a reference PDF) and the failures are known.
