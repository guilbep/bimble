# Drawing a very large building model fluidly: literature and practice review (LIT1)

Date: 27 November 2026. Status: research note, no decision.

Scope: a scene of 17 linked files, 252,000 elements, 110 M triangles, on a laptop integrated GPU.
Our measured cost is 0.6 to 1.0 ms per million triangles drawn. Draw calls are not the bottleneck.
About 95 % of the triangles are interior. 86 % are meshes given as-is by authoring tools.

Rules of this note:

- Each claim has a source key. The keys are in the list at the end. I opened each of those pages.
- "Inference" marks my own reasoning. It is not a measurement.
- Section (e) lists what I could not verify.

## (a) Techniques not yet in our plan, ranked

Cost: S = days, M = one to two weeks, L = a month or more.

| # | Technique | Expected gain for our scene | Cost | Risk and failure mode | WebGL2 / WebGPU / native | Source |
|---|-----------|-----------------------------|------|-----------------------|--------------------------|--------|
| 1 | **Offline exterior shell.** Render an id buffer from many viewpoints around each file. Mark each element (and tile) "seen from outside". Build the coarse pyramid levels from shell elements only. | Largest. Inference from the 95 % interior fact: an outside view needs about 5 % of the triangles, so about 5 M instead of a budget spent partly on hidden parts. Far Voxels removed 43 % of the coarse voxels of a 350 M triangle aircraft by the same idea [FV]. | M | The method is aggressive: it can miss an element seen only through a small opening [GVS]. Glass must not occlude. A section, or hiding a shell element, makes the bake invalid: then fall back to run-time occlusion. | All three. It is baked data. | [2RT] [VOX] [PXZ] [GVS] [FV] |
| 2 | **Two-pass Hi-Z** in place of reprojected depth. Pass 1 draws what was visible last frame. Build the pyramid. Pass 2 tests the rest and draws what became visible. | Fixes the dropped visible elements. Karis: reprojection is "always approximate and not conservative"; two passes are "conservative" and fail only "under extreme visibility changes" [K21]. Gain in triangles is the interior share when the camera is inside (inference). | M | Needs the visible set per tile from the last frame. Costs a second cull and pyramid build. | Native: yes. WebGPU: yes with compute. WebGL2: only with CPU-side tests or queries, see row 3. | [K21] [HA15] |
| 3 | **Hardware occlusion queries per tile** (CHC++ style) as the web path. | Same class of gain as row 2 on the web. Inference: we have about 2,750 leaf tiles (110 M / 40 k), so the query count is small. Far Voxels culled its hierarchy this way [FV]. | M | Results come one frame or more late. CHC++ reduces queries and stalls with temporal coherence and batching [CHC]. Never wait for a result. | WebGL2 has `ANY_SAMPLES_PASSED_CONSERVATIVE` [MDN]. WebGPU and native: yes. | [CHC] [MDN] [FV] |
| 4 | **Clip planes without `discard`.** Clip the section box with clip distances in the vertex stage. | Unknown size, probably real on Apple GPUs (inference). Apple puts `discard` and depth feedback in a slower class and says the GPU must "go back to HSR" [AP2]. | S | Capped sections still need a fragment path. | WebGPU: clip distances arrived in Chrome 131 [W1]. Native: yes. WebGL2: extension, not verified. | [AP1] [AP2] [W1] |
| 5 | **Contribution culling per element**, and drop-out ordered by size and distance under a frame budget. | Inference: large for interior views, because small MEP parts and furniture dominate the count. Navisworks culls below 1×1 pixel by default and lets the user raise it [NW1] [NW2]. xeokit has a solid-angle limit [XK4b]. | S | Thin long parts (pipes, rails) vanish too early with a bounding-box test. Use the longest side. | All three. | [NW1] [NW2] [NW3] |
| 6 | **Progressive completion when still.** While the camera rests, keep drawing the rest of the tiles over several frames into the same buffers. | Hides cost. The still image becomes exact. Navisworks "fills in" dropped detail on stop [NW1]. APS and xeokit do the same kind of thing [APS1] [XK3]. | S | Edges and picking must wait for the complete id buffer. | All three. | [NW1] [XK3] [APS1] |
| 7 | **meshoptimizer at bake time**: vertex cache and fetch order; index and vertex codecs for the cache file. | Size: index codec about 1 to 1.2 bytes per triangle, vertex data 2 to 4× smaller, decode 3 to 6 GB/s [MO]. Speed gain per triangle on our GPU: unknown, must be measured. | S | The source is C++ with a C header [MO]. A pure-C build needs a C++ compiler, or we keep it in the offline baker only. MIT licence. | All three. | [MO] [MC] |
| 8 | **Edge-collapse simplification with locked borders** for coarse levels, in place of vertex clustering. | Better shape per triangle. meshoptimizer says the topology-free mode gives worse quality; `LockBorder` avoids cracks; `Permissive` helps flat-shaded meshes with many seams [MO]. | M | Borders locked on the same planes at each level collect dense "cruft". Nanite regroups clusters at each level to avoid it [K21]. | Baked. | [MO] [K21] |
| 9 | **Cluster cone culling** (64-triangle clusters, one cone test each). | 10 to 30 % of triangles in a game city [HA15]; 3 to 32 % depending on cluster size and LOD [ZX]. Inference: less for us, because a small pipe or handle fills a whole cluster with all normal directions. | M | More draw ranges or an index compaction step. | Native and WebGPU. Weak on WebGL2. | [HA15] [ZX] [MO] |
| 10 | **Per-room visibility sets** from IfcSpace and door openings. | Interior views only. Inference: strong where spaces exist. | L | Many files have no or poor spaces. Open-plan floors gain little. Aggressive sampling can miss parts [GVS]. | Baked. | [GVS] |
| 11 | **Analytic pipes**: draw cylinders and cones as ray-cast impostors. | Small in time: pipes are about 5 M of 110 M triangles. Good for memory and round silhouettes. One GeForce 6800 drew 99 k spheres and 198 k cylinders at 37 fps [SG]. | L | Pipes arrive as meshes, so we must first recognise them. The shader writes depth, which Apple penalises [AP2]. | All three. | [SG] [CSG] |
| 12 | **Similar-part instancing** beyond exact matches. | Memory, not frame time (inference: our cost is per triangle drawn). Pixyz ships a "loose" and a "precise" detector for BIM [PXZ]. xeokit found batching faster than instancing on most models, at more memory [XK2]. | M | False matches change geometry. | Baked. | [PXZ] [XK2] |
| 13 | **Software occlusion (Intel masked)**. | Not for us. | n/a | The library targets AVX and SSE, is archived, and does not mention ARM [MOC]. | Would need a port. | [MOC] |
| 14 | **Nanite-style cluster DAG, visibility buffer.** | Low. Nanite's gain comes from pixel-sized triangles and a software rasteriser that is 3× faster than hardware for them [K21]. Our fragments are already an id, a normal and a depth (inference: nothing to defer). | L | Many small disjoint parts cannot be simplified across parts. | Native only in practice. | [K21] [UE] |

