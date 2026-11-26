/* Reference planes in the session (WAVE1B, ADR 0075, src/refplane.h): the active work plane and
 * the three-point pick tool. Session state: no history step, rev unchanged.
 *
 * The work plane of the place tools and of Measure is the active storey's level (lib.rs
 * storey_point) until one is chosen here: a stored reference plane (an IfcVirtualElement), a level
 * (the horizontal plane at its elevation) or a grid axis (the vertical plane through it). The two
 * last are implicit: listed, never stored. A chosen plane that no longer exists (deleted, undone)
 * is no work plane: the storey's level again.
 *
 * Calls (op "tool"):
 *   work_plane {"kind": "plane" | "level" | "axis" | null, "id": GlobalId}  choose (null: the storey)
 * The action reference_planes turns the pick tool on and off: three clicks in a view (the measure
 * tool's snapping: corners, edges, faces, the work plane) make a plane through the three points
 * (verb reference_plane_add, one history step); Esc takes the last point back, then stops.
 *
 * query reference_planes (null without an editable model):
 *   {"active": {"id", "kind", "name"} | null,
 *    "implicit": [{"elevation", "id", "kind": "level", "name"}, {"end", "id", "kind": "axis", "name", "start"}],
 *    "pick": {"on": b, "points": [[x, y, z]...]},
 *    "planes": [{"corners": [[x,y,z] x 4], "height", "id", "name", "normal", "origin", "storey", "width",
 *                "x_axis"}]}                                         world mm, planes in element order */
#ifndef CCORE_SESSION_REFPLANES_H
#define CCORE_SESSION_REFPLANES_H
#include "buf.h"
#include "json_r.h"
#include "json_w.h"
#include "model.h"

enum { CC_WP_NONE, CC_WP_PLANE, CC_WP_LEVEL, CC_WP_AXIS };

typedef struct {
    int kind; /* CC_WP_* */
    cc_gid id;
    int pick;     /* the three-point tool is on */
    int npick;    /* points taken */
    double pts[3][3]; /* world mm */
} cc_wplane;

/* The active work plane of `m`: 1 with its origin and unit normal (world mm), 0 when none (or the
 * chosen one is gone). `level`: the active storey's elevation (a grid axis' plane passes there). */
int cc_wplane_frame(const cc_wplane *wp, const cc_model *m, double level, double o[3], double n[3]);
/* tool work_plane: 0 ok, -1 with the reason in `err`. */
int cc_wplane_set(cc_wplane *wp, const cc_model *m, const char *body, size_t n, Buf *err);
/* The query's answer. */
void cc_wplane_json(const cc_wplane *wp, const cc_model *m, JsonW *w);
#endif
