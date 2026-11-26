/* The views a session shows and its tabs: see views.h. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "guid.h"
#include "links.h"
#include "session.h"
#include "view_effective.h"
#include "view_io.h"
#include "view_range.h"
#include "views.h"
#ifndef CC_VIEW_ONLY
#include "draw_style.h" /* SEC1: cc_category_label, the family names a view hides */
#endif

static int loaded(const struct cc_session *s) { return s->open || s->view; }

/* S1: the levels of a read-only scene (scene.rs levels): its storeys as read, key the file's
 * GlobalId text or the name when it has none. */
int cc_sv_scene_levels(int kind, const void *scene, Arena *a, cc_level **out, size_t *n) {
    const cc_ifc_scene *ifc = kind == CC_VIEW_IFC ? scene : NULL;
    const cc_viewfile *vf = kind == CC_VIEW_FILE ? scene : NULL;
    size_t k = ifc ? ifc->nstoreys : vf ? vf->nstoreys : 0;
    cc_level *lv = arena_alloc(a, (k ? k : 1) * sizeof *lv, sizeof(double));
    *out = lv, *n = k;
    if (!lv) return -1;
    for (size_t i = 0; i < k; i++) {
        cc_str name = ifc ? ifc->storeys[i].name : vf->storeys[i].name, id = ifc ? ifc->storeys[i].id : vf->storeys[i].id;
        lv[i].key = id.n ? id : name, lv[i].name = name;
        lv[i].elevation = ifc ? ifc->storeys[i].elevation_mm : vf->storeys[i].elevation;
    }
    return 0;
}

int cc_sv_levels(const struct cc_session *s, Arena *a, cc_level **out, size_t *n) {
    if (s->open) return cc_levels_of_model(&s->model, a, out, n);
    return cc_sv_scene_levels(s->view, s->view == CC_VIEW_IFC ? (const void *)s->ifc : (const void *)s->vf, a, out, n);
}

int cc_sv_list(struct cc_session *s, Arena *a, cc_view **out, size_t *n) {
    *out = NULL, *n = 0;
    if (!loaded(s)) return 0;
    cc_level *lv;
    size_t nl;
    if (cc_sv_levels(s, a, &lv, &nl)) return -1;
    return cc_views_effective_lv(s->open ? &s->model : NULL, lv, nl, a, out, n);
}

static int has_view(const cc_view *v, size_t n, const cc_gid *id) {
    for (size_t i = 0; i < n; i++)
        if (!cc_gid_cmp(&v[i].id, id)) return 1;
    return 0;
}

static int tabs_push(struct cc_session *s, const cc_gid *id) {
    if (s->ntabs == s->captabs) {
        size_t c = s->captabs ? 2 * s->captabs : 8;
        cc_gid *p = realloc(s->tabs, c * sizeof *p);
        if (!p) return -1;
        s->tabs = p, s->captabs = c;
    }
    s->tabs[s->ntabs++] = *id;
    return 0;
}

int cc_sv_tab_open(const struct cc_session *s, const cc_gid *id) {
    for (size_t i = 0; i < s->ntabs; i++)
        if (!cc_gid_cmp(&s->tabs[i], id)) return 1;
    return 0;
}

static void tabs_remove(struct cc_session *s, const cc_gid *id) {
    size_t k = 0;
    for (size_t i = 0; i < s->ntabs; i++)
        if (cc_gid_cmp(&s->tabs[i], id)) s->tabs[k++] = s->tabs[i];
    s->ntabs = k;
}

void cc_sv_loaded(struct cc_session *s) {
    s->ntabs = 0;
    s->active = cc_view_derived_id("bimble view 3d", NULL);
    s->has_active = 1;
    s->tab_open = 1; /* the 3D view is shown on load */
    tabs_push(s, &s->active); /* out of memory: sync adds it again */
    cc_sv_vis_load(s); /* SEC1: what the 3D view hides, if it is stored */
}

