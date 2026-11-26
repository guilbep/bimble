/* The measure tool: see measure.h. */
#include "measure.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "fmt.h"
#include "json_r.h"
#include "math.h"

#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust (glam's f32 dot) */

#define PARALLEL_COS 0.99985f /* measure.rs: cos(1 degree) */
#define BOX_EPS 0.5           /* mm: a snapped point on the box's face is inside it */
#define FAR 1e300             /* "infinitely" far, in pixels or mm */

void cc_measure_free(cc_measure *m) {
    free(m->dims);
    memset(m, 0, sizeof *m);
}

void cc_measure_reset(cc_measure *m) { m->n = 0, m->has_first = 0, m->has_hover = 0; }

/* p of `s` in the frame of origin `org` (f32; the same point when the origins are equal). */
static void in_frame(const cc_msnap *s, const double org[3], float out[3]) {
    for (int k = 0; k < 3; k++)
        out[k] = s->org[k] == org[k] ? s->p[k] : (float)((double)s->p[k] + (s->org[k] - org[k]));
}

/* glam Vec3::length of b - a, each step rounded to f32 (no contraction: one operation each). */
static float length3(const float a[3], const float b[3]) {
    float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    float xx = dx * dx, yy = dy * dy, zz = dz * dz;
    float s = xx + yy;
    s = s + zz;
    return cc_sqrtf(s);
}

float cc_mdim_distance(const cc_mdim *d) {
    float b[3];
    in_frame(&d->b, d->a.org, b);
    return length3(d->a.p, b);
}

int cc_mdim_perpendicular(const cc_mdim *d, float *out) {
    if (!d->a.has_n || !d->b.has_n) return 0;
    const float *n1 = d->a.n, *n2 = d->b.n;
    float c = n1[0] * n2[0];
    c = c + n1[1] * n2[1];
    c = c + n1[2] * n2[2];
    if (!((c < 0 ? -c : c) >= PARALLEL_COS)) return 0;
    float b[3];
    in_frame(&d->b, d->a.org, b);
    float dx = b[0] - d->a.p[0], dy = b[1] - d->a.p[1], dz = b[2] - d->a.p[2];
    float t = dx * n1[0];
    t = t + dy * n1[1];
    t = t + dz * n1[2];
    *out = t < 0 ? -t : t;
    return 1;
}

/* ---- format_mm ---- */

void cc_format_mm(Buf *b, float x) {
    Buf s;
    buf_init(&s);
    double v = (double)x; /* exact */
    if (v != v) buf_puts(&s, "NaN"); /* format!("{:.1}", NaN) */
    else if (v >= 100.0 || v <= -100.0) { /* format!("{}", x.round() as i64): saturating */
        double a = v < 0 ? -v : v, r = a + 0.5;
        int64_t i;
        if (a >= 9223372036854775808.0) i = v < 0 ? INT64_MIN : INT64_MAX;
        else {
            /* floor(a + 0.5): f32::round, half away from zero (a + 0.5 is exact in f64 below 2^52) */
            uint64_t u = a >= 4503599627370496.0 ? (uint64_t)a : (uint64_t)r;
            i = v < 0 ? -(int64_t)u : (int64_t)u;
        }
        buf_i64(&s, i);
    } else fmt_f64_fixed(&s, v, 1);
    const char *t = buf_cstr(&s);
    size_t n = s.len;
    size_t ni = 0;
    while (ni < n && t[ni] != '.') ni++;
    const char *dot = ni < n ? t + ni : NULL;
    const char *dig = t;
    size_t nd = ni;
    if (nd && dig[0] == '-') buf_putc(b, '-'), dig++, nd--;
    for (size_t k = 0; k < nd; k++) {
        if (k > 0 && (nd - k) % 3 == 0) buf_puts(b, "\xe2\x80\x89"); /* U+2009 thin space */
        buf_putc(b, dig[k]);
    }
    if (dot) buf_append(b, dot, n - ni);
    buf_puts(b, " mm");
    if (s.oom) b->oom = 1;
    buf_free(&s);
}

