/* Linked models (ticket B5; Rust: crates/bimble-viewer/src/lib.rs `Linked`, links.rs, ADR 0016):
 * other files superposed, read-only, on the open model (an editable one or a read-only scene).
 * Mission 1 "superposition of multiple files", Mission 2 "check another office's IFC against
 * yours". A link is an IFC read as it is (cc_ifc_read), a .bimbleview (cc_viewfile_read) or a
 * .bimble / .json model (its scene); never edited, never saved, not in the history.
 *
 * Calls (calls.c, calls.h):
 *   action link_model   {"path": "<file>"}: loads it next to the open model (enabled when
 *                       something is open, ribbon.rs `_ => loaded`). The same file name linked
 *                       again replaces that link in its place (lib.rs:1516-1527). 400 with the
 *                       reader's error; nothing changes then. No body: 400 (no file dialog).
 *                       The wasm module stages the bytes (wasm_api.h host_open, flag 4).
 *   action manage_links {"op": "list"|"remove"|"reload"|"show"|"hide"|"locate", "name": "<file name>"}:
 *                       (LNK1: locate {"path"} links another file in its place: a missing one, below)
 *                       list (also with no body) answers query links; remove drops the link;
 *                       reload reads its path again (in place; the wasm module has no path to
 *                       read again: 400 unless the page staged the bytes); show / hide draw it
 *                       or not (Rust's LinkMode::Hidden). Enabled when something is open.
 *   query links         [{"id","kind","name","visible"}] (state.rs links_json; no reference
 *                       plans in the C core); [] without links.
 *   select              {"link": "<file name>", "ids": ["<id>"]} selects an element of a link
 *                       (one id; [] clears): the open model's selection is emptied. The array
 *                       form selects in the open model and drops the linked selection.
 *   query element       {"id", "link"}: the linked element, read-only (see cc_link_element_json).
 * The change JSON gains "linked": {"id","link"} while a linked element is selected (and its id
 * is the one in "selection"), and "links": links_gen once any link changed (absent before: the
 * change JSON of a session without links is Rust's, byte for byte).
 *
 * GET /mesh with links: the open scene's BMSH, unchanged, then a trailer (mesh.c cc_links_bmsh):
 *   0  char[4] "LNKS"
 *   4  u32     version (1)
 *   8  u32     n links
 *   then per link, in link order:
 *      u32 name bytes L, the file name (UTF-8), zero padding to a multiple of 4
 *      u32 flags (bit 0: visible)
 *      u32 BMSH bytes B, then the link's BMSH (version 1, its own origin_mm; its rev field is the
 *          session rev). B is a multiple of 4, so every block stays 4-byte aligned.
 * A reader draws link k at (its origin_mm - the open scene's origin_mm), computed in f64 (lib.rs:1499
 * `offset`): one scene origin for all. Without links, no trailer: the bytes are the single-model
 * BMSH exactly. */
#ifndef CCORE_SESSION_LINKS_H
#define CCORE_SESSION_LINKS_H
#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "ifc_read.h"
#include "json_w.h"
#include "model.h"
#include "scene.h"
#include "session.h"
#include "viewfile.h"

enum { CC_LINK_IFC = 1, CC_LINK_VIEW, CC_LINK_MODEL };

#define CC_MAX_LINKS 64 /* Rust draws up to three layers beside the model; 64 is a budget, not a goal */

struct cc_link {
    char *path;       /* malloc'd: where it was read (reload reads it again) */
    const char *name; /* its file name, inside `path`: how calls name it (lib.rs:171 name()) */
    int kind;         /* CC_LINK_*; 0: missing (LNK1, below) */
    cc_ifc_scene *ifc; /* CC_LINK_IFC (malloc'd) */
    cc_viewfile *vf;   /* CC_LINK_VIEW (malloc'd) */
    cc_model *model;   /* CC_LINK_MODEL (malloc'd) */
    cc_scene scene;    /* the scene over vf or model (CC_LINK_VIEW, CC_LINK_MODEL) */
    int visible;
    double load_ms;
};

