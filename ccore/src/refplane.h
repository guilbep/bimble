/* WAVE1B (ADR 0075 decisions 9-11, DEVIATIONS 32): reference planes. A named work plane is an
 * element of class IfcVirtualElement: no generator (no shape, nothing to cut, no material
 * quantity), its placement the plane's frame (origin in the storey's frame as every element's,
 * z_axis = the unit normal, x_axis in the plane), its size the properties Bimble/PlaneWidth and
 * Bimble/PlaneHeight (mm, along x and along z x x; left out: CC_REFPLANE_SIZE). So a plane is
 * created, renamed, moved, deleted, undone, merged, diffed and saved by the element paths that
 * exist, and a model without one keeps its bytes. At most CC_REFPLANES_MAX in a model.
 * Levels and grid axes are the implicit planes (listed by the session, never stored).
 *
 * The verb (a C addition): reference_plane_add {storey, name?, width?, height?, and either
 *   origin [x, y, z] (world mm) + normal [x, y, z] (+ x_axis), or points [[x,y,z] x 3] (world mm,
 *   not on one line: the plane through them, origin the first, x_axis towards the second)}.
 * Handlers follow verb_table.h's cc_verb_fn. Pure (layer L1): no files, clock or globals. */
#ifndef CCORE_REFPLANE_H
#define CCORE_REFPLANE_H
#include "buf.h"
#include "command.h"
#include "json_r.h"
#include "model.h"

#define CC_REFPLANES_MAX 1000        /* reference planes in a model (an untrusted file's budget) */
#define CC_REFPLANE_SIZE 10000.0     /* mm: the default width and height of a plane's outline */
#define CC_REFPLANE_WIDTH_KEY "Bimble/PlaneWidth"
#define CC_REFPLANE_HEIGHT_KEY "Bimble/PlaneHeight"

/* 1 for a class with no generator of its own (IfcElementAssembly, IfcVirtualElement). */
int cc_class_shapeless(cc_ifc_class c);
/* The checks of a reference plane element beyond the placement's (model.c, command.c): no
 * generator, no openings, the two sizes (when present) finite within [10, 1e6] mm. 0 ok, -1
 * with the reason appended to `b`. */
int cc_refplane_check(const cc_element *e, Buf *b);
/* 0 when setting `key` to `value` (NULL: removing it) keeps a reference plane valid. */
int cc_refplane_prop_ok(cc_str key, const cc_str *value);
/* Its width and height (mm): the properties, else CC_REFPLANE_SIZE. */
void cc_refplane_size(const cc_element *e, double *w, double *h);
/* Its frame in world mm: origin (the storey's elevation added), unit normal, x and y axes. */
void cc_refplane_frame(const cc_element *e, double storey_elevation, double o[3], double n[3], double x[3], double y[3]);
/* Its four corners in world mm (the outline, centred on the origin). */
void cc_refplane_corners(const cc_element *e, double storey_elevation, double c[4][3]);
/* The plane through three points: unit normal n (right-handed from p0 -> p1 -> p2), x along
 * p0 -> p1. 0 ok, -1 when the points are (nearly) on one line or not finite. */
int cc_plane_from_points(const double p[3][3], double n[3], double x[3]);
/* A unit x axis in the plane of unit normal n: world x projected, else world y. */
void cc_plane_x_axis(const double n[3], double x[3]);
/* The ray o + t d meets the plane (origin po, normal n): 0 with the point in q (t >= 0),
 * -1 when parallel or behind. */
int cc_ray_meets_plane(const double o[3], const double d[3], const double po[3], const double n[3], double q[3]);
/* The point q moved onto the plane along its normal. */
void cc_plane_project(const double po[3], const double n[3], const double q[3], double out[3]);

int cc_vh_reference_plane_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err);

#endif
