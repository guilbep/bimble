/* The place tool: snap targets, snapping, the ghost (tool.h). No libm (wasm is freestanding). */
#include <stdlib.h>
#include <string.h>

#include "math.h"
#include "tool.h"

#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust (snap.rs) */

#define MAX_TARGETS 200000 /* snap.rs MAX_TARGETS */

static double fabs_(double x) { return x < 0.0 ? -x : x; }
static double min_(double a, double b) { return b < a ? b : a; }
static double max_(double a, double b) { return b > a ? b : a; }

double cc_round10(double v) {
    double x = v / 10.0;
    if (!(fabs_(x) < 4503599627370496.0)) return v + 0.0; /* 2^52: already whole (or NaN) */
    double t = (double)(int64_t)x;                          /* trunc, exact */
    double d = x - t;                                       /* exact */
    if (d >= 0.5) t += 1.0;
    else if (d <= -0.5) t -= 1.0;
    return t * 10.0 + 0.0;
}

const char *cc_snap_label(int kind) {
    static const char *const L[] = {"axis crossing", "column centre", "corner", "grid axis", "edge"};
    return kind >= 0 && kind < 5 ? L[kind] : NULL;
}

void cc_ortho(const double a[2], const double p[2], double out[2]) {
    if (fabs_(p[0] - a[0]) >= fabs_(p[1] - a[1])) out[0] = p[0], out[1] = a[1];
    else out[0] = a[0], out[1] = p[1];
}

int cc_ray_plane(const double r[6], double z, double out[2]) {
    for (int k = 0; k < 6; k++)
        if (!(r[k] == r[k]) || fabs_(r[k]) > 1e15) return -1;
    if (fabs_(r[5]) < 1e-6) return -1;
    double t = (z - r[2]) / r[5];
    if (t < 0.0) return -1;
    out[0] = r[0] + r[3] * t, out[1] = r[1] + r[4] * t;
    return 0;
}

/* ---- targets (snap.rs:73-160) ---- */

void cc_snap_targets_free(cc_snap_targets *t) {
    free(t->points), free(t->lines);
    memset(t, 0, sizeof *t);
}

static int add_point(cc_snap_targets *t, double x, double y, int kind) {
    if (t->npoints == t->cap_points) {
        size_t c = t->cap_points ? 2 * t->cap_points : 64;
        cc_snap_point *p = realloc(t->points, c * sizeof *p);
        if (!p) return -1;
        t->points = p, t->cap_points = c;
    }
    cc_snap_point *q = &t->points[t->npoints++];
    q->p[0] = x, q->p[1] = y, q->kind = kind;
    return 0;
}

static int add_line(cc_snap_targets *t, const double a[2], const double b[2], int kind) {
    if (t->nlines == t->cap_lines) {
        size_t c = t->cap_lines ? 2 * t->cap_lines : 64;
        cc_snap_line *p = realloc(t->lines, c * sizeof *p);
        if (!p) return -1;
        t->lines = p, t->cap_lines = c;
    }
    cc_snap_line *q = &t->lines[t->nlines++];
    q->a[0] = a[0], q->a[1] = a[1], q->b[0] = b[0], q->b[1] = b[1], q->kind = kind;
    return 0;
}

typedef struct {
    int64_t k[2];
} Key;
typedef struct {
    Key a, b;
} KeyPair;

static int64_t key1(double v) { /* (v * 10.0).round() as i64 */
    double x = v * 10.0;
    if (!(fabs_(x) < 9.0e18)) return x > 0 ? INT64_MAX : (x < 0 ? INT64_MIN : 0);
    double t = (double)(int64_t)x, d = x - t;
    if (fabs_(x) < 4503599627370496.0) {
        if (d >= 0.5) t += 1.0;
        else if (d <= -0.5) t -= 1.0;
    }
    return (int64_t)t;
}
static int key_cmp(const Key *a, const Key *b) {
    for (int i = 0; i < 2; i++)
        if (a->k[i] != b->k[i]) return a->k[i] < b->k[i] ? -1 : 1;
    return 0;
}
static int key_qcmp(const void *a, const void *b) { return key_cmp(a, b); }
static int pair_qcmp(const void *a, const void *b) {
    const KeyPair *x = a, *y = b;
    int c = key_cmp(&x->a, &y->a);
    return c ? c : key_cmp(&x->b, &y->b);
}
static int dbl_qcmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* snap.rs `clusters`: sorted values closer than tol to the running mean, their mean. In place;
 * returns the count. */
