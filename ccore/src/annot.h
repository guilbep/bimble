/* Dimensions and texts of a view, resolved for drawing (ticket C4, ADR 0068 rule 9).
 *
 * A dimension's end is an Anchor {at, element?, feature?}. Rust's Anchor has `at` (world mm) and
 * `element`; `feature` is a C addition (golden/DEVIATIONS.md): the point of the element's
 * generator the end is taken on, so the dimension follows the element when it moves or changes
 * size. The point is computed when asked (cc_anchor_point), never stored back. Features:
 *   "origin"    the placement origin: a column's or footing's centre, a beam's or wall's start,
 *               a slab's or polygon's local origin;
 *   "end"       a beam's end (origin + length along its z axis), a wall's end (along its x axis);
 *   "corner:N"  the N-th corner of the generator's profile at its base: a rect column or pad
 *               footing 0..3 ((-w/2,-d/2), (w/2,-d/2), (w/2,d/2), (-w/2,d/2)), a beam 0..3 at its
 *               start and 4..7 at its end, a wall 0..3 ((0,-t/2), (L,-t/2), (L,t/2), (0,t/2)),
 *               a slab or extruded polygon its outline's points, a mesh its positions.
 * All through the element's placement, on its storey's elevation (as scene.c places meshes).
 *
 * The drawing follows Rust (crates/bimble-drawing/src/viewdraw.rs dimension_on_paper,
 * crates/bimble-viewer/src/layout.rs dim_axis_and_offset, template.rs DimUnit::format) in plan
 * coordinates: world x, y; north up (the C plan has no true-north turn yet). */
#ifndef CCORE_ANNOT_H
#define CCORE_ANNOT_H
#include <stddef.h>
#include "buf.h"
#include "json_w.h"
#include "model.h"

/* The feature text is one this file knows the shape of ("origin", "end", "corner:N"). */
int cc_feature_name_ok(cc_str f);
/* The world point (mm) of `feature` on element `e` of `m`. 0 ok, -1 the element has no such
 * feature. */
int cc_feature_point(const cc_model *m, const cc_element *e, cc_str feature, double out[3]);
/* An anchor's point now: its feature's when the element and the feature are there, else `at`.
 * Returns 1 when it follows a feature, 0 when `at` is used (also with `m` NULL). */
int cc_anchor_point(const cc_model *m, const cc_anchor *a, double out[3]);
/* The feature of an element of `m` within `tol` mm (plan distance) of `p` (world x, y), elements
 * on `storey` (NULL: any) only; the nearest, ties to the lower class (columns first), then the
 * lower id. Writes the element, the feature (NUL-terminated, < 24 bytes) and its point.
 * 1 found, 0 none. */
int cc_feature_near(const cc_model *m, const double p[2], double tol, const cc_gid *storey, cc_gid *el, char feat[24],
                    double at[3]);

/* dimension_on_paper in plan coordinates (world x, y; offset to the left of a->b): the unit
 * direction, the line's ends and the measured length. 0 ok, -1 degenerate (Rust's None). */
int cc_dim_line(const double a[2], const double b[2], double off, cc_dim_axis axis, double u[2], double qa[2],
                double qb[2], double *len);
/* layout.rs dim_axis_and_offset: the axis and offset a third point `c` gives (plan coords). */
void cc_dim_axis_offset(const double a[2], const double b[2], const double c[2], cc_dim_axis *axis, double *off);

enum { CC_UNIT_MM, CC_UNIT_CM, CC_UNIT_M };
/* DimUnit::format. */
void cc_dim_format(Buf *b, double mm, int unit);

/* The view's annotations resolved, keys sorted:
 *   {"items":[…],"scale":n,"unit":"mm"|"cm"|"m","view":id}
 * a dimension: {"a":[x,y],"anchors":[A,A],"axis","b":[x,y],"dir":[x,y],"id","kind":null|s,
 *   "mid":[x,y],"offset_mm","qa":[x,y],"qb":[x,y],"text","type":"dimension","value_mm"}
 *   with A = {"element":id|null,"feature":s|null,"follows":bool}; dir: the value text's reading
 *   direction (unit, x > 0, or x = 0 and y > 0); a degenerate one has qa, qb, mid, dir null,
 *   value_mm 0, text "".
 * a text: {"angle_deg","at":[x,y],"id","kind","size_mm","text","type":"text"}.
 * World plan mm (size_mm: paper mm, as Rust: times the view's scale on the model). */
void cc_annotations_json(JsonW *w, const cc_model *m, const cc_view *v, int unit);
#endif
