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
/* Sheet::to_pdf of sheet `sh` (ticket D6, pdf_out.h), drawn as sheet_svg draws it (not kept:
 * drawn again), appended to `pdf`. `fonts`: Roboto-Regular and -Bold (the style's font is Roboto),
 * which the carrier hands over (350 KB each, not in the wasm); NULL or a missing weight the text
 * uses is an error. The producer is "Bimble <the C core's version>". 0 ok; else -1 with the text
 * in `err` (the drawing's error, "the Roboto font files are needed", out of memory; the viewer
 * build: "this build is view-only"). */
int cc_paper_sheet_pdf(struct cc_session *s, const cc_scene *sc, const cc_sheet *sh, const unsigned char *const fonts[2],
                       const size_t font_len[2], Buf *pdf, Buf *err);
/* Frees what is kept (cc_session_free). */
void cc_paper_cache_free(struct cc_session *s);

/* ---- D7: the PDF export in the app ----
 * The queries `sheet_pdf {"sheet": id, "path"?: file}` and `view_pdf {"view": id, "path"?: file}`
 * (CC_OK / CC_ERR as cc_call): the sheet drawn as sheet_svg draws it, or the 2D view drawn as Rust's
 * "Export PDF…" of the plan panel (lib.rs plan_options: the view's scale, the smallest paper it
 * fits on, not bare, the section marks of every view; the same style, template, project, date and
 * version as sheet_svg), written by cc_paper_pdf (Sheet::to_pdf).
 *   - With "path" on a carrier with files (native): written there atomically (pf->write_file, as
 *     export_ifc) and the answer is {"path": file, "bytes": n}.
 *   - Without "path", and always in the wasm module (no files): the answer is the PDF's bytes
 *     themselves (the native host sends them as the POST body, the module as its answer buffer).
 *   - SIZE1: the text is Helvetica (one of the 14 fonts every PDF reader has: nothing embedded, no
 *     file shipped), drawn with Helvetica's metrics, unless the body asks `"font": "roboto"` (also
 *     `"helvetica"`, or null; another value is the error `font: "helvetica" or "roboto"`). Roboto
 *     is used when the carrier handed the two shipped files over (cc_paper_set_fonts) or they are
 *     in the folder of cc_paper_set_font_dir (read on that export); else, or when a file is not
 *     the shipped one, the drawing is made in Helvetica and a note goes to the log: the export
 *     does not fail.
 * Errors: "no sheet with id \"…\"", "no view with id \"…\"", "no model open", the drawing's own
 * text, "<file>: cannot write"; the viewer build: "this build is view-only". `view`: 0 sheet_pdf,
 * 1 view_pdf. */
int cc_paper_pdf_query(struct cc_session *s, const cc_scene *sc, int view, const char *body, size_t n, Buf *out);
/* SIZE1: the query `pdf_fonts` (no body) -> {"helvetica":true,"roboto":bool}: roboto when the two
 * shipped Roboto files are handed over or in the font folder (read now). The viewer build: "this
 * build is view-only". */
int cc_paper_fonts_query(struct cc_session *s, Buf *out);
/* The two Roboto files (Regular, Bold), copied; NULL / 0 drops one. 0 ok, -1 out of memory. */
int cc_paper_set_fonts(struct cc_session *s, const unsigned char *regular, size_t nr, const unsigned char *bold,
                       size_t nb);
/* The folder holding Roboto-Regular.ttf and Roboto-Bold.ttf, read through pf->read_file on the
 * first Roboto export or `pdf_fonts` that finds no files handed over, again until both are found
 * (copied; NULL clears). 0 ok, -1 out of memory. */
int cc_paper_set_font_dir(struct cc_session *s, const char *dir);
/* The version the PDF export names (the frame's note, the producer); NULL: the C core's
 * (CC_IFC_VERSION). For tests that compare with Rust's goldens. 0 ok, -1 out of memory. */
int cc_paper_set_version(struct cc_session *s, const char *version);
/* Frees the fonts, the folder and the version (cc_session_free). */
void cc_paper_fonts_free(struct cc_session *s);
#endif
