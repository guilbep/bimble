/* Managing a reference plan (ticket L2; Rust: bimble-viewer links.rs Manage links,
 * views.rs:263 reload_reference; ADR 0021). The references themselves (link_pdf, link_dxf, the
 * rows of query links, query view_references) are links_ref.h's (L1); this file holds what a user
 * does next with one, PDF or DXF alike.
 *
 * action manage_links {"op", "id": "<reference GlobalId>", ...} (calls.c, the `L2` block: a body
 * with "id" and no "name"; "name" still names a linked model, links.h):
 *   move {"offset_mm": [x, y]}  rotate {"rotation_deg": d}  show  hide  remove
 *   reload            its `source` read again (natively), kept where it is (offset, rotation,
 *                     visibility, id: views.rs:263); a PDF at its view's scale (Rust's pdf_scale)
 *   reload {"path"}   Reload from…: another file in its place (the wasm module: a file the page
 *                     staged under that name)
 * One set_view Command each (one undo step); the same values again: no Command, no step.
 * Errors: "no reference plan GlobalId(…)", "op: one of …", the reader's text. The viewer-only
 * build refuses reload ("this build is view-only"); the other ops need an editable model. */
#ifndef CCORE_SESSION_LINKS_DXF_H
#define CCORE_SESSION_LINKS_DXF_H
#include "arena.h"
#include "buf.h"
#include "command.h"
#include "json_r.h"

struct cc_session;

/* `body`: the parsed object (with "id"). 0 ok (`l` may stay empty: no change), -1 with the error
 * in `out`. The Command's strings live in `a`. */
int cc_ref_manage(struct cc_session *s, const JrVal *body, Arena *a, cc_cmds *l, Buf *out);
#endif
