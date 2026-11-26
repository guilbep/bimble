/* Geometry for drawings (L2), byte for byte as bimble-drawing's cut.rs and crop.rs: horizontal
 * sections of meshes, what of a mesh is seen from above, segments clipped to a height range,
 * and a view's crop rectangle. Points are scene mm, f64; meshes are f32 (scene.h), widened.
 *
 * Outputs live in the caller's arena. Every function returns 0, or -1 when out of memory. */
#ifndef CCORE_DRAW_CUT_H
#define CCORE_DRAW_CUT_H
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "scene.h"

/* A loop (closed, the first point not repeated) or an open chain. */
typedef struct {
    double (*p)[2];
    size_t n;
} cc_ring;

/* cut::Section: closed loops (outlines and holes, even-odd) and open chains. */
typedef struct {
    cc_ring *loops;
    size_t nloops;
    cc_ring *open;
    size_t nopen;
} cc_section;

/* cut::section: the section of `m` by the plane z = h (vertices on the plane count as above). */
int cc_cut_section(const cc_mesh *m, double h, Arena *a, cc_section *out);

/* cut::chain: segments joined at end points (welded at 1e-4 mm) into loops and open chains. */
int cc_cut_chain(const double (*segs)[2][2], size_t n, Arena *a, cc_section *out);

/* cut::facing_up_region: the outline loops of the faces of `m` turned upwards. */
int cc_cut_facing_up(const cc_mesh *m, Arena *a, cc_ring **loops, size_t *nloops);

/* cut::clip_z: the part of a-b with z in [z0, z1], projected; 1 and `out` set, or 0 (none). */
int cc_cut_clip_z(const double a[3], const double b[3], double z0, double z1, double out[2][2]);

/* cut::edge_seen_from_above, for the 8 snorm8 normals of an edge (scene.h). */
int cc_cut_edge_seen_from_above(const int8_t n[8]);

/* cut::area: signed area, counter-clockwise positive. */
double cc_cut_area(const cc_ring *l);

/* crop::Rect, an axis-aligned rectangle. */
typedef struct {
    double lo[2], hi[2];
} cc_crop_rect;

int cc_crop_contains(const cc_crop_rect *r, const double p[2]);
/* Rect::polygon (Sutherland–Hodgman): `out` set (n >= 3), or out->n = 0 when nothing is left. */
int cc_crop_polygon(const cc_crop_rect *r, const cc_ring *poly, Arena *a, cc_ring *out);
/* Rect::segment (Liang–Barsky): 1 and `out` set, or 0 when outside. */
int cc_crop_segment(const cc_crop_rect *r, const double s[2][2], double out[2][2]);
/* Rect::polyline: the pieces inside. */
int cc_crop_polyline(const cc_crop_rect *r, const cc_ring *line, Arena *a, cc_ring **pieces, size_t *npieces);
#endif
