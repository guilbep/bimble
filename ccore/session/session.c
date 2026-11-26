/* A session: see session.h. */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE /* macOS hides st_mtimespec under _POSIX_C_SOURCE alone */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "command.h"
#include "file.h"
#include "ifc_in.h"
#include "json_r.h"
#include "guid.h"
#include "inflate.h"
#include "json_w.h"
#include "mesh.h"
#include "session.h"
#include "concrete.h" /* CONC1 */
#include "links.h"
#include "link_cache.h" /* LINKF */
#include "sheet_svg.h"
#include "views.h"
#include "schedules.h" /* Q2 */
#include "settings_file.h" /* FS1 */
#include "versions.h" /* H2a: cc_versions_free */

cc_session *cc_session_new(const cc_platform *pf) {
    cc_session *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->pf = pf;
    buf_init(&s->mesh);
    cc_vis_init(&s->vis);
    s->link_sel = -1;
    s->link_cache_min = CC_LINK_CACHE_MIN; /* LINKF */
    s->edges = s->hatching = 1; /* Q2: bimble-render lib.rs:1431-1432 defaults */
    return s;
}

static void persist_free(cc_session *s);

static void drop_view(cc_session *s);

void cc_session_free(cc_session *s) {
    if (!s) return;
    if (s->open) cc_history_free(&s->hist), cc_model_free(&s->model);
    drop_view(s);
    cc_links_clear(s);
    cc_links_session_free(s); /* LINKF: a conversion still running is left to end on its own */
    free(s->links);
    free(s->tabs);
    free(s->paper_views); /* PAPER1 */
    free(s->undo_ids), free(s->redo_ids), free(s->also), free(s->material);
    if (s->scene_rev) cc_scene_free(&s->scene);
    buf_free(&s->mesh);
    cc_mesh_proxies_free(s); /* 3D2 */
    cc_paper_cache_free(s);
    cc_paper_fonts_free(s); /* D7 */
    cc_versions_free(s); /* H2a */
    cc_schedules_cache_free(s); /* Q2 */
    cc_recent_free(s);      /* FS1 */
    cc_tool_free(&s->tool);
    cc_vis_free(&s->vis);
    cc_measure_free(&s->measure);
    cc_session_mboxes_free(s); /* MEAS1 */
    persist_free(s);
    free(s);
}

const cc_model *cc_session_model(const cc_session *s) { return s->open ? &s->model : NULL; }
uint32_t cc_session_rev(const cc_session *s) { return (uint32_t)s->rev; }

uint64_t cc_session_step(const cc_session *s) { return s->nundo_ids ? s->undo_ids[s->nundo_ids - 1] : 0; }
int cc_session_unsaved(const cc_session *s) {
    return s->open && (cc_session_step(s) != s->saved_step || cc_links_changed(s)); /* LNK1: the links are saved too */
}

static int ends_with_ci(const char *s, const char *suffix);

/* LINKF: each format's limit (DEVIATIONS 37): an IFC the STEP reader's, a .bimbleview its reader's,
 * the rest (.bimble, .json) CC_MAX_FILE_BYTES (the JSON DOM's). Was CC_MAX_FILE_BYTES for every
 * file: "<file>.ifc: larger than 268435456 bytes" for an IFC Rust reads. */
size_t cc_session_read_limit(const char *path, size_t *peak_per_byte) {
    size_t lim = CC_MAX_FILE_BYTES, k = 4; /* a .bimble: the text, the DOM (json_r.h), the model */
    if (ends_with_ci(path, ".ifc")) lim = CC_IFC_MAX_FILE_BYTES, k = CC_IFC_READ_PEAK_PER_BYTE;
    else if (ends_with_ci(path, ".bimbleview")) lim = CC_VIEWFILE_MAX_FILE_BYTES, k = CC_VIEWFILE_READ_PEAK_PER_BYTE;
    if (peak_per_byte) *peak_per_byte = k;
    return lim;
}

static double gb(uint64_t b) { return (double)b / 1e9; }

int cc_session_fits(const cc_platform *pf, const char *path, uint64_t size, cc_error *err) {
    size_t k;
    size_t lim = cc_session_read_limit(path, &k);
    if (size > (uint64_t)lim) {
        snprintf(err->msg, sizeof err->msg, "%s: larger than %zu bytes", path, lim);
        return -1;
    }
    uint64_t avail = pf->mem_available ? pf->mem_available(pf->ctx) : 0;
    uint64_t need = size > UINT64_MAX / k ? UINT64_MAX : size * k;
    uint64_t base = ends_with_ci(path, ".ifc") ? CC_IFC_READ_PEAK_BASE : (uint64_t)64 << 20;
    if (size < base / 6) base = size * 6; /* a small file: at most 10 times its size in all */
    need = need > UINT64_MAX - base ? UINT64_MAX : need + base;
    if (avail && need > avail) {
        snprintf(err->msg, sizeof err->msg,
                 "%s: too large for this machine: reading %.2f GB needs about %.1f GB of memory, %.1f GB is free", path,
                 gb(size), gb(need), gb(avail));
        return -2;
    }
    return 0;
}

/* The whole file through the platform, NUL-terminated, at most its format's limit, refused first
 * when the machine has not the memory its reader needs (cc_session_fits). NULL with `err`. */
static char *read_all(const cc_session *s, const char *path, size_t *n, cc_error *err) {
    char *p = NULL;
    size_t lim = cc_session_read_limit(path, NULL);
    cc_pf_stat st0;
    if (s->pf->stat_path && s->pf->stat_path(s->pf->ctx, path, 1, &st0) == CC_PF_OK && st0.kind == CC_PF_FILE &&
        cc_session_fits(s->pf, path, st0.size, err))
        return NULL;
    int st = s->pf->read_file(s->pf->ctx, path, lim, &p, n);
    if (st == CC_PF_OK) return p;
    if (st == CC_PF_NOT_FOUND) snprintf(err->msg, sizeof err->msg, "%s: cannot open", path);
    else if (st == CC_PF_TOO_LARGE) snprintf(err->msg, sizeof err->msg, "%s: larger than %zu bytes", path, lim);
    else snprintf(err->msg, sizeof err->msg, "%s: %s", path, st == CC_PF_IO ? "read error" : "out of memory");
    return NULL;
}

static int ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    if (n < k) return 0;
    for (size_t i = 0; i < k; i++) {
        char c = s[n - k + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suffix[i]) return 0;
    }
    return 1;
}

/* lib.rs:503-521 editable_copy_path: "<stem>.bimble", "<stem> (imported).bimble",
 * "<stem> (imported 2).bimble"… next to the IFC, the first that does not exist. */
static int editable_copy_path(const cc_session *s, const char *ifc, char *out, size_t cap) {
    const char *slash = cc_path_last_sep(ifc);
    size_t dir = slash ? (size_t)(slash - ifc + 1) : 0;
    const char *name = ifc + dir;
    const char *dot = strrchr(name, '.');
    size_t stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    for (unsigned k = 0; k < 100000; k++) {
        int w;
        if (k == 0) w = snprintf(out, cap, "%.*s%.*s.bimble", (int)dir, ifc, (int)stem, name);
        else if (k == 1) w = snprintf(out, cap, "%.*s%.*s (imported).bimble", (int)dir, ifc, (int)stem, name);
        else w = snprintf(out, cap, "%.*s%.*s (imported %u).bimble", (int)dir, ifc, (int)stem, name, k);
        if (w < 0 || (size_t)w >= cap) return -1;
        if (!s->pf->exists(s->pf->ctx, out)) return 0; /* without files, the first name is free */
    }
    return -1;
}

static int ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && !strcmp(s + n - k, suffix);
}

/* The session file's side of the file actions (below, per platform). */
static void persist_follow(cc_session *s, const char *model_file, int again);
static void persist_drop_cmds(cc_session *s);

/* s->path := path (it fits: the callers checked), with the file name and the schema. */
static void set_path(cc_session *s, const char *path) {
    if (s->path != path) memmove(s->path, path, strlen(path) + 1);
    const char *slash = cc_path_last_sep(s->path);
    s->file = slash ? slash + 1 : s->path;
    s->schema_bimble = ends_with(s->file, ".bimble") && strlen(s->file) > 7; /* calls.rs:244 */
}

/* Drops the read-only scene, if any. */
static void drop_view(cc_session *s) {
    cc_mesh_proxies_free(s); /* 3D2: built on the scene dropped here */
    if (s->ifc) cc_ifc_scene_free(s->ifc), free(s->ifc), s->ifc = NULL;
    if (s->vf) cc_scene_free(&s->vscene), cc_viewfile_free(s->vf), free(s->vf), s->vf = NULL;
    s->view = CC_VIEW_NONE;
}

/* Drops what belongs to the open model (or read-only scene): model, history, selection,
 * placing, the caches. */
