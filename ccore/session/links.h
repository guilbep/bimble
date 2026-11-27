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
 * BMSH exactly.
 * LINKF: version 2 when a link is placed (cc_link_placement not the identity) or has a colour;
 * per link after its flags: f64 cos, f64 sin (the turn about Z), f64[3] its placed origin (host
 * world mm: R origin_mm + t), i32 colour (-1 none, else 0..11), then its BMSH as read (its own
 * coordinates). A reader draws a vertex p of link k at R p + (placed origin - the open scene's
 * origin_mm). Version 1 otherwise, byte for byte as before: an old reader fails on version 2. */
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
    /* ---- LINKF (below) ---- */
    uint32_t uid;         /* the session's number for it (a conversion finds its link by it) */
    int state;            /* CC_LS_*; kind is 0 until it is ready */
    char *msg;            /* malloc'd or NULL: why it failed or was refused; a note when ready */
    uint64_t bytes;       /* the file's size when linked */
    unsigned progress;    /* 0..1000 while converting */
    char *cache, *log;    /* malloc'd or NULL: its .bimbleview cache and report */
    int64_t cache_mtime_s;
    int from_cache;       /* the last load read the cache, not the IFC */
    int auto_retried;     /* SIZE2: refused for memory, queued again once by itself (cc_links_tick) */
    int origin;           /* CC_LO_* (positioning) */
    double move_mm[3];    /* the manual offset, mm */
    double rotation_deg;  /* the manual rotation about Z, degrees */
    int pinned;           /* 1: not moved by accident (default) */
    int colour;           /* -1: its own colours; else a palette index 0..11, flat */
    /* TESS1 (ADR 0085): its own reader detail, named at a Reload (manage_links reload {detail, chord_mm,
     * max_deg}); detail_own 0: the settings' link_detail. Kept across reloads, not saved in the .bimble. */
    int detail_own;
    cc_ifc_read_opts detail;
    /* where the file says it is (set when it became ready; cc_link_placement) */
    int geo_map;          /* it has a map conversion (IfcMapConversion, a georeference) */
    double geo_e, geo_n, geo_h, geo_rot; /* m, m, m, degrees */
    char geo_crs[64];
    double base_mm[3], survey_mm[3]; /* its base point, its survey point (its world mm) */
    /* the placed scene cc_link_scene answers when the placement is not the identity */
    int placed;
    cc_scene pscene;
    double pc, ps, pt[3]; /* the placement pscene was built with */
    /* M1LINKS (ifc_psets.h): a small IFC's property sets table, in memory (malloc'd, NULL: none); a
     * cache's stays in its file (vf->has_psets: read lazily) */
    char *psets;
    size_t npsets;
    /* SIZE3 (ADR 0079 amendment): a cache read in place. `map` is the cache's mapping (pf->map_file) that
     * `vf` reads (lazy: its meshes' counts only); the meshes are decoded on demand (cc_link_scene,
     * cc_link_raw_scene) into `marena` (`mbytes`, resident), and dropped again by cc_links_tick past
     * the session's budget (link_mesh_budget), least recently used first, never one used in the last
     * CC_LINK_KEEP_MS. `pwant`: its placement is not the identity (pc, ps, pt), pscene built once
     * resident. */
    void *map;
    int resident, pwant;
    Arena marena;
    uint64_t mbytes, mused, mused_ms_tick;
    double mused_ms;
    const struct cc_platform *map_pf; /* who mapped it */
    cc_mesh *zshapes; /* SIZE3: the zeroed shapes its scene shows while not decoded (in the scene's arena) */
    /* SIZE3 (E): ready, its cache without proxies yet: 1 waiting for its indexing job, 2 the job baking them (the
     * Links panel's "indexing"; cancel stops it: the cache stays without, completed at the next open); then 0 and
     * proxies_gen moves (query link_draw "proxies": the page asks its proxies again) */
    int indexing;
    unsigned proxies_gen;
};
#define CC_LINK_MESH_BUDGET ((uint64_t)256 << 20) /* SIZE3: decoded link meshes kept at rest, all links */
#define CC_LINK_KEEP_MS 10000.0                    /* SIZE3: a link used this recently is kept past the budget */

