/* The work plane and the reference planes' list (WAVE1B): see refplanes.h. */
#include "refplanes.h"
#include <string.h>
#include "math.h"
#include "refplane.h"

static const cc_grid_axis *find_axis(const cc_model *m, const cc_gid *id, const cc_grid **g) {
    for (size_t i = 0; i < m->ngrids; i++)
        for (size_t k = 0; k < m->grids[i].naxes; k++)
            if (!cc_gid_cmp(&m->grids[i].axes[k].id, id)) {
                if (g) *g = &m->grids[i];
                return &m->grids[i].axes[k];
            }
    return NULL;
}

static double storey_elev(const cc_model *m, const cc_gid *id) {
    const cc_storey *st = cc_model_storey(m, id);
    return st ? st->elevation : 0.0;
}

int cc_wplane_frame(const cc_wplane *wp, const cc_model *m, double level, double o[3], double n[3]) {
    if (wp->kind == CC_WP_PLANE) {
        const cc_element *e = cc_model_element(m, &wp->id);
        if (!e || e->cls != CC_IFC_VIRTUAL_ELEMENT) return 0;
        double x[3], y[3];
        cc_refplane_frame(e, storey_elev(m, &e->storey), o, n, x, y);
        return 1;
    }
    if (wp->kind == CC_WP_LEVEL) {
        const cc_storey *st = cc_model_storey(m, &wp->id);
        if (!st) return 0;
        o[0] = o[1] = 0.0, o[2] = st->elevation;
        n[0] = n[1] = 0.0, n[2] = 1.0;
        return 1;
    }
    if (wp->kind == CC_WP_AXIS) {
        const cc_grid_axis *a = find_axis(m, &wp->id, NULL);
        if (!a) return 0;
        double dx = a->end[0] - a->start[0], dy = a->end[1] - a->start[1], l = cc_hypot(dx, dy);
        if (!(l > 1e-9)) return 0;
        o[0] = a->start[0], o[1] = a->start[1], o[2] = level;
        n[0] = -dy / l + 0.0, n[1] = dx / l + 0.0, n[2] = 0.0;
        return 1;
    }
    return 0;
}

static int fail(Buf *err, const char *msg) {
    buf_puts(err, msg);
    return -1;
}

int cc_wplane_set(cc_wplane *wp, const cc_model *m, const char *body, size_t n, Buf *err) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = n ? jr_parse(&a, body, n, &je) : NULL;
    int rc = -1;
    if (!v || v->type != JR_OBJ) {
        fail(err, "work_plane: {\"kind\": \"plane\" | \"level\" | \"axis\" | null, \"id\": GlobalId}");
        goto end;
    }
    const JrVal *k = jr_get(v, "kind"), *id = jr_get(v, "id");
    if (!k || k->type == JR_NULL) {
        wp->kind = CC_WP_NONE;
        rc = 0;
        goto end;
    }
    int kind = k->type != JR_STR ? -1
               : !strcmp(k->s, "plane") ? CC_WP_PLANE
               : !strcmp(k->s, "level") ? CC_WP_LEVEL
               : !strcmp(k->s, "axis")  ? CC_WP_AXIS
                                         : -1;
    if (kind < 0) {
        fail(err, "work_plane: kind is \"plane\", \"level\", \"axis\" or null");
        goto end;
    }
    if (!id || id->type != JR_STR || !cc_gid_valid(id->s, id->n)) {
        fail(err, "work_plane: id: a GlobalId is required");
        goto end;
    }
    cc_gid g;
    memcpy(g.c, id->s, 22);
    const cc_element *e = kind == CC_WP_PLANE ? cc_model_element(m, &g) : NULL;
    if ((kind == CC_WP_PLANE && (!e || e->cls != CC_IFC_VIRTUAL_ELEMENT)) || (kind == CC_WP_LEVEL && !cc_model_storey(m, &g)) ||
        (kind == CC_WP_AXIS && !find_axis(m, &g, NULL))) {
        fail(err, "work_plane: no such plane");
        goto end;
    }
    wp->kind = kind, wp->id = g;
    rc = 0;
end:
    arena_free(&a);
    return rc;
}

static void v3(JsonW *w, const double v[3]) {
    jw_arr_begin(w);
    for (int k = 0; k < 3; k++) jw_f64(w, v[k] + 0.0);
    jw_arr_end(w);
}
static void v2(JsonW *w, const double v[2]) {
    jw_arr_begin(w);
    for (int k = 0; k < 2; k++) jw_f64(w, v[k] + 0.0);
    jw_arr_end(w);
}

