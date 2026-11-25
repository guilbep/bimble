/* viewdraw.rs of bimble-drawing: see draw_view.h. */
#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */
#include "draw_view.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "arena.h"
#include "draw_style.h" /* cc_category_label */
#include "georef.h"
#include "ifc_math.h" /* cc_sin, cc_cos */
#include "view_range.h"

static double fmin_rs(double a, double b) { return a != a ? b : b != b ? a : a < b ? a : b; }
static double fmax_rs(double a, double b) { return a != a ? b : b != b ? a : a > b ? a : b; }
static float fmin32(float a, float b) { return a != a ? b : b != b ? a : a < b ? a : b; }
static float fmax32(float a, float b) { return a != a ? b : b != b ? a : a > b ? a : b; }

/* ---- Turn and ViewFrame ---- */

/* Facing::dir_turned (as draw_annot.c's, which keeps it to itself). */
static void dir_turned(cc_facing f, double a, double out[2]) {
    double x = f == CC_FACING_EAST ? 1.0 : f == CC_FACING_WEST ? -1.0 : 0.0;
    double y = f == CC_FACING_NORTH ? 1.0 : f == CC_FACING_SOUTH ? -1.0 : 0.0;
    double s = cc_sin(a), c = cc_cos(a);
    out[0] = c * x - s * y + 0.0;
    out[1] = s * x + c * y + 0.0;
}

void cc_view_facing_dir(const cc_view *v, cc_facing f, const cc_georef *g, double out[2]) {
    dir_turned(f, cc_georef_plan_turn(g, v->true_north), out);
}

void cc_turn_of(const cc_view *v, const cc_georef *g, cc_turn *out) {
    memset(out, 0, sizeof *out);
    double a = cc_georef_plan_turn(g, v->true_north);
    if (v->kind == CC_VIEW_PLAN && v->look == CC_LOOK_UP) {
        out->kind = CC_TURN_REFLECT;
        out->a = a;
    } else if (v->kind == CC_VIEW_SECTION) {
        out->kind = CC_TURN_SECTION;
        dir_turned(v->facing, a, out->d);
    } else {
        out->kind = CC_TURN_PLAN;
        out->a = a;
    }
}

/* rotate: p turned by a radians counter-clockwise about the vertical (0: unchanged, exactly). */
static void rotate(const double p[3], double a, double out[3]) {
    if (a == 0.0) {
        out[0] = p[0], out[1] = p[1], out[2] = p[2];
        return;
    }
    double s = cc_sin(a), c = cc_cos(a);
    double x = c * p[0] - s * p[1], y = s * p[0] + c * p[1];
    out[0] = x, out[1] = y, out[2] = p[2];
}

void cc_turn_invert(const cc_turn *t, const double q[3], double out[3]) {
    if (t->kind == CC_TURN_SECTION) {
        double dx = t->d[0], dy = t->d[1];
        double x = q[0] * dy - q[2] * dx, y = -q[0] * dx - q[2] * dy, z = q[1];
        out[0] = x, out[1] = y, out[2] = z;
    } else if (t->kind == CC_TURN_REFLECT) {
        double r[3] = {q[0], q[1], -q[2]};
        rotate(r, t->a, out);
    } else {
        double r[3] = {q[0], q[1], q[2]};
        rotate(r, t->a, out);
    }
}

void cc_view_frame_to_world(const cc_view_frame *f, const double paper[2], double out[3]) {
    double q[3] = {f->x0 + (paper[0] - f->ox) / f->k, f->y1 - (paper[1] - f->oy) / f->k, f->cut}, p[3];
    cc_turn_invert(&f->turn, q, p);
    out[0] = p[0] + f->origin[0];
    out[1] = p[1] + f->origin[1];
    out[2] = p[2] + f->origin[2];
}

cc_view_frame cc_view_frame_shifted(cc_view_frame f, const double d[2]) {
    f.ox += d[0];
    f.oy += d[1];
    return f;
}