static void drop_model(cc_session *s) {
    if (s->open) cc_history_free(&s->hist), cc_model_free(&s->model);
    drop_view(s);
    cc_links_clear(s); /* lib.rs:1668: a linked model means nothing for another model */
    persist_drop_cmds(s); /* after the history that borrows from them */
    if (s->scene_rev) cc_scene_free(&s->scene);
    s->scene_rev = 0, s->scene_ok = 0, s->mesh_status = 0, s->mesh_model = NULL;
    cc_paper_cache_free(s); /* the last sheet drawn is the old model's */
    s->nundo_ids = s->nredo_ids = 0;
    s->has_primary = 0, s->nalso = 0;
    s->placing = 0, s->has_storey = 0, s->polygon = 0, s->top_offset_mm = 0;
    cc_tool_clear(&s->tool);
    cc_vis_reset(&s->vis); /* lib.rs:1657-1676: a box or a hide means nothing for another model */
    cc_measure_reset(&s->measure); /* lib.rs:1677: measurements mean nothing for another model */
    memset(&s->wp, 0, sizeof s->wp); /* WAVE1B: the storey's level again, the plane tool off */
    cc_session_mboxes_free(s); /* MEAS1: the boxes are the old scenes' */
    cc_mesh_proxies_free(s); /* 3D2: so are the proxies */
    free(s->material), s->material = NULL;
    s->open = 0, s->untitled = 0;
    s->ntabs = 0, s->has_active = 0, s->tab_open = 0;
    s->npaper_views = 0; /* PAPER1: lib.rs:1661 paper_views.clear() */
}

/* Makes `m` (loaded, validated) the session's model at `path`: a fresh history and editor (as
 * a load does), rev + 1 (lib.rs:1679 `scene_rev += 1`: 1 after the first load). */
static void install(cc_session *s, cc_model *m, const char *path, uint64_t saved_step, double t0) {
    drop_model(s);
    s->model = *m;
    cc_history_init(&s->hist);
    set_path(s, path);
    s->open = 1;
    s->rev++;
    s->next_step = 1;
    s->saved_step = saved_step; /* UINT64_MAX: edit.rs `Editor::imported`, starts dirty */
    /* edit.rs `Editor::new`: the lowest storey at or above -1 mm, else the first one. */
    const cc_storey *best = NULL;
    for (size_t i = 0; i < s->model.nstoreys; i++) {
        const cc_storey *st = &s->model.storeys[i];
        if (st->elevation >= -1.0 && (!best || st->elevation < best->elevation)) best = st;
    }
    if (!best && s->model.nstoreys) best = &s->model.storeys[0];
    if (best) s->has_storey = 1, s->storey = best->id;
    s->kind = CC_KIND_COLUMN;
    cc_conc_defaults(s->conc); /* CONC1 */
    double sizes[5] = {300.0, 250.0, 500.0, 200.0, 250.0}; /* verbs.rs:86-96 */
    memcpy(s->sizes, sizes, sizeof sizes);
    s->offset[0] = 1000.0, s->offset[1] = 0.0, s->offset[2] = 0.0;
    cc_sv_loaded(s); /* the 3D view is shown on load, its tab open */
    s->load_ms = s->pf->now_ms(s->pf->ctx) - t0;
}

/* Opens `path` (cc_session_open); with `model_text`, the model is that .bimble text instead of
 * the file's content (a session snapshot: the file is not read), the rest as for the file.
 * The open model is replaced only once the new one loaded. */
static int open_impl(cc_session *s, const char *path, const char *model_text, size_t model_n, cc_error *err) {
    err->offset = CC_NO_OFFSET;
    err->msg[0] = 0;
    double t0 = s->pf->now_ms(s->pf->ctx);
    int ifc = ends_with_ci(path, ".ifc");
    char to[sizeof s->path];
    if (ifc ? editable_copy_path(s, path, to, sizeof to) != 0 : strlen(path) >= sizeof to) {
        snprintf(err->msg, sizeof err->msg, "%s: path too long", path);
        return -1;
    }
    if (!ifc) strcpy(to, path);
    cc_model m;
    int rc;
    if (model_text) rc = cc_model_load(&m, model_text, model_n, err);
    else {
        size_t n = 0;
        char *text = read_all(s, path, &n, err);
        if (!text) return -1;
        if (ifc) {
            cc_ifc_report rep;
            rc = cc_ifc_import(&m, &rep, text, n, err);
            cc_ifc_report_free(&rep);
            if (rc == 0) { /* lib.rs:529-536: the project takes the file stem, then validate */
                const char *name = cc_path_last_sep(path);
                name = name ? name + 1 : path;
                const char *dot = strrchr(name, '.');
                size_t k = dot && dot != name ? (size_t)(dot - name) : strlen(name);
                char *p = arena_dup(&m.arena, name, k ? k : 1);
                if (p && k) m.project_name = (cc_str){p, k};
                cc_error ve;
                if (cc_model_validate(&m, &ve)) {
                    snprintf(err->msg, sizeof err->msg, "imported model invalid: %s", ve.msg);
                    rc = -1;
                }
            }
        } else rc = cc_model_load(&m, text, n, err);
        free(text);
    }
    if (rc) {
        cc_model_free(&m);
        return -1;
    }
    install(s, &m, to, ifc ? UINT64_MAX : 0, t0);
    return 0;
}

int cc_session_open(cc_session *s, const char *path, cc_error *err) {
    if (open_impl(s, path, NULL, 0, err)) return -1;
    if (!ends_with_ci(path, ".ifc") && cc_links_from_model(s, path)) /* LNK1: the links it names */
        cc_log(s->pf, "bimble-ccore: %s: out of memory linking its models", path);
    persist_follow(s, path, 1); /* the model is the file's content (an IFC: its import) */
    return 0;
}

int cc_session_new_model(cc_session *s, cc_error *err) {
    err->offset = CC_NO_OFFSET;
    double t0 = s->pf->now_ms(s->pf->ctx);
    cc_model m;
    cc_model_init(&m);
    cc_guid pid = cc_guid_new(), sid = cc_guid_new(); /* verbs.rs:102-103: Model::new, then the storey */
    cc_storey *st = arena_alloc(&m.arena, sizeof *st, 8);
    char *name = arena_dup(&m.arena, "Untitled", 8), *level = arena_dup(&m.arena, "Level 0", 7);
    if (!st || !name || !level) {
        cc_model_free(&m);
        snprintf(err->msg, sizeof err->msg, "out of memory");
        return -1;
    }
    memcpy(m.project_id.c, pid.c, 22);
    m.project_name = (cc_str){name, 8};
    memcpy(st->id.c, sid.c, 22);
    st->name = (cc_str){level, 7};
    st->elevation = 0.0;
    m.storeys = st, m.nstoreys = 1;
    if (cc_model_validate(&m, err)) {
        cc_model_free(&m);
        return -1;
    }
    install(s, &m, CC_UNTITLED, UINT64_MAX, t0); /* lib.rs:1701: loaded as an imported copy: unsaved */
    s->untitled = 1;
    persist_follow(s, NULL, 1);
    return 0;
}

/* The cc_scene over a view file (session.h `vscene`): element rows in its own arena, the meshes
 * and shapes borrowed from `v`. The GlobalId is the file's text when it has 22 bytes, else
 * zeros. 0 ok, -1 out of memory. */
int cc_view_scene(cc_scene *sc, const cc_viewfile *v, cc_error *err) {
    memset(sc, 0, sizeof *sc);
    arena_init(&sc->arena, 0);
    memcpy(sc->origin, v->origin, sizeof sc->origin);
    sc->shapes = v->shapes, sc->nshapes = v->nshapes;
    sc->warnings = v->warnings, sc->nwarnings = v->nwarnings;
    if (v->nelements) {
        sc->elements = arena_alloc(&sc->arena, v->nelements * sizeof *sc->elements, 8);
        if (!sc->elements) {
            snprintf(err->msg, sizeof err->msg, "out of memory");
            return -1;
        }
    }
    for (size_t k = 0; k < v->nelements; k++) {
        const cc_view_element *x = &v->elements[k];
        cc_scene_element *e = &sc->elements[k];
        memset(e, 0, sizeof *e);
        if (x->global_id.n == 22) memcpy(e->id.c, x->global_id.p, 22);
        e->cls = CC_IFC_PROXY; /* not drawn from: class_name holds the file's class */
        e->class_name = x->class_name;
        e->category = x->category;
        e->model_index = k;
        e->shape = x->shape;
        e->mesh = x->mesh;
        e->to_scene = x->to_scene;
    }
    sc->nelements = v->nelements;
    return 0;
}

/* links.h: a file read as a link: an IFC as it is, a view file, else a .bimble / .json model's
 * scene (lib.rs:1454 link: load_scene_progress reads any of them). */
