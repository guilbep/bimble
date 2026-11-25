/* Dimensions and texts of a view, resolved: see annot.h. No libm (wasm is freestanding). */
#include "annot.h"
#include <string.h>
#include "math.h"
#include "view_io.h"

#pragma STDC FP_CONTRACT OFF

static double fabs_(double x) { return x < 0 ? -x : x; }

static double round_(double x) { /* f64::round: half away from zero */
    if (!(fabs_(x) < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x, d = x - t;
    if (d >= 0.5) t += 1.0;
    else if (d <= -0.5) t -= 1.0;
    return t == 0.0 ? x * 0.0 : t;
}

/* "corner:N": N, else -1. */
static long corner_index(cc_str f) {
    if (f.n < 8 || f.n > 14 || memcmp(f.p, "corner:", 7)) return -1;
    if (f.p[7] == '0' && f.n > 8) return -1; /* no leading zeros */
    long k = 0;
    for (size_t i = 7; i < f.n; i++) {
        if (f.p[i] < '0' || f.p[i] > '9') return -1;
        k = k * 10 + (f.p[i] - '0');
    }
    return k;
}

static int is(cc_str f, const char *s) { return f.n == strlen(s) && !memcmp(f.p, s, f.n); }

int cc_feature_name_ok(cc_str f) { return is(f, "origin") || is(f, "end") || corner_index(f) >= 0; }

/* The local point of `feature` on generator `g`. 0 ok, -1 none. */
static int local_point(const cc_generator *g, cc_str f, double p[3]) {
    p[0] = p[1] = p[2] = 0.0;
    if (is(f, "origin")) return 0;
    if (is(f, "end")) {
        if (g->kind == CC_GEN_RECT_BEAM) return p[2] = g->u.beam.length, 0;
        if (g->kind == CC_GEN_WALL) return p[0] = g->u.wall.length, 0;
        return -1;
    }
    long k = corner_index(f);
    if (k < 0) return -1;
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING:
    case CC_GEN_RECT_BEAM: {
        int beam = g->kind == CC_GEN_RECT_BEAM;
        if (k > (beam ? 7 : 3)) return -1;
        double x = (beam ? g->u.beam.width : g->u.col.width) / 2.0;
        double y = (beam ? g->u.beam.height : g->u.col.depth) / 2.0;
        static const int SX[4] = {-1, 1, 1, -1}, SY[4] = {-1, -1, 1, 1};
        p[0] = SX[k % 4] * x, p[1] = SY[k % 4] * y;
        if (k >= 4) p[2] = g->u.beam.length;
        return 0;
    }
    case CC_GEN_WALL: {
        if (k > 3) return -1;
        double t = g->u.wall.thickness / 2.0;
        p[0] = (k == 1 || k == 2) ? g->u.wall.length : 0.0;
        p[1] = k < 2 ? -t : t;
        return 0;
    }
    case CC_GEN_SLAB:
    case CC_GEN_EXTRUDED_POLYGON: {
        const cc_loop *l = g->kind == CC_GEN_SLAB ? &g->u.slab.outline : &g->u.poly.outline;
        if ((size_t)k >= l->n) return -1;
        p[0] = l->pts[k][0], p[1] = l->pts[k][1];
        return 0;
    }
    case CC_GEN_MESH:
        if ((size_t)k >= g->u.mesh.npositions) return -1;
        memcpy(p, g->u.mesh.positions[k], sizeof(double) * 3);
        return 0;
    default: return -1;
    }
}

/* Through the placement (scene.c to_world), on the storey's elevation. */
static void to_world(const cc_model *m, const cc_element *e, const double p[3], double out[3]) {
    const double *x = e->placement.x_axis, *z = e->placement.z_axis, *o = e->placement.origin;
    double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    const cc_storey *st = cc_model_storey(m, &e->storey);
    double t[4][3] = {{x[0], x[1], x[2]}, {y[0], y[1], y[2]}, {z[0], z[1], z[2]}, {o[0], o[1], o[2] + (st ? st->elevation : 0.0)}};
    for (int r = 0; r < 3; r++) out[r] = t[0][r] * p[0] + t[1][r] * p[1] + t[2][r] * p[2] + t[3][r] + 0.0;
}

int cc_feature_point(const cc_model *m, const cc_element *e, cc_str feature, double out[3]) {
    double p[3];
    if (local_point(&e->generator, feature, p)) return -1;
    to_world(m, e, p, out);
    for (int r = 0; r < 3; r++)
        if (!(out[r] - out[r] == 0.0)) return -1;
    return 0;
}

int cc_anchor_point(const cc_model *m, const cc_anchor *a, double out[3]) {
    if (m && a->has_element && a->has_feature) {
        const cc_element *e = cc_model_element(m, &a->element);
        if (e && !cc_feature_point(m, e, a->feature, out)) return 1;
    }
    memcpy(out, a->at, sizeof a->at);
    return 0;
}

int cc_feature_near(const cc_model *m, const double p[2], double tol, const cc_gid *storey, cc_gid *el, char feat[24],
                    double at[3]) {
    int found = 0;
    double bd = 0.0;
    int bcls = 0;
    for (size_t i = 0; i < m->nelements; i++) {
        const cc_element *e = &m->elements[i];
        if (storey && cc_gid_cmp(&e->storey, storey)) continue;
        size_t nc = 0;
        switch (e->generator.kind) {
        case CC_GEN_RECT_BEAM: nc = 8; break;
        case CC_GEN_SLAB: nc = e->generator.u.slab.outline.n; break;
        case CC_GEN_EXTRUDED_POLYGON: nc = e->generator.u.poly.outline.n; break;
        case CC_GEN_MESH: nc = 0; break; /* a mesh's points: by name only */
        default: nc = 4;
        }
        for (size_t k = 0; k < nc + 2; k++) {
            char name[24];
            if (k == 0) memcpy(name, "origin", 7);
            else if (k == 1) memcpy(name, "end", 4);
            else {
                size_t c = k - 2, n = 0;
                char d[12];
                do d[n++] = (char)('0' + c % 10), c /= 10;
                while (c && n < sizeof d);
                memcpy(name, "corner:", 7);
                for (size_t j = 0; j < n; j++) name[7 + j] = d[n - 1 - j];
                name[7 + n] = 0;
            }
            cc_str f = {name, strlen(name)};
            double w[3];
            if (cc_feature_point(m, e, f, w)) continue;
            double d = cc_hypot(w[0] - p[0], w[1] - p[1]);
            if (!(d <= tol)) continue;
            /* nearest; then the lower class; then the lower id (the elements are in id order) */
            if (found && !(d < bd || (d == bd && (int)e->cls < bcls))) continue;
            found = 1, bd = d, bcls = (int)e->cls;
            *el = e->id;
            memcpy(feat, name, strlen(name) + 1);
            memcpy(at, w, sizeof w);
        }
    }
    return found;
}

/* ---- the drawing (viewdraw.rs, layout.rs), in paper axes (y down) as Rust, k = 1 ---- */

static int dim_on_paper(const double pa[2], const double pb[2], double off, cc_dim_axis axis, double u[2], double qa[2],
                        double qb[2], double *len) {
    double d0[2] = {pb[0] - pa[0], pb[1] - pa[1]};
    if (cc_hypot(d0[0], d0[1]) < 1e-6) return -1;
    double d[2];
    switch (axis) {
    case CC_AXIS_HORIZONTAL: d[0] = d0[0] < 0.0 ? -1.0 : 1.0, d[1] = 0.0; break;
    case CC_AXIS_VERTICAL: d[0] = 0.0, d[1] = d0[1] < 0.0 ? -1.0 : 1.0; break;
    default: d[0] = d0[0], d[1] = d0[1];
    }
    double dl = cc_hypot(d[0], d[1]);
    u[0] = d[0] / dl, u[1] = d[1] / dl;
    double n[2] = {u[1], -u[0]};
    qa[0] = pa[0] + n[0] * off, qa[1] = pa[1] + n[1] * off;
    double along = (pb[0] - qa[0]) * u[0] + (pb[1] - qa[1]) * u[1];
    qb[0] = qa[0] + u[0] * along, qb[1] = qa[1] + u[1] * along;
    *len = fabs_(along);
    return *len > 1e-6 ? 0 : -1;
}

int cc_dim_line(const double a[2], const double b[2], double off, cc_dim_axis axis, double u[2], double qa[2],
                double qb[2], double *len) {
    double pa[2] = {a[0], -a[1]}, pb[2] = {b[0], -b[1]};
    if (dim_on_paper(pa, pb, off, axis, u, qa, qb, len)) return -1;
    u[1] = -u[1] + 0.0, qa[1] = -qa[1] + 0.0, qb[1] = -qb[1] + 0.0;
    u[0] += 0.0, qa[0] += 0.0, qb[0] += 0.0;
    return 0;
}

void cc_dim_axis_offset(const double wa[2], const double wb[2], const double wc[2], cc_dim_axis *axis, double *off) {
    double a[2] = {wa[0], -wa[1]}, b[2] = {wb[0], -wb[1]}, c[2] = {wc[0], -wc[1]};
    double x0 = a[0] < b[0] ? a[0] : b[0], x1 = a[0] < b[0] ? b[0] : a[0];
    double y0 = a[1] < b[1] ? a[1] : b[1], y1 = a[1] < b[1] ? b[1] : a[1];
    cc_dim_axis ax;
    if (fabs_(b[0] - a[0]) < 1e-6 || fabs_(b[1] - a[1]) < 1e-6) ax = CC_AXIS_ALIGNED;
    else if (c[1] < y0 || c[1] > y1) ax = CC_AXIS_HORIZONTAL;
    else if (c[0] < x0 || c[0] > x1) ax = CC_AXIS_VERTICAL;
    else ax = CC_AXIS_ALIGNED;
    double d[2];
    switch (ax) {
    case CC_AXIS_HORIZONTAL: d[0] = b[0] < a[0] ? -1.0 : 1.0, d[1] = 0.0; break;
    case CC_AXIS_VERTICAL: d[0] = 0.0, d[1] = b[1] < a[1] ? -1.0 : 1.0; break;
    default: d[0] = b[0] - a[0], d[1] = b[1] - a[1];
    }
    double l = cc_hypot(d[0], d[1]);
    if (!(l > 1e-12)) l = 1e-12;
    double n[2] = {d[1] / l, -d[0] / l};
    *axis = ax;
    *off = (c[0] - a[0]) * n[0] + (c[1] - a[1]) * n[1];
}

/* ---- DimUnit::format ---- */

static void put_digits(Buf *b, double whole) { /* a non-negative whole number < 2^53 */
    char d[24];
    size_t n = 0;
    uint64_t v = (uint64_t)whole;
    do d[n++] = (char)('0' + v % 10), v /= 10;
    while (v);
    while (n) buf_putc(b, d[--n]);
}

void cc_dim_format(Buf *b, double mm, int unit) {
    if (unit == CC_UNIT_MM || !(fabs_(mm) < 1e15)) {
        cc_buf_display_f64(b, round_(mm) + 0.0);
        return;
    }
    if (unit == CC_UNIT_CM) {
        double r = round_(mm) + 0.0; /* cm = r / 10 */
        double q = round_(r / 10.0);
        if (q * 10.0 == r) {
            cc_buf_display_f64(b, r / 10.0 + 0.0);
            return;
        }
        double a = fabs_(r), w = (double)(uint64_t)(a / 10.0);
        if (r < 0) buf_putc(b, '-');
        put_digits(b, w);
        buf_putc(b, '.');
        put_digits(b, a - w * 10.0);
        return;
    }
    double s = round_(mm / 10.0) + 0.0; /* m = s / 100, two decimals */
    double a = fabs_(s), w = (double)(uint64_t)(a / 100.0), f = a - w * 100.0;
    if (s < 0) buf_putc(b, '-');
    put_digits(b, w);
    buf_putc(b, '.');
    if (f < 10.0) buf_putc(b, '0');
    put_digits(b, f);
}

/* ---- the query ---- */

static void pair(JsonW *w, double x, double y) {
    jw_arr_begin(w);
    jw_f64(w, x + 0.0);
    jw_f64(w, y + 0.0);
    jw_arr_end(w);
}

static void anchor_json(JsonW *w, const cc_anchor *a, int follows) {
    jw_obj_begin(w);
    jw_key(w, "element");
    if (a->has_element) jw_str_n(w, a->element.c, 22);
    else jw_null(w);
    jw_key(w, "feature");
    if (a->has_feature) jw_str_n(w, a->feature.p, a->feature.n);
    else jw_null(w);
    jw_key(w, "follows"), jw_bool(w, follows);
    jw_obj_end(w);
}

void cc_annotations_json(JsonW *w, const cc_model *m, const cc_view *v, int unit) {
    static const char *const AXIS[] = {"aligned", "horizontal", "vertical"};
    static const char *const UNIT[] = {"mm", "cm", "m"};
    jw_obj_begin(w);
    jw_key(w, "items"), jw_arr_begin(w);
    for (size_t i = 0; i < v->nannotations; i++) {
        const cc_annotation *x = &v->annotations[i];
        jw_obj_begin(w);
        if (x->type == CC_ANN_DIMENSION) {
            double pa[3], pb[3], u[2], qa[2], qb[2], len = 0.0;
            int fa = cc_anchor_point(m, &x->a, pa), fb = cc_anchor_point(m, &x->b, pb);
            int ok = !cc_dim_line(pa, pb, x->offset_mm, x->axis, u, qa, qb, &len);
            jw_key(w, "a"), pair(w, pa[0], pa[1]);
            jw_key(w, "anchors"), jw_arr_begin(w), anchor_json(w, &x->a, fa), anchor_json(w, &x->b, fb), jw_arr_end(w);
            jw_key(w, "axis"), jw_str(w, AXIS[x->axis]);
            jw_key(w, "b"), pair(w, pb[0], pb[1]);
            jw_key(w, "dir");
            if (ok) {
                double d[2] = {u[0], u[1]};
                if (d[0] < 0.0 || (d[0] == 0.0 && d[1] < 0.0)) d[0] = -d[0], d[1] = -d[1];
                pair(w, d[0], d[1]);
            } else jw_null(w);
            jw_key(w, "id"), jw_str_n(w, x->id.c, 22);
            jw_key(w, "kind");
            if (x->has_kind) jw_str_n(w, x->kind.p, x->kind.n);
            else jw_null(w);
            jw_key(w, "mid");
            if (ok) pair(w, (qa[0] + qb[0]) / 2.0, (qa[1] + qb[1]) / 2.0);
            else jw_null(w);
            jw_key(w, "offset_mm"), jw_f64(w, x->offset_mm);
            jw_key(w, "qa");
            if (ok) pair(w, qa[0], qa[1]);
            else jw_null(w);
            jw_key(w, "qb");
            if (ok) pair(w, qb[0], qb[1]);
            else jw_null(w);
            jw_key(w, "text");
            Buf t;
            buf_init(&t);
            if (ok) cc_dim_format(&t, len, unit);
            jw_str_n(w, t.oom ? "" : t.p, t.oom ? 0 : t.len);
            buf_free(&t);
            jw_key(w, "type"), jw_str(w, "dimension");
            jw_key(w, "value_mm"), jw_f64(w, ok ? len : 0.0);
        } else {
            jw_key(w, "angle_deg"), jw_f64(w, x->angle_deg);
            jw_key(w, "at"), pair(w, x->at[0], x->at[1]);
            jw_key(w, "id"), jw_str_n(w, x->id.c, 22);
            jw_key(w, "kind");
            if (x->has_kind) jw_str_n(w, x->kind.p, x->kind.n);
            else jw_null(w);
            jw_key(w, "size_mm"), jw_f64(w, x->size_mm);
            jw_key(w, "text"), jw_str_n(w, x->text.p, x->text.n);
            jw_key(w, "type"), jw_str(w, "text");
        }
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_key(w, "scale"), jw_u64(w, v->scale);
    jw_key(w, "unit"), jw_str(w, UNIT[unit >= 0 && unit < 3 ? unit : 0]);
    jw_key(w, "view"), jw_str_n(w, v->id.c, 22);
    jw_obj_end(w);
}
