/* A plan view's range in absolute levels (L2): view.rs `resolve`, byte for byte, shared by the
 * screen (session/views.c, the plan's clip box) and the drawing (draw_view.c, viewdraw::setup).
 * In the viewer build too (no drawing code here). */
#ifndef CCORE_VIEW_RANGE_H
#define CCORE_VIEW_RANGE_H
#include "buf.h"
#include "model.h"

/* view.rs resolve of the plan `v` on the model's storeys: its planes in world mm, top, cut,
 * bottom, depth as CC_RANGE_* (+-infinity where unlimited: top and cut up, bottom down, depth
 * down looking down and up looking up). Checks the offsets (within +-1000 m), that the cut has a
 * level and the planes' order. 0 ok; 1 not valid, with Rust's text appended to `err` (may be
 * NULL); -1 out of memory. A view that is not a plan is "unknown storey". */
int cc_view_resolve(const cc_model *m, const cc_view *v, double out[4], Buf *err);
#endif