## (b) Techniques we already plan: pitfalls and standard fixes

**Hi-Z occlusion**

- Reprojected depth is not conservative [K21]. This is the usual cause of dropped visible objects. Fix: row 2.
- Assassin's Creed Unity used the 300 best occluders at full resolution, down-sampled to 512×256, merged with the last frame's depth. Total cost was under 1 ms on a PS4 [HA15].
- Pick the mip level so the box covers at most 2×2 texels: `ceil(log2(max(w, h) / 2))` [RG].
- Keep the farthest depth when reducing. For odd sizes, also read the edge texels, or the test leaks [RG].
- Inference: occluders must exclude glazing, hidden elements and anything cut by a section. A box that crosses the near plane must count as visible.
- GPU occlusion "will reduce performance if most of the model is visible" [NW1]. Navisworks leaves it off by default and runs CPU occlusion on a spare core. Inference: skip Hi-Z when the shell (row 1) applies.

**Tile pyramid and LOD**

- Popping is the complaint that made Autodesk rebuild its consolidation with a hierarchical LOD grouped "by size and spatial regions" [APS2].
- Decimation below 10 to 20 % of the triangles "can distort geometry beyond usability" [PXZ].
- 3D Tiles selects by geometric error turned into screen-space error, with replace or add refinement [3DT]. Our selection is of the same family. The spec says nothing on occlusion [3DT].
- Far Voxels at 1 pixel tolerance held 20 to 45 frames per second on average but dropped to 3 to 9 in the worst case [FV]. A budget alone does not bound the worst case: a drop-out rule does (row 5).