static void label_json(JsonW *w, size_t k, float mm) {
    Buf t;
    buf_init(&t);
    buf_u64(&t, (uint64_t)k + 1);
    buf_puts(&t, ". ");
    cc_format_mm(&t, mm);
    if (t.oom) w->b->oom = 1;
    jw_str_n(w, t.p ? t.p : "", t.len);
    buf_free(&t);
}

void cc_measurements_json(JsonW *w, const cc_mdim *d, size_t n) { /* state.rs measurements_json */
    jw_arr_begin(w);
    for (size_t k = 0; k < n; k++) {
        float mm = cc_mdim_distance(&d[k]);
        jw_obj_begin(w);
        jw_key(w, "id"), jw_u64(w, k);
        jw_key(w, "label"), label_json(w, k, mm);
        jw_key(w, "value_mm"), jw_f64(w, (double)mm);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

/* ---- the pick and the snap ---- */

static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static int unit_ray(const double in[6], double o[3], double d[3]) {
    double l = cc_sqrt(dot3(in + 3, in + 3));
    if (!(l > 1e-12)) return 0;
    for (int k = 0; k < 3; k++) o[k] = in[k], d[k] = in[3 + k] / l;
    return 1;
}

static int inside(const cc_mview *v, const double p[3], double eps) {
    if (!v->clip_min || !v->clip_max) return 1;
    for (int k = 0; k < 3; k++)
        if (!(p[k] >= (double)v->clip_min[k] - eps && p[k] <= (double)v->clip_max[k] + eps)) return 0;
    return 1;
}

typedef struct {
    double o[3], d[3];        /* the pointer ray (unit), shown scene coordinates */
    double o2[3], d2[3];      /* ray_tol */
    int has_tol;
    double tol_px;
} Ray;

/* Pixels between the point `p` (shown scene coordinates) and the ray, at p's depth; INFINITY when
 * it is behind the eye or beyond `max_depth` (measure.rs `visible`). */
static double screen_dist(const Ray *r, const double p[3], double max_depth) {
    double w[3] = {p[0] - r->o[0], p[1] - r->o[1], p[2] - r->o[2]};
    double t = dot3(w, r->d);
    if (!(t <= max_depth) || !(t > 0)) return FAR;
    double q[3], g[3];
    for (int k = 0; k < 3; k++) q[k] = w[k] - t * r->d[k], g[k] = (r->o2[k] + t * r->d2[k]) - (r->o[k] + t * r->d[k]);
    double gap = cc_sqrt(dot3(g, g));
    if (!(gap > 1e-9)) return FAR;
    return cc_sqrt(dot3(q, q)) / gap * r->tol_px;
}

/* measure.rs closest_on_segment_to_ray (in f64). */
static void closest_on_segment(const double a[3], const double b[3], const double o[3], const double d[3], double out[3]) {
    double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {a[0] - o[0], a[1] - o[1], a[2] - o[2]};
    double uu = dot3(u, u), ud = dot3(u, d), dd = dot3(d, d), uw = dot3(u, w), dw = dot3(d, w);
    double den = uu * dd - ud * ud, s = 0.0;
    double lim = 1e-9 * (uu > 1.0 ? uu : 1.0);
    if (!((den < 0 ? -den : den) < lim)) {
        s = (ud * dw - dd * uw) / den;
        s = s < 0 ? 0 : s > 1 ? 1 : s;
    }
    for (int k = 0; k < 3; k++) out[k] = a[k] + u[k] * s;
}

typedef struct {
    size_t layer, el;
    double t; /* its nearest hit along the ray */
} Cand;

typedef struct {
    int found;
    double t;    /* the nearest hit along the ray */
    double n[3]; /* its triangle's unit normal */
    Cand *c;     /* every element the ray meets (inside the box), in the arena */
    size_t nc, cap;
} Hits;

/* The triangles of one layer's visible elements the ray meets inside the box (Möller–Trumbore,
 * two-sided, in f64): the nearest overall, and each element's nearest. */
static int pick_layer(const cc_mview *v, size_t li, const Ray *r, Arena *a, Arena *keep, Hits *h) {
    const cc_mlayer *L = &v->layers[li];
    const cc_scene *sc = L->sc;
    double o[3] = {r->o[0] - L->off[0], r->o[1] - L->off[1], r->o[2] - L->off[2]};
    for (size_t k = 0; k < sc->nelements; k++) {
        if (L->vis && !L->vis->reveal && cc_vis_hidden(L->vis, &sc->elements[k].id)) continue;
        cc_mesh m;
        arena_reset(a); /* the previous element's placed mesh */
        if (cc_scene_world_mesh(sc, k, a, &m)) return -1;
        double tmin = FAR;
        for (uint32_t i = 0; i + 2 < m.ni; i += 3) {
            if (m.idx[i] >= m.nv || m.idx[i + 1] >= m.nv || m.idx[i + 2] >= m.nv) continue;
            const float *A = m.pos[m.idx[i]], *B = m.pos[m.idx[i + 1]], *C = m.pos[m.idx[i + 2]];
            double e1[3], e2[3], p[3], tv[3], q[3];
            for (int c = 0; c < 3; c++) e1[c] = (double)B[c] - A[c], e2[c] = (double)C[c] - A[c], tv[c] = o[c] - A[c];
            p[0] = r->d[1] * e2[2] - r->d[2] * e2[1], p[1] = r->d[2] * e2[0] - r->d[0] * e2[2], p[2] = r->d[0] * e2[1] - r->d[1] * e2[0];
            double det = dot3(e1, p);
            if (!(det > 1e-12 || det < -1e-12)) continue;
            double inv = 1.0 / det, uu = dot3(tv, p) * inv;
            if (uu < 0 || uu > 1) continue;
            q[0] = tv[1] * e1[2] - tv[2] * e1[1], q[1] = tv[2] * e1[0] - tv[0] * e1[2], q[2] = tv[0] * e1[1] - tv[1] * e1[0];
            double vv = dot3(r->d, q) * inv;
            if (vv < 0 || uu + vv > 1) continue;
            double t = dot3(e2, q) * inv;
            if (!(t > 0) || !(t < tmin)) continue;
            double at[3] = {r->o[0] + t * r->d[0], r->o[1] + t * r->d[1], r->o[2] + t * r->d[2]};
            if (!inside(v, at, BOX_EPS)) continue; /* ADR 0014: what the box removes is not hit */
            double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            double l = cc_sqrt(dot3(n, n));
            if (!(l > 0)) continue;
            tmin = t;
            if (!h->found || t < h->t) {
                h->found = 1, h->t = t;
                for (int c = 0; c < 3; c++) h->n[c] = n[c] / l;
            }
        }
        if (tmin == FAR) continue;
        if (h->nc == h->cap) {
            size_t cap = h->cap ? 2 * h->cap : 64;
            Cand *c = arena_alloc(keep, cap * sizeof *c, sizeof(double));
            if (!c) return -1;
            if (h->nc) memcpy(c, h->c, h->nc * sizeof *c);
            h->c = c, h->cap = cap;
        }
        h->c[h->nc].layer = li, h->c[h->nc].el = k, h->c[h->nc].t = tmin, h->nc++;
    }
    return 0;
}

int cc_measure_snap(const cc_mview *v, const double ray[6], const double *ray_tol, double tol_px, int free_, cc_msnap *out) {
    Ray r;
    memset(&r, 0, sizeof r);
    if (!v || !v->nlayers || !unit_ray(ray, r.o, r.d)) return 0;
    r.has_tol = ray_tol && unit_ray(ray_tol, r.o2, r.d2) && tol_px > 0;
    r.tol_px = tol_px;
    Arena a, keep;
    arena_init(&a, 0), arena_init(&keep, 0);
    Hits h;
    memset(&h, 0, sizeof h);
    int rc = 0;
    for (size_t li = 0; li < v->nlayers; li++) {
        if (pick_layer(v, li, &r, &a, &keep, &h)) {
            rc = -1;
            goto end;
        }
        arena_reset(&a);
    }
    if (!h.found) goto end;
    memset(out, 0, sizeof *out);
    memcpy(out->org, v->org, sizeof out->org);
    int kind = CC_MSNAP_FACE;
    double best = FAR, bp[3];
    /* measure.rs snap: a corner, else an edge point, of what is under the cursor: the elements the
     * ray meets within the visible depth (Rust: the element hit only; a column standing on a slab
     * meets it at the same depth, either may be "the" hit). */
    double max_depth = h.t + (h.t * 0.002 > 5.0 ? h.t * 0.002 : 5.0);
    for (int pass = 0; !free_ && r.has_tol && pass < 2 && kind == CC_MSNAP_FACE; pass++)
        for (size_t c = 0; c < h.nc; c++) {
            if (!(h.c[c].t <= max_depth)) continue;
            const cc_mlayer *L = &v->layers[h.c[c].layer];
            cc_mesh m;
            if (cc_scene_world_mesh(L->sc, h.c[c].el, &a, &m)) {
                rc = -1;
                goto end;
            }
            if (pass == 0)
                for (uint32_t i = 0; i < m.nv; i++) {
                    double p[3] = {(double)(m.pos[i][0] + L->off[0]), (double)(m.pos[i][1] + L->off[1]), (double)(m.pos[i][2] + L->off[2])};
                    double d = screen_dist(&r, p, max_depth);
                    if (d <= CC_MEASURE_VERTEX_PX && d < best && inside(v, p, BOX_EPS)) best = d, memcpy(bp, p, sizeof bp), kind = CC_MSNAP_VERTEX;
                }
            else
                for (uint32_t i = 0; i < m.ne; i++) {
                    double ea[3], eb[3], p[3];
                    for (int k = 0; k < 3; k++) ea[k] = (double)(m.edges[i][k] + L->off[k]), eb[k] = (double)(m.edges[i][3 + k] + L->off[k]);
                    closest_on_segment(ea, eb, r.o, r.d, p);
                    double d = screen_dist(&r, p, max_depth);
                    if (d <= CC_MEASURE_EDGE_PX && d < best && inside(v, p, BOX_EPS)) best = d, memcpy(bp, p, sizeof bp), kind = CC_MSNAP_EDGE;
                }
            arena_reset(&a);
        }
    out->kind = kind;
    if (kind == CC_MSNAP_FACE) {
        for (int k = 0; k < 3; k++) out->p[k] = (float)(r.o[k] + h.t * r.d[k]), out->n[k] = (float)h.n[k];
        out->has_n = 1;
    } else
        for (int k = 0; k < 3; k++) out->p[k] = (float)bp[k];
    rc = 1;
end:
    arena_free(&a), arena_free(&keep);
    return rc;
}

void cc_measure_orthogonal(const cc_msnap *from, cc_msnap *to) { /* measure.rs orthogonal */
    float f[3];
    in_frame(from, to->org, f);
    float d[3] = {to->p[0] - f[0], to->p[1] - f[1], to->p[2] - f[2]};
    float a[3] = {d[0] < 0 ? -d[0] : d[0], d[1] < 0 ? -d[1] : d[1], d[2] < 0 ? -d[2] : d[2]};
    int axis = a[0] >= a[1] && a[0] >= a[2] ? 0 : a[1] >= a[2] ? 1 : 2;
    for (int k = 0; k < 3; k++) to->p[k] = k == axis ? f[k] + d[k] : f[k];
    to->has_n = 0;
}

/* ---- the calls ---- */

static const char *const NAMES[] = {"measure_pointer", "measure_click", "measure_cancel", "measure_clear"};

int cc_measure_name(const char *name) {
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        if (!strcmp(name, NAMES[i])) return 1;
    return 0;
}

void cc_measure_toggle(cc_measure *m) {
    m->on = !m->on;
    m->has_first = 0, m->has_hover = 0;
}

typedef struct {
    int has_ray, has_ray_tol, free_, ortho, chain;
    double ray[6], ray_tol[6], tol_px;
} Args;

static int nums(const JrVal *x, double *out, size_t n) {
    if (x->type != JR_ARR || x->n != n) return -1;
    for (size_t i = 0; i < n; i++) {
        if (x->items[i].type != JR_NUM || !(x->items[i].num >= -1e12 && x->items[i].num <= 1e12)) return -1;
        out[i] = x->items[i].num;
    }
    return 0;
}

/* {"ray":[6], "ray_tol":[6], "tol_px": n, "free", "ortho", "chain": booleans}; empty or null: no position. */
static int parse(const char *body, size_t n, Args *t, Buf *out) {
    memset(t, 0, sizeof *t);
    t->tol_px = 12.0;
    size_t k = 0;
    while (k < n && (body[k] == ' ' || body[k] == '\n' || body[k] == '\t' || body[k] == '\r')) k++;
    if (k == n || (n - k == 4 && !memcmp(body + k, "null", 4))) return 0;
    Arena a;
    arena_init(&a, 0);
    JrError je;
    int rc = -1;
    const JrVal *v = jr_parse(&a, body, n, &je);
    if (!v) {
        buf_puts(out, je.msg);
        goto end;
    }
    if (v->type != JR_OBJ) {
        buf_puts(out, "the measure tool's body is an object");
        goto end;
    }
    for (size_t i = 0; i < v->n; i++) {
        const JrMember *m = &v->members[i];
        const JrVal *x = &m->val;
        const char *bad = NULL;
        if (jr_key_is(m, "ray")) bad = nums(x, t->ray, 6) ? "ray: expected [ox, oy, oz, dx, dy, dz]" : NULL, t->has_ray = 1;
        else if (jr_key_is(m, "ray_tol")) bad = nums(x, t->ray_tol, 6) ? "ray_tol: expected [ox, oy, oz, dx, dy, dz]" : NULL, t->has_ray_tol = 1;
        else if (jr_key_is(m, "tol_px")) {
            if (x->type != JR_NUM || !(x->num > 0 && x->num <= 1000)) bad = "tol_px: expected a number within (0, 1000]";
            else t->tol_px = x->num;
        } else if (jr_key_is(m, "free") || jr_key_is(m, "ortho") || jr_key_is(m, "chain")) {
            if (x->type != JR_BOOL) bad = "free, ortho, chain: expected a boolean";
            else if (jr_key_is(m, "free")) t->free_ = x->b;
            else if (jr_key_is(m, "ortho")) t->ortho = x->b;
            else t->chain = x->b;
        } else {
            buf_puts(out, "unknown field: "), buf_append(out, m->key, m->key_n);
            goto end;
        }
        if (bad) {
            buf_puts(out, bad);
            goto end;
        }
    }
    rc = 0;
end:
    arena_free(&a);
    return rc;
}

static const char *const KIND[] = {"corner", "edge", "face"};

static void point_json(JsonW *w, const cc_msnap *s) {
    jw_arr_begin(w);
    for (int k = 0; k < 3; k++) jw_f64(w, (double)s->p[k] + s->org[k]);
    jw_arr_end(w);
}

static void text_of(const cc_mdim *d, Buf *t) { /* lib.rs draw_dimensions */
    float mm = cc_mdim_distance(d), nrm;
    cc_format_mm(t, mm);
    if (cc_mdim_perpendicular(d, &nrm)) {
        float diff = nrm - mm;
        if ((diff < 0 ? -diff : diff) > 0.5f) buf_puts(t, "  (faces "), cc_format_mm(t, nrm), buf_putc(t, ')');
    }
}

void cc_measure_json(const cc_measure *m, JsonW *w) {
    jw_obj_begin(w);
    jw_key(w, "active"), jw_bool(w, m->on);
    jw_key(w, "first");
    if (m->on && m->has_first) point_json(w, &m->first);
    else jw_null(w);
    jw_key(w, "hover");
    if (m->on && m->has_hover) {
        jw_obj_begin(w);
        jw_key(w, "at"), point_json(w, &m->hover);
        jw_key(w, "snap"), jw_str(w, KIND[m->hover.kind]);
        jw_obj_end(w);
    } else jw_null(w);
    jw_key(w, "items"), jw_arr_begin(w);
    Buf t;
    buf_init(&t);
    for (size_t k = 0; k < m->n; k++) {
        const cc_mdim *d = &m->dims[k];
        float mm = cc_mdim_distance(d);
        jw_obj_begin(w);
        jw_key(w, "a"), point_json(w, &d->a);
        jw_key(w, "b"), point_json(w, &d->b);
        jw_key(w, "id"), jw_u64(w, k);
        jw_key(w, "label"), label_json(w, k, mm);
        buf_clear(&t);
        text_of(d, &t);
        jw_key(w, "text"), jw_str_n(w, t.p ? t.p : "", t.len);
        jw_key(w, "value_mm"), jw_f64(w, (double)mm);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_key(w, "preview"); /* the rubber line from the first point to the hover */
    if (m->on && m->has_first && m->has_hover) {
        cc_mdim d = {m->first, m->hover};
        float mm = cc_mdim_distance(&d);
        buf_clear(&t);
        cc_format_mm(&t, mm);
        jw_obj_begin(w);
        jw_key(w, "text"), jw_str_n(w, t.p ? t.p : "", t.len);
        jw_key(w, "value_mm"), jw_f64(w, (double)mm);
        jw_obj_end(w);
    } else jw_null(w);
    if (t.oom) w->b->oom = 1;
    buf_free(&t);
    jw_obj_end(w);
}

static int answer(const cc_measure *m, Buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    cc_measure_json(m, &w);
    return jw_finish(&w) == JW_OK && !out->oom ? 200 : (out->oom = 1, 400);
}

int cc_measure_call(cc_measure *m, const char *name, const char *body, size_t n, const cc_mview *v, Buf *out) {
    Args a;
    if (parse(body, n, &a, out)) return 400;
    if (!strcmp(name, "measure_clear")) { /* the Measurements panel's "clear" (lib.rs:4481) */
        m->n = 0, m->has_first = 0;
        return answer(m, out);
    }
    if (!strcmp(name, "measure_cancel")) { /* Esc (lib.rs:3031): the first point, else the tool */
        if (m->has_first) m->has_first = 0;
        else m->on = 0, m->has_hover = 0;
        return answer(m, out);
    }
    int click = !strcmp(name, "measure_click");
    if (!m->on) {
        if (!click) return answer(m, out);
        buf_puts(out, "the measure tool is off: action measure turns it on");
        return 400;
    }
    if (a.has_ray) { /* lib.rs: the hover follows the pointer (Alt: no snap, Shift: along an axis) */
        cc_msnap s;
        int r = v ? cc_measure_snap(v, a.ray, a.has_ray_tol ? a.ray_tol : NULL, a.tol_px, a.free_, &s) : 0;
        if (r < 0) {
            buf_puts(out, "out of memory");
            return 400;
        }
        m->has_hover = r == 1;
        if (r == 1) {
            if (a.ortho && m->has_first) cc_measure_orthogonal(&m->first, &s);
            m->hover = s;
        }
    }
    if (!click || !m->has_hover) return answer(m, out);
    if (!m->has_first) {
        m->first = m->hover, m->has_first = 1;
        return answer(m, out);
    }
    if (m->n >= CC_MEASURE_MAX) {
        buf_puts(out, "too many measurements (10000): clear them first");
        return 400;
    }
    if (m->n == m->cap) {
        size_t cap = m->cap ? 2 * m->cap : 16;
        cc_mdim *p = realloc(m->dims, cap * sizeof *p);
        if (!p) {
            buf_puts(out, "out of memory");
            return 400;
        }
        m->dims = p, m->cap = cap;
    }
    m->dims[m->n].a = m->first, m->dims[m->n].b = m->hover, m->n++;
    if (a.chain) m->first = m->hover; /* Ctrl: go on from there */
    else m->has_first = 0;
    return answer(m, out);
}
