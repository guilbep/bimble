/* Polygon triangulation: a port of the Rust crate earcutr 0.5.0 (`earcutr::earcut`, ISC
 * licence, see vendor/earcutr/LICENSE), itself a port of Mapbox earcut.
 *
 * Bimble's scene must match Rust byte for byte, so this is a faithful port: the same node order,
 * the same z-order hash (used when the flat array has 80 values or more), the same f64
 * expressions in the same order, and the crate's quirks kept (e.g. add_contour reads the bounding
 * box of a hole at the hole's relative indices; split_earcut always takes the hashed path).
 *
 * Limits that Rust does not have (files are untrusted input): the recursion depth of the
 * split/pass chain and the total number of loop steps are bounded; both give
 * CC_EARCUT_LIMIT instead of a stack overflow or a hang. */
#ifndef CCORE_EARCUT_H
#define CCORE_EARCUT_H
#include <stddef.h>
#include <stdint.h>

#define CC_EARCUT_UNKNOWN (-1) /* earcutr's Error::Unknown */
#define CC_EARCUT_OOM (-2)
#define CC_EARCUT_LIMIT (-3) /* recursion depth or step budget exceeded */

/* earcutr::earcut(v[0..nv], holes[0..nholes], 2). `v` is x0, y0, x1, y1, ...; `holes` are the
 * point index where each hole starts. On 0, *tri is a malloc'd array of *ntri indices (3 per
 * triangle, may be NULL when *ntri is 0; the caller frees it). On error, *tri is NULL. */
int cc_earcut(const double *v, size_t nv, const size_t *holes, size_t nholes, uint32_t **tri, size_t *ntri);
#endif