**Instancing and merging**

- Merging reduces draws but "can prevent culling", destroys instances and can make things slower [PXZ].
- APS consolidation "reduces the granularity of the scene, which conflicts with ... progressive rendering" and fine frustum culling [APS1].
- xeokit measured three modes: batching was fastest for most models and used the most GPU memory; instancing was "mixed"; data textures were slower but smallest [XK2].

**Draw order and depth on tile-based GPUs**

- Apple: hidden surface removal is "pixel perfect and submission order independent". "We don't actually need to sort opaque meshes." Draw opaque first, then discard or depth feedback, then translucent. Do not interleave [AP2].
- Imagination says the same for PowerVR: no benefit from sorting opaque geometry [IMG1]; HSR removes overdraw "regardless of draw call submission order" [IMG2]. Its guide lists a rule titled "Do Not Use Depth Pre-pass" [IMG2].
- Inference: on Apple GPUs a depth pre-pass doubles the vertex and tiling work, which is our real cost. Front-to-back order and index order for overdraw matter only on immediate-mode GPUs (most Windows laptops).
- The tiler stores transformed vertices in a buffer. When it fills, the GPU does a "partial render" [AP2]. Inference: 100 M triangles in one pass may hit this. It is one more reason to cut triangles before the vertex stage.

**Depth precision**

- Use reversed-Z with a float depth buffer [NV]. Navisworks warns that a far/near ratio above 10,000 gives artefacts [NW2]. xeokit offers a logarithmic depth buffer, off by default [XK4].

**Stand-ins while moving**

- xeokit hides edges, ambient shadows and transparent objects while moving, can scale the canvas to 0.5 (75 % fewer pixels), and restores after 0.5 s [XK3].
- Inference: resolution scaling helps us little, because our cost follows triangles, not pixels. Scale the triangle budget instead.

**Web limits**

- WebGPU multi-draw indirect is still experimental in Chromium: feature `chromium-experimental-multi-draw-indirect`, behind the "Unsafe WebGPU Support" flag [W1]. The Chrome release notes up to 155–156 do not announce it as shipped [W2]. A survey shows 0.12 % of browsers expose it [W3]. A forum post of April 2026 calls the standard proposal stalled since November 2024 [W5].
- `indirect-first-instance` is exposed by 99.97 % of tested browsers [W4].
- WebGL2 has `WEBGL_multi_draw` with `gl_DrawID`, community approved [W7].
- Inference: since draws are cheap for us, the web build can cull on the CPU and issue plain draws.

## (c) What comparable viewers do

1. **Navisworks**: guaranteed frame rate with drop-out by size and distance, fill-in on stop, pixel-area cull (default 1×1), CPU occlusion on by default, GPU occlusion off [NW1] [NW2] [NW3].
2. **Autodesk APS viewer**: progressive rendering; consolidation by merging and instancing with a 100 MB GPU cap [APS1]; new hierarchical LOD in beta, built in the browser after download [APS2]; streaming load 8 to 16× faster on ten models up to 2 GB, on an M1 Mac [APS3].
3. **xeokit**: batching or instancing or data textures [XK2]; frustum culling plugin that "may actually degrade performance" on whole-model views [XK1]; fast-navigation mode [XK3]. No published triangle or fps numbers in text.
4. **That Open Fragments**: FlatBuffers format, three.js viewer, "millions of elements in seconds"; no hardware, fps or memory numbers [TO].
5. **Speckle**: three.js; automatic batching, instancing, multi-level BVH, progressive ambient occlusion; no numbers [SP].
6. **Cesium 3D Tiles**: error-driven tile tree, implicit octrees, glTF content [3DT]; meshopt compression is a ratified glTF extension, decode about 1 GB/s [MC].
7. **Unreal Nanite**: 128-triangle clusters, two-pass Hi-Z, software rasteriser [K21]; opaque and masked materials only, DX12-class hardware, 16 M instance cap [UE]. Inference: glazing falls outside it.
8. **Pixyz (Unity)**: bake-time removal of occluded geometry from 256 view spheres at 512 pixels; targets 10 to 100 M triangles on desktop and 100 k to 1 M on WebGL [PXZ].
9. **Far Voxels** (research): 350 M triangles at 44 fps average, 8 minimum, 640×480, on a 2004 GPU with 1 GB RAM [FV].
10. **Assassin's Creed Unity** (games): 64-triangle clusters, GPU culling, baked back-face masks culling 10 to 30 % [HA15].