int cc_sv_sync(struct cc_session *s) {
    if (!loaded(s)) {
        s->ntabs = 0, s->has_active = 0, s->tab_open = 0;
        return 0;
    }
    Arena a;
    arena_init(&a, 0);
    cc_view *v;
    size_t n;
    int rc = cc_sv_list(s, &a, &v, &n);
    if (!rc) {
        if (!s->has_active || !has_view(v, n, &s->active)) { /* refresh_views */
            s->has_active = n > 0;
            if (n) s->active = v[0].id;
        }
        size_t k = 0;
        for (size_t i = 0; i < s->ntabs; i++)
            if (has_view(v, n, &s->tabs[i])) s->tabs[k++] = s->tabs[i];
        s->ntabs = k;
        if (s->tab_open && s->has_active && !cc_sv_tab_open(s, &s->active)) rc = tabs_push(s, &s->active);
    }
    arena_free(&a);
    return rc;
}

int cc_sv_active(struct cc_session *s, Arena *a, cc_view *out) {
    if (cc_sv_sync(s)) return -1;
    if (!s->has_active) return 0;
    cc_view *v;
    size_t n;
    if (cc_sv_list(s, a, &v, &n)) return -1;
    for (size_t i = 0; i < n; i++)
        if (!cc_gid_cmp(&v[i].id, &s->active)) {
            *out = v[i];
            return 1;
        }
    return 0;
}

void cc_sv_go(struct cc_session *s, const cc_gid *id) {
    int same = s->has_active && !cc_gid_cmp(&s->active, id);
    s->tab_open = 1;
    if (!cc_sv_tab_open(s, id)) tabs_push(s, id); /* sync_tabs: the current one has its tab */
    if (same) return;
    cc_vis_clear_temporary(&s->vis); /* hidden for now belongs to the view it was done in */
    s->active = *id, s->has_active = 1;
    Arena a;
    arena_init(&a, 0);
    cc_view v;
    if (s->open && cc_sv_active(s, &a, &v) == 1 && v.kind == CC_VIEW_PLAN && v.storey.n == 22) {
        cc_gid sid; /* placing elements on a plan goes on its storey */
        memcpy(sid.c, v.storey.p, 22);
        if (cc_model_storey(&s->model, &sid)) s->has_storey = 1, s->storey = sid;
    }
    arena_free(&a);
    cc_sv_vis_load(s); /* SEC1: viewing.rs:378 apply_visibility (out of memory: nothing hidden more) */
}

void cc_sv_close(struct cc_session *s, int is_view, const cc_gid *id) {
    int current = s->tab_open && s->has_active && is_view && !cc_gid_cmp(&s->active, id);
    if (is_view) tabs_remove(s, id);
    if (!s->ntabs) { /* nothing left open: Home, the model kept loaded */
        s->tab_open = 0;
        return;
    }
    if (current) {
        cc_gid g = s->tabs[s->ntabs - 1];
        cc_sv_go(s, &g);
    }
}

int cc_sv_resolve(const cc_model *m, const cc_view *v, double out[4]) {
    if (!m || v->kind != CC_VIEW_PLAN) return -1;
    return cc_view_resolve(m, v, out, NULL) ? -1 : 0; /* view_range.c, shared with the drawing */
}

int cc_sv_plan_range(const struct cc_session *s, const cc_view *v, double out[4]) {
    if (v->kind != CC_VIEW_PLAN || !loaded(s)) return -1;
    Arena a;
    arena_init(&a, 0);
    cc_level *lv;
    size_t n;
    int rc = cc_sv_levels(s, &a, &lv, &n) || cc_view_resolve_lv(lv, n, v, out, NULL) ? -1 : 0;
    arena_free(&a);
    return rc;
}