int cc_link_load(cc_session *s, const char *path, cc_link *k, cc_error *err) {
    err->offset = CC_NO_OFFSET;
    err->msg[0] = 0;
    int ifc = ends_with_ci(path, ".ifc"), vw = ends_with_ci(path, ".bimbleview");
    double t0 = s->pf->now_ms(s->pf->ctx);
    size_t n = 0;
    char *text = read_all(s, path, &n, err);
    if (!text) return -1;
    cc_link l;
    memset(&l, 0, sizeof l);
    int rc = -1;
    l.kind = ifc ? CC_LINK_IFC : vw ? CC_LINK_VIEW : CC_LINK_MODEL;
    l.path = malloc(strlen(path) + 1);
    if (!l.path) snprintf(err->msg, sizeof err->msg, "out of memory");
    else if (ifc) {
        if (!(l.ifc = malloc(sizeof *l.ifc))) snprintf(err->msg, sizeof err->msg, "out of memory");
        else if (cc_ifc_read(text, n, l.ifc, err) != 0) cc_ifc_scene_free(l.ifc), free(l.ifc), l.ifc = NULL;
        else rc = 0;
    } else if (vw) {
        if (!(l.vf = malloc(sizeof *l.vf))) snprintf(err->msg, sizeof err->msg, "out of memory");
        else if (cc_viewfile_read((const uint8_t *)text, n, l.vf, err) != 0) cc_viewfile_free(l.vf), free(l.vf), l.vf = NULL;
        else if (cc_view_scene(&l.scene, l.vf, err) != 0) cc_scene_free(&l.scene), cc_viewfile_free(l.vf), free(l.vf), l.vf = NULL;
        else rc = 0;
    } else {
        if (!(l.model = malloc(sizeof *l.model))) snprintf(err->msg, sizeof err->msg, "out of memory");
        else if (cc_model_load(l.model, text, n, err) != 0) cc_model_free(l.model), free(l.model), l.model = NULL;
        else if (cc_scene_build(&l.scene, l.model, err) != 0) cc_scene_free(&l.scene), cc_model_free(l.model), free(l.model), l.model = NULL;
        else rc = 0;
    }
    free(text);
    if (rc) {
        free(l.path);
        return -1;
    }
    strcpy(l.path, path);
    const char *slash = cc_path_last_sep(l.path);
    l.name = slash ? slash + 1 : l.path;
    l.visible = 1;
    l.load_ms = s->pf->now_ms(s->pf->ctx) - t0;
    *k = l;
    return 0;
}

int cc_session_open_file(cc_session *s, const char *path, cc_error *err) {
    int ifc = ends_with_ci(path, ".ifc"), vw = ends_with_ci(path, ".bimbleview");
    if (!ifc && !vw) return cc_session_open(s, path, err);
    err->offset = CC_NO_OFFSET;
    err->msg[0] = 0;
    if (strlen(path) >= sizeof s->path) {
        snprintf(err->msg, sizeof err->msg, "%s: path too long", path);
        return -1;
    }
    double t0 = s->pf->now_ms(s->pf->ctx);
    size_t n = 0;
    char *text = read_all(s, path, &n, err);
    if (!text) return -1;
    cc_ifc_scene *is = NULL;
    cc_viewfile *vf = NULL;
    cc_scene vs;
    memset(&vs, 0, sizeof vs);
    int rc = -1;
    if (ifc ? !(is = malloc(sizeof *is)) : !(vf = malloc(sizeof *vf))) snprintf(err->msg, sizeof err->msg, "out of memory");
    else if (ifc) rc = cc_ifc_read(text, n, is, err); /* always initializes */
    else {
        rc = cc_viewfile_read((const uint8_t *)text, n, vf, err);
        if (!rc) rc = cc_view_scene(&vs, vf, err);
    }
    free(text);
    if (rc) {
        if (is) cc_ifc_scene_free(is), free(is);
        if (vf) cc_scene_free(&vs), cc_viewfile_free(vf), free(vf);
        return -1;
    }
    drop_model(s);
    set_path(s, path);
    s->view = ifc ? CC_VIEW_IFC : CC_VIEW_FILE;
    s->ifc = is, s->vf = vf, s->vscene = vs;
    s->rev++;
    s->next_step = 1, s->saved_step = 0;
    cc_sv_loaded(s); /* the 3D view is shown on load, its tab open */
    s->load_ms = s->pf->now_ms(s->pf->ctx) - t0;
    persist_follow(s, NULL, 1); /* nothing to keep: no edits */
    return 0;
}

void cc_session_close(cc_session *s) {
    if (!s->open && !s->view) return;
    drop_model(s);
    s->rev++; /* the view changed: nothing to show */
    persist_follow(s, NULL, 1);
}

int cc_session_save(cc_session *s, const char *path, cc_error *err) {
    err->offset = CC_NO_OFFSET;
    err->msg[0] = 0;
    if (!s->open) {
        snprintf(err->msg, sizeof err->msg, "no model open");
        return -1;
    }
    const char *to = path ? path : s->path;
    if (strlen(to) >= sizeof s->path || !*to) {
        snprintf(err->msg, sizeof err->msg, "%s: %s", to, *to ? "path too long" : "no path");
        return -1;
    }
    /* LNK1: the links go into the file (links.h cc_links_to_model, DEVIATIONS 29) */
    if (cc_links_to_model(s, to)) {
        snprintf(err->msg, sizeof err->msg, "out of memory");
        return -1;
    }
    Buf b;
    buf_init(&b);
    /* file.rs:145: the extension "json" (exactly) gives the text form, anything else the binary. */
    int ok = (ends_with(to, ".json") ? cc_model_save(&s->model, &b) : cc_model_save_v2(&s->model, &b)) == 0 && !b.oom;
    /* Through the platform: a file natively; in the module, the bytes kept for the page. */
    if (ok) ok = s->pf->write_file(s->pf->ctx, to, b.p, b.len) == 0;
    buf_free(&b);
    if (!ok) {
        snprintf(err->msg, sizeof err->msg, "%s: cannot write", to);
        cc_log(s->pf, "bimble-ccore: %s", err->msg);
        return -1;
    }
    int moved = strcmp(to, s->path) != 0;
    if (moved) set_path(s, to), s->untitled = 0;
    s->saved_step = cc_session_step(s);
    cc_links_saved(s); /* LNK1 */
    cc_log(s->pf, "bimble-ccore: Saved %s", s->path);
    persist_follow(s, s->path, 0); /* a no-op when the session file is already the saved path's */
    return 0;
}

/* ---- the session file (session.h, tickets T12, T18) ---- */

#ifdef __wasm__
/* No files in the module: persistence is never on. */
int cc_session_open_persisted(cc_session *s, const char *model_path, const char *session_path, cc_error *err) {
    (void)session_path;
    return cc_session_open(s, model_path, err);
}
void cc_session_persist_now(cc_session *s) { (void)s; }
const char *cc_session_set_aside(const cc_session *s) { (void)s; return NULL; } /* FIXSES */
void cc_session_note_apply(cc_session *s, const cc_command *cmds, size_t n) { (void)s, (void)cmds, (void)n; }
static void persist_free(cc_session *s) { (void)s; }
static void persist_follow(cc_session *s, const char *model_file, int again) { (void)s, (void)model_file, (void)again; }
static void persist_drop_cmds(cc_session *s) { (void)s; }
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
/* Windows (ticket WIN1): the file calls below through host/platform_win.c (session/platform.h):
 * UTF-8 paths, binary mode (no "\n" -> "\r\n" in the session file), a rename that replaces. */
#define CC_WIN_FLAGS(flags, ...) flags /* the mode (0644) is the system's default there */
#define open(path, ...) cc_win_open(path, CC_WIN_FLAGS(__VA_ARGS__, 0))
#define rename(from, to) cc_win_rename(from, to)
#define unlink(path) cc_win_unlink(path)
#define access(path, mode) (cc_win_exists(path) ? 0 : -1)
#endif

/* Format 3 (ticket J1): the snapshot's model, when it is not the model file, is in a side file
 * `<session>.model-a` or `-b` (the .bimble v2 binary, gzip in stored blocks: Rust reads it), named
 * in the line with its size and BLAKE3; the slots alternate, so the line a crash leaves always
 * names a complete side file. Format 2 (the model as v1 JSON text inside the line) still
 * restores. */
#define SESSION_FORMAT 3
#define SESSION_FORMAT_TEXT 2
#define SESSION_MAX_LABEL 4096
#define SESSION_MAX_ENTRIES 2000   /* entries after the snapshot before a new snapshot */
#define SESSION_LOG_FACTOR 4       /* or entry bytes above 4x the snapshot (model text or file) */
#define SESSION_MAX_LINES 1000000  /* replay bound (untrusted file) */

