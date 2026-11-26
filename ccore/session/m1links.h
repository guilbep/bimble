/* Mission 1 across linked models (ticket M1LINKS, ADR 0081, DEVIATIONS 39): what a project made
 * of links alone (File > New, then Link IFC… or Link this folder) needs to be viewed as one
 * building: levels and floor plans from the links, the section box, sections and Fit all over
 * every link, hide / isolate of a link's elements by element, IFC class and storey, a link's
 * object tree. Rust draws at most three linked layers and none of this (lib.rs `Linked`).
 *
 * Levels from links: each loaded link's storeys (an IFC's or a .bimbleview's as read, a
 * .bimble's storeys), at their placed elevation (the storey's elevation + the link's placement
 * z, links.h cc_link_placement: a turn about Z leaves heights), merged: a level within
 * CC_LINK_LEVEL_TOL_MM of one of the host's own levels is that level (none added); within it of
 * a link level already taken (links in link order, their storeys lowest first) it is that one
 * (its name the first link's). Each other one is a level "from links": its key the 22 characters
 * of GlobalId::derived("bimble link level <link name> <storey key>"), read only, never written
 * into the model. The session's levels (views.h cc_sv_levels) are the host's then these; the
 * views list a floor plan per link level after the host's defaults (no ceiling plan), id
 * GlobalId::derived("bimble view plan down <key>") as any default plan.
 *
 * Link element visibility: a linked element is hidden as a host element by its GlobalId (the
 * session's hidden / isolated / for-now sets, the view's hidden_elements, isolated_elements), by
 * its IFC class (hidden_classes), by its storey (hidden_storeys holds a link storey as
 * "<link name>/<storey name>", a host storey by its name as before) and by its link
 * (hidden_links). The session folds these rules into by_view (visibility.h), which the page
 * reads with the other sets. */
#ifndef CCORE_SESSION_M1LINKS_H
#define CCORE_SESSION_M1LINKS_H
#include <stddef.h>
#include "arena.h"
#include "buf.h"
#include "json_w.h"
#include "model.h"
#include "scene.h"
#include "view_effective.h"
#include "visibility.h"

struct cc_session;
struct cc_link;
struct JrVal;

#define CC_LINK_LEVEL_TOL_MM 200.0 /* two levels this close are one: a floor build-up apart (ADR 0081) */
#define CC_LINK_LEVELS_MAX 512    /* a budget: levels from links beyond it are left out */

/* The levels from links (above), lowest first, in `a`; `from[i]` the index of the link that gave
 * level i (NULL: not wanted). `own` the host's levels. 0 ok, -1 out of memory. */
int cc_link_levels(const struct cc_session *s, const cc_level *own, size_t nown, Arena *a, cc_level **out, size_t *n,
                   size_t **from);
/* The link level with key `key`: 1 and its link index (and its storey name), else 0. */
int cc_link_level_find(const struct cc_session *s, cc_str key, size_t *link);

/* The section box around the visible elements of `sc` and of the visible links (Fit all, the
 * first box, a plan's or a section's clip): cc_vis_fit with the links. Unchanged when empty. */
void cc_vis_fit_session(struct cc_session *s, cc_vis *v, const cc_scene *sc, const cc_gid *primary);

/* A linked element's texts, whatever the link's kind: class, name, storey, material. */
void cc_link_texts(const struct cc_link *k, size_t i, cc_str *cls, cc_str *name, cc_str *storey, cc_str *material);
/* The view's rules (class, family, storey, link lists) over the links' elements: each hidden
 * one's GlobalId added to `set`. 0 ok, -1 out of memory. */
int cc_links_by_view(struct cc_session *s, const cc_visibility *w, cc_gidset *set);
/* 1 when the view `w` hides element `i` of link `k` by a rule that only the links have (a link
 * storey "<link>/<storey>"); the paper drawing leaves it out. */
int cc_link_hidden_by_rule(const struct cc_link *k, size_t i, const cc_visibility *w);

/* query scene_tree {"link": name} / scene_elements {"link", "storey", "class"?, "limit"?}: a
 * link's object tree (links.h cc_scene_tree_json's shape, plus "link") and a row's elements.
 * 0 ok, -1 with the error in `out`. */
int cc_link_tree_json(struct cc_session *s, const char *name, size_t nn, Buf *out);
int cc_link_elements_json(struct cc_session *s, const char *name, size_t nn, const char *storey, size_t sn,
                          const char *cls, size_t cn, size_t limit, Buf *out);
/* query links_tree: every link with its storeys (placed elevation) for the Navigator:
 *   [{"link","storeys":[{"count","elevation_mm","name"}],"total","visible"}] */
int cc_links_tree_json(struct cc_session *s, Buf *out);
/* query link_levels: the levels from links [{"elevation_mm","key","link","name","view"}] (view:
 * the id of its floor plan). */
int cc_link_levels_json(struct cc_session *s, Buf *out);
/* ---- notes on linked elements (ADR 0081) ----
 * A note: a text on an element of a link, kept by the session and saved in the .bimble's link entry
 * (model.h cc_model_note: the link's name, the element's GlobalId, the anchor in the link's own world
 * mm, the text), so it moves with the link's placement and outlives a Reload; an element no longer
 * in the link makes it an orphan, shown at its last point. Not model data (DEVIATIONS 39: the links
 * section is the session's, as LNK1's): no undo step; Save writes it, a change marks the model unsaved.
 *   manage_links {op: "note_add", name, element, text, at?: [x, y, z] (host world mm; default: the
 *     top of the element's box, at its centre)} / {op: "note_remove", name, index}
 *   query link_notes: [{"at" (host world mm, placed), "element", "index", "link", "orphan", "text",
 *     "visible"}] in the order made; a plan or section drawn on paper shows each visible link's notes
 *     (a plan: those between its bottom and top) as texts (cc_notes_into_view). */
typedef struct cc_note {
    char *link, *text; /* malloc'd */
    char element[22];
    double at[3];
} cc_note;
void cc_notes_clear(struct cc_session *s);
int cc_notes_from_model(struct cc_session *s);
int cc_notes_to_model(struct cc_session *s, Arena *a, cc_model_link *l, const char *name);
void cc_notes_sig(const struct cc_session *s, Buf *b);
int cc_notes_op(struct cc_session *s, int add, const char *link, size_t ln, const struct JrVal *v, Buf *out);
int cc_notes_json(struct cc_session *s, Buf *out);
int cc_notes_into_view(struct cc_session *s, cc_view *v, Arena *a);

/* The property sets of element `i` of link `k` (ifc_psets.h), appended to the object being written
 * in `out` as `,"property_sets":[…],"property_sets_note":null|"why none"`: a small IFC's from memory,
 * a cache's read from its file (two reads: its entry's ends, its text; checked to be JSON). 0. */
int cc_link_psets_json(struct cc_session *s, const struct cc_link *k, size_t i, Buf *out);
#endif