int cc_sv_level(const struct cc_session *s, cc_str key, double *elevation) {
    Arena a;
    arena_init(&a, 0);
    cc_level *lv;
    size_t n;
    int found = 0;
    if (loaded(s) && !cc_sv_levels(s, &a, &lv, &n))
        for (size_t i = 0; i < n && !found; i++) /* the first with that key, by elevation as resolve */
            if (lv[i].key.n == key.n && (!key.n || !memcmp(lv[i].key.p, key.p, key.n))) {
                double best = lv[i].elevation;
                for (size_t j = i + 1; j < n; j++)
                    if (lv[j].key.n == key.n && (!key.n || !memcmp(lv[j].key.p, key.p, key.n)) && lv[j].elevation < best)
                        best = lv[j].elevation;
                *elevation = best, found = 1;
            }
    arena_free(&a);
    return found;
}

void cc_sv_tab_json(JsonW *w, const cc_gid *id) {
    jw_obj_begin(w);
    jw_key(w, "id"), jw_str_n(w, id->c, 22);
    jw_key(w, "type"), jw_str(w, "view");
    jw_obj_end(w);
}

static void w_pair(JsonW *w, const double *v) {
    jw_arr_begin(w);
    jw_f64(w, v[0]);
    jw_f64(w, v[1]);
    jw_arr_end(w);
}

/* viewing.rs auto_name: the default with this id and name, as {"kind","storey"}; else null. */
static void auto_name(JsonW *w, const cc_level *lv, size_t nl, const cc_view *v) {
    Arena a;
    arena_init(&a, 0);
    size_t nd = 1 + 2 * nl;
    cc_view *d = arena_alloc(&a, nd * sizeof *d, sizeof(double));
    int done = 0;
    if (d && !cc_views_default_lv(lv, nl, &a, d))
        for (size_t i = 0; i < nd && !done; i++) {
            if (cc_gid_cmp(&d[i].id, &v->id) || d[i].name.n != v->name.n || memcmp(d[i].name.p, v->name.p, v->name.n))
                continue;
            done = 1;
            jw_obj_begin(w);
            jw_key(w, "kind");
            jw_str(w, d[i].kind == CC_VIEW_THREE_D ? "3d" : d[i].look == CC_LOOK_DOWN ? "plan" : "ceiling");
            jw_key(w, "storey");
            const cc_level *st = NULL; /* levels.find(key): the first in their order */
            for (size_t k = 0; d[i].kind == CC_VIEW_PLAN && k < nl && !st; k++)
                if (lv[k].key.n == d[i].storey.n && (!lv[k].key.n || !memcmp(lv[k].key.p, d[i].storey.p, lv[k].key.n)))
                    st = &lv[k];
            if (st) jw_str_n(w, st->name.p, st->name.n);
            else jw_null(w);
            jw_obj_end(w);
        }
    if (!done) jw_null(w);
    arena_free(&a);
}

