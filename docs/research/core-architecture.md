# The C core and its interfaces: architecture

25 Nov 2026. Fable 5.2. Study only: nothing was built or run. **M** = measured or read in the
repo (source cited). **E** = estimate. **I** = inference, not checked in code or on a machine.
Paths are relative to this worktree (`rewrite/c-core`). Short paths: `ts` =
`web-ui/src/bimble.ts`, `large.md` = `ccore/results/large.md`, `api/` = `crates/bimble-api/src/`.
It extends `docs/research/future-architecture.md` (shells, builds, port order).

Pierre's question: "IPC / MCP / verb first / database talk directly?" Short answer:

- **Verb first.** One write path for the UI, agents, the CLI, tests and sync.
- **One call function, several carriers.** MCP and HTTP are carriers, not second APIs.
- **Nobody talks to a database directly.** The store is behind the Commands. The file stays
  `.bimble`; an append-only journal of Commands fixes the measured save cost. SQLite is not
  the model file now; it fits later for analysis (Mission 3).

## 0. The picture

```
 Vue page      CLI      external agent      in-app agent      other user
    |           |       (MCP stdio/HTTP)    (chat panel)      (sync)
    +-----------+--------------+------------------+---------------+
                               |
          cc_call(session, op, name, body) -> status, bytes     <- the one waist
                               |
   +---------+---------+-------+-------+----------+-----------+
   | query   | verb    | tool          | session  | job       |
   | read    | intent  | pointer ->    | select,  | open,     |
   | only    | checked | snap, ghost   | hide,    | export,   |
   |         |         | (no change)   | box, tab | sync      |
   +---------+----+----+-------+-------+----------+-----------+
                  | Commands (validated, canonical, inverse)
                  v
        History.apply -> model (GlobalIds) -> journal (append) -> change feed
                  |
        derived, never stored as truth: scene, BVH, cuts, drawings, quantities
```

## 1. The API spine

**Recommendation: every model change enters as a verb. Commands become the internal and
storage format.**

Facts:

- A verb already only *returns* Commands; the caller applies them (M, `ccore/src/verbs.h:2-3`,
  `:20`). So a dry run costs nothing: run the verb, do not apply.
- Commands validate, make canonical, and return their inverse (M, `ccore/src/command.h:68-74`).
  Verbs add budgets: 10,000 ids, ±10 km (M, `api/dispatch.rs:13-15`).
- The lab bypasses verbs twice: `rename` and `set_project` go through `apply` with raw
  Commands (M, `ts:606`, `ts:616`). The Rust MCP server exposes verbs only (M,
  `crates/bimble-cli/src/mcp.rs:104-131`).

Why split the roles: a Command is written to files and logs and must replay for years; a
verb is an API and must be free to evolve (I).

| Kind | Changes | Undo | Logged | Examples |
|---|---|---|---|---|
| **query** | nothing; pure function of (rev, args) | — | no | `elements`, `element`, `pick`, `view_draw`, `quantities` |
| **verb** | the model, one History step | yes | yes | `place`, `move`, `add_dimension`, `set_view` |
| **tool** | tool state only; answers a preview | — | no | `tool place_beam`, pointer moves |
| **session** | what this user sees | no | no | `select`, hide "for now", section box, tab, camera |
| **job** | long work, progress in the change feed | per job | result only | `open`, `import_ifc`, `export`, `synchronize` |

`apply` (raw Commands) stays for tests, replay and sync. It is not offered to agents, and the
lab's two uses move to verbs.

**Drag, snap, ghost.** Interaction state lives in C as a *tool* (as Rust's `Editor::click`,
`crates/bimble-viewer/src/edit.rs:318`). The page sends pointer rays; C answers
`{snap, ghost, hint, valid}`. The ghost is the mesh of the Commands a dry run would give.
While dragging, the page moves the existing GPU ranges by the snapped vector; the model is
untouched. On release, C runs **one** verb: one step, one journal record. Rejected: a
Command per pointer move, merged later. It rebuilds the scene on every move and floods the
journal (I).

