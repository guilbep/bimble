# ADR 0001 — Rust core, own wgpu renderer, IFC-typed native format

- Status: Accepted (5 Nov 2026). Supersedes the earlier Python/IfcOpenShell proposal.
- Reversal cost: high for the language, low for the format (POC JSON, versioned).

## Problem
The main pain point for our users is 3D speed on old machines. Python plus an off-the-shelf viewer can't
promise that. We also want our own format that goes beyond IFC (generators, history, branches) while keeping
IFC as the model and vocabulary.

## Decision
- Rust workspace. wgpu for rendering (Vulkan/Metal/DX12 plus an OpenGL fallback; WebGPU/WebGL2 later via wasm).
- Drawing-style rendering: flat shading, edges, ~12-colour palette, hatch patterns per material, no textures.
- The model is typed with IFC classes and uses IFC GlobalIds. `.bimble` is a superset of IFC.
- Element-level canonical serialization and content hashing from day 1 (for versioning).
- POC geometry: own extrusion mesher. The exact-geometry kernel is deferred to a later ADR.
- IFC import: IfcOpenShell (C++) from Rust is the main option (ADR at I3).

## Alternatives rejected
- Python + IfcOpenShell as the native model: fast to start, but slow rendering and IFC-only storage.
- Game engines (Bevy, Godot, Unity): too much we don't need, less control over memory and GPU budget.
- C++ with OCCT as the core: slower development; OCCT can still be pulled in later behind a geometry trait.

## Revisit if
wgpu's OpenGL fallback can't meet the performance target on the reference old machine.
