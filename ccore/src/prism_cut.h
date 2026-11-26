/* Openings cut without the boolean kernel (ticket SLAB1): the exact special cases of a prism host
 * (an extrusion along its local z, 0..depth) and prism openings, for the builds without manifold
 * (wasm, CC_NO_KERNEL). The native build keeps the kernel (scene.c calls these only when
 * cc_boolean_kernel() is NULL). Not the kernel's mesh: the same solid (volume, area, box).
 *
 *   box:   the host profile is an axis-aligned rectangle without voids (a rectangular column,
 *          beam, wall, slab), each opening an axis-aligned box in the host's frame (an imported
 *          window or door: 8 corners, closed): the solid split into the cells of every distinct
 *          coordinate, the faces between solid and empty cells kept.
 *   holes: the host profile is any polygon (with voids), each opening a convex prism along z
 *          through the whole depth, strictly inside the outline, apart from the voids and from
 *          each other: each becomes one more void loop (clockwise) of the extrusion.
 * Anything else is not a special case: the caller draws the element uncut, with its warning.
 * Layer L2 (geom): no files, clock or globals; no libm. */
#ifndef CCORE_PRISM_CUT_H
#define CCORE_PRISM_CUT_H
#include <stddef.h>
#include "arena.h"
#include "boolean.h"

typedef struct {
    double (*p)[2];
    size_t n;
} cc_ploop;

#define CC_PRISM_MAX_CELLS 2000000 /* box: (distinct x - 1)(y - 1)(z - 1) at most */

/* box: loops[0] the profile (nloops 1), `cut` the openings' triangles in the host's frame. 0: the
 * cut solid's triangles (outward, arena) in *tris; 1: not this case; -1 out of memory. */
int cc_prism_cut_box(const cc_ploop *loops, size_t nloops, double depth, const cc_tri_set *cut, size_t nc, Arena *a,
                     double (**tris)[3][3], size_t *ntris);
/* holes: 0: *holes (arena, nc loops, clockwise) to add to the profile's loops; 1: not this case;
 * -1 out of memory. loops[0] is the outline (counter-clockwise or not), loops[1..] its voids. */
int cc_prism_cut_holes(const cc_ploop *loops, size_t nloops, double depth, const cc_tri_set *cut, size_t nc, Arena *a,
                       cc_ploop **holes);
#endif