## (d) Three experiments to run first (one day each)

1. **Shell bake.** For one link, render the id buffer from 256 viewpoints on a sphere at 512×512, glazing not drawn as an occluder. Count the share of elements, tiles and triangles marked. Repeat with 4,096 viewpoints as the reference and count missed elements. Then fly a scripted outside path with shell-only tiles and report frame time against today's build. Pass: under 10 % of triangles marked, under 0.1 % of visible elements missed.
2. **Hi-Z truth test.** On a scripted path, inside and outside, render each frame twice: with culling, and without. Compare the two id buffers. Report the count of missing ids per frame (target 0), triangles drawn and milliseconds, for reprojected Hi-Z and for two-pass Hi-Z.
3. **Cost per triangle on the Apple GPU.** Same view, same triangles, four switches: section clip by `discard` or by clip distance; opaque order random or front-to-back; depth pre-pass on or off; baked index and vertex order raw or reordered for vertex cache and fetch. Report milliseconds per million triangles for each.

## (e) What I could not verify

- OutDet (exterior detection for BIM in GIS, 2019): the page refused access. I know it only from search summaries.
- The Arm guides on front-to-back order and its hardware "fragment prepass": pages returned errors. Not used above.
- The Imagination "Do Not Use Depth Pre-pass" page: I saw the title only, not the text.
- Quick-VDR, HLOD (Erikson), Lindstrom's out-of-core simplification, Yoon and Manocha's book, CHC++ full text, the visibility-buffer paper: search summaries or abstracts only. Search results quote 10 to 35 fps for Quick-VDR; I did not open the paper.
- xeokit solid-angle culling ([XK4b], search result only), LOD culling options and XKT numbers; That Open tile and worker internals; Speckle numbers: nothing found in text.
- Solibri, BIMcollab: nothing found. Dalux: marketing claims only ("over 1 million objects"). Bonsai and Blender limits: not searched here; see the two notes of 26 and 27 November.
- Datasmith with Nanite on BIM files: forum reports only.
- How products treat glazing in baked visibility: no vendor states it. "Glass does not occlude" is my inference.
- WebGL2 clip-distance and clip-control extensions and their coverage.
- Firefox and Safari status for multi-draw indirect. The Chromium status comes from release notes and a survey, not from a status page.
- Every "expected gain" in the table is an estimate until experiment 1 to 3 give numbers.

## Sources