/* str::trim().is_empty(): nothing but Unicode White_Space in the UTF-8 text. */
int cc_view_name_blank(cc_str name) {
    const unsigned char *p = (const unsigned char *)name.p;
    size_t i = 0, n = name.n;
    while (i < n) {
        unsigned c = p[i], cp;
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if (i + len > n) return 0;
        if (len == 1) cp = c;
        else if (len == 2) cp = (c & 0x1Fu) << 6 | (p[i + 1] & 0x3Fu);
        else if (len == 3) cp = (c & 0x0Fu) << 12 | (p[i + 1] & 0x3Fu) << 6 | (p[i + 2] & 0x3Fu);
        else return 0; /* no white space past the BMP */
        int ws = (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
                 (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
                 cp == 0x3000;
        if (!ws) return 0;
        i += len;
    }
    return 1;
}

/* ---- what a view keeps ---- */

/* SceneElement::bounds (Aabb): of its own mesh's points, or of the 8 corners of its shape's box
 * placed (f32). 0 when empty. */
static int elem_bounds(const cc_scene *s, size_t k, float lo[3], float hi[3]) {
    const cc_scene_element *e = &s->elements[k];
    const cc_mesh *m = e->shape >= 0 ? &s->shapes[e->shape] : &e->mesh;
    for (int j = 0; j < 3; j++) lo[j] = 1.0f / 0.0f, hi[j] = -1.0f / 0.0f;
    for (uint32_t i = 0; i < m->nv; i++)
        for (int j = 0; j < 3; j++) lo[j] = fmin32(lo[j], m->pos[i][j]), hi[j] = fmax32(hi[j], m->pos[i][j]);
    for (int j = 0; j < 3; j++)
        if (lo[j] > hi[j]) return 0;
    if (e->shape < 0) return 1;
    const cc_placement32 *p = &e->to_scene;
    float l[3] = {1.0f / 0.0f, 1.0f / 0.0f, 1.0f / 0.0f}, h[3] = {-1.0f / 0.0f, -1.0f / 0.0f, -1.0f / 0.0f};
    for (int i = 0; i < 8; i++) {
        float c[3];
        for (int j = 0; j < 3; j++) c[j] = (i >> j & 1) == 0 ? lo[j] : hi[j];
        for (int j = 0; j < 3; j++) {
            float r = p->x[j] * c[0] + p->y[j] * c[1] + p->z[j] * c[2] + p->t[j];
            l[j] = fmin32(l[j], r), h[j] = fmax32(h[j], r);
        }
    }
    memcpy(lo, l, sizeof l);
    memcpy(hi, h, sizeof h);
    return 1;
}

int cc_view_within_heights(const cc_scene *s, size_t k, double lo, double hi) {
    float bl[3], bh[3];
    if (!elem_bounds(s, k, bl, bh)) return 1;
    double oz = s->origin[2];
    double zl = (double)bl[2] + oz, zh = (double)bh[2] + oz;
    return zh >= lo - 1.0 && zl <= hi + 1.0;
}

/* A set of strings (a view's visibility list): sorted by bytes, searched. */
typedef struct {
    cc_str *v;
    size_t n;
} Set;

static int str_cmp(const void *x, const void *y) {
    const cc_str *a = x, *b = y;
    size_t n = a->n < b->n ? a->n : b->n;
    int c = n ? memcmp(a->p, b->p, n) : 0;
    return c ? c : (a->n > b->n) - (a->n < b->n);
}

static int set_of(const cc_strs *l, Set *s) {
    s->n = l->n;
    s->v = malloc((l->n ? l->n : 1) * sizeof *s->v);
    if (!s->v) return -1;
    if (l->n) memcpy(s->v, l->v, l->n * sizeof *s->v);
    if (l->n > 1) qsort(s->v, l->n, sizeof *s->v, str_cmp);
    return 0;
}

static int set_has(const Set *s, const char *p, size_t n) {
    cc_str key = {p, n};
    size_t lo = 0, hi = s->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = str_cmp(&key, &s->v[mid]);
        if (!c) return 1;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return 0;
}

/* viewdraw::Shown: what a view shows (its visibility lists). */
typedef struct {
    Set families, classes, storeys, materials, elements, isolated;
} Shown;

static void shown_free(Shown *sh) {
    free(sh->families.v), free(sh->classes.v), free(sh->storeys.v);
    free(sh->materials.v), free(sh->elements.v), free(sh->isolated.v);
    memset(sh, 0, sizeof *sh);
}

static int shown_of(const cc_visibility *v, Shown *sh) {
    memset(sh, 0, sizeof *sh);
    if (set_of(&v->hidden_families, &sh->families) || set_of(&v->hidden_classes, &sh->classes) ||
        set_of(&v->hidden_storeys, &sh->storeys) || set_of(&v->hidden_materials, &sh->materials) ||
        set_of(&v->hidden_elements, &sh->elements) || set_of(&v->isolated_elements, &sh->isolated)) {
        shown_free(sh);
        return -1;
    }
    return 0;
}

static int shown_keeps(const Shown *sh, const cc_model *m, const cc_scene_element *e) {
    const char *fam = cc_category_label(e->category);
    if (set_has(&sh->families, fam, strlen(fam))) return 0;
    if (set_has(&sh->classes, e->class_name.p, e->class_name.n)) return 0;
    const cc_element *me = &m->elements[e->model_index];
    const cc_storey *st = cc_model_storey(m, &me->storey);
    if (st && set_has(&sh->storeys, st->name.p, st->name.n)) return 0;
    const cc_material *mt = cc_model_material(m, &me->material);
    if (set_has(&sh->materials, mt ? mt->name.p : "", mt ? mt->name.n : 0)) return 0;
    if (set_has(&sh->elements, e->id.c, 22)) return 0;
    return !sh->isolated.n || set_has(&sh->isolated, e->id.c, 22);
}

static int visibility_empty(const cc_visibility *v) {
    return !v->hidden_families.n && !v->hidden_classes.n && !v->hidden_storeys.n && !v->hidden_links.n &&
           !v->hidden_materials.n && !v->hidden_elements.n && !v->isolated_elements.n;
}

/* ---- map_scene ---- */

static void map_point(const cc_turn *t, const float p[3], float out[3]) {
    double q[3] = {(double)p[0], (double)p[1], (double)p[2]}, r[3];
    cc_turn_apply_point(t, q, r);
    out[0] = (float)r[0], out[1] = (float)r[1], out[2] = (float)r[2];
}

/* f32::round (half away from zero). */
static float round32(float x) {
    float a = x < 0 ? -x : x;
    if (!(a < 8388608.0f)) return x;
    float t = (float)(int32_t)x, d = x - t;
    if (d >= 0.5f) t += 1.0f;
    else if (d <= -0.5f) t -= 1.0f;
    return t;
}

/* lib.rs pack_normal. */
static int8_t q8(float c) {
    if (c != c) return 0; /* `NaN as i8` */
    float x = c < -1.0f ? -1.0f : c > 1.0f ? 1.0f : c;
    return (int8_t)round32(x * 127.0f);
}

int cc_view_map_scene(const cc_scene *s, cc_view_keep keep, const void *ctx, const cc_turn *turn, int reflect,
                      cc_scene *out) {
    memset(out, 0, sizeof *out);
    arena_init(&out->arena, 0);
    memcpy(out->origin, s->origin, sizeof out->origin);
    Arena *a = &out->arena;
    out->elements = arena_alloc(a, (s->nelements ? s->nelements : 1) * sizeof *out->elements, _Alignof(cc_scene_element));
    if (!out->elements) return -1;
    Arena tmp;
    arena_init(&tmp, 0);
    int rc = 0;
    for (size_t k = 0; k < s->nelements && !rc; k++) {
        if (!keep(ctx, s, k)) continue;
        arena_reset(&tmp);
        cc_mesh w, m;
        if (cc_scene_world_mesh(s, k, &tmp, &w)) {
            rc = -1;
            break;
        }
        /* The view's transform applies to world triangles: an instance becomes its own. */
        m = w;
        m.pos = arena_alloc(a, ((size_t)w.nv + 1) * sizeof m.pos[0], 4);
        m.idx = arena_alloc(a, ((size_t)w.ni + 1) * sizeof(uint32_t), 4);
        m.edges = arena_alloc(a, ((size_t)w.ne + 1) * sizeof m.edges[0], 4);
        m.normals = arena_alloc(a, ((size_t)w.nn + 1) * sizeof m.normals[0], 1);
        if (!m.pos || !m.idx || !m.edges || !m.normals) {
            rc = -1;
            break;
        }
        for (uint32_t i = 0; i < w.nv; i++) map_point(turn, w.pos[i], m.pos[i]);
        if (w.ni) memcpy(m.idx, w.idx, w.ni * sizeof(uint32_t));
        if (reflect) /* so triangles keep facing outwards */
            for (uint32_t t = 0; t + 2 < w.ni; t += 3) {
                uint32_t x = m.idx[t + 1];
                m.idx[t + 1] = m.idx[t + 2];
                m.idx[t + 2] = x;
            }
        for (uint32_t i = 0; i < w.ne; i++) {
            map_point(turn, w.edges[i], m.edges[i]);
            map_point(turn, w.edges[i] + 3, m.edges[i] + 3);
        }
        for (uint32_t i = 0; i < w.nn; i++)
            for (int f = 0; f < 2; f++) {
                const int8_t *n = w.normals[i] + 4 * f;
                int8_t *o = m.normals[i] + 4 * f;
                if (!n[0] && !n[1] && !n[2]) { /* unknown (zero) stays unknown */
                    memcpy(o, n, 4);
                    continue;
                }
                float u[3] = {(float)n[0] / 127.0f, (float)n[1] / 127.0f, (float)n[2] / 127.0f}, v[3];
                map_point(turn, u, v);
                o[0] = q8(v[0]), o[1] = q8(v[1]), o[2] = q8(v[2]), o[3] = 0;
            }
        cc_scene_element *e = &out->elements[out->nelements++];
        *e = s->elements[k];
        e->shape = -1;
        e->mesh = m;
    }
    arena_free(&tmp);
    return rc;
}

/* ---- setup ---- */

typedef struct {
    const cc_model *m;
    const Shown *shown;
    int has_range;
    double lo, hi;   /* world mm (plans) */
    int section;
    double d[2], cut, level; /* sections: facing, the cut and the depth in the turned scene */
} Keep;

static int keep_view(const void *ctx, const cc_scene *s, size_t k) {
    const Keep *kp = ctx;
    if (!shown_keeps(kp->shown, kp->m, &s->elements[k])) return 0;
    if (kp->has_range && !cc_view_within_heights(s, k, kp->lo, kp->hi)) return 0;
    if (kp->section) {
        /* From the cut on along its facing, to its depth. */
        float bl[3], bh[3];
        if (!elem_bounds(s, k, bl, bh)) return 1;
        double lo = 1.0 / 0.0, hi = -1.0 / 0.0;
        for (int c = 0; c < 4; c++) {
            double x = (double)((c & 1) == 0 ? bl[0] : bh[0]), y = (double)((c & 2) == 0 ? bl[1] : bh[1]);
            double z = -(kp->d[0] * x + kp->d[1] * y); /* height in the turned scene: towards the viewer */
            lo = fmin_rs(lo, z);
            hi = fmax_rs(hi, z);
        }
        return lo <= kp->cut + 1.0 && hi >= kp->level - 1.0;
    }
    return 1;
}

static void view_name(Buf *err, const cc_view *v) {
    buf_puts(err, "view ");
    buf_append(err, v->name.p, v->name.n);
}

static int64_t tkey(double x) { /* f64::total_cmp */
    int64_t k;
    memcpy(&k, &x, 8);
    return k ^ (int64_t)((uint64_t)(k >> 63) >> 1);
}

/* The model's storeys by elevation (stable): the first, and the one with id `key`. */
static const cc_storey *storey_by(const cc_model *m, cc_str key, int first) {
    const cc_storey *best = NULL;
    for (size_t i = 0; i < m->nstoreys; i++) {
        const cc_storey *st = &m->storeys[i];
        if (first) {
            if (!best || tkey(st->elevation) < tkey(best->elevation)) best = st; /* the first of equals: stable */
        } else if (key.n == 22 && !memcmp(st->id.c, key.p, 22)) {
            return st;
        }
    }
    return best;
}

int cc_view_prepare(const cc_scene *s, const cc_model *m, const cc_view *v, cc_view_setup *out, Buf *err) {
    memset(out, 0, sizeof *out);
    out->scene = s;
    double oz = s->origin[2];
    cc_turn_of(v, &m->georef, &out->turn);
    if (v->kind == CC_VIEW_THREE_D) {
        view_name(err, v);
        buf_puts(err, ": a 3D view has no paper drawing");
        return 1;
    }
    Shown shown;
    if (shown_of(&v->visibility, &shown)) return -1;
    Keep kp;
    memset(&kp, 0, sizeof kp);
    kp.m = m;
    kp.shown = &shown;
    int rc = 0;
    if (v->kind == CC_VIEW_PLAN) {
        const cc_storey *st = storey_by(m, v->storey, 0);
        if (!st) {
            view_name(err, v);
            buf_puts(err, ": no storey ");
            buf_append(err, v->storey.p, v->storey.n);
            buf_puts(err, " in this model");
            rc = 1;
            goto end;
        }
        double r[4];
        rc = cc_view_resolve(m, v, r, err);
        if (rc) goto end;
        double top = r[CC_RANGE_TOP], cut = r[CC_RANGE_CUT], depth = r[CC_RANGE_DEPTH];
        /* What the drawing can show: an element wholly outside the view's heights is left out. */
        kp.has_range = 1;
        if (v->look == CC_LOOK_DOWN) {
            kp.lo = fmin_rs(depth, cut);
            kp.hi = fmax_rs(top, cut);
        } else {
            double lo = cut, hi = depth; /* ResolvedRange::visible(Up) */
            kp.lo = fmin_rs(lo, hi);
            kp.hi = fmax_rs(hi, lo);
        }
        out->storey_name = st->name;
        out->storey_elevation_mm = st->elevation;
        out->axes = 1;
        out->grids = 1;
        if (v->look == CC_LOOK_DOWN) {
            if (!(visibility_empty(&v->visibility) && out->turn.kind == CC_TURN_PLAN && out->turn.a == 0.0)) {
                out->mapped = 1;
                rc = cc_view_map_scene(s, keep_view, &kp, &out->turn, 0, &out->own);
                if (rc) goto end;
            }
            out->level = depth - oz;
            out->cut = cut - oz;
            out->next = top - oz;
            out->has_storey = 1;
            out->storey = st->name;
            out->plan_down = 1;
        } else {
            /* Reflected: what is higher is nearer the viewer below. */
            out->mapped = 1;
            rc = cc_view_map_scene(s, keep_view, &kp, &out->turn, 1, &out->own);
            if (rc) goto end;
            out->level = -(depth - oz);
            out->cut = -(cut - oz);
            out->next = -(cut - oz);
        }
    } else {
        /* Its facing in project north (or true north): along the building's grid. */
        double d[2];
        cc_view_facing_dir(v, v->facing, &m->georef, d);
        double o[2] = {v->origin[0] - s->origin[0], v->origin[1] - s->origin[1]};
        double cut = -(d[0] * o[0] + d[1] * o[1]);
        double level = v->depth_mm > 0.0 ? cut - v->depth_mm : -1.0 / 0.0;
        kp.section = 1;
        kp.d[0] = d[0], kp.d[1] = d[1];
        kp.cut = cut;
        kp.level = level;
        out->mapped = 1;
        rc = cc_view_map_scene(s, keep_view, &kp, &out->turn, 0, &out->own);
        if (rc) goto end;
        cc_str none = {"", 0};
        const cc_storey *first = storey_by(m, none, 1);
        out->storey_name = first ? first->name : none;
        out->storey_elevation_mm = first ? first->elevation : 0.0;
        out->level = level;
        out->cut = cut;
        out->next = cut;
    }
    if (out->mapped) out->scene = &out->own;
end:
    shown_free(&shown);
    return rc;
}

static int keep_within(const void *ctx, const cc_scene *s, size_t k) {
    const Keep *kp = ctx;
    return cc_view_within_heights(s, k, kp->lo, kp->hi);
}

int cc_view_storey_turned(const cc_scene *s, double lo, double hi, cc_view_setup *su) {
    Keep kp;
    memset(&kp, 0, sizeof kp);
    kp.lo = lo, kp.hi = hi;
    su->mapped = 1;
    su->grids = 1;
    int rc = cc_view_map_scene(s, keep_within, &kp, &su->turn, 0, &su->own);
    su->scene = &su->own;
    return rc;
}

void cc_view_setup_free(cc_view_setup *su) {
    if (su->mapped) cc_scene_free(&su->own);
    su->mapped = 0;
    su->scene = NULL;
}

void cc_view_turn_axes(const cc_view_setup *su, cc_plan_axis *axes, size_t n) {
    if (!su->mapped || !su->grids) return;
    const double *o = su->scene->origin;
    for (size_t i = 0; i < n; i++) {
        double *ends[2] = {axes[i].a, axes[i].b};
        for (int j = 0; j < 2; j++) {
            double p[3] = {ends[j][0], ends[j][1], 0.0}, q[3];
            cc_turn_apply_point(&su->turn, p, q);
            double wx = q[0] + o[0], wy = q[1] + o[1]; /* the turned grid, world mm */
            ends[j][0] = wx - o[0];
            ends[j][1] = wy - o[1];
        }
    }
}

int cc_view_crop_rect(const cc_view *v, const cc_turn *turn, const double origin[3], cc_crop_rect *out) {
    if (!v || !v->has_crop) return 0;
    double t[3];
    cc_turn_apply_point(turn, origin, t);
    out->lo[0] = v->crop_min[0] - t[0];
    out->lo[1] = v->crop_min[1] - t[1];
    out->hi[0] = v->crop_max[0] - t[0];
    out->hi[1] = v->crop_max[1] - t[1];
    return 1;
}
