/* The call router (L5): one function per call, as `Viewer::call` in
 * crates/bimble-viewer/src/calls.rs, on a session (session.h). The carriers (host/server.c over
 * HTTP, src/wasm_api.c in the module) own the transport and the change feed. */
#ifndef CCORE_SESSION_CALLS_H
#define CCORE_SESSION_CALLS_H
#include <stddef.h>
#include "buf.h"
#include "session.h"

typedef Buf cc_buf;

/* Status of a call, as HTTP: OK = 200 with the answer text (empty for a void call),
 * ERR = 400 with the error text the lab throws (calls.rs `Err(String)`, serve.rs:282-283). */
enum { CC_OK = 200, CC_ERR = 400 };

/* One call. `op` is one of cc_known_op (the router checked it), `name` is the decoded name
 * segment ("" for "-"), `body` holds `n` bytes of UTF-8 (NUL-terminated after n). Writes the
 * answer or the error text into `out` (empty on entry) and returns CC_OK or CC_ERR. */
/* op "verb" (ADR 0068 rule 3): `name` is the verb (Rust's 14 or a C addition, verb_table.h), the
 * body Rust's: the verb's argument object (empty or "null": {}). An object body may also carry two
 * top-level keys, taken out before the verb reads its arguments (no verb has fields of these names):
 *   "dry_run": true  -> 200 with the commands the verb makes (serde JSON array, maybe []); nothing
 *                       applied: no history step, rev unchanged. Command::apply's own checks (a
 *                       blank name) are not run by a dry run.
 *   "if_rev": N      -> runs only if N is the session's rev (the change feed's "rev"); else 400
 *                       with "conflict: the model changed (rev <rev>, expected N)". A 400, not 409:
 *                       the carriers (HTTP host, wasm) know 200 and 400 only; the text tells.
 * A body without them is passed through unchanged (old bodies work as before). A real run answers
 * 200 with an empty text, as Rust's. */
/* op "tool" (a C addition, ADR 0068 rule 2; session/tool.h): the place tool. The page forwards the
 * pointer while `placing` is active; C snaps, keeps the points and makes the ghost. Names:
 *   pointer  body {"ray":[ox,oy,oz,dx,dy,dz], "ray_tol":[…]} (scene coordinates, as GET /mesh:
 *            world mm minus the BMSH origin; ray_tol is the ray 12 px beside it and gives the snap
 *            tolerance) or {"at":[x,y], "tol_mm":t} (world plan mm), plus "free" (no snap, rounded
 *            to 10 mm: Rust's Shift) and "ortho" (the 2nd point along x or y from the 1st), both
 *            optional booleans. Moves the hover; no model change. While not placing: 200, inactive.
 *   click    the same body (empty: where the pointer was): adds the hover as a point; when the
 *            kind has its points (column 1, beam/wall/slab rectangle 2) runs `verb place` once
 *            (the session's sizes, material, storey; a slab gets top_offset_mm): one history step,
 *            rev + 1, the points dropped (also on an error, as edit.rs). 400: not placing, the
 *            verb's error, a polygon slab (not ported).
 *   cancel   Esc: stops placing and drops the points.
 * Each answers the tool's state, as `query ghost`:
 *   {"active":bool,"ghost":null|{"edges":[…],"idx":[…],"origin":[x,y,z],"pos":[…]},
 *    "ghost_error":null|text,"hover":null|{"at":[x,y],"snap":null|label,"z":elevation},
 *    "kind":…,"placed":null|GlobalId,"points":[[x,y],…]}
 * ghost: the element the next click adds (a dry run of the verb meshed by the scene code), f32
 * positions relative to `origin` (the shown scene's origin); ghost_error: why there is none; snap:
 * "axis crossing", "column centre", "corner", "grid axis", "edge" or null (rounded).
 * The section box's tool (op "tool", ticket B3, session/visibility.h), with any scene shown (else
 * 400 "no model open"). Session state: no history step, no rev change.
 *   box_press   {"ray":[…], "ray_tol":[…]} (scene coordinates): grabs the face handle under the
 *               pointer (the face centre nearest to the ray, within the ray_tol distance); none,
 *               or the box off: inactive, and the page orbits.
 *   box_move    {"ray":[…]}: the held face follows the pointer (the point of its axis nearest to
 *               the ray), at least 10 mm thick; the session's box is unchanged (a preview).
 *   box_release {"ray":[…]} or empty: the preview becomes the session's box (one change).
 *   box_cancel  the drag dropped, the box as it was.
 *   box_set     {"min":[x,y,z], "max":[x,y,z], "on"?: bool}: the box (scene coordinates, mm
 *               relative to the scene origin, as query visibility), at least 10 mm per axis.
 * Each answers {"active":bool,"box":null|{"max":[…],"min":[…],"yaw":0.0},"face":null|0..5,"on":bool}
 * (face k: axis k/2, its min side for even k); box: the preview while active.
 * query "visibility_sets" (a C addition): the whole sets the page draws from (query visibility
 * caps its lists at 50): {"hidden_in_view":[…],"isolated_in_view":[…],"now":null|{"ids":[…],
 * "isolate":bool},"reveal":bool}.
 * The visibility actions (hide_now, isolate_now, hide_in_view, isolate_in_view, unhide,
 * reveal_hidden, clear_temporary, show_all_in_view, section_box, box_to_selection) run as
 * ribbon.rs run_action; the change JSON's "view" carries their signature (calls.rs:152-161).
 * Linked models (ticket B5): action link_model {"path"}, action manage_links {"op","name"},
 * query links, select {"link","ids"}, query element {"id","link"}, the change JSON's "linked" and
 * "links", and the /mesh LNKS trailer: session/links.h. */
int cc_call(cc_session *s, const char *op, const char *name, const char *body, size_t n, cc_buf *out);

/* The change JSON (`Viewer::change_json`, calls.rs:134-167) for the state now. The host
 * publishes it after each call (as the twin's loop does) and once at start. */
void cc_change_json(cc_session *s, cc_buf *out);

/* 1 if `op` is one of the ops of `dispatch_op` (calls.rs:27-41), else 0. */
int cc_known_op(const char *op);
#endif
