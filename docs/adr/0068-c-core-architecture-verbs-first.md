# ADR 0068 — The C core's architecture: verbs first, one call function, a command journal

- Status: Accepted, 25 Nov 2026
- Decided by: Pierre delegated the decision ("I won't be there to validate the next ADR, so accept
  everything and advance without me! let's follow fable5.2 recommendation"). Recommendations
  from Fable 5.2, `docs/research/core-architecture.md` (its "Decisions for Pierre" table).
- Reversal cost: medium. The spine is cheap now and expensive once the port grows.
- Extends: ADR 0067 (move to C), ADR 0048 (verbs for agents), ADR 0042 (binary .bimble),
  ADR 0038 (git-like sync)

## Context

Pierre asked how the core should talk to everything around it: "IPC / MCP / verb first /
database talk directly?". The C core already has one call function (`cc_call`) behind both
the wasm module and the local HTTP host. It also has verbs that return Commands, and Commands
that validate and return their inverse. At 20,000 to 60,000 elements, the dev loop spends 100–800 ms per change
rewriting the whole session file (`ccore/results/large.md`).

## Decision

1. **Verbs first.** Every writer goes through verbs: the UI, agents, MCP, CLI and tests.
   Commands are the internal and storage format. `rename` and `set_project` move from raw
   `apply` to verbs.
2. **Five call kinds**: query (pure), verb (one undo step), tool (interactive state in C: drag,
   snap, ghost; one verb on release), session (selection, visibility, tabs), job (long work that
   reports through the change feed).
3. Every verb takes `dry_run` and `if_rev`. Each verb has one static table entry, which gives
   its JSON schema, its validation and its documentation.
4. **One call function, several carriers**: wasm in the page, local HTTP, MCP over stdio (then
   `/mcp` on the local server), CLI. `bimble.ts` stays synchronous; long work becomes jobs.
   No WebSocket or shared memory for now.
5. **MCP in C** from the verb table, with few meta-tools and modes read-only / propose / edit.
   Every step records its actor.
6. **Persistence: a journal of Commands** beside an unchanged `.bimble` snapshot. A change
   appends one line; a restore replays the lines. In the browser the snapshot and journal live
   in the browser's own storage (OPFS, else IndexedDB; Pierre: "maybe even indexDB in the
   frontend"), so a reload restores the model and its undo history, also offline.
   **SQLite is not the model file** for now: ADRs 0003, 0017 and 0042 chose a file format, and
   SQLite compiles to 590–750 KB against a 244 KB viewer wasm. It may come later as an optional
   native module for analysis (Mission 3).
7. **No live co-editing now.** Sync stays git-like (ADR 0038); a relay later if asked.
8. **Layers L0–L6** (base, model, io and geometry, api, store, session, carriers). A file
   includes only its own layer and lower ones. L0–L3 have no files, clock, threads or globals.
   The session is a handle, so several models can be open at once (Mission 1: superposed files).
   One error style.
9. **The 2D view**: WebGL2 with an orthographic camera and 2D primitives computed in C; pick and
   snap in C. Dimensions are model objects with their own Commands and feature anchors. This is a
   deliberate deviation from the oracle, recorded in `ccore/golden/DEVIATIONS.md`.
10. **Full IFC compatibility is the goal** (Pierre: "we need 100% compatibility"). The model
    holds open classes, relations and types as objects with GlobalIds: assemblies
    (`IfcElementAssembly`) and typed connections (`IfcRelConnects…`). This leaves room for facades
    and the RECLAIMED connection vocabulary without changing the product scope (ADR 0003).
11. The mixed clang/tcc dev build is measured before it is adopted.

## Consequences

- The ticket plan of `docs/research/core-architecture.md` (A1–A7, B1–B7, C1–C6) is the work
  order, in Pierre's priority: IFC in and out and `.bimble` files first, then 3D and the UI with
  the section box, then the dynamic 2D view. PDF later.
- Two writers can no longer bypass validation: the lab's raw `apply` calls become verbs.
- A web reload no longer loses work.

## Not decided here

- SQLite as an analysis store: revisit when Mission 3 needs queries the model cannot answer.
- Live co-editing: when a user asks for it.