/* The session file's state, per session (s->persist; NULL: persistence off). */
struct cc_persist {
    char sess_path[4096], sess_model[4096];
    int on, sess_fd;
    cc_cmds sess_cmds; /* the snapshot's steps' commands: they live as long as the model */
    /* What the file says now (the state after its last line), to tell what a call changed. */
    struct {
        uint64_t size, mtime; /* the model file's stamp in the snapshot */
        uint64_t rev, next_step, saved_step;
        uint64_t *undo, *redo;
        size_t nundo, nredo, capundo, capredo;
        int has_primary;
        cc_gid primary;
        cc_gid *also;
        size_t nalso, capalso;
    } W;
    int model_is_file; /* the open model is the model file's content (no change since it was read) */
    size_t snap_bytes, log_bytes, log_entries; /* the file: snapshot line, then entry lines */
    uint64_t model_file_bytes;
    int snap_null; /* the snapshot line has no model text (it stands for the model file) */
    size_t side_bytes; /* the snapshot's side file (format 3), 0 without one */
    char side_slot;    /* 'a' or 'b': the side file the line names, 0 none */
    /* The forward commands of the step the current call pushed (cc_session_note_apply). */
    Buf pend_cmds;
    uint64_t pend_id;
    int pend_ok;
    int follow;    /* set up (cc_session_open_persisted returned): the file actions move it */
    int auto_name; /* sess_path is "<sess_model>.ccore-session": it moves with the model */
    char aside[4096 + 48]; /* FIXSES: where a session file with steps not saved was set aside, "" none */
};
static void close_session(cc_session *s);

static void persist_free(cc_session *s) {
    cc_persist *P = s->persist;
    if (!P) return;
    if (P->sess_fd >= 0) close(P->sess_fd);
    free(P->W.undo), free(P->W.redo), free(P->W.also);
    buf_free(&P->pend_cmds);
    cc_cmds_free(&P->sess_cmds); /* after the history that borrows from it (cc_session_free) */
    free(P);
    s->persist = NULL;
}

/* The model file's size and mtime (ns): Save rewrites it, an outside edit too. */
static int file_stamp(const char *path, uint64_t *size, uint64_t *mtime_ns) {
#ifdef _WIN32
    return cc_win_stamp(path, size, mtime_ns);
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *size = (uint64_t)st.st_size;
#ifdef __APPLE__
    *mtime_ns = (uint64_t)st.st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st.st_mtimespec.tv_nsec;
#else
    *mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
#endif
    return 0;
#endif
}

static int copy_u64s(uint64_t **dst, size_t *n, size_t *cap, const uint64_t *src, size_t k) {
    if (k > *cap) {
        uint64_t *p = realloc(*dst, k * sizeof *p);
        if (!p) return -1;
        *dst = p, *cap = k;
    }
    if (k) memcpy(*dst, src, k * sizeof *src);
    *n = k;
    return 0;
}

/* W := the session now. -1 out of memory (W then forces a snapshot next time: nundo = SIZE_MAX). */
static int remember(cc_session *s) {
    cc_persist *P = s->persist;
    P->W.rev = s->rev, P->W.next_step = s->next_step, P->W.saved_step = s->saved_step;
    P->W.has_primary = s->has_primary;
    if (s->has_primary) P->W.primary = s->primary;
    int bad = copy_u64s(&P->W.undo, &P->W.nundo, &P->W.capundo, s->undo_ids, s->nundo_ids);
    bad |= copy_u64s(&P->W.redo, &P->W.nredo, &P->W.capredo, s->redo_ids, s->nredo_ids);
    if (!bad && s->nalso > P->W.capalso) {
        cc_gid *p = realloc(P->W.also, s->nalso * sizeof *p);
        if (!p) bad = 1;
        else P->W.also = p, P->W.capalso = s->nalso;
    }
    if (!bad) {
        if (s->nalso) memcpy(P->W.also, s->also, s->nalso * sizeof *s->also);
        P->W.nalso = s->nalso;
    }
    if (bad) P->W.nundo = SIZE_MAX;
    return bad ? -1 : 0;
}

static int same_ids(const uint64_t *a, size_t na, const uint64_t *b, size_t nb) {
    return na == nb && (!na || !memcmp(a, b, na * sizeof *a));
}

static void write_sel(const cc_session *s, JsonW *w) {
    jw_key(w, "also"), jw_arr_begin(w);
    for (size_t i = 0; i < s->nalso; i++) jw_str_n(w, s->also[i].c, 22);
    jw_arr_end(w);
}
static void write_counters(const cc_session *s, JsonW *w, int with_primary) {
    jw_key(w, "next_step"), jw_u64(w, s->next_step);
    if (with_primary) {
        jw_key(w, "primary");
        if (s->has_primary) jw_str_n(w, s->primary.c, 22);
        else jw_null(w);
    }
}
static void write_saved(const cc_session *s, JsonW *w) {
    jw_key(w, "saved_step");
    if (s->saved_step == UINT64_MAX) jw_null(w);
    else jw_u64(w, s->saved_step);
}

static void write_steps(JsonW *w, const cc_step *st, const uint64_t *ids, size_t n) {
    jw_arr_begin(w);
    for (size_t i = 0; i < n; i++) {
        Buf c;
        buf_init(&c);
        cc_commands_json(st[i].cmds, st[i].n, &c);
        jw_obj_begin(w);
        jw_key(w, "cmds"), jw_str_n(w, c.p ? c.p : "[]", c.p ? c.len : 2);
        jw_key(w, "id"), jw_u64(w, ids[i]);
        jw_key(w, "label"), jw_str(w, st[i].label ? st[i].label : "");
        jw_obj_end(w);
        if (c.oom) w->b->oom = 1;
        buf_free(&c);
    }
    jw_arr_end(w);
}

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k, n -= (size_t)k;
    }
    return 0;
}

/* A new file: the snapshot line (format 2), through `<path>.tmp` + rename; the entries start
 * after it. The model text is left out (null) while the model is the model file's content.
 * 0 written; -1 with the reason logged. */
