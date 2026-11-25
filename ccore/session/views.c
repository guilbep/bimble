/* The views a session shows and its tabs: see views.h. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "guid.h"
#include "session.h"
#include "view_effective.h"
#include "view_io.h"
#include "view_range.h"
#include "views.h"

static int loaded(const struct cc_session *s) { return s->open || s->view; }

int cc_sv_list(struct cc_session *s, Arena *a, cc_view **out, size_t *n) {
    *out = NULL, *n = 0;
    if (!loaded(s)) return 0;
    return cc_views_effective(s->open ? &s->model : NULL, a, out, n);
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
static void auto_name(JsonW *w, const cc_model *m, const cc_view *v) {
    Arena a;
    arena_init(&a, 0);
    size_t nd = 1 + (m ? 2 * m->nstoreys : 0);
    cc_view *d = arena_alloc(&a, nd * sizeof *d, sizeof(double));
    int done = 0;
    if (d && !cc_views_default(m, &a, d))
        for (size_t i = 0; i < nd && !done; i++) {
            if (cc_gid_cmp(&d[i].id, &v->id) || d[i].name.n != v->name.n || memcmp(d[i].name.p, v->name.p, v->name.n))
                continue;
            done = 1;
            jw_obj_begin(w);
            jw_key(w, "kind");
            jw_str(w, d[i].kind == CC_VIEW_THREE_D ? "3d" : d[i].look == CC_LOOK_DOWN ? "plan" : "ceiling");
            jw_key(w, "storey");
            const cc_storey *st = NULL;
            if (d[i].kind == CC_VIEW_PLAN) {
                cc_gid sid;
                memcpy(sid.c, d[i].storey.p, 22);
                st = cc_model_storey(m, &sid);
            }
            if (st) jw_str_n(w, st->name.p, st->name.n);
            else jw_null(w);
            jw_obj_end(w);
        }
    if (!done) jw_null(w);
    arena_free(&a);
}

void cc_sv_summary_json(JsonW *w, const cc_model *m, const cc_view *v) {
    static const char *const PLANE[] = {"bottom", "cut", "depth", "top"};
    static const int PIDX[] = {CC_RANGE_BOTTOM, CC_RANGE_CUT, CC_RANGE_DEPTH, CC_RANGE_TOP};
    jw_obj_begin(w);
    jw_key(w, "annotations"), jw_u64(w, v->nannotations);
    jw_key(w, "auto_name"), auto_name(w, m, v);
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