void cc_wplane_json(const cc_wplane *wp, const cc_model *m, JsonW *w) {
    jw_obj_begin(w);
    jw_key(w, "active");
    double o[3], nn[3];
    if (cc_wplane_frame(wp, m, 0.0, o, nn)) {
        static const char *const K[] = {"", "plane", "level", "axis"};
        jw_obj_begin(w);
        jw_key(w, "id"), jw_str_n(w, wp->id.c, 22);
        jw_key(w, "kind"), jw_str(w, K[wp->kind]);
        jw_key(w, "name");
        if (wp->kind == CC_WP_PLANE) {
            const cc_element *e = cc_model_element(m, &wp->id);
            jw_str_n(w, e->name.n ? e->name.p : "", e->name.n);
        } else if (wp->kind == CC_WP_LEVEL) {
            const cc_storey *st = cc_model_storey(m, &wp->id);
            jw_str_n(w, st->name.n ? st->name.p : "", st->name.n);
        } else {
            const cc_grid_axis *a = find_axis(m, &wp->id, NULL);
            jw_str_n(w, a->tag.n ? a->tag.p : "", a->tag.n);
        }
        jw_obj_end(w);
    } else jw_null(w);
    jw_key(w, "implicit"), jw_arr_begin(w);
    for (size_t i = 0; i < m->nstoreys; i++) { /* levels in id order (the page sorts by elevation) */
        const cc_storey *st = &m->storeys[i];
        jw_obj_begin(w);
        jw_key(w, "elevation"), jw_f64(w, st->elevation + 0.0);
        jw_key(w, "id"), jw_str_n(w, st->id.c, 22);
        jw_key(w, "kind"), jw_str(w, "level");
        jw_key(w, "name"), jw_str_n(w, st->name.n ? st->name.p : "", st->name.n);
        jw_obj_end(w);
    }
    for (size_t i = 0; i < m->ngrids; i++)
        for (size_t k = 0; k < m->grids[i].naxes; k++) {
            const cc_grid_axis *a = &m->grids[i].axes[k];
            jw_obj_begin(w);
            jw_key(w, "end"), v2(w, a->end);
            jw_key(w, "id"), jw_str_n(w, a->id.c, 22);
            jw_key(w, "kind"), jw_str(w, "axis");
            jw_key(w, "name"), jw_str_n(w, a->tag.n ? a->tag.p : "", a->tag.n);
            jw_key(w, "start"), v2(w, a->start);
            jw_obj_end(w);
        }
    jw_arr_end(w);
    jw_key(w, "pick"), jw_obj_begin(w);
    jw_key(w, "on"), jw_bool(w, wp->pick);
    jw_key(w, "points"), jw_arr_begin(w);
    for (int i = 0; wp->pick && i < wp->npick; i++) v3(w, wp->pts[i]);
    jw_arr_end(w);
    jw_obj_end(w);
    jw_key(w, "planes"), jw_arr_begin(w);
    for (size_t i = 0; i < m->nelements; i++) {
        const cc_element *e = &m->elements[i];
        if (e->cls != CC_IFC_VIRTUAL_ELEMENT) continue;
        double elev = storey_elev(m, &e->storey), c[4][3], fo[3], fn[3], fx[3], fy[3], pw, ph;
        cc_refplane_corners(e, elev, c);
        cc_refplane_frame(e, elev, fo, fn, fx, fy);
        cc_refplane_size(e, &pw, &ph);
        jw_obj_begin(w);
        jw_key(w, "corners"), jw_arr_begin(w);
        for (int q = 0; q < 4; q++) v3(w, c[q]);
        jw_arr_end(w);
        jw_key(w, "height"), jw_f64(w, ph);
        jw_key(w, "id"), jw_str_n(w, e->id.c, 22);
        jw_key(w, "name"), jw_str_n(w, e->name.n ? e->name.p : "", e->name.n);
        jw_key(w, "normal"), v3(w, fn);
        jw_key(w, "origin"), v3(w, fo);
        jw_key(w, "storey"), jw_str_n(w, e->storey.c, 22);
        jw_key(w, "width"), jw_f64(w, pw);
        jw_key(w, "x_axis"), v3(w, fx);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_obj_end(w);
}