static int write_snapshot(cc_session *s) {
    cc_persist *P = s->persist;
    uint64_t size = 0, mt = 0;
    if (file_stamp(P->sess_model, &size, &mt)) {
        cc_log(s->pf, "bimble-ccore: session not written: %s: cannot stat", P->sess_model);
        return -1;
    }
    Buf model, b;
    buf_init(&model), buf_init(&b);
    int rc = -1, fd = -1;
    char slot = 0, hex[65];
    if (!P->model_is_file) {
        /* The side file first (tmp + rename), in the slot the current line does not name. */
        slot = P->side_slot == 'a' ? 'b' : 'a';
        if (cc_model_save_v2_stored(&s->model, &model) != 0 || model.oom) goto end;
        unsigned char h[32];
        cc_blake3(model.p, model.len, h);
        for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
        char side[sizeof P->sess_path + 16], stmp[sizeof side + 8];
        snprintf(side, sizeof side, "%s.model-%c", P->sess_path, slot);
        snprintf(stmp, sizeof stmp, "%s.tmp", side);
        fd = open(stmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) goto end;
        int sbad = write_all(fd, model.p, model.len);
        sbad |= close(fd) != 0;
        fd = -1;
        if (sbad || rename(stmp, side) != 0) {
            unlink(stmp);
            goto end;
        }
    }
    JsonW w;
    jw_init(&w, &b, 0);
    jw_obj_begin(&w);
    write_sel(s, &w);
    char mts[24];
    snprintf(mts, sizeof mts, "%llu", (unsigned long long)mt);
    jw_key(&w, "file_mtime_ns"), jw_str(&w, mts); /* a string: above 2^53 */
    jw_key(&w, "file_size"), jw_u64(&w, size);
    jw_key(&w, "format"), jw_u64(&w, SESSION_FORMAT);
    jw_key(&w, "model");
    if (P->model_is_file) jw_null(&w);
    else {
        char sl[2] = {slot, 0};
        jw_obj_begin(&w);
        jw_key(&w, "blake3"), jw_str(&w, hex);
        jw_key(&w, "bytes"), jw_u64(&w, model.len);
        jw_key(&w, "slot"), jw_str(&w, sl);
        jw_obj_end(&w);
    }
    jw_key(&w, "model_path"), jw_str(&w, P->sess_model);
    write_counters(s, &w, 1);
    jw_key(&w, "redo"), write_steps(&w, s->hist.redo, s->redo_ids, s->nredo_ids);
    jw_key(&w, "rev"), jw_u64(&w, s->rev);
    write_saved(s, &w);
    jw_key(&w, "undo"), write_steps(&w, s->hist.undo, s->undo_ids, s->nundo_ids);
    jw_obj_end(&w);
    if (jw_finish(&w) != JW_OK || b.oom) goto end;
    buf_putc(&b, '\n');
    if (b.oom) goto end;
    char tmp[sizeof P->sess_path + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", P->sess_path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) goto end;
    int bad = write_all(fd, b.p, b.len);
    bad |= close(fd) != 0;
    fd = -1;
    if (bad || rename(tmp, P->sess_path) != 0) {
        unlink(tmp);
        goto end;
    }
    if (P->sess_fd >= 0) close(P->sess_fd);
    P->sess_fd = open(P->sess_path, O_WRONLY | O_APPEND);
    if (P->sess_fd < 0) goto end;
    P->snap_bytes = b.len, P->log_bytes = 0, P->log_entries = 0, P->snap_null = P->model_is_file;
    {
        /* The line now names `slot` (or none): the other side file is dead. */
        P->side_slot = slot, P->side_bytes = slot ? model.len : 0;
        for (char k = 'a'; k <= 'b'; k++)
            if (k != slot) {
                char side[sizeof P->sess_path + 16];
                snprintf(side, sizeof side, "%s.model-%c", P->sess_path, k);
                unlink(side);
            }
    }
    P->W.size = size, P->W.mtime = mt, P->model_file_bytes = size;
    rc = remember(s);
end:
    if (rc) cc_log(s->pf, "bimble-ccore: session not written to %s", P->sess_path);
    buf_free(&model), buf_free(&b);
    return rc;
}

/* One entry line after the snapshot: `op` apply / undo / redo / state, then the state fields.
 * apply: the forward commands (as given to the call), the inverses the history keeps, label, id;
 * undo / redo: the id of the step moved. Written with one write(2) before the call answers. */
static int append_entry(cc_session *s, const char *op) {
    cc_persist *P = s->persist;
    Buf b, inv;
    buf_init(&b), buf_init(&inv);
    JsonW w;
    jw_init(&w, &b, 0);
    jw_obj_begin(&w);
    write_sel(s, &w);
    const cc_step *top = s->hist.nundo ? &s->hist.undo[s->hist.nundo - 1] : NULL;
    if (!strcmp(op, "apply")) {
        cc_commands_json(top->cmds, top->n, &inv);
        jw_key(&w, "cmds"), jw_str_n(&w, P->pend_cmds.p ? P->pend_cmds.p : "[]", P->pend_cmds.p ? P->pend_cmds.len : 2);
        jw_key(&w, "id"), jw_u64(&w, s->undo_ids[s->nundo_ids - 1]);
        jw_key(&w, "inv"), jw_str_n(&w, inv.p ? inv.p : "[]", inv.p ? inv.len : 2);
        jw_key(&w, "label"), jw_str(&w, top->label ? top->label : "");
    } else if (!strcmp(op, "undo")) {
        jw_key(&w, "id"), jw_u64(&w, s->redo_ids[s->nredo_ids - 1]);
    } else if (!strcmp(op, "redo")) {
        jw_key(&w, "id"), jw_u64(&w, s->undo_ids[s->nundo_ids - 1]);
    }
    write_counters(s, &w, 0);
    jw_key(&w, "op"), jw_str(&w, op);
    jw_key(&w, "primary");
    if (s->has_primary) jw_str_n(&w, s->primary.c, 22);
    else jw_null(&w);
    jw_key(&w, "rev"), jw_u64(&w, s->rev);
    write_saved(s, &w);
    jw_obj_end(&w);
    int rc = -1;
    if (jw_finish(&w) == JW_OK && !b.oom && !inv.oom) {
        buf_putc(&b, '\n');
        if (!b.oom && P->sess_fd >= 0 && write_all(P->sess_fd, b.p, b.len) == 0) {
            P->log_bytes += b.len, P->log_entries++;
            rc = remember(s);
        }
    }
    buf_free(&b), buf_free(&inv);
    return rc;
}

void cc_session_note_apply(cc_session *s, const cc_command *cmds, size_t n) {
    cc_persist *P = s->persist;
    if (!P || !P->on) return;
    buf_clear(&P->pend_cmds);
    P->pend_cmds.oom = 0;
    cc_commands_json(cmds, n, &P->pend_cmds);
    P->pend_ok = !P->pend_cmds.oom;
    P->pend_id = s->nundo_ids ? s->undo_ids[s->nundo_ids - 1] : 0;
}

void cc_session_persist_now(cc_session *s) {
    cc_persist *P = s->persist;
    if (!P || !P->on || !s->open) return;
    /* Mid-call (a step pushed, its id not yet): wait for the next look. */
    if (s->nundo_ids != s->hist.nundo || s->nredo_ids != s->hist.nredo) return;
    uint64_t size = 0, mt = 0;
    file_stamp(P->sess_model, &size, &mt);
    int stack_same = same_ids(s->undo_ids, s->nundo_ids, P->W.undo, P->W.nundo) && same_ids(s->redo_ids, s->nredo_ids, P->W.redo, P->W.nredo);
    const char *op = NULL;
    if (size != P->W.size || mt != P->W.mtime || P->sess_fd < 0) {
        /* Saved (or changed by another program): a new snapshot. After a Save to the model file
         * itself, the model is that file again: no model text. */
        (void)stack_same;
        P->model_is_file = s->saved_step != P->W.saved_step && !strcmp(s->path, P->sess_model) && cc_session_step(s) == s->saved_step;
        write_snapshot(s);
        P->pend_ok = 0;
        return;
    }
    if (stack_same) {
        if (s->rev == P->W.rev && s->next_step == P->W.next_step && s->saved_step == P->W.saved_step &&
            s->has_primary == P->W.has_primary && (!s->has_primary || !cc_gid_cmp(&s->primary, &P->W.primary)) &&
            s->nalso == P->W.nalso && (!s->nalso || !memcmp(s->also, P->W.also, s->nalso * sizeof *s->also)))
            return; /* nothing changed */
        op = "state";
    } else if (P->W.nundo != SIZE_MAX && s->nundo_ids == P->W.nundo + 1 && same_ids(s->undo_ids, P->W.nundo, P->W.undo, P->W.nundo) &&
               s->nredo_ids == 0 && P->pend_ok && P->pend_id == s->undo_ids[P->W.nundo])
        op = "apply";
    else if (P->W.nundo != SIZE_MAX && P->W.nundo && s->nundo_ids == P->W.nundo - 1 && same_ids(s->undo_ids, s->nundo_ids, P->W.undo, s->nundo_ids) &&
             s->nredo_ids == P->W.nredo + 1 && same_ids(s->redo_ids, P->W.nredo, P->W.redo, P->W.nredo) &&
             s->redo_ids[P->W.nredo] == P->W.undo[P->W.nundo - 1])
        op = "undo";
    else if (P->W.nundo != SIZE_MAX && P->W.nredo && s->nredo_ids == P->W.nredo - 1 && same_ids(s->redo_ids, s->nredo_ids, P->W.redo, s->nredo_ids) &&
             s->nundo_ids == P->W.nundo + 1 && same_ids(s->undo_ids, P->W.nundo, P->W.undo, P->W.nundo) &&
             s->undo_ids[P->W.nundo] == P->W.redo[P->W.nredo - 1])
        op = "redo";
    P->pend_ok = 0;
    if (op && strcmp(op, "state")) P->model_is_file = 0;
    size_t base = P->snap_bytes + P->side_bytes + (P->snap_null ? (size_t)P->model_file_bytes : 0);
    if (!op || P->log_entries >= SESSION_MAX_ENTRIES || P->log_bytes > SESSION_LOG_FACTOR * base) {
        if (!op) P->model_is_file = 0; /* more than one change in a call: no telling */
        write_snapshot(s);
        return;
    }
    if (append_entry(s, op)) {
        cc_log(s->pf, "bimble-ccore: session entry not written to %s; writing a snapshot", P->sess_path);
        write_snapshot(s);
    }
}

/* ---- restore ---- */

/* An integer in [lo, hi] (JSON number without fraction or exponent). */
static int get_u64(const JrVal *o, const char *key, uint64_t lo, uint64_t hi, uint64_t *out) {
    const JrVal *v = jr_get(o, key);
    if (!v || v->type != JR_NUM || !v->num_int || v->num_neg || v->num > 9007199254740992.0) return -1;
    uint64_t x = (uint64_t)v->num;
    if (x < lo || x > hi) return -1;
    *out = x;
    return 0;
}

static int get_gid(const JrVal *v, const cc_model *m, cc_gid *out) {
    if (!v || v->type != JR_STR || v->n != 22 || !cc_gid_valid(v->s, 22)) return -1;
    memcpy(out->c, v->s, 22);
    return cc_model_element(m, out) ? 0 : -1;
}

static int gid_sort(const void *a, const void *b) { return cc_gid_cmp(a, b); }

/* The selection of a snapshot or entry, checked against the open model: S's on success. */
static int read_selection(cc_session *s, const JrVal *root, const char **why) {
    cc_gid primary, *also = NULL;
    int has_primary = 0;
    size_t nalso = 0;
    const JrVal *pv = jr_get(root, "primary");
    if (!pv) return *why = "no primary", -1;
    if (pv->type != JR_NULL) {
        if (get_gid(pv, &s->model, &primary)) return *why = "primary: not an element", -1;
        has_primary = 1;
    }
    const JrVal *av = jr_get(root, "also");
    if (!av || av->type != JR_ARR || (av->n && !has_primary)) return *why = "bad also", -1;
    if (av->n) {
        also = malloc(av->n * sizeof *also);
        if (!also) return *why = "out of memory", -1;
        for (size_t i = 0; i < av->n; i++)
            if (get_gid(&av->items[i], &s->model, &also[i])) {
                free(also);
                return *why = "also: not an element", -1;
            }
        qsort(also, av->n, sizeof *also, gid_sort);
        for (size_t i = 0; i < av->n; i++)
            if (!nalso || cc_gid_cmp(&also[nalso - 1], &also[i])) also[nalso++] = also[i];
    }
    s->has_primary = has_primary;
    if (has_primary) s->primary = primary;
    free(s->also);
    s->also = also, s->nalso = nalso, s->capalso = nalso;
    return 0;
}

/* Steps of one stack into `st`/`ids` (malloc'd, `n` of them); commands into P->sess_cmds. */
static int read_steps(cc_persist *P, const JrVal *arr, uint64_t next_step, cc_step **st, uint64_t **ids, size_t *n,
                      const char **why) {
    *st = NULL, *ids = NULL, *n = 0;
    if (!arr || arr->type != JR_ARR) return *why = "undo/redo: not an array", -1;
    if (!arr->n) return 0;
    *st = calloc(arr->n, sizeof **st);
    *ids = calloc(arr->n, sizeof **ids);
    if (!*st || !*ids) return *why = "out of memory", -1;
    for (size_t i = 0; i < arr->n; i++) {
        const JrVal *o = &arr->items[i];
        if (o->type != JR_OBJ) return *why = "step: not an object", -1;
        if (get_u64(o, "id", 1, next_step - 1, &(*ids)[i])) return *why = "step: bad id", -1;
        const JrVal *label = jr_get(o, "label"), *cmds = jr_get(o, "cmds");
        if (!label || label->type != JR_STR || label->n > SESSION_MAX_LABEL || memchr(label->s, 0, label->n))
            return *why = "step: bad label", -1;
        if (!cmds || cmds->type != JR_STR) return *why = "step: bad cmds", -1;
        size_t first = P->sess_cmds.n;
        Buf err;
        buf_init(&err);
        int bad = cc_commands_parse(cmds->s, cmds->n, &P->sess_cmds, &err);
        buf_free(&err);
        if (bad) return *why = "step: commands do not parse", -1;
        size_t k = P->sess_cmds.n - first;
        cc_step *sp = &(*st)[*n];
        sp->label = malloc(label->n + 1);
        sp->cmds = malloc((k ? k : 1) * sizeof *sp->cmds);
        if (!sp->label || !sp->cmds) {
            free(sp->label), free(sp->cmds);
            return *why = "out of memory", -1;
        }
        memcpy(sp->label, label->s, label->n), sp->label[label->n] = 0;
        if (k) memcpy(sp->cmds, P->sess_cmds.v + first, k * sizeof *sp->cmds);
        sp->n = k;
        (*n)++;
    }
    return 0;
}

static void free_steps(cc_step *st, size_t n) {
    for (size_t i = 0; i < n; i++) free(st[i].label), free(st[i].cmds);
    free(st);
}

static int push_u64(uint64_t **v, size_t *n, size_t *cap, uint64_t x) {
    if (*n == *cap) {
        size_t c = *cap ? 2 * *cap : 16;
        uint64_t *p = realloc(*v, c * sizeof *p);
        if (!p) return -1;
        *v = p, *cap = c;
    }
    (*v)[(*n)++] = x;
    return 0;
}

/* Replays one entry line on the open session through the same history code as the calls.
 * 0 ok; -1 with `why` (the session keeps the lines before; a failed apply changed nothing). */
static int replay_entry(cc_session *s, const char *line, size_t n, const char **why) {
    cc_persist *P = s->persist;
    Arena a;
    arena_init(&a, 0);
    JrError je;
    int rc = -1;
    cc_cmds cmds;
    cc_cmds_init(&cmds);
    Buf err, inv;
    buf_init(&err), buf_init(&inv);
    const JrVal *o = jr_parse(&a, line, n, &je);
    const JrVal *opv = o && o->type == JR_OBJ ? jr_get(o, "op") : NULL;
    uint64_t id = 0, rev, next_step, saved_step = UINT64_MAX;
    if (!opv || opv->type != JR_STR) { *why = "not an entry"; goto end; }
    if (get_u64(o, "rev", 1, 1ull << 52, &rev) || get_u64(o, "next_step", 1, 1ull << 52, &next_step)) {
        *why = "bad rev or next_step";
        goto end;
    }
    const JrVal *sv = jr_get(o, "saved_step");
    if (!sv || (sv->type != JR_NULL && get_u64(o, "saved_step", 0, next_step - 1, &saved_step))) {
        *why = "bad saved_step";
        goto end;
    }
    const char *op = opv->s;
    if (!strcmp(op, "apply")) {
        const JrVal *c = jr_get(o, "cmds"), *iv = jr_get(o, "inv"), *label = jr_get(o, "label");
        if (get_u64(o, "id", 1, 1ull << 52, &id) || id != s->next_step || next_step != id + 1) { *why = "apply: bad id"; goto end; }
        if (!c || c->type != JR_STR || !iv || iv->type != JR_STR) { *why = "apply: bad cmds"; goto end; }
        if (!label || label->type != JR_STR || label->n > SESSION_MAX_LABEL || memchr(label->s, 0, label->n)) {
            *why = "apply: bad label";
            goto end;
        }
        if (cc_commands_parse(c->s, c->n, &cmds, &err)) { *why = "apply: commands do not parse"; goto end; }
        if (push_u64(&s->undo_ids, &s->nundo_ids, &s->cap_undo_ids, id)) { *why = "out of memory"; goto end; }
        if (cc_history_apply(&s->hist, &s->model, cmds.v, cmds.n, &err)) {
            s->nundo_ids--;
            *why = "apply: the commands fail on the model";
            goto end;
        }
        s->nredo_ids = 0;
        s->next_step = id + 1;
        cc_step *top = &s->hist.undo[s->hist.nundo - 1];
        cc_commands_json(top->cmds, top->n, &inv);
        char *l = malloc(label->n + 1);
        if (inv.oom || inv.len != iv->n || memcmp(inv.p, iv->s, iv->n) || !l) {
            /* Not the step that was logged: take it back (the redo stack was cleared by it). */
            free(l);
            cc_history_undo(&s->hist, &s->model);
            if (s->hist.nredo) {
                cc_step *r = &s->hist.redo[--s->hist.nredo];
                free(r->label), free(r->cmds);
            }
            s->nundo_ids--, s->next_step = id;
            *why = "apply: the inverse differs from the logged one";
            goto end;
        }
        memcpy(l, label->s, label->n), l[label->n] = 0;
        free(top->label), top->label = l;
    } else if (!strcmp(op, "undo") || !strcmp(op, "redo")) {
        int undo = op[0] == 'u';
        uint64_t *from = undo ? s->undo_ids : s->redo_ids;
        size_t nfrom = undo ? s->nundo_ids : s->nredo_ids;
        if (get_u64(o, "id", 1, 1ull << 52, &id) || !nfrom || from[nfrom - 1] != id || next_step != s->next_step) {
            *why = "undo/redo: bad id";
            goto end;
        }
        if (undo ? push_u64(&s->redo_ids, &s->nredo_ids, &s->cap_redo_ids, id) : push_u64(&s->undo_ids, &s->nundo_ids, &s->cap_undo_ids, id)) {
            *why = "out of memory";
            goto end;
        }
        if (!(undo ? cc_history_undo(&s->hist, &s->model) : cc_history_redo(&s->hist, &s->model))) {
            if (undo) s->nredo_ids--;
            else s->nundo_ids--;
            *why = "undo/redo: nothing to take";
            goto end;
        }
        if (undo) s->nundo_ids--;
        else s->nredo_ids--;
    } else if (!strcmp(op, "state")) {
        if (next_step != s->next_step) { *why = "state: bad next_step"; goto end; }
    } else {
        *why = "unknown op";
        goto end;
    }
    if (strcmp(op, "state")) P->model_is_file = 0;
    s->rev = rev, s->saved_step = saved_step;
    if (read_selection(s, o, why)) {
        s->has_primary = 0, s->nalso = 0; /* the op is kept; the selection is not */
        goto end;
    }
    rc = 0;
end:
    cc_cmds_free(&cmds);
    buf_free(&err), buf_free(&inv);
    arena_free(&a);
    return rc;
}

static void close_session(cc_session *s) { /* a restore that failed: as if never opened */
    drop_model(s);
    s->rev = 0;
}

static void persist_drop_cmds(cc_session *s) {
    cc_persist *P = s->persist;
    if (!P || !P->follow) return; /* a restore in progress fills them before the model opens */
    cc_cmds_free(&P->sess_cmds);
    cc_cmds_init(&P->sess_cmds);
}

/* The session file follows the file actions (session.h): `model_file` is the file the open model
 * now is (just opened or saved); NULL: none (a new model, a close) and the session file pauses.
 * `again` 0: nothing to do when the session file is already that file's (a Save in place: the
 * next cc_session_persist_now sees the new stamp). */
static void persist_follow(cc_session *s, const char *model_file, int again) {
    cc_persist *P = s->persist;
    if (!P || !P->follow) return;
    if (!again && model_file && s->open && P->on && !strcmp(model_file, P->sess_model) && P->sess_fd >= 0) return; /* the same file */
    if (P->sess_fd >= 0) close(P->sess_fd);
    P->sess_fd = -1, P->on = 0, P->pend_ok = 0;
    if (!model_file || !s->open) return;
    size_t k = strlen(model_file);
    if (k >= sizeof P->sess_model || (P->auto_name && k + 15 >= sizeof P->sess_path)) {
        cc_log(s->pf, "bimble-ccore: no session file for %s (a path too long)", model_file);
        return;
    }
    strcpy(P->sess_model, model_file);
    if (P->auto_name) snprintf(P->sess_path, sizeof P->sess_path, "%s.ccore-session", model_file);
    P->on = 1;
    P->model_is_file = 1; /* just read or just written: the file's content */
    if (write_snapshot(s) == 0) cc_log(s->pf, "bimble-ccore: session file %s", P->sess_path);
}

/* FIXSES: 1 when the session file's text may hold steps that are not saved, or cannot be read
 * enough to tell; 0 when it holds none. The stacks of the snapshot line are followed through the
 * entries (apply, undo, redo): the file holds none when, after its last complete line, the redo
 * stack is empty and the step is the saved one. Bounded by the text (one id per step). */
static int holds_unsaved(const char *text, size_t n) {
    const char *nl = memchr(text, '\n', n);
    if (!nl) return n != 0;
    Arena a;
    arena_init(&a, 0);
    JrError je;
    int dirty = 1;
    uint64_t *u = NULL, saved = UINT64_MAX, x;
    size_t nu = 0, cu = 0, nr = 0;
    const JrVal *root = jr_parse(&a, text, (size_t)(nl - text), &je);
    const JrVal *ua = root && root->type == JR_OBJ ? jr_get(root, "undo") : NULL;
    const JrVal *ra = root && root->type == JR_OBJ ? jr_get(root, "redo") : NULL;
    const JrVal *sv = root && root->type == JR_OBJ ? jr_get(root, "saved_step") : NULL;
    if (!ua || ua->type != JR_ARR || !ra || ra->type != JR_ARR || !sv) goto end;
    for (size_t i = 0; i < ua->n; i++)
        if (get_u64(&ua->items[i], "id", 1, 1ull << 52, &x) || push_u64(&u, &nu, &cu, x)) goto end;
    nr = ra->n;
    if (sv->type != JR_NULL && get_u64(root, "saved_step", 0, 1ull << 52, &saved)) goto end;
    for (const char *at = nl + 1, *e; at < text + n && (e = memchr(at, '\n', (size_t)(text + n - at))); at = e + 1) {
        arena_free(&a);
        arena_init(&a, 0);
        const JrVal *o = jr_parse(&a, at, (size_t)(e - at), &je);
        const JrVal *op = o && o->type == JR_OBJ ? jr_get(o, "op") : NULL;
        const JrVal *s2 = o && o->type == JR_OBJ ? jr_get(o, "saved_step") : NULL;
        if (!op || op->type != JR_STR || !s2) goto end;
        saved = UINT64_MAX;
        if (s2->type != JR_NULL && get_u64(o, "saved_step", 0, 1ull << 52, &saved)) goto end;
        if (!strcmp(op->s, "apply")) {
            if (get_u64(o, "id", 1, 1ull << 52, &x) || push_u64(&u, &nu, &cu, x)) goto end;
            nr = 0;
        } else if (!strcmp(op->s, "undo")) {
            if (!nu) goto end;
            nu--, nr++;
        } else if (!strcmp(op->s, "redo")) {
            if (!nr || get_u64(o, "id", 1, 1ull << 52, &x) || push_u64(&u, &nu, &cu, x)) goto end;
            nr--;
        } else if (strcmp(op->s, "state"))
            goto end;
    }
    dirty = nr != 0 || (nu ? u[nu - 1] : 0) != saved;
end:
    free(u);
    arena_free(&a);
    return dirty;
}

/* FIXSES: the session file (and its model side files) renamed `<session>.orphan-<unix time>`
 * (`-2`, `-3`… when taken), never overwritten: it holds steps that are not saved and the session
 * does not keep them (written for another model path, a model file changed since, a line that
 * does not replay). The time is the carrier's clock (pf->unix_s). Reported by `query state`
 * (`session_set_aside`) and logged. 0 set aside, -1 not (logged: the caller then writes no
 * snapshot over it). */
static int set_aside(cc_session *s) {
    cc_persist *P = s->persist;
    long long t = s->pf->unix_s ? (long long)s->pf->unix_s(s->pf->ctx) : 0;
    char to[sizeof P->aside];
    for (unsigned k = 1; k < 1000; k++) {
        int w = k == 1 ? snprintf(to, sizeof to, "%s.orphan-%lld", P->sess_path, t)
                       : snprintf(to, sizeof to, "%s.orphan-%lld-%u", P->sess_path, t, k);
        if (w < 0 || (size_t)w >= sizeof to) break;
        if (access(to, F_OK) == 0) continue;
        if (rename(P->sess_path, to) != 0) break;
        for (char c = 'a'; c <= 'b'; c++) { /* the side files the line may name follow it */
            char from[sizeof P->sess_path + 16], side[sizeof to + 16];
            snprintf(from, sizeof from, "%s.model-%c", P->sess_path, c);
            snprintf(side, sizeof side, "%s.model-%c", to, c);
            if (access(from, F_OK) == 0 && rename(from, side) != 0)
                cc_log(s->pf, "bimble-ccore: %s: cannot be renamed %s", from, side);
        }
        memcpy(P->aside, to, sizeof to);
        cc_log(s->pf, "bimble-ccore: session %s holds changes not saved: set aside as %s", P->sess_path, to);
        return 0;
    }
    cc_log(s->pf, "bimble-ccore: session %s holds changes not saved and cannot be set aside: left as it is, "
           "no session file this time", P->sess_path);
    return -1;
}

const char *cc_session_set_aside(const cc_session *s) {
    return s->persist && s->persist->aside[0] ? s->persist->aside : NULL;
}

/* FIXSES: the model path a session file names is the open model's when both name one file: the
 * same text, or the same path once the carrier resolved both (pf->real_path; `m.json` and
 * `./m.json` share the default session file `m.json.ccore-session`). */
static int same_model_path(const cc_session *s, const char *recorded, size_t n, const char *model) {
    if (strlen(model) == n && !memcmp(recorded, model, n)) return 1;
    if (!s->pf->real_path || n >= 4096 || memchr(recorded, 0, n)) return 0;
    char a[4096], ra[4096], rb[4096];
    memcpy(a, recorded, n), a[n] = 0;
    return !s->pf->real_path(s->pf->ctx, a, ra, sizeof ra) && !s->pf->real_path(s->pf->ctx, model, rb, sizeof rb) &&
           !strcmp(ra, rb);
}

/* Opens the session from the session file. 1 restored (S open; *keep: bytes of the file that are
 * valid, *rewrite: the file needs a new snapshot), 0 not (no file, or ignored with the reason
 * logged: S closed). *unsaved (FIXSES): the file holds steps not saved that the session does not
 * keep (ignored, or lines dropped): the caller sets it aside before any new snapshot. */
static int restore(cc_session *s, size_t *keep, int *rewrite, int *unsaved) {
    cc_persist *P = s->persist;
    cc_error err;
    size_t n = 0;
    *keep = 0, *rewrite = 0, *unsaved = 0;
    if (access(P->sess_path, F_OK) != 0) return 0;
    char *text = read_all(s, P->sess_path, &n, &err);
    if (!text) {
        cc_log(s->pf, "bimble-ccore: session %s ignored: %s", P->sess_path, err.msg);
        *unsaved = 1; /* FIXSES: not read, so not known to be clean */
        return 0;
    }
    const char *why = NULL;
    Arena a;
    arena_init(&a, 0);
    cc_step *undo = NULL, *redo = NULL;
    uint64_t *uids = NULL, *rids = NULL;
    size_t nu = 0, nr = 0;
    uint64_t fmt, size, mt, fsize, rev, next_step, saved_step = UINT64_MAX;
    char *side = NULL; /* format 3: the model side file's bytes */
    size_t side_n = 0;
    char slot = 0;
    cc_cmds_init(&P->sess_cmds);
    const char *nl = memchr(text, '\n', n);
    size_t first = nl ? (size_t)(nl - text) : n;
    JrError je;
    const JrVal *root = nl ? jr_parse(&a, text, first, &je) : NULL;
    if (!root) { why = "not JSON"; goto fail; }
    if (root->type != JR_OBJ) { why = "not an object"; goto fail; }
    if (get_u64(root, "format", 0, UINT64_MAX, &fmt) || (fmt != SESSION_FORMAT && fmt != SESSION_FORMAT_TEXT)) {
        why = "unknown format";
        goto fail;
    }
    const JrVal *mp = jr_get(root, "model_path");
    if (!mp || mp->type != JR_STR || !same_model_path(s, mp->s, mp->n, P->sess_model)) { /* FIXSES: resolved */
        why = "written for another model path";
        goto fail;
    }
    const JrVal *mtv = jr_get(root, "file_mtime_ns");
    if (file_stamp(P->sess_model, &size, &mt) || get_u64(root, "file_size", 0, 1ull << 52, &fsize) || !mtv ||
        mtv->type != JR_STR) {
        why = "no file stamp";
        goto fail;
    }
    char mts[24];
    snprintf(mts, sizeof mts, "%llu", (unsigned long long)mt);
    if (fsize != size || strlen(mts) != mtv->n || memcmp(mts, mtv->s, mtv->n)) {
        why = "the model file changed since";
        goto fail;
    }
    if (get_u64(root, "rev", 1, 1ull << 52, &rev) || get_u64(root, "next_step", 1, 1ull << 52, &next_step)) {
        why = "bad rev or next_step";
        goto fail;
    }
    const JrVal *sv = jr_get(root, "saved_step");
    if (!sv) { why = "no saved_step"; goto fail; }
    if (sv->type != JR_NULL && get_u64(root, "saved_step", 0, next_step - 1, &saved_step)) { why = "bad saved_step"; goto fail; }
    const JrVal *mt_text = jr_get(root, "model");
    if (!mt_text || (mt_text->type != (fmt == SESSION_FORMAT ? JR_OBJ : JR_STR) && mt_text->type != JR_NULL)) {
        why = "no model text";
        goto fail;
    }
    /* Format 3: the side file, checked against the line (size, BLAKE3) before it is parsed. */
    if (mt_text->type == JR_OBJ) {
        const JrVal *hv = jr_get(mt_text, "blake3"), *sv2 = jr_get(mt_text, "slot");
        uint64_t want_n;
        if (!hv || hv->type != JR_STR || hv->n != 64 || !sv2 || sv2->type != JR_STR || sv2->n != 1 ||
            (sv2->s[0] != 'a' && sv2->s[0] != 'b') || get_u64(mt_text, "bytes", 0, CC_MAX_BINARY_FILE_BYTES, &want_n)) {
            why = "bad model side file entry";
            goto fail;
        }
        slot = sv2->s[0];
        char path[sizeof P->sess_path + 16];
        snprintf(path, sizeof path, "%s.model-%c", P->sess_path, slot);
        cc_error se;
        side = read_all(s, path, &side_n, &se);
        if (!side) { why = "the model side file cannot be read"; goto fail; }
        unsigned char h[32];
        char hex[65];
        cc_blake3(side, side_n, h);
        for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
        if (side_n != want_n || memcmp(hex, hv->s, 64)) {
            why = "the model side file does not match the snapshot (size or BLAKE3)";
            goto fail;
        }
    }
    if (read_steps(P, jr_get(root, "undo"), next_step, &undo, &uids, &nu, &why)) goto fail;
    if (read_steps(P, jr_get(root, "redo"), next_step, &redo, &rids, &nr, &why)) goto fail;
    for (size_t i = 1; i < nu; i++)
        if (uids[i] <= uids[i - 1]) { why = "undo ids not increasing"; goto fail; }
    for (size_t i = 1; i < nr; i++)
        if (rids[i] >= rids[i - 1]) { why = "redo ids not decreasing"; goto fail; }
    /* The model: the snapshot's text (the model file is not read), or the model file itself. */
    P->model_is_file = P->snap_null = mt_text->type == JR_NULL;
    {
        int bad = P->model_is_file ? cc_session_open(s, P->sess_model, &err)
                  : side         ? open_impl(s, P->sess_model, side, side_n, &err)
                                 : open_impl(s, P->sess_model, mt_text->s, mt_text->n, &err);
        free(side);
        side = NULL;
        if (bad) {
            why = P->model_is_file ? "the model file does not load" : "the model text does not load";
            goto fail;
        }
    }
    P->side_slot = slot, P->side_bytes = slot ? side_n : 0;
    if (read_selection(s, root, &why)) goto fail;
    cc_history_free(&s->hist);
    cc_history_init(&s->hist);
    s->hist.undo = undo, s->hist.nundo = s->hist.capundo = nu;
    s->hist.redo = redo, s->hist.nredo = s->hist.capredo = nr;
    undo = redo = NULL;
    free(s->undo_ids), free(s->redo_ids);
    s->undo_ids = uids, s->nundo_ids = s->cap_undo_ids = nu;
    s->redo_ids = rids, s->nredo_ids = s->cap_redo_ids = nr;
    uids = rids = NULL;
    s->rev = rev, s->next_step = next_step, s->saved_step = saved_step;
    P->snap_bytes = first + 1, P->model_file_bytes = size;
    P->W.size = size, P->W.mtime = mt;

    /* The entries: each replayed; a bad one ends the replay, the lines before are kept. */
    size_t at = first + 1, ok = 0;
    while (at < n) {
        const char *e = memchr(text + at, '\n', n - at);
        if (!e) {
            why = "a truncated last line";
            break;
        }
        size_t len = (size_t)(e - (text + at));
        if (ok >= SESSION_MAX_LINES) { why = "too many lines"; *rewrite = 1; break; }
        if (replay_entry(s, text + at, len, &why)) { *rewrite = 1; break; }
        at += len + 1, ok++;
    }
    *keep = at;
    if (*rewrite) *unsaved = holds_unsaved(text, n); /* FIXSES: the lines from the bad one would be dropped */
    P->log_bytes = at - P->snap_bytes, P->log_entries = ok;
    cc_sv_vis_load(s); /* ISS72: what the active view hides, as the restored steps left it (SEC1's rebuild does it after a live edit) */
    if (at < n)
        cc_log(s->pf, "bimble-ccore: session %s ignored: from line %zu: %s; kept the snapshot and %zu entries",
                P->sess_path, ok + 2, why, ok);
    cc_log(s->pf, "bimble-ccore: session restored from %s (rev %llu, %zu undo, %zu redo, %zu selected, %zu entries replayed)",
            P->sess_path, (unsigned long long)s->rev, s->nundo_ids, s->nredo_ids, (size_t)s->has_primary + s->nalso, P->log_entries);
    arena_free(&a);
    free(text);
    return 1;
fail:
    cc_log(s->pf, "bimble-ccore: session %s ignored: %s; starting from %s", P->sess_path, why ? why : "invalid",
            P->sess_model);
    *unsaved = holds_unsaved(text, n); /* FIXSES */
    close_session(s);
    free(side);
    free_steps(undo, nu), free_steps(redo, nr);
    free(uids), free(rids);
    cc_cmds_free(&P->sess_cmds);
    arena_free(&a);
    free(text);
    return 0;
}

int cc_session_open_persisted(cc_session *s, const char *model_path, const char *session_path, cc_error *err) {
    persist_free(s);
    cc_persist *P = calloc(1, sizeof *P);
    if (!P) {
        cc_log(s->pf, "bimble-ccore: no session file (out of memory)");
        return cc_session_open(s, model_path, err);
    }
    P->sess_fd = -1;
    buf_init(&P->pend_cmds);
    cc_cmds_init(&P->sess_cmds);
    s->persist = P;
    if (strlen(session_path) >= sizeof P->sess_path || strlen(model_path) >= sizeof P->sess_model) {
        cc_log(s->pf, "bimble-ccore: no session file (a path too long)");
        persist_free(s);
        return cc_session_open(s, model_path, err);
    }
    strcpy(P->sess_path, session_path);
    strcpy(P->sess_model, model_path);
    {
        size_t k = strlen(model_path);
        P->auto_name = !strncmp(session_path, model_path, k) && !strcmp(session_path + k, ".ccore-session");
    }
    size_t keep = 0;
    int rewrite = 0, unsaved = 0;
    if (restore(s, &keep, &rewrite, &unsaved)) {
        P->on = 1;
        if (!rewrite) {
            P->sess_fd = open(P->sess_path, O_WRONLY | O_APPEND);
            if (P->sess_fd >= 0 && ftruncate(P->sess_fd, (off_t)keep) == 0 && remember(s) == 0) return P->follow = 1, 0;
        }
        /* FIXSES: never a new snapshot over lines with steps not saved: set aside first. */
        if (unsaved && set_aside(s)) return P->on = 0, 0; /* no file this run: it is left alone */
        write_snapshot(s);
        P->follow = 1;
        return 0;
    }
    if (cc_session_open(s, model_path, err)) {
        persist_free(s);
        return -1;
    }
    /* FIXSES: the file the session ignored, when it holds steps not saved, is set aside, never
     * overwritten; when it cannot be, the session runs without a file this time (on, follow 0). */
    if (unsaved && set_aside(s)) return 0;
    P->on = 1;
    P->model_is_file = 1;
    write_snapshot(s); /* no model text: cheap */
    P->follow = 1;
    return 0;
}
#endif
