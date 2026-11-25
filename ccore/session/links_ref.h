/* Reference plans in the session (ticket L1; Rust: crates/bimble-viewer/src/viewing.rs:1470-1600
 * add_reference and its placement, state.rs:199-216 links_json, views.rs:150-200). A reference
 * is a PDF's (or, ticket L2, a DXF's) lines kept in a 2D view of the model (view.h
 * cc_reference): added through one set_view (or add_view, for a default view) Command, so one
 * undo step, saved with the model. The reader is src/ref_read.h (src/pdf_in.h).
 *
 * Calls (calls.c hooks, marked L1):
 *   action link_pdf {"path": "<file>", "scale"?: N, "offset_mm"?: [x, y], "rotation_deg"?: d}
 *       reads the file (the wasm module: the bytes the page staged, wasm_api.h host_open flag 8)
 *       at 1:N (default: the view's scale), adds it to the active 2D view. Placed as Rust places
 *       it (ADR 0045): as it is when it overlaps the model, else by the survey coordinates of a
 *       georeferenced model, else centred on the model, upright in a turned plan. offset_mm /
 *       rotation_deg (a C addition, for agents and tests) place it as given instead. 400 with the
 *       reader's text; nothing changes then. Enabled when an editable model is open.
 *   query links: the linked models (links.h), then every view's references
 *       {"id": GlobalId, "kind": "pdf"|"dxf", "name", "visible"} (state.rs links_json).
 *   query view_references {"view"?: id}: the references of a view (default: the active one),
 *       placed, as polylines in the view's plane (a plan: world x, y mm):
 *       {"references": [{"id","name","polylines": [[x0,y0,x1,y1,…],…],"segments": n,"visible"}],
 *        "segments": all, "shown": drawn, "view": id}, keys sorted. Hidden references are listed
 *       without polylines. At most CC_REF_SHOWN_MAX segments are given (shown < segments then).
 * Deviations (DEVIATIONS.md section 13): a section view's reference is placed as it is (Rust also
 * centres it on the model across the section); no file dialog (the page has its picker). */
#ifndef CCORE_SESSION_LINKS_REF_H
#define CCORE_SESSION_LINKS_REF_H
#include <stddef.h>
#include "arena.h"
#include "buf.h"
#include "command.h"
#include "json_w.h"
#include "scene.h"

struct cc_session;


#define CC_REF_SHOWN_MAX ((size_t)500000)

/* link_pdf: the body read, the file read and placed; the Command (in `cmds`, its view in `a`,
 * which must live until the Command is applied). 0 ok, -1 with the error text in `out`. */
int cc_refs_link(struct cc_session *s, const char *format, const char *body, size_t n, const cc_scene *sc,
                 Arena *a, cc_cmds *cmds, Buf *out);
/* query links: `out` holds the linked models' array; the references' rows go before its ']'. */
void cc_refs_links_append(struct cc_session *s, Buf *out);
/* query view_references. 0 ok, -1 with the error text in `out`. */
int cc_refs_view_references(struct cc_session *s, const char *body, size_t n, Buf *out);
#endif