/* ---- LINKF (ADR 0079, DEVIATIONS 37): link a folder, conversions in turn, positioning ----
 * A link of an IFC (or a .bimbleview) at least s->link_cache_min bytes is not read in the call: it
 * is queued (state "queued", kind 0, nothing drawn) and its conversion (link_cache.h: the cache
 * beside the IFC loaded, else the IFC read and the cache written) runs on the carrier's thread
 * (pf->spawn), one at a time, the smallest queued first, while the session answers. A carrier
 * calls cc_links_tick often while cc_links_busy (the native host: every 100 ms of its poll loop):
 * it takes a finished conversion (state "ready", the link drawn) and starts the next. Only a
 * finished conversion bumps links_gen (the change long-poll tells the page, which refreshes and
 * fetches the mesh); the start of one and its progress are served by query links_panel, which the
 * page polls while a link is queued or converting (LinksPanel.vue). Without pf->spawn (the wasm module, the
 * C suites) and for smaller files the conversion runs in the call, as before.
 *   action link_folder {path, recursive?}: links every *.ifc of the folder (and each *.bimbleview
 *     without its IFC: not a cache), sorted by name, at most CC_MAX_LINKS in all; one undo step
 *     (cc_links_undo: undo unlinks them, redo links them again).
 *   manage_links {op, name, ...}: also "cancel" (a queued or converting link: failed, "cancelled"),
 *     "unload" / "load" (keep the link, free its memory / read it again), "reload_all",
 *     "place" {origin?, move_mm?, rotation_deg?} (refused while pinned), "pin" / "unpin",
 *     "colour" {colour: 0..11 | null}.
 * Positioning (Revit's Link IFC options): origin "file" (default: the file's georeference to the
 * host's, ADR 0075: both georeferenced, the link lands at its true place; else shared local
 * origins), "origin" (the link's local origin on the host's), "base_point" (the link's base
 * point, its building frame origin, on the host's), "survey_point" (the link's survey point, its
 * georeference's local origin, on the host's); then the manual rotation about Z and offset. */
enum { CC_LS_READY = 0, CC_LS_MISSING, CC_LS_QUEUED, CC_LS_CONVERTING, CC_LS_FAILED, CC_LS_REFUSED, CC_LS_UNLOADED };
enum { CC_LO_FILE = 0, CC_LO_ORIGIN, CC_LO_BASE_POINT, CC_LO_SURVEY_POINT };
extern const char *const CC_LINK_STATE_NAME[]; /* "ready", "missing", "queued", "converting", "failed", "refused", "unloaded" */
extern const char *const CC_LINK_ORIGIN_NAME[]; /* "file", "origin", "base_point", "survey_point" */

int cc_links_busy(const cc_session *s);
/* Takes a finished conversion and starts the next. 1 when a link changed (publish the change). */
int cc_links_tick(cc_session *s);
/* Cancels link i's conversion (queued or running). */
void cc_links_cancel(cc_session *s, int i);
/* Frees link i's memory, keeps the link (state "unloaded"); load queues it again. */
void cc_links_unload(cc_session *s, int i);
int cc_links_load(cc_session *s, int i, cc_error *err);
/* link_folder: the files of folder `dir` to link (calls.c), added in one undo step. 0 ok with
 * *added; -1 with err. */
int cc_links_add_folder(cc_session *s, const char *dir, int recursive, size_t *added, cc_error *err);
/* link_model {paths: [...]} (LINKF): each path linked as cc_links_add does, in one undo step; a
 * path already linked is skipped (a note); one the platform refuses (pf->path_allowed: outside the
 * dialog's roots), one beyond CC_MAX_LINKS or one that cannot be read gets a row "refused" with its
 * reason; the others go on. `answer`: {"added": n, "refused": [{"path", "reason"}], "skipped":
 * [{"note", "path"}]}. 0 ok, -1 out of memory. */
int cc_links_add_list(cc_session *s, const char *const *paths, size_t n, Buf *answer, cc_error *err);
/* link_folder's undo and redo (calls.c undo_redo): 1 when it took the step. */
int cc_links_undo_step(cc_session *s, int undo);
int cc_links_can_undo(const cc_session *s, int undo);
/* A model edit drops the links' redo steps. */
void cc_links_undo_clear_redo(cc_session *s);
/* The link's placement: a point of the link's world (mm) w goes to the host's world at
 * R(c, sn) w + t: a rotation about Z, then a translation (mm). The origin rule first (links.h
 * above; "file": both georeferenced (a map conversion, the same or an unnamed system): through the
 * survey coordinates, R = map turn of the link - the host's; else none), then the manual rotation
 * about the host's world Z axis and the offset: R_m (R_r w + t_r) + move. */
void cc_link_placement(const cc_session *s, const cc_link *k, double *c, double *sn, double t[3]);
/* The link's scene as read (its own world), whatever its placement: the /mesh trailer's. SIZE3: a link
 * read in place has its meshes decoded first (cc_link_scene too): the ONLY way to a link's geometry.
 * cc_link_rows answers without decoding: its rows (ids, classes, categories, shapes, placements) and its
 * origin are right, but a link read in place and not decoded shows EMPTY meshes there (counts 0): what a
 * list or an id needs, never geometry (its counts: cc_link_triangles). */
const cc_scene *cc_link_raw_scene(const cc_link *k);
const cc_scene *cc_link_rows(const cc_link *k);
/* SIZE3: link k's meshes decoded now (they stay until cc_links_tick drops them past the budget). 0 ok
 * (or nothing to do), -1 out of memory or a cache changed on disk (the link then draws nothing). */
int cc_link_resident(cc_link *k);
/* SIZE3: link k's stored triangles (each shared shape once), from its counts: nothing decoded. */
uint64_t cc_link_triangles(const cc_link *k);
/* SIZE3: a link read in place and not decoded: its box in the open scene's frame (`org`, the open scene's origin;
 * its placement applied, from the counts' read: no mesh decoded): 1; else 0 (test it whole). */