static size_t clusters(double *v, size_t n, double tol) {
    qsort(v, n, sizeof *v, dbl_qcmp);
    size_t k = 0;
    double sum = 0.0;
    size_t cnt = 0;
    for (size_t i = 0; i < n; i++) {
        if (cnt && fabs_(v[i] - sum / (double)cnt) <= tol) {
            sum += v[i], cnt++;
            continue;
        }
        if (cnt) v[k++] = sum / (double)cnt;
        sum = v[i], cnt = 1;
    }
    if (cnt) v[k++] = sum / (double)cnt;
    return k;
}

/* snap.rs `crossing`: where two infinite lines cross, if not parallel. */
static int crossing(const double a0[2], const double a1[2], const double b0[2], const double b1[2], double out[2]) {
    double d1[2] = {a1[0] - a0[0], a1[1] - a0[1]}, d2[2] = {b1[0] - b0[0], b1[1] - b0[1]};
    double den = d1[0] * d2[1] - d1[1] * d2[0];
    double scale = cc_hypot(d1[0], d1[1]) * cc_hypot(d2[0], d2[1]);
    if (fabs_(den) <= 1e-9 * max_(scale, 1e-12)) return -1;
    double w[2] = {b0[0] - a0[0], b0[1] - a0[1]};
    double t = (w[0] * d2[1] - w[1] * d2[0]) / den;
    out[0] = a0[0] + d1[0] * t, out[1] = a0[1] + d1[1] * t;
    return 0;
}

int cc_snap_targets_build(cc_snap_targets *t, const cc_scene *sc, double level, double next) {
    return cc_snap_targets_build_grid(t, sc, level, next, NULL, 0);
}

int cc_snap_targets_build_grid(cc_snap_targets *t, const cc_scene *sc, double level, double next, const double (*grid)[4],
                               size_t ngrid) {
    return cc_snap_targets_build_boxes(t, sc, level, next, grid, ngrid, NULL);
}

