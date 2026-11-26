/* The measure tool (ticket T1, ADR 0068 rule 2: interactive tools live in C): two clicks on the
 * view make one measurement. The page forwards the pointer (op "tool", names measure_*); this file
 * picks the surface under it, snaps to a corner, else an edge, else the face, and keeps the
 * measurements. Session state: no history step, no rev change; read-only scenes and linked models
 * are measured as the open model.
 *
 * Ported from the Rust desktop (crates/bimble-viewer):
 *   measure.rs `snap` (VERTEX_SNAP_PX 10, EDGE_SNAP_PX 8; corners and edges behind the surface
 *     under the cursor are not snapped: depth <= hit + max(0.2 % of it, 5 mm)), `unsnapped` (Alt),
 *     `orthogonal` (Shift), `Dimension` (`distance` in f32, `perpendicular` within cos 1 degree);
 *   bimble_api::measure `format_mm` and `measurements_json` (the viewer's, moved below it so the
 *     golden `golden/measure.txt` is made by the same code);
 *   lib.rs `snap_at` (the open model's visible elements and the visible links, the nearest hit;
 *     a linked point is moved by the link's offset), the measure branch of the pointer handler
 *     (a click takes the hover; the second makes a dimension; Ctrl goes on from there), Esc
 *     (lib.rs:3031: drops the first point, a second Esc leaves the tool), `draw_dimensions` (the
 *     text: the distance, and "(faces …)" when both points lie on parallel faces).
 * Deviations (DEVIATIONS.md 7):
 *   - the screen distance is not projected through the camera (C has none): the page sends the
 *     pointer ray and `ray_tol`, the ray `tol_px` pixels beside it; a point's pixel distance is its
 *     distance to the ray over the gap between the two rays at its depth.
 *   - corners and edge points outside the section box (or a plan's range) are not snapped; Rust
 *     snaps them when they are in front of the hit.
 *   - the corners and edges snapped to are those of every element the ray meets within the
 *     visible depth, not only the nearest one's (a column on a slab meets the ray at the slab's
 *     depth too: either could be "the" hit, and the corner under the cursor would be lost).
 *   - reference plans (DXF, PDF) are not in the C core: no snap on them. */
#ifndef CCORE_SESSION_MEASURE_H
#define CCORE_SESSION_MEASURE_H
#include <stddef.h>
#include "buf.h"
#include "json_w.h"
#include "scene.h"
#include "visibility.h"

enum { CC_MSNAP_VERTEX, CC_MSNAP_EDGE, CC_MSNAP_FACE }; /* measure.rs SnapKind */

#define CC_MEASURE_VERTEX_PX 10.0 /* measure.rs VERTEX_SNAP_PX */
#define CC_MEASURE_EDGE_PX 8.0    /* measure.rs EDGE_SNAP_PX */
#define CC_MEASURE_MAX 10000      /* measurements kept: a budget (a click past it answers 400) */

typedef struct { /* measure.rs Snap */
    float p[3];    /* scene coordinates: mm relative to `org` (f32, as Rust) */
    double org[3]; /* the shown scene's origin when it was taken (world mm) */
    int kind;      /* CC_MSNAP_* */
    int has_n;
    float n[3]; /* the face normal (unit), for a point on a face */
} cc_msnap;

typedef struct { /* measure.rs Dimension */
    cc_msnap a, b;
} cc_mdim;

typedef struct cc_measure {
    int on; /* lib.rs measure_on */
    int has_first, has_hover;
    cc_msnap first, hover; /* lib.rs measure_first, hover_snap */
    cc_mdim *dims;         /* lib.rs dimensions, oldest first */
    size_t n, cap;
} cc_measure;

/* A scene the pointer can hit: the shown one, or a linked model's. */
typedef struct {
    const cc_scene *sc;
    float off[3];       /* added to its points (a link: its origin - the shown scene's, f32 as Rust) */
    const cc_vis *vis;  /* its hidden elements (visibility.h; reveal shows them); NULL: none */
} cc_mlayer;

/* What a pointer call sees: the layers (the shown scene first), the box that cuts (scene
 * coordinates; NULL: none) and the shown scene's origin. */
typedef struct {
    const cc_mlayer *layers;
    size_t nlayers;
    const float *clip_min, *clip_max;
    const double *org;
} cc_mview;

void cc_measure_free(cc_measure *m);  /* the measurements; the state is zeroed */
void cc_measure_reset(cc_measure *m); /* another model: the points and measurements go (lib.rs:1677) */

/* measure.rs Dimension::distance: |b - a| in f32 (x*x + y*y + z*z, then sqrt, as glam). */
float cc_mdim_distance(const cc_mdim *d);
/* Dimension::perpendicular: 1 with the distance between the two faces when both points are on
 * parallel faces, else 0. */
int cc_mdim_perpendicular(const cc_mdim *d, float *out);
/* bimble_api::measure::format_mm, appended to `b`: "6 000 mm" (U+2009), "12.3 mm", "NaN mm". */
void cc_format_mm(Buf *b, float x);
/* bimble_api::measure::measurements_json: [{"id","label","value_mm"}] for the dimensions. */
void cc_measurements_json(JsonW *w, const cc_mdim *d, size_t n);

/* measure.rs snap (free: unsnapped) on the nearest surface the ray meets in `v`: 1 with `out`,
 * 0 when the ray meets nothing, -1 out of memory. `ray_tol` NULL: no corner or edge snapping. */
int cc_measure_snap(const cc_mview *v, const double ray[6], const double *ray_tol, double tol_px, int free_, cc_msnap *out);
/* measure.rs orthogonal: `to` moved onto the x, y or z axis through `from` (its offset's longest
 * part); no normal. */
void cc_measure_orthogonal(const cc_msnap *from, cc_msnap *to);

/* 1 if `name` is one of the tool's calls: measure_pointer, measure_click, measure_cancel,
 * measure_clear. */
int cc_measure_name(const char *name);
/* The action `measure` (ribbon.rs:691): on / off, the first point and the hover dropped. */
void cc_measure_toggle(cc_measure *m);
/* One call (calls.h "measure"). `v` NULL: nothing is shown. Answers the tool's state into `out`
 * (or the error text): 200 / 400 as calls.h. */
int cc_measure_call(cc_measure *m, const char *name, const char *body, size_t n, const cc_mview *v, Buf *out);
/* The tool's state (calls.h "measure"), keys sorted. */
void cc_measure_json(const cc_measure *m, JsonW *w);
#endif
