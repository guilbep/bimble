/* What this build of the C core serves (ticket FAKE1): `query capabilities` answers one object,
 * every name below with true or false, keys sorted. The page reads it once (bimble.ts) and shows
 * a page, panel or tool only when its capability is true, else the one "Not available yet"
 * state (web-ui/src/shell/NotAvailable.vue); it never guesses from the build it runs on.
 *
 * Why a query and not a list in the page: the native host, the full wasm, the viewer-only wasm
 * (CC_VIEW_ONLY) and later the Rust twin differ, and only the core knows what it was built with.
 * Actions keep their own `enabled` flags (query actions); a capability is what is not an action:
 * a page, a panel, a data set, an export. A query the dispatcher stubs (calls.c EMPTY: a constant
 * answer) is not a capability: recent_files, user and settings stay false until tickets FS1 and
 * SET1 make them real, then their row here turns true (the page lights up by itself).
 *
 * The rows (DEVIATIONS.md section 19: Rust has no such query; the Rust twin is "everything"):
 *   always true:  georef, grids, levels (storeys, grids, north/coordinates; ticket G2's pages),
 *                 history (the undo list), materials (MAT1: query materials_page, verbs material_*),
 *                 measure, palette, project_info, references, sheets
 *   full builds:  export_ifc, export_pdf, export_svg, quantities, schedules (VIEW_DENY'd in the
 *                 viewer: false there)
 *   native only:  versions (H1's read-only history store: a folder next to the file)
 *   false here:   about, collaboration, export_view, history_write, lca, paper_view,
 *                 phases, plan_annotations, rebar, recent_files, section_catalogue, settings,
 *                 sheet_tools, site, standards, steel, subprojects, user, users */
#ifndef CCORE_SESSION_CAPABILITIES_H
#define CCORE_SESSION_CAPABILITIES_H
#include "buf.h"

struct cc_session;

/* The answer of `query capabilities` into `out`. CC_OK, or CC_ERR when out of memory. */
int cc_capabilities_json(const struct cc_session *s, Buf *out);
#endif
