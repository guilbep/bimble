/* Sheets in the session (ticket SH1): what the Sheets page draws before the drawings exist (D5).
 *
 * The query `sheet_layout {id}` (a C addition; Rust draws the whole sheet, `sheet_svg`):
 *   {"height_mm", "id", "name", "number", "paper", "placed": [...], "portrait", "width_mm"}
 * width and height are the paper's in the sheet's orientation (sheet.rs size_mm). Each placed
 * view, in drawing order:
 *   {"at_mm": [x, y], "box": "drawing"|"estimate", "box_mm": [x0, y0, x1, y1], "id", "kind",
 *    "scale", "view", "view_name"}
 * scale: the 1:N the view is drawn at (ADR 0046: the view's own; a view whose scale is not said
 * takes the one scale all its placements share, sheet.rs adopted_scales). box_mm: paper mm from
 * the top-left corner. "drawing" (D5): the box of the view as the sheet's drawing has it
 * (SheetInfo.placed, paper.h). "estimate": a view with no drawing (the 3D view), a sheet that
 * does not draw, or the viewer build: centred on at_mm, the view's crop, else the model's box
 * (plan: x and y; a section: its width and height), divided by the scale. Keys sorted. */
#ifndef CCORE_SESSION_SHEETS_H
#define CCORE_SESSION_SHEETS_H
#include <stddef.h>
#include "buf.h"
#include "model.h"
#include "scene.h"

struct cc_session;

/* The query; `sc` is the session's scene of the open model (NULL: boxes of size 0).
 * CC_OK with the JSON, or CC_ERR with the text ("no sheet GlobalId(…)", the read-only text). */
int cc_sheet_layout(struct cc_session *s, const cc_scene *sc, const char *body, size_t n, Buf *out);

/* A sheet tab (calls.c `tab`, type "sheet"): 1 when `id` is a sheet of the open model. The page
 * opens on that sheet; no tab state is kept in C (views.h: only view tabs open). */
int cc_sheet_known(const struct cc_session *s, const cc_gid *id);
#endif
