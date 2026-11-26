/* The Links panel's rows (ticket LNK1; Rust: bimble-viewer links.rs:38 link_rows, the "Manage
 * links" window, and lib.rs:4402 the side panel's Links group). `query links` stays Rust's
 * (state.rs links_json: id, kind, name, visible); the panel needs what that answer leaves out:
 * where each row shows, where it can be read again from, and a reference plan's placement for the
 * move and rotate fields.
 *
 * query links_panel -> {"models": [...], "references": [...]} (keys sorted):
 *   models     in link order: {"kind": "ifc"|"bimble"|"bimbleview"|"json"|…, "missing": bool,
 *              "name", "path": "<as linked>", "visible"} and (LINKF, links.h) "bytes" (the file),
 *              "cache" (its .bimbleview cache, or null), "cache_mtime_s", "colour" (0..11 or null),
 *              "elements", "from_cache", "log" (its conversion report, or null), "message" (why
 *              it failed or was refused, a note when ready, or null), "move_mm" [x, y, z],
 *              "origin" ("file"|"origin"|"base_point"|"survey_point"), "pinned", "progress"
 *              (0..100), "rotation_deg", "state" ("ready"|"missing"|"queued"|"converting"|
 *              "failed"|"refused"|"unloaded"), "triangles" (each shared shape once).
 *              "missing" (DEVIATIONS 29): a link the
 *              open .bimble names whose file was not found when it opened; it draws nothing and
 *              "Locate…" links it again from another path (manage_links {op: "locate", name, path}).
 *   references every stored view's reference plans, view order then the view's order:
 *              {"id", "kind": "pdf"|"dxf", "name", "offset_mm": [x, y], "rotation_deg",
 *               "source": "<path>"|null, "view": id, "view_name", "visible"}.
 * Both arrays empty with nothing open. */
#ifndef CCORE_SESSION_LINKS_PANEL_H
#define CCORE_SESSION_LINKS_PANEL_H
#include "buf.h"

struct cc_session;

/* 0 ok, -1 out of memory (`out` then holds nothing useful). */
int cc_links_panel_json(struct cc_session *s, Buf *out);
#endif
