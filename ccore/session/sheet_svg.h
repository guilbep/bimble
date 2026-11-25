/* Sheets on paper in the session (ticket D5): the query `sheet_svg {id}` as calls.rs:487-512,
 * and each drawn view's box on the sheet (SheetInfo.placed), for the handles of the Sheets page.
 * sheet_layout (sheets.h) still gives its estimates: its answers are SH1's own golden
 * (golden/sheets/verbs.txt), rewritten only with the owner's consent.
 *   {"svg": "<svg …"}: sheetdraw::draw (src/draw_sheet.h) of the open model's sheet with the
 *   options the sheet export uses (viewing.rs export_sheet): the default graphic style (the C
 *   session keeps none of its own), the default plan template with the model's project
 *   information, the project named after the file's stem, today's date from the platform's wall
 *   clock (UTC, bimble_core::date::today), and the C core's version in the frame's note.
 * Errors as Rust's: "no sheet with id \"…\"", the drawing's own text.
 * The last sheet drawn is kept per (rev, sheet, date, file): sheet_svg and sheet_layout of the
 * same sheet at the same rev draw it once. The viewer-only build has no drawing code: sheet_svg
 * answers "this build is view-only", cc_paper_placed_box 0. */
#ifndef CCORE_SESSION_SHEET_SVG_H
#define CCORE_SESSION_SHEET_SVG_H
#include <stddef.h>
#include "buf.h"
#include "model.h"
#include "scene.h"

struct cc_session;

/* The query; `sc` is the session's scene of the open model. CC_OK with the JSON, or CC_ERR with
 * the text. */
int cc_paper_sheet_svg(struct cc_session *s, const cc_scene *sc, const char *body, size_t n, Buf *out);
/* The box (paper mm: x0, y0, x1, y1) of placed view `placed` of sheet `sh` as the drawing has it
 * (SheetInfo.placed): 1 with `out`; 0 not drawn (a view with no drawing, a sheet that does not
 * draw, the viewer build, out of memory). */
int cc_paper_placed_box(struct cc_session *s, const cc_scene *sc, const cc_sheet *sh, const cc_gid *placed,
                        double out[4]);
/* Frees what is kept (cc_session_free). */
void cc_paper_cache_free(struct cc_session *s);
#endif
