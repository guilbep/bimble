# Research: state of the art of BIM authoring, open and closed, AI and MCP

_20 Nov 2026. Five SEARCH tickets (Sonnet), one re-check ticket, read by the orchestrator. "Opened" =
page read. "Snippet" = search summary only, unverified. "Inference" = our reading, not a source._

## Summary (inference)

- Open source has one full IFC-native authoring tool: Bonsai (Blender). Everything else is a viewer,
  a library, or early layout (Pascal Editor).
- No open or new closed tool we found makes structural concrete drawings (formwork, rebar) the
  product. Tekla and Allplan work in that space, closed (detailing depth and price not verified).
- MCP is the common agent interface in 2026: Revit (preview), Archicad 30 (beta), Bonsai/IfcOpenShell,
  ifc-lite, Pascal Editor, community Tekla. An MCP over Bimble's commands is expected, not unique.
- The closest open competitor on Bimble's stack (Rust to wasm, WebGPU) is ifc-lite, viewer-first
  with light authoring.

## Open source

| Project | Kind | Stack | Licence | Agent access | Source |
|---|---|---|---|---|---|
| Bonsai + IfcOpenShell | IFC-native authoring in Blender, and a library | Blender add-on (language not verified) | GPL-3.0+ / LGPL-3.0 | `ifcmcp`, AGENTS.md | opened github.com/IfcOpenShell/IfcOpenShell, docs.bonsaibim.org |
| FreeCAD BIM | general CAD + BIM workbench | C++, Qt | LGPL-2.1 | not checked | opened github.com/FreeCAD/FreeCAD |
| ifc-lite (LTplus-AG) | viewer + light authoring (mutations, undo, export) | Rust to wasm, WebGPU, DuckDB-wasm | MPL-2.0, 478 stars | `packages/mcp` | opened api.github.com/repos/LTplus-AG/ifc-lite |
| Pascal Editor | browser authoring, architectural layout, own scene graph | React Three Fiber, WebGPU | MIT, 25 649 stars | `packages/mcp`, CLI | opened api.github.com/repos/pascalorg/editor |
| That Open (web-ifc, components) | IFC library + viewer toolkit | C++/emscripten, three.js | MPL-2.0 / MIT | none seen | opened GitHub |
| xeokit | viewer SDK | WebGL | AGPL-3.0 | none seen | opened GitHub |
| Speckle | data platform + viewer | Node, Vue, three.js | Apache-2.0 | community MCP only | opened github.com/specklesystems/speckle-server |
| BIMserver | IFC server | not verified | AGPL-3.0 | none | opened GitHub |
| Rust crates | `openbim-step`, `ifc-model`, `ifc-schema` (Nov 2026, small), `ifc_rs` stale | Rust | | | opened crates.io API |

Free closed viewers (not authoring): BIMvision (Windows, plugins, Python bridge), DiStellar (browser),
Flinker (Teams/Power BI), Open IFC Viewer (ODA, desktop), icBIM (no data found).

## Closed source

| Product | What | AI / agents | Source |
|---|---|---|---|
| Revit 2027 | incumbent | Autodesk Assistant (preview), MCP server, Technical Preview, Read/Write/Experimental installers, Revit 2027.2+ | opened blog.autodesk.io/revit-mcp-server-update/. "Apache-2.0, 102 tools" only in a third-party post |
| Archicad 30 | incumbent | AI Assistant (beta), Archicad MCP for batch edits, by plan | opened help.graphisoft.com (AC 30) |
| Tekla Structures 2026 | structural steel/concrete detailing (unsourced) | AI fabrication drawings (human in the loop), Trimble Assistant, community MCP | snippet only |
| Allplan 2026 | concrete, precast, bridges (unsourced) | AI Assistant on Connect (subscription), AI Coder, Python API (PythonParts) | snippet only |
| Qonic | browser, IFC-native editing and coordination | classification, checks, no LLM modelling claim, open API | opened qonic.com |
| Motif | browser BIM, humans and agents on one live model | agents "can perform any action a human user can" | opened motif.io |
| Snaptrude | prompt to massing to BIM to drawings, web | agent | opened snaptrude.com |
| ArchiLabs | agent inside Revit/AutoCAD | creates and edits native elements | opened archilabs.ai |
| TestFit, Finch, Higharc, Augmenta, Arcol, Rayon | feasibility, generative, homebuilding, MEP routing, interiors | various, TestFit has an MCP add-on | opened |

Not found: Vectorworks, BricsCAD BIM, Nemetschek AI layer detail, Swapp (cert error), Hypar (empty page).

## Dalux and Speckle

- Dalux (opened dalux.com, architosh.com 2025): Box, Field, free BIM Viewer, Handover, FM. Web, iOS,
  Android, offline. Claims "fastest BIM viewer", no benchmark. No AI on its pages. The engine and API
  docs: not found. Inference: adoption comes from free viewing and site use on tablets.
- Speckle (opened docs.speckle.systems data schema): collections of objects, geometry as display
  meshes, content-hash ids, versions as immutable snapshots. Inference: a publish target for Bimble,
  not its model, because parameters and IFC semantics do not round-trip.

## MCP servers: what works (opened pages)

- Archicad Tapir MCP (github.com/SzamosiMate/tapir-archicad-MCP, MIT): five meta-tools (search
  commands, get schema, execute, job status) over 800+ commands. Few tools, large discoverable surface.
- Revit community MCP (MIT): 26 tools plus C# code execution. Blender MCP: arbitrary Python.
- MCP4IFC (arxiv.org/html/2511.05533v1): 50+ tools cost about 40k context tokens. IFC queries 75-83 %
  correct. Generated buildings missed wall junctions, openings, spaces, materials.
- Inference for Bimble: a few tools (list verbs, describe verb, apply, query, render a picture) over
  typed commands. No code execution: files and parameters are data (CLAUDE.md).

## PDF to BIM

- Buildings AI PDF2BIM (opened, vendor blog): walls, doors, rooms from plans, human sets the scale and
  checks. 16 h vs 45 h on one office building. No error rate.
- Academic (snippets): ResBIM dataset, Sketch2BIM (Oct 2025). Self-reported accuracy only.
- Inference: works for simple architectural plans with a human in the loop. Nothing found for
  structural drawings (grids, columns, beams). Bimble already reads vector PDF and DXF as reference
  plans (ADR 0021): an agent tracing over them with verbs is a realistic first step.

## Your links

luw.ai IFC tool: free wasm viewer, not AI. clashwise: page failed. bimcorner: ChatGPT writes
IfcOpenShell scripts run by hand. crais.io: embeddable viewer, MCP "in development". keplerworks:
analytics. terraquantis: LCA from Revit. Microsoft Store 9NDXQRVVRQ0J: Open IFC Viewer. None is an
AI-native modeller.
