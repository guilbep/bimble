# Round 5: every control of the ribbon does something

Plan of 26 November 2026, by the orchestrator, asked by Pierre ("plan the full implementation of
all the button not implemented"). Design choices are delegated ("don't wait for me to validate the
ADR, just do your best, I'll review once it's implemented"): each ticket records its choices in an
ADR with status Accepted, to be reviewed on the implementation.

## Where we stand

Counted from `web-ui/src/shell/ribbon-tabs.ts` on 26 November: a control is "bound to nothing"
when it has no action, page, panel or dialog. This is the ribbon only (107 controls, the Timber tab
aside); menus, pages, panels and the palette are counted by `ccore/tools/ux.mjs` (ticket UXMAP),
which also says whether a bound control really works. Until that report exists, "bound" below does
not mean "works".

| Tab | Controls | Bound to nothing on `main` (preview.3) | After the tickets merged or running on `next` |
|---|---|---|---|
| Project | 6 | 2 | 2 |
| Collaborate | 6 | 1 | 1 |
| Setting out | 8 | 5 | 1 (SETOUT, on `next`) |
| Site | 8 | 7 | 7 |
| Concrete | 11 | 6 | 6 |
| Rebar | 9 | 8 | 8 |
| Steel | 7 | 6 | 6 |
| Annotate | 9 | 2 | 0 if VIEWSHEET lands (running) |
| View | 15 | 2 | 0 if VIEWSHEET lands |
| Sheets | 8 | 2 | 0 if VIEWSHEET lands |
| Graphics | 7 | 5 | 5 |
| LCA | 5 | 5 | 5 |
| Quantities | 8 | 6 | 6 |
| **All** | **107** | **57** | **47** |

Most of the 57 never existed in Rust v0.19.0: the ribbon was copied from the wireframes as the
design target. They are new features, not port gaps, so there is no Rust reference to compare
with: each needs C-own tests with hand-computed values, an IFC mapping checked with IfcOpenShell
(`scripts/check_ifc.py`, `scripts/validate_ifc.py`), and a paper drawing where one applies.

## Rules for every ticket of this round

- A control is done when its full gesture gives its result on the 72-element fixture, with undo,
  save and reopen, IFC export, and its drawing on paper where it has one. A button that opens an
  empty dialog is not done.
- What is not finished shows "not available yet" through `ccore/session/capabilities.c`. No dead
  button is merged.
- Each interaction answers in under 200 ms (ADR 0074); the ticket prints its times, and a slower
  one shows progress within 200 ms and can be cancelled.
- One lab test per tab clicks every control of the tab and fails on a dead one
  (`lab_setting_out.mjs` is the model).
- IFC class names in code and UI; mutations through verbs and Commands; canonical save; a model
  without the new data keeps its bytes; new file fields validated as untrusted input.
- At most two browser-driving jobs at a time on the 16 GB machine; a full gate alone after each
  merge.

## Waves

Sizes: S under half a day of one builder, M a day, L several days and more than one ticket.
"Core has" is what I know from the tickets merged so far; UXMAP's study corrects it.

### Wave 1 — wiring what the core already has (running)

| Ticket | Controls | Core has | Size |
|---|---|---|---|
| SETOUT (merged on `next`) | Survey point, Project base point, True / project north, Project zero | georeference, levels, grids | done |
| VIEWSHEET (running) | New view…, Section, Place view, Revision, Text type, Dimension type | views, sections, sheets, sheet tools | M |
| MEAS1 | Measure is slow on a mesh-heavy model (about 1,750 elements) | the tool exists; the cause comes from UXMAP's profile | S–M |
| SETOUT2 | Reference planes; the two-click grid axis tool; grids in 3D; base point moved by clicks | grids, work plane of the placing tools | M |

### Wave 2 — concrete, the first mission's core (IFC classes exist, geometry is prisms)

| Ticket | Controls | IFC | Size |
|---|---|---|---|
| CONC1 | Opening, Sleeve | `IfcOpeningElement` + `IfcRelVoidsElement` (the cut exists: box and prism); a sleeve is a round opening with a `PredefinedType` | M |
| CONC2 | Footing, Raft | `IfcFooting` (PAD_FOOTING, STRIP_FOOTING), raft as `IfcSlab` BASESLAB; the fixture already has pad footings as columns' bases | M |
| CONC3 | Stairs, Ramp | `IfcStair` + `IfcStairFlight`, `IfcRamp` + `IfcRampFlight`; straight flights first (rise, going, width, waist), drawn in plan with the cut line and arrow | L |

### Wave 3 — what is drawn and counted

| Ticket | Controls | Notes | Size |
|---|---|---|---|
| GFX1 | Line types, Text types, Dimension types, Hatches, Pens per scale | pages over `query standards` (today a stub: the standards must first be served and saved); feeds Annotate's type selectors | L |
| QTY1 | New quantities, Columns, By material | schedules exist (`schedules.c`); a schedule becomes a saved object with chosen columns, filter and grouping | M |
| QTY2 | Assign CFC, CFC catalogue | the Swiss cost code (eCCC-Bât / CFC): a catalogue as data, a property on elements, a column of the schedules; the catalogue's licence must be checked before any of it is committed | M, blocked on the data's licence |
| PROJ1 | Global parameters, Shared parameters, Active sub-project | named values and property definitions (`IfcPropertySetTemplate`); the sub-project selector needs the sub-projects page to be real first | M |

### Wave 4 — rebar (the first whole domain; concrete drawings need it)

| Ticket | Controls | Notes | Size |
|---|---|---|---|
| RBR1 | Bar, Shape codes | `IfcReinforcingBar` with a swept disk along a polyline; shape codes of SIA 262 / ISO 3766 as data; host element and cover | L |
| RBR2 | Stirrup, Lap, Rebar area | sets of bars along a path, spacing rules, laps by diameter | L |
| RBR3 | Mesh | `IfcReinforcingMesh`, standard Swiss meshes as data | M |
| RBR4 | Bar schedule (also the Quantities tab's), Rebar drawing | the schedule by mark, shape, length and mass; bars drawn in plans and sections with marks | L |

### Wave 5 — steel

| Ticket | Controls | Notes | Size |
|---|---|---|---|
| STL1 | Sections, Column, Beam | a profile catalogue (European I and H, hollow sections) as `IfcIShapeProfileDef` and others; placing reuses the concrete tools | L |
| STL2 | Bracing, Base plate, Connection | `IfcMember`, `IfcPlate`, `IfcMechanicalFastener`; a connection is a named assembly, drawn, not calculated | L |

### Wave 6 — site

| Ticket | Controls | Notes | Size |
|---|---|---|---|
| SITE1 | Import points, Import contours, Topo surface | points from CSV, contours from DXF (the reader exists), a triangulated surface (`IfcGeographicElement` with a `IfcTriangulatedFaceSet`); a constrained Delaunay triangulation is new code | L |
| SITE2 | Pad, Excavation, Slope, Volumes | a pad cuts the surface, slopes by angle, cut and fill volumes by prisms between two surfaces, with a hand-computed test | L |

### Wave 7 — life-cycle assessment

| Ticket | Controls | Notes | Size |
|---|---|---|---|
| LCA1 | KBOB data, Assign | the KBOB list as data (its terms of use must be checked before it is committed), a material mapped to a KBOB entry | M, blocked on the data's terms |
| LCA2 | Carbon balance, Compare variants, Report | quantities × factors per element, storey and material; two branches of the history compared; a report page and its PDF | L |

## Order and what decides it

1. Wave 1 now: it removes the dead controls a visitor meets first, with little new code.
2. Wave 2 next: concrete is the product's first mission and its IFC classes are simple.
3. Wave 3 before the domains: the type pages and saved schedules are used by every later wave.
4. Rebar before steel: a concrete drawing is not finished without bars. Steel first is a fair
   alternative if steel jobs come first.
5. Site and LCA last: the most new geometry and the most outside data.

Roughly: waves 1 to 3 are days of builder time at the pace of this week; waves 4 to 7 are each a
round of their own. With two browser jobs at a time, a wave of three M tickets takes about a day
with its gates.

## Open points (not blocking wave 1 or 2)

- The licence or terms of the CFC catalogue and of the KBOB list.
- Which reinforcement and steel standards come first (SIA 262 and European profiles assumed).
- Whether files using the new classes must still open in Rust v0.19.0 (they will not, as for
  grids and the survey point).
- The Timber tab is not counted here.
