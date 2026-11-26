/* The render scene as BMSH version 3: shapes once, a placement per copy (ticket 3D1, ADR 0078).
 *
 * BMSH version 1 (session/mesh.c) places every shared shape (cc_scene_world_mesh) and sends each
 * copy's triangles. Version 3 sends each shared shape once, in its own coordinates, and a placement
 * per element, so the lab draws the copies instanced (ADR 0034 phase 2: GPU memory follows unique
 * triangles). Little-endian, no padding between blocks:
 *   0   char[4]  "BMSH"
 *   4   u32      version (3)
 *   8   u32      rev
 *   12  u32      nE elements, 16 u32 nV vertices, 20 u32 nI indices, 24 u32 nS edge segments: the
 *                blocks' counts, every own mesh and every shape that an element places, once
 *   28  f64[3]   origin_mm
 *   52  nE rows of 48 bytes, as version 1 (GlobalId, MaterialCategory, pad, first vertex, vertex
 *                count, first index, index count, first segment, segment count); the row of a placed
 *                shape gives the shape's ranges (the same for each of its copies)
 *   then f32[3] x nV positions: an own mesh in scene coordinates, a shape in its own
 *   then u32 x nI indices, absolute; f32[6] x nS edge segments; i8[8] x nS face normals, as version 1
 *   then nE x 52 bytes: u32 shape (0: own mesh; k + 1: the element places shape k), f32[12] the
 *                Placement32 columns x, y, z, t (p_scene = x p.x + y p.y + z p.z + t; zeros for an
 *                own mesh)
 * Blocks come in first-use order (the first element that places a shape writes it). An element
 * whose placement mirrors (det < 0) keeps the shape's winding: the reader flips it (as
 * cc_scene_world_mesh does). */
#ifndef CCORE_SCENE_BMSH_H
#define CCORE_SCENE_BMSH_H
#include <stdint.h>
#include "buf.h"
#include "scene.h"

#define CC_BMSH_SHAPES_VERSION 3u

/* The 22-byte id text of element k of `s` (NULL: the element's GlobalId). */
typedef void (*cc_bmsh_id_fn)(const void *ctx, const cc_scene *s, size_t k, char out[22]);

/* Appends the scene as BMSH version 3 to `out`. 0 ok, -1 with `err` (out of memory, a count over
 * u32, an index out of range). */
int cc_scene_bmsh3(const cc_scene *s, cc_bmsh_id_fn id, const void *ctx, uint32_t rev, Buf *out, cc_error *err);
#endif
