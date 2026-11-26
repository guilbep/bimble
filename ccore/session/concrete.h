/* CONC1 (ADR 0077): the Concrete tab's place kinds on the place tool (calls.c `tool`, tool.h):
 * footing (a pad: one click, under a column centred on it), strip_footing (one click on a wall:
 * along it; else two clicks), raft (the polygon slab's clicks, Enter closes), opening (two clicks
 * on a wall or a slab: its ends along the wall, or two corners on the slab) and sleeve (one click
 * on a wall or a slab). Each runs its verb (src/verbs_concrete.h) through cc_verb_run_ex, so a
 * gesture is one Command list and one history step. The sizes are session state, set with
 * `set_placing {"concrete": {...}}` and answered in `query placing` as "concrete" while one of
 * these kinds is chosen (the other kinds' answer keeps Rust's bytes). */
#ifndef CCORE_SESSION_CONCRETE_H
#define CCORE_SESSION_CONCRETE_H
#include "json_r.h"
#include "json_w.h"
#include "session.h"

/* The kinds this file runs (session.h cc_kind). */
int cc_conc_kind(cc_kind k);
/* Clicks a gesture takes: 1 (footing, sleeve), 2 (strip footing, opening), 0 (raft: a polygon). */
int cc_conc_clicks(cc_kind k);
/* The status bar's text while placing. */
const char *cc_conc_hint(const cc_session *s);
/* The commands of one gesture: a (and b, NULL for a one-click kind or a strip footing's first
 * click on a wall). dry_run: for the ghost. 0 ok, -1 with the verb's text in err. */
int cc_conc_cmds(cc_session *s, const double *a, const double *b, int dry_run, cc_cmds *l, Buf *err);
/* A raft from the clicked outline (raft_add). */
int cc_conc_raft_cmds(cc_session *s, const double (*pts)[2], size_t n, cc_cmds *l, Buf *err);
/* set_placing's "concrete" object: sizes in mm, each within [10, 100000] (a sill [0, 1e7]). All or
 * nothing. 0 ok, -1 with the reason in err. */
int cc_conc_set(cc_session *s, const JrVal *v, Buf *err);
/* {"footing_depth":…,…} sorted keys. */
void cc_conc_json(const cc_session *s, JsonW *w);
/* The defaults (session.c at start). */
void cc_conc_defaults(double *p);
/* ---- WAVE1B (B2): openings selected and previewed ----
 * query openings {host}: [{"centre":[x,y,z],"corners":[[x,y,z] x 8],"diameter"?,"height"?,"id","kind":
 *   "opening"|"sleeve"|"other","length"?,"sill"?,"wall":b,"width"?}] in the host's order, world mm (the
 *   centre: the plan centre `a` opening_set takes, at the middle of the opening's box in z). An opening
 *   without a GlobalId (an imported one) is "other" with id "". */
void cc_conc_openings_json(const cc_model *m, const cc_element *host, JsonW *w);
/* The opening under the ray (world mm: origin, direction; any length): the one whose box (grown by
 * `tol` mm) the ray enters first, not beyond t_max (the distance to the surface the ray meets first;
 * a negative t_max: none). 1 with the host and the opening's id, 0 none. */
int cc_conc_opening_at(const cc_model *m, const double ray[6], double t_max, double tol, cc_gid *host, cc_gid *id);
/* The ghost of an opening being placed: the dry run's set_openings `in` made into one add_element
 * of the new opening's solid (in the host's storey, its placement through the host's) in `out`, so
 * cc_tool_ghost meshes it. 0 ok, -1 none (no set_openings, out of memory). */
int cc_conc_opening_ghost(const cc_model *m, const cc_cmds *in, cc_cmds *out);
#endif