void cc_sv_summary_json(JsonW *w, const cc_level *lv, size_t nl, const cc_view *v) {
    static const char *const PLANE[] = {"bottom", "cut", "depth", "top"};
    static const int PIDX[] = {CC_RANGE_BOTTOM, CC_RANGE_CUT, CC_RANGE_DEPTH, CC_RANGE_TOP};
    jw_obj_begin(w);
    jw_key(w, "annotations"), jw_u64(w, v->nannotations);
    jw_key(w, "auto_name"), auto_name(w, lv, nl, v);
    jw_key(w, "crop");
    if (v->has_crop) {
        jw_obj_begin(w);
        jw_key(w, "max"), w_pair(w, v->crop_max);
        jw_key(w, "min"), w_pair(w, v->crop_min);
        jw_obj_end(w);
    } else {
        jw_null(w);
    }
    if (v->kind == CC_VIEW_SECTION) jw_key(w, "depth_mm"), jw_f64(w, v->depth_mm);
    if (v->kind == CC_VIEW_SECTION) jw_key(w, "facing"), jw_str(w, cc_facing_name(v->facing));
    jw_key(w, "id"), jw_str_n(w, v->id.c, 22);
    if (v->kind == CC_VIEW_PLAN) jw_key(w, "look"), jw_str(w, cc_look_name(v->look));
    jw_key(w, "name"), jw_str_n(w, v->name.p, v->name.n);
    if (v->kind == CC_VIEW_SECTION) jw_key(w, "origin"), w_pair(w, v->origin);
    if (v->kind == CC_VIEW_PLAN) {
        jw_key(w, "range");
        jw_obj_begin(w);
        for (int k = 0; k < 4; k++) {
            jw_key(w, PLANE[k]);
            jw_obj_begin(w);
            jw_key(w, "level"), jw_str(w, cc_level_ref_name(v->range[PIDX[k]].level));
            jw_key(w, "offset_mm"), jw_f64(w, v->range[PIDX[k]].offset_mm);
            jw_obj_end(w);
        }
        jw_obj_end(w);
    }
    jw_key(w, "references"), jw_u64(w, v->nreferences);
    jw_key(w, "scale"), jw_u64(w, v->scale);
    if (v->kind == CC_VIEW_PLAN) jw_key(w, "storey"), jw_str_n(w, v->storey.p, v->storey.n);
    jw_key(w, "true_north"), jw_bool(w, v->true_north);
    jw_key(w, "type"), jw_str(w, cc_view_kind_name(v->kind));
    jw_obj_end(w);
}

/* ---- S1: query grid_axes (views.h) ---- */

static void w_axis(JsonW *w, cc_str tag, const double *a, const double *b) {
    jw_obj_begin(w);
    jw_key(w, "end_mm"), w_pair(w, b);
    jw_key(w, "start_mm"), w_pair(w, a);
    jw_key(w, "tag"), jw_str_n(w, tag.p, tag.n);
    jw_obj_end(w);
}