**Schemas and versions.** Rust derives schemas with `schemars` (M, `api/dispatch.rs:356-370`).
C has no derive. Proposal: one static table per verb (field, type, unit, min, max, required,
doc). From it: the argument parser, the JSON Schema, the MCP tool list, and generated
TypeScript types. The schema text is a golden against the oracle. `query api` answers
`{api: 1, verbs, queries, features}`. Within a major version changes are additive; a verb
never changes meaning.

Two arguments every verb accepts: `dry_run` (answer the Commands and a diff, change nothing)
and `if_rev` (refuse if the model moved since the caller read it). The second makes a UI and
an agent on one model safe (I).

## 2. Transport

**Recommendation: one protocol, `cc_call`, with four carriers. Keep `bimble.ts` synchronous.
Long work becomes jobs. JSON for calls, binary for bulk.**

The waist exists: `cc_call(op, name, body) -> status, text` (M, `ccore/host/calls.h:19`). The
wasm export `call` and the HTTP route `POST /<op>/<name>` are the same function (M,
`ccore/src/wasm_api.h:56-59`). The native and wasm answers are compared byte for byte (M,
`ccore/README.md:63-64`).

| Carrier | Use | Status | Verdict |
|---|---|---|---|
| wasm in the page, direct call | web viewer, web editor | exists | keep, sync |
| local HTTP, sync XHR + long-poll `/change` | desktop, dev loop | exists (`ts:276`, `ts:307`) | keep |
| stdio JSON-RPC | MCP, scripts | Rust only | port (section 3) |
| HTTP `/mcp` on the same server | agent attached to the open model | new | add after stdio |
| WebSocket | push | — | **not now**: long-poll already pushes (E 300 lines of C). Revisit for live presence |
| shared memory for meshes | — | — | **no**: a browser cannot map it. Loopback is not the cost: 14.4 MB fetched in 20.7 ms (M, `large.md:161`). The rebuild is (220 ms, `large.md:123`) |

**Sync or async.** A verb or query takes 1–4 ms up to 60k elements (M, `large.md:118`, `:143`).
Sync is right for those and wrong for `open` and `export`, which block the page. Jobs fix
this without touching Vue callers: `job start` answers an id at once; progress and the result
arrive through the change feed. CI rule: no sync call over 16 ms on the 20k fixture (E).

One limit: synchronous browser file storage needs a Worker (section 4). Keep the core on the
main thread; a small Worker appends the journal records. The interface stays sync (I).

**Binary.** Numeric arrays and anything over about 100 KB go binary with a versioned header:
BMSH today, a **BMSH delta** (section 7) and a 2D primitive buffer (section 6) next.
Everything else is canonical JSON.

## 3. MCP and agents

**Recommendation: port `bimble mcp` to C over stdio first, then `/mcp` on the local server.
Few meta-tools. Agent work is grouped, labelled, dry-runnable and bounded.**