- [K21] Karis, Nanite, SIGGRAPH 2021: https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf
- [HA15] Haar and Aaltonen, GPU-driven rendering pipelines, SIGGRAPH 2015: https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf
- [RG] Hi-Z map occlusion culling: https://www.rastergrid.com/?p=85
- [CHC] CHC++: https://www.cg.tuwien.ac.at/research/publications/2008/mattausch-2008-CHC/
- [MOC] Intel masked occlusion culling: https://github.com/GameTechDev/MaskedOcclusionCulling
- [MDN] WebGL2 queries: https://developer.mozilla.org/en-US/docs/Web/API/WebGL2RenderingContext/beginQuery
- [2RT] Two-stage ray tracing for external BIM objects: https://cris.iucc.ac.il/en/publications/a-two-stage-recursive-ray-tracing-algorithm-to-automatically-iden/
- [VOX] IfcOpenShell voxelization toolkit: https://github.com/IfcOpenShell/voxelization_toolkit
- [PXZ] Unity Asset Transformer optimisation guidelines: https://docs.unity.com/en-us/asset-transformer-sdk/2026.1/manual/sdktips/optimization-guidelines
- [GVS] GVS++ visibility sampling: https://www.cg.tuwien.ac.at/research/publications/2021/KOCH-2021-GVS/
- [FV] Far Voxels: https://www.crs4.it/vic/data/papers/sig2005-farvox.pdf
- [SG] GPU-based ray-casting of quadratic surfaces: https://cg.cs.tu-dortmund.de/publications/2006-quadrics.pdf
- [CSG] CSG ray tracing of massive models: https://www.scitepress.org/Papers/2017/61364/61364.pdf
- [MO] meshoptimizer: https://github.com/zeux/meshoptimizer
- [ZX] Meshlet size trade-offs: https://zeux.io/2023/01/16/meshlet-size-tradeoffs/
- [MC] EXT_meshopt_compression: https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Vendor/EXT_meshopt_compression/README.md
- [UE] Nanite documentation: https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine
- [NW1] Navisworks display options: https://help.autodesk.com/cloudhelp/2026/ENU/Navisworks/files/GUID-B470F105-8626-44FF-B531-8864FBD79CDA.htm
- [NW2] Navisworks culling options: https://help.autodesk.com/cloudhelp/2026/ENU/Navisworks/files/GUID-EBDDC0F7-E3F6-43DC-A604-0DAAC044DEDA.htm
- [NW3] Navisworks drop-out: https://help.autodesk.com/cloudhelp/2020/ENU/Navisworks/files/GUID-1A87025C-F276-4978-847D-52CD63AF3F29.htm
- [XK1] xeokit performance tips: https://xeokit.io/blog/viewer-performance-tips
- [XK2] xeokit performance and memory trade-offs: https://xeokit.io/blog/xeokit-performance-and-memory-trade-offs
- [XK3] xeokit FastNavPlugin: https://xeokit.io/blog/smoother-navigation-with-fastnavplugin
- [XK4] xeokit Scene reference: https://xeokit.github.io/xeokit-sdk/docs/class/src/viewer/scene/scene/Scene.js~Scene.html
- [XK4b] xeokit culling module (search result only, see section e): https://xeokit.github.io/sdk/docs/api/modules/spatial.culling.html
- [APS1] APS consolidation: https://aps.autodesk.com/node/527
- [APS2] APS smooth navigation: https://aps.autodesk.com/node/3443
- [APS3] APS streaming load: https://aps.autodesk.com/blog/viewer-performance-update-part-1-3-speedier-svf2-loading
- [TO] That Open Fragments: https://github.com/ThatOpen/engine_fragment
- [SP] Speckle viewer overview: https://docs.speckle.systems/developers/viewer/overview.md
- [3DT] 3D Tiles specification: https://github.com/CesiumGS/3d-tiles/blob/main/specification/README.adoc
- [AP1] Apple, tile-based deferred rendering: https://developer.apple.com/tutorials/data/documentation/metal/tailor-your-apps-for-apple-gpus-and-tile-based-deferred-rendering.md
- [AP2] Apple WWDC20 session 10602: https://developer.apple.com/videos/play/wwdc2020/10602/
- [IMG1] PowerVR, sorting geometry: https://docs.imgtec.com/performance-guides/graphics-recommendations/html/topics/sorting-geometry-effectively-on-powervr.html
- [IMG2] PowerVR, HSR efficiency: https://docs.imgtec.com/starter-guides/powervr-architecture/html/topics/hidden-surface-removal-efficiency.html
- [NV] Depth precision visualized: https://developer.nvidia.com/blog/visualizing-depth-precision/
- [W1] New in WebGPU, Chrome 131: https://developer.chrome.com/blog/new-in-webgpu-131
- [W2] WebGPU release notes index: https://developer.chrome.com/docs/web-platform/webgpu/news
- [W3] Survey, multi-draw indirect: https://web3dsurvey.com/webgpu/features/chromium-experimental-multi-draw-indirect
- [W4] Survey, indirect-first-instance: https://web3dsurvey.com/webgpu/features/indirect-first-instance
- [W5] Forum post on the stalled proposal (11 April 2026): https://forum.babylonjs.com/t/webgl-multi-draw-extension/47345/9
- [W7] WEBGL_multi_draw: https://registry.khronos.org/webgl/extensions/WEBGL_multi_draw/
