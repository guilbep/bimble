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
#endif
