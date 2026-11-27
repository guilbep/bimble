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
#define CC_BMSH_COMPACT_VERSION 4u

/* The 22-byte id text of element k of `s` (NULL: the element's GlobalId). */
typedef void (*cc_bmsh_id_fn)(const void *ctx, const cc_scene *s, size_t k, char out[22]);

/* Appends the scene as BMSH version 3 to `out`. 0 ok, -1 with `err` (out of memory, a count over
 * u32, an index out of range). */
int cc_scene_bmsh3(const cc_scene *s, cc_bmsh_id_fn id, const void *ctx, uint32_t rev, Buf *out, cc_error *err);
/* SIZE2: BMSH version 4, version 3 with its geometry in the compact mesh codec (meshcodec.h, BMG1: the
 * link caches' own form, 1/16 mm steps from each block's lower corner, varint deltas): what a link is
 * served as (GET /mesh?link=k), about a third of version 3's bytes and within a few % of its cache's
 * geometry section. Little-endian:
 *   0   char[4] "BMSH", 4 u32 version (4), 8 u32 rev, 12 u32 nE, 16 u32 nV, 20 u32 nI, 24 u32 nS,
 *   28  f64[3] origin_mm: as version 3 (the counts are what the blocks decode to)
 *   52  nE rows of 48 bytes, as version 3 (their ranges index the decoded blocks)
 *   then nE x u32: shape (0: own mesh; k + 1: places shape k), then for each element that places a
 *        shape, in element order, its f32[12] placement (version 3's columns; own meshes have none)
 *   then u32 nB, then nB blocks: u32 length, then that many bytes of one BMG1 mesh. Block j holds the
 *        vertices, indices (from 0: the block's first vertex is added by the reader), segments and normals
 *        version 3 writes j-th (first-use order); the normals' w come back 0 (BMG1 drops it).
 * A reader checks every count against the header and what is left, never trusts a length. */
int cc_scene_bmsh4(const cc_scene *s, cc_bmsh_id_fn id, const void *ctx, uint32_t rev, Buf *out, cc_error *err);
/* SIZE3: the same, each block's BMG1 bytes appended by `blk` (shape `i` when `shape`, else element i's own
 * mesh; 0 ok, -1 not available) instead of encoded from the scene's arrays: a link read in place serves
 * its cache's own bytes, its meshes not decoded (only their counts are read). */
typedef int (*cc_bmsh_block_fn)(const void *ctx, int shape, size_t i, Buf *out);
int cc_scene_bmsh4_blocks(const cc_scene *s, cc_bmsh_id_fn id, const void *ctx, uint32_t rev, cc_bmsh_block_fn blk, const void *bctx,
                          Buf *out, cc_error *err);
/* SIZE2: BMSH version 4 bytes (untrusted) to the version 3 bytes they stand for, appended to `out`; every
 * count bounded by the header and the input (at most `max_out` bytes written: a zip-bomb guard). 0 ok,
 * -1 with `err`. */
int cc_bmsh4_to_bmsh3(const uint8_t *p, size_t n, size_t max_out, Buf *out, cc_error *err);
#endif