int cc_link_box_lazy(const cc_link *k, const double org[3], double lo[3], double hi[3]);
/* SIZE3: element i of link k's geometry for a call that needs one element (its volume, its box): a link read in place
 * and not decoded has that element's block alone decoded into `tmp` (placed as the link), *at = 0; else the link's
 * scene, *at = i. Free `tmp` with cc_scene_free after (always initialized). */
const cc_scene *cc_link_element_scene(cc_link *k, size_t i, cc_scene *tmp, size_t *at);
/* SIZE3: whether the carrier should call cc_links_tick: a conversion queued or running, a refused link
 * waiting for its automatic retry, or decoded link meshes past the budget waiting to be dropped. */
int cc_links_wants_tick(const cc_session *s);
/* Brings every ready link's placed scene (cc_link_scene) up to date with its placement and the
 * host's georeference (the session calls it before each call and each /mesh). 1 when one changed
 * (links_gen bumped). -1 out of memory (that link drawn unplaced). */
int cc_links_sync_placement(cc_session *s);
/* query link_log {name} (LINKF): the link's conversion report, {"name", "path", "text"} (the file
 * the conversion wrote, never a path from the caller; at most 4 MiB, UTF-8). 0 ok, -1 with the
 * error in `out`. */
int cc_link_log_text(cc_session *s, const char *body, size_t n, Buf *out);
/* The session goes: a running conversion orphaned (link_cache.h), the undo steps freed. */
void cc_links_session_free(cc_session *s);
/* The cc_scene over a view file (session.c): rows in its own arena, meshes borrowed from `v`. */
int cc_view_scene(cc_scene *sc, const cc_viewfile *v, cc_error *err);
/* ---- end LINKF ---- */

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
/* LNK3D (ADR 0078 amendment 3): query link_draw, what the viewport needs to draw each link without
 * fetching /mesh again: [{"colour" (-1 or 0..11), "cos", "name", "origin" ([x, y, z]: the placed
 * origin, host world mm), "sin", "visible" (drawn: shown and loaded)}] in link order, the numbers of
 * the LNKS trailer's version 2. A vertex p of link k goes to R(cos, sin) p + (origin - the open
 * scene's origin_mm). The change JSON's "links_mesh" (present with "links") counts the changes of
 * the links' geometry; "links" counts every change (a hide, a move, a colour too).
 * query mesh_key (calls.c): {"draw", "links", "rev"}: draw + 1 with each rev that may change what the
 * open scene draws (not views, sheets or schedules alone: a hide in a view), links = links_mesh; the
 * page fetches /mesh only when one of them moves (the change JSON stays Rust's without links). */
void cc_links_draw_json(cc_session *s, JsonW *w);
/* query element {"id","link"}: the element of a link, read-only, in query element's shape
 * (ElementDetail: id, class, name, storey and material ids empty, generator "", no params, no
 * openings, no properties, an identity placement) plus "family" (its MaterialCategory label),
 * "link" (the file name), "read_only": true and "volume_m3" (lib.rs:4508 link_properties rows).
 * Storey and material names in storey_name / material_name (null when empty). 0 ok, -1 with the
 * error text in `out`: "no linked model <name>", "no element with this id". */
int cc_link_element_json(cc_session *s, const char *name, size_t nn, const char *id, size_t idn, Buf *out);

/* query scene_element {"id"} (FIXUI, a C addition): an element of the read-only scene (an IFC or a
 * .bimbleview open as it is), in cc_link_element_json's shape without "link" and with "triangles":
 * the rows Rust's viewer shows for it (lib.rs:4570 `properties`: class, Name, GlobalId, Storey,
 * Material, Family, Triangles, Volume), read from the display reader (ifc_read.h cc_ifc_info; no
 * property sets: the reader does not keep them, as Rust's). `query element` keeps Rust's refusal
 * ("this file is read only"). 0 ok, -1 with the error text: "no element with this id", "no model
 * open", or the file is editable. */
int cc_scene_element_json(cc_session *s, const char *id, size_t idn, Buf *out);

/* query scene_tree (FIXUI, a C addition): the Navigator's object tree of the read-only scene,
 * grouped by the reader's storey (container) name, then IFC class:
 *   {"storeys": [{"classes": [{"class", "count"}], "count", "name"}], "total"}
 * storeys in the file's storey order (lowest first; other containers and "" after, in name
 * order), classes in byte order; total = the scene's elements. O(n log n). No read-only scene
 * (no model, or an editable one: its tree is query storeys): the empty tree (the page may ask
 * while a file opens or closes). 0 ok, -1 out of memory. */
int cc_scene_tree_json(cc_session *s, Buf *out);

/* query scene_elements {"storey": name, "class"?: name, "limit"?: n (default 200)} (FIXUI): the
 * elements of one tree row, [{"class", "id", "name"}] in scene order; those without a valid
 * GlobalId (not selectable) are left out. */
int cc_scene_elements_json(cc_session *s, const char *storey, size_t sn, const char *cls, size_t cn, size_t limit,
                           Buf *out);

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
