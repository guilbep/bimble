/* The views a session shows and its tabs: see views.h. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "guid.h"
#include "session.h"
#include "view_io.h"
#include "view_range.h"
#include "views.h"

static int loaded(const struct cc_session *s) { return s->open || s->view; }

static cc_gid derived_view(const char *pre, const cc_gid *storey) {
    char seed[64];
    size_t n = strlen(pre);
    memcpy(seed, pre, n);
    if (storey) memcpy(seed + n, storey->c, 22), n += 22;
    cc_guid g = cc_guid_derived(seed, n);
    cc_gid out;
    memcpy(out.c, g.c, 22);
    return out;
}

static int64_t tkey(double x) { /* f64::total_cmp */
    int64_t k;
    memcpy(&k, &x, 8);
    return k ^ (int64_t)((uint64_t)(k >> 63) >> 1);
}

static double round_(double x) { /* f64::round: half away from zero */
    double a = x < 0 ? -x : x;
    if (!(a < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x;
    double d = x - t;
    if (d >= 0.5) t += 1.0;
    else if (d <= -0.5) t -= 1.0;
    return t == 0.0 ? x * 0.0 : t;
}

/* The storeys by elevation (a stable sort of the id-ordered storeys, total_cmp): indices. */
static size_t *sorted_storeys(const cc_model *m, Arena *a) {
    size_t n = m->nstoreys;
    size_t *o = arena_alloc(a, (n ? n : 1) * sizeof *o, sizeof(size_t));
    if (!o) return NULL;
    for (size_t i = 0; i < n; i++) {
        size_t j = i;
        int64_t k = tkey(m->storeys[i].elevation);
        while (j > 0 && tkey(m->storeys[o[j - 1]].elevation) > k) o[j] = o[j - 1], j--;
        o[j] = i;
    }
    return o;
}

static cc_str cat2(Arena *a, const char *pre, cc_str s) {
    size_t n = strlen(pre);
    char *p = arena_alloc(a, n + s.n + 1, 1);
    cc_str out = {NULL, 0};
    if (!p) return out;
    memcpy(p, pre, n);
    if (s.n) memcpy(p + n, s.p, s.n);
    p[n + s.n] = 0;
    out.p = p, out.n = n + s.n;
    return out;
}

/* view.rs default_views on the model's storeys, into `out` (1 + 2 * nstoreys views). */
static int defaults(const cc_model *m, Arena *a, cc_view *out) {
    size_t n = m ? m->nstoreys : 0, k = 0;
    memset(&out[k], 0, sizeof out[k]);
    out[k].id = derived_view("bimble view 3d", NULL);
    out[k].name.p = "3D", out[k].name.n = 2;
    out[k].kind = CC_VIEW_THREE_D;
    out[k].scale = 100;
    k++;
    if (!n) return 0;
    size_t *o = sorted_storeys(m, a);
    if (!o) return -1;
    for (int look = 0; look < 2; look++)
        for (size_t i = 0; i < n; i++) {
            const cc_storey *s = &m->storeys[o[i]];
            int any = 0;
            double h = 0.0;
            for (size_t j = i + 1; j < n; j++) { /* the first storey above more than 1 mm higher */
                double d = m->storeys[o[j]].elevation - s->elevation;
                if (d > 1.0) {
                    any = 1, h = d;
                    break;
                }
            }
            cc_view *v = &out[k++];
            memset(v, 0, sizeof *v);
            v->id = derived_view(look ? "bimble view plan up " : "bimble view plan down ", &s->id);
            v->name = cat2(a, look ? "Ceiling " : "Plan ", s->name);
            char *sid = arena_dup(a, s->id.c, 22);
            if (!v->name.p || !sid) return -1;
            v->kind = CC_VIEW_PLAN;
            v->storey.p = sid, v->storey.n = 22;
            v->look = look ? CC_LOOK_UP : CC_LOOK_DOWN;
            cc_range_plane top = {CC_LEVEL_ABOVE, 0.0}, cut = {CC_LEVEL_ASSOCIATED, look ? 2300.0 : 1200.0},
                           bottom = {CC_LEVEL_ASSOCIATED, 0.0}, depth = {look ? CC_LEVEL_ABOVE : CC_LEVEL_ASSOCIATED, 0.0};
            if (any && cut.offset_mm >= h) cut.offset_mm = round_(h / 2.0);
            v->range[CC_RANGE_TOP] = top, v->range[CC_RANGE_CUT] = cut;
            v->range[CC_RANGE_BOTTOM] = bottom, v->range[CC_RANGE_DEPTH] = depth;
            v->scale = 100;
        }
    return 0;
}

int cc_sv_list(struct cc_session *s, Arena *a, cc_view **out, size_t *n) {
    *out = NULL, *n = 0;
    if (!loaded(s)) return 0;
    const cc_model *m = s->open ? &s->model : NULL;
    size_t nd = 1 + (m ? 2 * m->nstoreys : 0), ns = m ? m->nviews : 0;
    cc_view *v = arena_alloc(a, (nd + ns) * sizeof *v, sizeof(double));
    if (!v || defaults(m, a, v)) return -1;
    size_t k = nd;
    for (size_t i = 0; i < nd; i++) { /* stored over the defaults */
        const cc_view *st = m ? cc_model_view(m, &v[i].id) : NULL;
        if (st) v[i] = *st;
    }
    for (size_t i = 0; i < ns; i++) { /* the other stored views, id order */
        const cc_view *st = &m->views[i];
        int dflt = 0;
        for (size_t j = 0; j < nd && !dflt; j++) dflt = !cc_gid_cmp(&v[j].id, &st->id);
        if (!dflt) v[k++] = *st;
    }
    *out = v, *n = k;
    return 0;
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
    s->active = derived_view("bimble view 3d", NULL);
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
    if (d && !defaults(m, &a, d))
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