/* "axes" and "levels" of a model (its grids, id order, axes in order) or a read-only scene. */
static void w_scene(JsonW *w, const cc_model *m, int kind, const void *scene) {
    const cc_ifc_scene *ifc = kind == CC_VIEW_IFC ? scene : NULL;
    const cc_viewfile *vf = kind == CC_VIEW_FILE ? scene : NULL;
    jw_key(w, "axes"), jw_arr_begin(w);
    for (size_t g = 0; m && g < m->ngrids; g++)
        for (size_t i = 0; i < m->grids[g].naxes; i++) {
            const cc_grid_axis *x = &m->grids[g].axes[i];
            w_axis(w, x->tag, x->start, x->end);
        }
    for (size_t i = 0; ifc && i < ifc->ngrids; i++) w_axis(w, ifc->grids[i].tag, ifc->grids[i].start_mm, ifc->grids[i].end_mm);
    for (size_t i = 0; vf && i < vf->ngrids; i++) w_axis(w, vf->grids[i].tag, vf->grids[i].start, vf->grids[i].end);
    jw_arr_end(w);
    Arena a;
    arena_init(&a, 0);
    cc_level *lv = NULL;
    size_t n = 0;
    if (m ? cc_levels_of_model(m, &a, &lv, &n) : cc_sv_scene_levels(kind, scene, &a, &lv, &n)) w->b->oom = 1, n = 0;
    jw_key(w, "levels"), jw_arr_begin(w);
    for (size_t i = 0; i < n; i++) {
        jw_obj_begin(w);
        jw_key(w, "elevation_mm"), jw_f64(w, lv[i].elevation);
        jw_key(w, "key"), jw_str_n(w, lv[i].key.p, lv[i].key.n);
        jw_key(w, "name"), jw_str_n(w, lv[i].name.p, lv[i].name.n);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    arena_free(&a);
}

void cc_sv_grid_axes_json(struct cc_session *s, JsonW *w) {
    if (!loaded(s)) {
        jw_null(w);
        return;
    }
    jw_obj_begin(w);
    w_scene(w, s->open ? &s->model : NULL, s->view, s->view == CC_VIEW_IFC ? (const void *)s->ifc : (const void *)s->vf);
    jw_key(w, "links"), jw_arr_begin(w);
    for (size_t i = 0; i < s->nlinks; i++) {
        const cc_link *k = &s->links[i];
        if (!k->kind) continue; /* LNK1: a missing link (links.h) has no axes nor levels */
        jw_obj_begin(w);
        w_scene(w, k->kind == CC_LINK_MODEL ? k->model : NULL, k->kind == CC_LINK_IFC ? CC_VIEW_IFC : CC_VIEW_FILE,
                k->kind == CC_LINK_IFC ? (const void *)k->ifc : (const void *)k->vf);
        jw_key(w, "name"), jw_str(w, k->name);
        jw_key(w, "visible"), jw_bool(w, k->visible);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_obj_end(w);
}

/* ---- SEC1: what the active view hides (views.h cc_sv_vis_load) ---- */

static int strs_has(const cc_strs *l, const char *p, size_t n) {
    for (size_t i = 0; i < l->n; i++)
        if (l->v[i].n == n && (!n || !memcmp(l->v[i].p, p, n))) return 1;
    return 0;
}

static int gids_of(cc_gidset *set, const cc_strs *l) {
    set->n = 0;
    for (size_t i = 0; i < l->n; i++) {
        if (l->v[i].n != 22 || !cc_gid_valid(l->v[i].p, 22)) continue;
        cc_gid g;
        memcpy(g.c, l->v[i].p, 22);
        if (cc_gidset_add(set, &g)) return -1;
    }
    return 0;
}

int cc_sv_vis_load(struct cc_session *s) {
    if (!s->open) return 0; /* a read-only scene: "in view" stays session-wide (no stored views in C) */
    cc_vis *vis = &s->vis;
    Arena a;
    arena_init(&a, 0);
    cc_view v;
    int r = cc_sv_active(s, &a, &v), rc = 0;
    if (r < 0) return arena_free(&a), -1;
    vis->by_view.n = 0, vis->view_lists = 0;
    if (!r) {
        vis->hidden.n = vis->isolated.n = 0;
        return arena_free(&a), 0;
    }
    const cc_visibility *w = &v.visibility;
    rc = gids_of(&vis->hidden, &w->hidden_elements) || gids_of(&vis->isolated, &w->isolated_elements) ? -1 : 0;
    vis->view_lists = w->hidden_families.n + w->hidden_classes.n + w->hidden_storeys.n + w->hidden_materials.n +
                      w->hidden_links.n;
    const cc_scene *sc = vis->view_lists ? cc_session_scene(s) : NULL;
    for (size_t k = 0; !rc && sc && k < sc->nelements; k++) { /* viewdraw::Shown, lib.rs `visible` */
        const cc_scene_element *e = &sc->elements[k];
        int hide = strs_has(&w->hidden_classes, e->class_name.p, e->class_name.n);
#ifndef CC_VIEW_ONLY
        const char *fam = cc_category_label(e->category);
        hide = hide || strs_has(&w->hidden_families, fam, strlen(fam));
#endif
        if (!hide && e->model_index < s->model.nelements) {
            const cc_element *me = &s->model.elements[e->model_index];
            const cc_storey *st = cc_model_storey(&s->model, &me->storey);
            const cc_material *mt = cc_model_material(&s->model, &me->material);
            hide = (st && strs_has(&w->hidden_storeys, st->name.p, st->name.n)) ||
                   strs_has(&w->hidden_materials, mt ? mt->name.p : "", mt ? mt->name.n : 0);
        }
        if (hide) rc = cc_gidset_add(&vis->by_view, &e->id);
    }
    arena_free(&a);
    return rc;
}
/* ---- end SEC1 ---- */