/* Reads `path` as a link (session.c: the platform reads it). 0 ok with `*k` filled (visible);
 * -1 with `err`, `*k` untouched. */
int cc_link_load(cc_session *s, const char *path, cc_link *k, cc_error *err);
void cc_link_free(cc_link *k);
const cc_scene *cc_link_scene(const cc_link *k);

/* The BMSH id text of scene element `i` of the link (22 bytes, as cc_ifc_scene_bmsh or the BMSH
 * writer give it). */
void cc_link_id(const cc_link *k, size_t i, char id[22]);
/* The scene index of the element with BMSH id `id` (22 bytes), -1 when none. */
long cc_link_find(const cc_link *k, const char *id);
/* The link named `name` (n bytes), -1 when none. */
int cc_links_find(const cc_session *s, const char *name, size_t n);

/* The changes; each bumps links_gen. 0 ok, -1 with `err`. */
int cc_links_add(cc_session *s, const char *path, cc_error *err);
int cc_links_reload(cc_session *s, int i, cc_error *err);
void cc_links_remove(cc_session *s, int i);
void cc_links_set_visible(cc_session *s, int i, int visible);
/* Drops every link (the open model went: lib.rs:1668) and the linked selection. */
void cc_links_clear(cc_session *s);

/* query links (state.rs:199 links_json). */
void cc_links_json(const cc_session *s, JsonW *w);
/* query element {"id","link"}: the element of a link, read-only, in query element's shape
 * (ElementDetail: id, class, name, storey and material ids empty, generator "", no params, no
 * openings, no properties, an identity placement) plus "family" (its MaterialCategory label),
 * "link" (the file name), "read_only": true and "volume_m3" (lib.rs:4508 link_properties rows).
 * Storey and material names in storey_name / material_name (null when empty). 0 ok, -1 with the
 * error text in `out`: "no linked model <name>", "no element with this id". */
int cc_link_element_json(cc_session *s, const char *name, size_t nn, const char *id, size_t idn, Buf *out);

/* ---- LNK1: links kept in the .bimble (DEVIATIONS 29; model.h cc_model_link) ----
 * A link of kind 0 is missing: the file the saved .bimble names was not found when it opened. It
 * keeps its name, path and visibility, draws nothing (an empty scene), and is written again on
 * save; manage_links {op: "locate", name, path} links it from another path, reload tries its path
 * again, remove drops it.
 *   cc_links_from_model: after a .bimble opened at `path`, each link it names: first its
 *     `relative` from the file's folder, then its `path`; else a missing link. In the wasm module
 *     (no files) every one is missing (the page locates them). The list as opened is the saved one.
 *   cc_links_to_model: before a save to `path`: the model's list is the session's (each path made
 *     absolute when the platform can, `relative` from the saved file's folder when there is one).
 *   cc_links_saved: the list now is the saved one (after a save).
 *   cc_links_changed: the list differs from the saved one (names, paths, visibility): the model
 *     has unsaved changes (cc_session_unsaved). */
int cc_links_from_model(cc_session *s, const char *path);
int cc_links_to_model(cc_session *s, const char *path);
void cc_links_saved(cc_session *s);
int cc_links_changed(const cc_session *s);
int cc_links_locate(cc_session *s, int i, const char *path, cc_error *err);
/* The relative path from folder `dir` to file `file` (both absolute, '/' separated, as real_path
 * gives them; "" when there is none: another drive, a relative input). 0 ok, -1 too long. */
int cc_links_relative(const char *dir, const char *file, char *out, size_t n);
/* ---- end LNK1 ---- */

/* The /mesh trailer (above), appended to `out`. 0 ok, -1 with `err`. */
int cc_links_bmsh(cc_session *s, uint32_t rev, Buf *out, cc_error *err);
#endif