- MCP defines two transports: stdio (newline-delimited JSON-RPC) and Streamable HTTP (one
  endpoint; the server must validate `Origin` and should bind 127.0.0.1; a POST may be
  answered with plain JSON and GET with 405, so no SSE is needed)
  (https://modelcontextprotocol.io/specification/2025-06-18/basic/transports.md). The C host
  already checks token, `Origin` and `Host` (M, `ccore/host/server.c:473-479`).
- Tools carry `inputSchema`, optional `outputSchema`, `annotations`, and may return resource
  links (https://modelcontextprotocol.io/specification/2025-06-18/server/tools).
- Keep the Rust shape: 7 tools over 14 verbs (M, `mcp.rs:104-131`, `dispatch.rs:23`). A
  50-tool IFC server costs about 40k tokens (M, `docs/adr/0048-one-api-for-ui-cli-and-agents.md:58-60`).

| Tool | Notes |
|---|---|
| `list_verbs`, `describe_verb`, `run_verb` | `run_verb` takes `dry_run`, `if_rev`; answers ids added and a field diff |
| `query` | includes `quantities`, `pick`, `diff` |
| `undo`, `redo`, `save` | as today |
| `view` (new) | a view as SVG text or a structured summary (`ROADMAP.md:32-33`); replaces PNG |

Resources: `bimble://view/<gid>.svg`, `bimble://element/<gid>`, `bimble://schedule/<gid>.csv`.
No meshes for agents (`api/lib.rs:2`).

Safety, in order of value:

1. **Actor on every step** (`user`, `agent:<name>`). "Undo everything the agent did" is one
   action.
2. **Modes at launch:** `--read-only`, `--propose`, `--edit`. Propose = the agent's steps go
   to a branch (ADR 0017) and the user accepts the semantic diff.
3. **Budgets:** the verb limits, steps per minute, no path outside the project.
4. **Parameters are data** (CLAUDE.md). No tool evaluates text.

**The in-app agent (Mission 3)** needs no MCP: the chat panel maps a model's tool calls to
`cc_call` in process. Both tool lists come from the same verb table (I).

## 4. Data and persistence

Ranked by evidence:

**1. An append-only journal of Commands next to the snapshot. Do it first.**

- M: with the session file, each change rewrites the whole model: 100 / 260 ms at 20k and
  314 / 799 ms at 60k (clang / tcc; `large.md:127`, `:152`). The code saves the full model and
  embeds it as a JSON string (M, `ccore/host/session.c:306`, `:318`). Restore is 4× a plain
  start (M, `large.md:189-191`).
- M: ADR 0005 planned it: "the history can become the operation log"
  (`docs/adr/0005-commands-undo-transactions.md:16`).

```
house.bimble            snapshot. Unchanged: v1 JSON, v2 gzip of CBOR (ADR 0042).
                        Same model, same bytes. Written on Save (write, rename).
house.bimble.journal    append-only. Dropped after a clean Save.
  header : format, base = model hash of the snapshot it follows
  record : u32 length | payload | u32 check (BLAKE3, truncated)
  payload: {seq, time, actor, label, verb, args, commands[]}   one History step
           {seq, undo: n} | {seq, redo: n}                     stack moves
           {seq, ui: {selection, tab, placing}}                small, last one wins
           {seq, check: model hash}                            every N steps
house.bimble-history/   commits, branches, objects by hash (ADR 0017). Unchanged.
```

- **A row is one user step**, never one field. The record holds *Commands*, not only the
  verb: a verb creates GlobalIds, and replay must not create new ones (CLAUDE.md).
- **Undo** is a record, not a deletion. Replay rebuilds both stacks because `apply` returns
  the inverses again.
- **Open** = load the snapshot, then replay the journal if its base hash matches. A torn last
  record is dropped. A failed `check` stops with an error; nothing is guessed.
- **Cost (E):** a write of about 0.5–2 KB per step, under 1 ms. Restore at 20k: plain load
  77 / 158 ms (M, `large.md:111`) plus replay.
- **Mission 1, "history of changes":** who, when and what in words, per step. A commit
  (ADR 0017) seals a stretch of it.
- **Web:** records are appended in OPFS by a Worker, or as IndexedDB rows (I).

**2. Sync stays state-based and git-like (ADR 0038). The journal prepares live sync.**

*Offline first, then Synchronize* is decided and partly built: merge by GlobalId, then field
by field (M, `docs/adr/0017-history-store.md:24-29`). *Live co-editing* was rejected because a
merged model must stay valid (M, `docs/adr/0038-collaborate-like-git.md:72-73`). I agree: a
CRDT keeps fields convergent but not references valid, for example an element on a removed
storey (I). If live work is wanted later: a small relay orders the steps; each client
validates remote steps and rebases its unsent ones; borrowing (ADR 0038) keeps conflicts rare.
The journal record is already the message (I).

**3. SQLite: not the model file now. Yes later, for analysis and for packed history.**

| For | Against |
|---|---|
| One file, public domain (https://sqlite.org/copyright.html) | 238K lines, over 8.4 MB of source (https://sqlite.org/amalgamation.html) against a 15K-line core |
| Atomic, writes only changed pages (https://sqlite.org/appfileformat.html) | 590–750 KB compiled at -Os (https://sqlite.org/footprint.html); the viewer wasm is 244 KB today |
| SQL for Mission 3; language models write SQL well (I) | The model is loaded whole; random access is not used (M, `docs/adr/0042-every-element-and-a-binary-bimble.md:56-57`) |
| A ready index for packed objects (ADR 0017 revisit at 50k) | File bytes are not canonical. Hashes survive (ADR 0042:32); "same model, same file" does not |
| | Web: OPFS needs a Worker; the full VFS needs COOP/COEP headers; the pool VFS has one connection (https://sqlite.org/wasm/doc/trunk/persistence.md). Our wasm has no libc (`ccore/README.md:17`): shims and a VFS to write (E days) |
| | ADRs 0003:31-32 and 0017:37-38 chose against it. The new evidence, the rewrite cost, is solved by the journal |

Its session extension gives invertible changesets (https://sqlite.org/sessionintro.html), but
per row. Commands already do this, with meaning.

Where SQLite earns its place, as an **optional native module** (never in the viewer wasm):

```sql
-- derived, rebuilt from the model at a rev; read-only for agents (`query sql`, export)
element(gid TEXT PRIMARY KEY, class, ifc_entity, name, storey, material, hash BLOB, body BLOB);
storey(gid PRIMARY KEY, name, elevation_mm);   material(gid PRIMARY KEY, name, category, density);
quantity(gid PRIMARY KEY, volume_m3, area_m2, length_m, mass_kg);       -- from generators
rel(gid PRIMARY KEY, kind, relating, related);  property(gid, pset, name, value);
-- if it ever becomes the store: one transaction per step
op(seq INTEGER PRIMARY KEY, time, actor, label, verb, args, commands BLOB);
object(hash BLOB PRIMARY KEY, data BLOB);       ref(name TEXT PRIMARY KEY, hash BLOB);
```

A row is one IFC object keyed by GlobalId; `body` is its canonical bytes, so hashes do not
change. Triggers to make SQLite the store: an edited model that does not fit in memory; Save
over 1 s at the target size; two processes on one file.

## 5. Module layout

```
L6 carriers   wasm_api | http | mcp | cli            (platform code lives only here)
L5 session    call router, sessions, selection, visibility, tools, jobs, change feed
L4 store      journal, repo (objects, merge), remote
L3 api        verbs, query, schema tables, views (2D), quantities | draw: plan, svg, pdf, dxf
L2 io         file (v1, v2), step, ifc_in, ifc_out, viewfile | geom: mesh, scene, bvh, cut, boolean iface
L1 model      model, validate, canonical, command, history, hash, diff
L0 base       arena, buf, json, cbor, inflate, fmt, strtod, blake3, guid, math
```

Rules: a file includes only its own layer and lower ones. L0–L3 have no files, clock, threads
or globals, so they run unchanged in freestanding wasm. Carriers pass a `cc_platform` (read,
write, now, random, run job).

What breaks the rules today (M):

- One session per process, a global (`ccore/host/session.h:3`, `:46`). Mission 1 needs several
  files at once. Make it a handle.
- `host/calls.c`, `session.c`, `mesh.c` are compiled into the wasm (`ccore/build.sh:15`). They
  are L5, not "host". Move them.
- `verbs.h` includes `scene.h` for `cc_sqrt` (`ccore/src/verbs.h:24`). Move maths to L0.
- The scene cache is a static in `calls.c` (`ccore/host/calls.c:154-171`).

| Build | Layers |
|---|---|
| `ccore-view.wasm` | L0, L1, readers of L2, scene, bvh, cut, query, light-edit verbs, L5, wasm_api |
| `ccore-edit.wasm` | all but threads, http, mcp, remote |
| `bimble` native | all |

**Manifold boundary.** ADR 0010 names a three-function interface
(`docs/adr/0010-geometry-kernel.md:5`). In C: a table `cc_boolean {difference, intersection,
is_closed}` over `cc_mesh`, set at start. Native: manifold's C binding. Web: imports that
call manifold's own wasm. No kernel: drawn uncut with a warning (ADR 0010:26). 3D printing
uses the same table.

**Threads.** One writer: the session thread owns the model. Parallel work is pure functions
on a model frozen for the job: per-element meshing on pthreads (ADR 0012), one scratch arena
per thread (it exists, commit `8f3bf6f`). Web: heavy jobs in a Worker with its own instance.

**Memory.** Arenas by lifetime: model, History step, scene element, call (reset after each
`cc_call`), job. Each has a cap (ADR 0008). One problem to fix: applied Commands are copied
into the model's arena and History borrows from it (M, `command.h:69-70`,
`ccore/src/history.h:3`), so a long session only grows (I). Give steps their own arena and
compact the model arena on Save.

**Errors.** Three styles today: `cc_error` (`model.h:126-132`), `Buf *err` (`command.h:74`),
HTTP-like ints (`query.h:14-17`). One style: return a `cc_status` (ok, invalid, not_found,
conflict, too_large, unsupported, no_memory, internal) and fill `{key, text, offset, gid}`.
`key` is stable for agents and translation; `text` keeps Rust's words for the goldens. No
abort; out of memory is an error value.

## 6. Views: the dynamic 2D view

**Recommendation: C owns the view, the cut, snapping and hit-testing. The page draws two
layers: the model through WebGL2 with an orthographic camera, and C's 2D primitives on top.**

- 2D views already reuse the 3D renderer (M, `ROADMAP.md:105`); one view has two pictures,
  Model and On paper (ADR 0046). Keep both.
- A view is a model object with a GlobalId, a kind, a range (ADR 0021).
  `query view_draw {view}` answers a binary list of primitives in view mm: cut polygons with a
  hatch id, seen edges, grid axes, dimensions, texts, each with its GlobalId. The page strokes
  them on a Canvas 2D or SVG layer.
- **Step 1 (fast):** ortho camera + clip plane in the shader; C gives grid, dimensions, texts.
  **Step 2 (exact):** C cuts each solid by the plane (port of `drawing/cut.rs`). The same
  primitives later feed SVG, PDF and DXF, so screen and paper cannot drift.
- **Pick and snap in C.** Today the page scans every triangle in JS: 26 ms at 20k, 62 ms at
  60k, 80–215 ms cold (M, `large.md:165`). A box tree in C (ADR 0034) answers
  `query pick {view, ray | xy, tol}` with `{gid, point, snap kind}`. It honours the section
  box and hidden elements. One implementation for 3D, 2D, measuring and agents.

**Dimensions are model objects.** They exist: `Annotation::Dimension {id, a, b, offset_mm,
axis, kind}` with anchors `{at, element}`, stored in their view (M,
`crates/bimble-core/src/view.rs:79-103`, `:132-136`; ADR 0024). Three changes:

1. Anchors name a feature (`element`, `feature: "axis:start" | "corner:2" | "face:top"`), so a
   dimension follows its element (planned, `docs/adr/0024-sheets.md:25-26`).
2. Own Commands (`add_annotation`, `set_annotation`, `remove_annotation`). Today one edit
   replaces the whole view (`SetView`, M, `crates/bimble-core/src/command.rs:51-52`): large
   records, coarse merges. This is a deviation from the oracle; it goes in `DEVIATIONS.md`.
3. Verb `set_dimension_value`: typing a value moves or resizes the element
   (`ROADMAP.md:31`). Export as `IfcAnnotation` with 2D curves and text, plus a Bimble
   property set to read it back (I: IFC4 has no dimension entity; the buildingSMART page
   refused the fetch).

**Section box.** Display clipping stays in the shader (ADR 0014: discard, back faces as cut
surfaces). C holds the box as session state, or in the view through a verb. C computes exact
cuts only for 2D views and paper. Auto-floorplan = one derived plan view per storey with a
default range.

## 7. Dev loop at scale

M, tcc host at 20k (`large.md:189-193`): 2,090 ms = build 78 + stop 4 + restore 681 +
scene and BMSH about 750 + page about 600. At 60k: 5,955 ms.

| # | Change | Effect at 20k | Basis |
|---|---|---|---|
| 1 | Journal instead of the session rewrite (section 4) | restore 681 → about 170 ms; per change 260 → under 1 ms | E from M (`large.md:111`, `:127`) |
| 2 | Mixed build: unchanged files as clang -O2 objects, the edited file with tcc | scene + BMSH 747 → 257 ms; load 158 → 77 ms | M per compiler (`large.md:8-10`). I: tcc links clang objects on aarch64 |
| 3 | Scene per element: mesh cache keyed by generator hash, fixed scene origin, **BMSH delta** `GET /mesh?since=rev` | rebuild after an edit 220 / 715 ms → under 10 ms; no 14 MB refetch | E. Product value too: every user edit pays this today (`large.md:123`) |
| 4 | Keep the mesh across a restart when rev, model hash and geometry sources are unchanged (a new epoch refetches today, `web-ui/src/glviewport.ts:216-217`) | skips scene, fetch, upload | E 0.3–0.9 s |
| 5 | Pick in C; culling by chunk | pick 26 → under 1 ms | E; orbit needs a real GPU first |

With 1, 2 and 4: about 0.5–0.7 s at 20k and 1.0–1.5 s at 60k (E). Later option: keep the
process and reload the code as a shared object. Not before 1–3 are measured.

Measure with `bench/large.mjs`, which times each stage: one commit per change, before and
after, as T16 did. `bench/lab_loop.sh` on the 2k model is the CI budget. Daily work uses a
small model: 0.23–0.39 s today (M).

## 8. Scope: RECLAIMED, facades, connections

Four choices allow them without deciding ADR 0003. Each is cheap now and costly once files
exist:

1. **Open classes.** Today an enum of six (M, `ccore/src/model.h:26-34`) plus a proxy with
   `ifc_entity`. Store the IFC class name; keep a table of classes with what each can do.
2. **Relations as objects with GlobalIds**: one table `{gid, kind, relating, related[]}` and
   two Commands. Kinds are IFC names: `IfcRelAggregates` (assemblies), `IfcRelConnectsElements`
   and `IfcRelConnectsWithRealizingElements` (typed connections, fasteners),
   `IfcRelVoidsElement`. Openings are embedded in the element today (M,
   `crates/bimble-core/src/model.rs:510-514`): keep reading that form.
3. **Types and occurrences** (`IfcRelDefinesByType`): a component is a type with a GlobalId;
   placed copies refer to it. Shared shapes already work this way (ADR 0042).
4. **Generators as a registry** (kind, version, parameters as data), as the
   `Bimble_Generator` property set assumes (ADR 0003).

Disassembly data is then properties on connections.

## Decisions for Pierre

| # | Decision | Recommendation |
|---|---|---|
| 1 | Verb first for every writer; raw Commands internal | **Yes.** Move `rename` and `set_project` to verbs |
| 2 | Tools in C for drag, snap, ghost; one verb on release | **Yes** |
| 3 | `dry_run` and `if_rev` on every verb | **Yes** |
| 4 | Keep `bimble.ts` sync; long work as jobs | **Yes.** Async only if the core moves to a Worker |
| 5 | WebSocket; shared memory | **No** for now |
| 6 | MCP in C: stdio, then `/mcp`; meta-tools; modes read-only / propose / edit | **Yes**, after the verb table |
| 7 | Journal of Commands beside `.bimble`; snapshot format unchanged | **Yes, first** |
| 8 | SQLite as the model file | **No now.** Optional native module for analysis; revisit on the three triggers |
| 9 | Live co-editing | **No.** Git-like sync (ADR 0038); a relay later if asked |
| 10 | Layers L0–L6, session as a handle, one error style | **Yes**, before the code grows |
| 11 | Dimensions with own Commands and feature anchors (deviation from the oracle) | **Yes** |
| 12 | 2D view: WebGL2 ortho + C primitives; pick in C | **Yes**, exact cuts as step 2 |
| 13 | Open classes, relations, types in the model now | **Yes**: small now, costly later |
| 14 | Mixed clang/tcc dev build | **Measure first** (one day) |

Each accepted row needs an ADR. Rows 1, 7, 8 and 10 are structural.

## Tickets, first 2–3 weeks

Pierre's order. Every ticket ends with a golden from the frozen oracle (ADR 0067) or a
benchmark.

| # | Ticket | Done when |
|---|---|---|
| **Week 1: files in and out, the spine** | | |
| A1 | Layers: `host/` call code to `session/`, session handle, `cc_status`, `cc_platform` | all suites green; two models open in one process |
| A2 | Journal replaces the session file | `large.mjs`: write per change and restore, before and after |
| A3 | IFC4 export (old T10) | `cmp` with `golden/ifc/frame.ifc`; `scripts/check_ifc.py` passes |
| A4 | `.bimble` v2: CBOR, inflate, deflate; read every Rust file | load v2 goldens, save, reload, same model hash |
| A5 | New model, Save as, export for the viewer (`.bimbleview`) | transcript golden; lab test |
| A6 | Verb table: schema output, `dry_run`, `if_rev`; verbs `copy`, `rotate`, `mirror`, `set_project_field`, `rename` | 14 transcripts equal; schemas equal Rust's |
| A7 | IFC display reader, widened by class family (largest item) | `scripts/compare_ifcopenshell.py` on the Rust fixtures |
| **Week 2: stable 3D and the UI** | | |
| B1 | Scene per element, fixed origin, BMSH delta | full scene still byte-equal; rebuild after `place` at 20k under 20 ms |
| B2 | Box tree; `query pick` (ray); the page stops scanning triangles | pick at 60k under 2 ms; lab test |
| B3 | Section box: shader discard, cut faces, handles; hide and isolate | pixel test; pick ignores clipped parts |
| B4 | Tool `place` in C: clicks, snap, ghost; move by drag | lab test: one undo step per gesture |
| B5 | Several models: link an IFC view-only, show and hide per model | two fixtures drawn; pick names the model |
| B6 | Mixed build, measured | table in `ccore/results/` |
| B7 | `bimble mcp` (stdio) from the verb table | transcript equal to the Rust server's |
| **Week 3: the dynamic 2D view** | | |
| C1 | Views in the model, derived plan per storey, ortho camera, view range | view goldens; lab test |
| C2 | `cut`: plane sections of solids; `query view_draw` | one plan SVG byte-equal to the oracle |
| C3 | Pick and snap in 2D | lab test: place a wall in plan |
| C4 | Dimensions and texts: Commands, verbs, feature anchors, overlay | move an element: the dimension follows; undo |
| C5 | Grids and levels (ADR 0056) | goldens of their own (no oracle) |
| C6 | Boolean table and manifold binding: openings | volumes equal to the oracle within tolerance |
| Later | PDF and DXF from the same primitives; `/mcp`; analysis module | — |

A7 and C2 carry the most risk (breadth; float order): start both early.