int cc_snap_targets_build_boxes(cc_snap_targets *t, const cc_scene *sc, double level, double next, const double (*grid)[4],
                                size_t ngrid, const float (*box)[6]) {
    cc_snap_targets_free(t);
    const double *o = sc->origin;
    double *cx = NULL, *cy = NULL;
    Key *corners = NULL;
    KeyPair *edges = NULL;
    size_t ncent = 0, ncorners = 0, nedges = 0, capc = 0, capk = 0, cape = 0;
    double lo[2] = {1.0 / 0.0, 1.0 / 0.0}, hi[2] = {-1.0 / 0.0, -1.0 / 0.0};
    Arena a;
    arena_init(&a, 0);
    int rc = -1;
    for (size_t k = 0; k < sc->nelements; k++) {
        /* MEAS1: every point's z is within its box's (the f32 values, + o[2] rounds monotonically) */
        if (box && ((double)box[k][5] + o[2] < level - 1.0 || (double)box[k][2] + o[2] > next + 1.0)) continue;
        cc_mesh m;
        arena_reset(&a);
        if (cc_scene_world_mesh(sc, k, &a, &m)) goto end;
        double zmin = 1.0 / 0.0, zmax = -zmin, elo[2] = {zmin, zmin}, ehi[2] = {zmax, zmax};
        for (uint32_t v = 0; v < m.nv; v++) {
            double w[3] = {(double)m.pos[v][0] + o[0], (double)m.pos[v][1] + o[1], (double)m.pos[v][2] + o[2]};
            zmin = min_(zmin, w[2]), zmax = max_(zmax, w[2]);
            for (int i = 0; i < 2; i++) elo[i] = min_(elo[i], w[i]), ehi[i] = max_(ehi[i], w[i]);
        }
        /* In the storey's height band (a footing below it counts when it reaches its level). */
        if (!(zmax >= level - 1.0 && zmin <= next + 1.0) || !(fabs_(elo[0]) < 1.0 / 0.0)) continue;
        for (int i = 0; i < 2; i++) lo[i] = min_(lo[i], elo[i]), hi[i] = max_(hi[i], ehi[i]);
        if (sc->elements[k].cls == CC_IFC_COLUMN) {
            if (ncent == capc) {
                size_t c = capc ? 2 * capc : 32;
                double *x = realloc(cx, c * sizeof *x), *y = x ? realloc(cy, c * sizeof *y) : NULL;
                if (x) cx = x;
                if (!x || !y) goto end;
                cy = y, capc = c;
            }
            cx[ncent] = (elo[0] + ehi[0]) / 2.0, cy[ncent] = (elo[1] + ehi[1]) / 2.0, ncent++;
        }
        for (uint32_t e = 0; e < m.ne; e++) {
            double pa[2] = {(double)m.edges[e][0] + o[0], (double)m.edges[e][1] + o[1]};
            double pb[2] = {(double)m.edges[e][3] + o[0], (double)m.edges[e][4] + o[1]};
            Key ka = {{key1(pa[0]), key1(pa[1])}}, kb = {{key1(pb[0]), key1(pb[1])}};
            if (ncorners + 2 > capk) {
                size_t c = capk ? 2 * capk : 256;
                Key *p = realloc(corners, c * sizeof *p);
                if (!p) goto end;
                corners = p, capk = c;
            }
            if (ncorners < MAX_TARGETS) corners[ncorners++] = ka, corners[ncorners++] = kb;
            if (cc_hypot(pb[0] - pa[0], pb[1] - pa[1]) > 1.0 && nedges < MAX_TARGETS) {
                if (nedges == cape) {
                    size_t c = cape ? 2 * cape : 256;
                    KeyPair *p = realloc(edges, c * sizeof *p);
                    if (!p) goto end;
                    edges = p, cape = c;
                }
                KeyPair kp;
                if (key_cmp(&ka, &kb) <= 0) kp.a = ka, kp.b = kb;
                else kp.a = kb, kp.b = ka;
                edges[nedges++] = kp;
            }
        }
    }
    /* BTreeSet: sorted, unique. */
    if (ncorners) qsort(corners, ncorners, sizeof *corners, key_qcmp);
    if (nedges) qsort(edges, nedges, sizeof *edges, pair_qcmp);
    {
        size_t u = 0;
        for (size_t i = 0; i < ncorners; i++)
            if (!u || key_cmp(&corners[u - 1], &corners[i])) corners[u++] = corners[i];
        ncorners = u, u = 0;
        for (size_t i = 0; i < nedges; i++)
            if (!u || pair_qcmp(&edges[u - 1], &edges[i])) edges[u++] = edges[i];
        nedges = u;
    }
    /* SLAB1 (snap.rs:76-82): the model's grid axes (longer than 1 mm); without one, axes through
     * the columns, across what is on the storey (snap.rs:127). */
    double(*axes)[4] = NULL;
    size_t naxes = 0;
    for (size_t i = 0; i < ngrid; i++) {
        if (!(cc_hypot(grid[i][2] - grid[i][0], grid[i][3] - grid[i][1]) > 1.0)) continue;
        if (!axes && !(axes = malloc(ngrid * sizeof *axes))) goto end;
        memcpy(axes[naxes++], grid[i], sizeof *axes);
    }
    if (!naxes && ncent) {
        double *xs = malloc(2 * ncent * sizeof *xs);
        axes = malloc(2 * ncent * sizeof *axes);
        if (!xs || !axes) {
            free(xs);
            free(axes);
            goto end;
        }
        double m0[2] = {lo[0] - 1000.0, lo[1] - 1000.0}, m1[2] = {hi[0] + 1000.0, hi[1] + 1000.0};
        memcpy(xs, cx, ncent * sizeof *xs);
        size_t nx = clusters(xs, ncent, 50.0);
        for (size_t i = 0; i < nx; i++) {
            double *ax = axes[naxes++];
            ax[0] = xs[i], ax[1] = m0[1], ax[2] = xs[i], ax[3] = m1[1];
        }
        memcpy(xs, cy, ncent * sizeof *xs);
        size_t ny = clusters(xs, ncent, 50.0);
        for (size_t i = 0; i < ny; i++) {
            double *ax = axes[naxes++];
            ax[0] = m0[0], ax[1] = xs[i], ax[2] = m1[0], ax[3] = xs[i];
        }
        free(xs);
    }
    int bad = 0;
    for (size_t i = 0; i < naxes && !bad; i++)
        for (size_t j = i + 1; j < naxes && !bad; j++) {
            double p[2];
            if (!crossing(axes[i], axes[i] + 2, axes[j], axes[j] + 2, p)) bad = add_point(t, p[0], p[1], CC_SNAP_AXIS_CROSSING);
        }
    for (size_t i = 0; i < ncent && !bad; i++) bad = add_point(t, cx[i], cy[i], CC_SNAP_COLUMN_CENTRE);
    for (size_t i = 0; i < ncorners && !bad; i++)
        bad = add_point(t, (double)corners[i].k[0] / 10.0, (double)corners[i].k[1] / 10.0, CC_SNAP_CORNER);
    for (size_t i = 0; i < naxes && !bad; i++) bad = add_line(t, axes[i], axes[i] + 2, CC_SNAP_AXIS);
    for (size_t i = 0; i < nedges && !bad; i++) {
        double pa[2] = {(double)edges[i].a.k[0] / 10.0, (double)edges[i].a.k[1] / 10.0};
        double pb[2] = {(double)edges[i].b.k[0] / 10.0, (double)edges[i].b.k[1] / 10.0};
        bad = add_line(t, pa, pb, CC_SNAP_EDGE);
    }
    free(axes);
    rc = bad ? -1 : 0;
end:
    arena_free(&a);
    free(cx), free(cy), free(corners), free(edges);
    if (rc) cc_snap_targets_free(t);
    return rc;
}

/* snap.rs `on_segment`. */
static void on_segment(const double p[2], const double a[2], const double b[2], double out[2]) {
    double d[2] = {b[0] - a[0], b[1] - a[1]};
    double l2 = d[0] * d[0] + d[1] * d[1];
    if (l2 <= 0.0) {
        out[0] = a[0], out[1] = a[1];
        return;
    }
    double s = ((p[0] - a[0]) * d[0] + (p[1] - a[1]) * d[1]) / l2;
    s = s < 0.0 ? 0.0 : (s > 1.0 ? 1.0 : s);
    out[0] = a[0] + d[0] * s, out[1] = a[1] + d[1] * s;
}

int cc_snap(const cc_snap_targets *t, const double p[2], double tol, double out[2]) {
    int bk = 0;
    double bd = 0.0;
    const double *bq = NULL;
    for (size_t i = 0; i < t->npoints; i++) { /* min_by (kind, dist): the first of the least */
        const cc_snap_point *q = &t->points[i];
        double d = cc_hypot(q->p[0] - p[0], q->p[1] - p[1]);
        if (!(d <= tol)) continue;
        if (!bq || q->kind < bk || (q->kind == bk && d < bd)) bq = q->p, bk = q->kind, bd = d;
    }
    if (bq) {
        out[0] = bq[0], out[1] = bq[1];
        return bk;
    }
    int found = 0;
    double best[2] = {0, 0};
    for (size_t i = 0; i < t->nlines; i++) {
        const cc_snap_line *l = &t->lines[i];
        double q[2];
        on_segment(p, l->a, l->b, q);
        double d = cc_hypot(q[0] - p[0], q[1] - p[1]);
        if (!(d <= tol)) continue;
        if (!found || l->kind < bk || (l->kind == bk && d < bd)) found = 1, bk = l->kind, bd = d, best[0] = q[0], best[1] = q[1];
    }
    if (found) {
        /* Along the line, the position is rounded to 10 mm where the line is along x or y. */
        int vert = 0, horiz = 0;
        for (size_t i = 0; i < t->nlines && !vert; i++) {
            const cc_snap_line *l = &t->lines[i];
            vert = fabs_(l->a[0] - l->b[0]) < 1e-9 && fabs_(l->a[0] - best[0]) < 1e-9;
        }
        for (size_t i = 0; i < t->nlines && !vert && !horiz; i++) {
            const cc_snap_line *l = &t->lines[i];
            horiz = fabs_(l->a[1] - l->b[1]) < 1e-9 && fabs_(l->a[1] - best[1]) < 1e-9;
        }
        out[0] = best[0], out[1] = best[1];
        if (vert) out[1] = cc_round10(best[1]);
        else if (horiz) out[0] = cc_round10(best[0]);
        return bk;
    }
    out[0] = cc_round10(p[0]), out[1] = cc_round10(p[1]);
    return CC_SNAP_NONE;
}

/* ---- state ---- */

void cc_tool_free(cc_tool *t) {
    cc_snap_targets_free(&t->targets);
    free(t->annot_text);
    memset(t, 0, sizeof *t);
}

void cc_tool_clear(cc_tool *t) {
    t->npoints = 0, t->has_hover = 0, t->annot = 0;
    free(t->annot_text), t->annot_text = NULL;
}

/* ---- the ghost ---- */

int cc_tool_ghost(JsonW *w, const cc_model *m, const cc_command *cmds, size_t n, const double origin[3], Buf *err) {
    cc_model g;
    cc_model_init(&g);
    cc_scene sc;
    memset(&sc, 0, sizeof sc);
    int built = 0, rc = -1;
    cc_command inv;
    for (size_t i = 0; i < m->nstoreys; i++) {
        cc_command c;
        memset(&c, 0, sizeof c);
        c.op = CC_CMD_ADD_STOREY, c.storey = m->storeys[i];
        if (cc_command_apply(&g, &c, &inv, err)) goto end;
    }
    for (size_t i = 0; i < m->nmaterials; i++) {
        cc_command c;
        memset(&c, 0, sizeof c);
        c.op = CC_CMD_ADD_MATERIAL, c.material = m->materials[i];
        if (cc_command_apply(&g, &c, &inv, err)) goto end;
    }
    for (size_t i = 0; i < n; i++)
        if (cc_command_apply(&g, &cmds[i], &inv, err)) goto end;
    cc_error e;
    built = 1;
    if (cc_scene_build(&sc, &g, &e)) {
        buf_puts(err, e.msg);
        goto end;
    }
    Arena a;
    arena_init(&a, 0);
    double d[3] = {sc.origin[0] - origin[0], sc.origin[1] - origin[1], sc.origin[2] - origin[2]};
    jw_obj_begin(w);
    for (int pass = 0; pass < 3; pass++) { /* edges, idx, (origin), pos: sorted keys */
        static const char *const K[] = {"edges", "idx", "pos"};
        if (pass == 2) {
            jw_key(w, "origin"), jw_arr_begin(w);
            for (int k = 0; k < 3; k++) jw_f64(w, origin[k] + 0.0);
            jw_arr_end(w);
        }
        jw_key(w, K[pass]), jw_arr_begin(w);
        uint32_t base = 0;
        for (size_t k = 0; k < sc.nelements; k++) {
            cc_mesh mm;
            arena_reset(&a);
            if (cc_scene_world_mesh(&sc, k, &a, &mm)) {
                buf_puts(err, "out of memory");
                arena_free(&a);
                goto end;
            }
            if (pass == 0)
                for (uint32_t i = 0; i < mm.ne; i++)
                    for (int c = 0; c < 6; c++) jw_f64(w, (double)(float)((double)mm.edges[i][c] + d[c % 3]) + 0.0);
            else if (pass == 1)
                for (uint32_t i = 0; i < mm.ni; i++) jw_u64(w, base + mm.idx[i]);
            else
                for (uint32_t i = 0; i < mm.nv; i++)
                    for (int c = 0; c < 3; c++) jw_f64(w, (double)(float)((double)mm.pos[i][c] + d[c]) + 0.0);
            base += mm.nv;
        }
        jw_arr_end(w);
    }
    jw_obj_end(w);
    arena_free(&a);
    rc = 0;
end:
    if (built) cc_scene_free(&sc);
    cc_model_free(&g);
    return rc;
}
