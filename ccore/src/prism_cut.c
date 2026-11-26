/* Openings cut without the boolean kernel: prism special cases (prism_cut.h, ticket SLAB1). */
#include "prism_cut.h"
#include "math.h"
#include <stdlib.h>
#include <string.h>

static double ab(double x) { return x < 0 ? -x : x; }
static double mx(double a, double b) { return a > b ? a : b; }
static double mn(double a, double b) { return a < b ? a : b; }

/* Signed volume and area of a triangle set (divergence theorem). */
static void vol_area(const cc_tri_set *s, double *vol, double *area) {
    double v = 0, ar = 0;
    for (size_t i = 0; i < s->n; i++) {
        const double *p = s->t[i], *q = s->t[i] + 3, *r = s->t[i] + 6;
        v += (p[0] * (q[1] * r[2] - q[2] * r[1]) - p[1] * (q[0] * r[2] - q[2] * r[0]) + p[2] * (q[0] * r[1] - q[1] * r[0])) / 6.0;
        double u[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]}, w[3] = {r[0] - p[0], r[1] - p[1], r[2] - p[2]};
        double c[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        double x = cc_sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        ar += x / 2.0;
    }
    *vol = v, *area = ar;
}

/* The box a triangle set is: every corner on its min or max per axis (within tol), and closed
 * (its volume and area are the box's within 1e-9). 0 with lo, hi; 1 not a box. */
static int as_box(const cc_tri_set *s, double lo[3], double hi[3]) {
    if (s->n < 12) return 1;
    for (int k = 0; k < 3; k++) lo[k] = 1.0 / 0.0, hi[k] = -1.0 / 0.0;
    for (size_t i = 0; i < s->n; i++)
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++) lo[k] = mn(lo[k], s->t[i][3 * c + k]), hi[k] = mx(hi[k], s->t[i][3 * c + k]);
    double d[3];
    for (int k = 0; k < 3; k++) {
        d[k] = hi[k] - lo[k];
        if (!(d[k] > 0) || !(d[k] < 1e12)) return 1;
    }
    for (size_t i = 0; i < s->n; i++)
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++) {
                double v = s->t[i][3 * c + k], tol = 1e-9 * mx(1.0, d[k]);
                if (ab(v - lo[k]) > tol && ab(v - hi[k]) > tol) return 1;
            }
    double v, ar;
    vol_area(s, &v, &ar);
    double bv = d[0] * d[1] * d[2], ba = 2 * (d[0] * d[1] + d[1] * d[2] + d[0] * d[2]);
    if (ab(ab(v) - bv) > 1e-9 * bv || ab(ar - ba) > 1e-9 * ba) return 1;
    return 0;
}

static int dcmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static size_t uniq(double *v, size_t n) {
    qsort(v, n, sizeof *v, dcmp);
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (!k || v[i] != v[k - 1]) v[k++] = v[i];
    return k;
}

int cc_prism_cut_box(const cc_ploop *loops, size_t nloops, double depth, const cc_tri_set *cut, size_t nc, Arena *a,
                     double (**tris)[3][3], size_t *ntris) {
    if (nloops != 1 || loops[0].n != 4 || !nc || !(depth > 0)) return 1;
    const double(*p)[2] = (const double(*)[2])loops[0].p;
    double hlo[3] = {p[0][0], p[0][1], 0.0}, hhi[3] = {p[0][0], p[0][1], depth};
    for (size_t i = 0; i < 4; i++) { /* axis-aligned: each edge along x or y */
        const double *u = p[i], *w = p[(i + 1) % 4];
        if (u[0] != w[0] && u[1] != w[1]) return 1;
        for (int k = 0; k < 2; k++) hlo[k] = mn(hlo[k], u[k]), hhi[k] = mx(hhi[k], u[k]);
    }
    if (!(hhi[0] > hlo[0] && hhi[1] > hlo[1])) return 1;
    double(*bx)[2][3] = arena_alloc(a, nc * sizeof *bx, 8);
    double *c[3];
    size_t n[3];
    for (int k = 0; k < 3; k++) c[k] = arena_alloc(a, (2 * nc + 2) * sizeof(double), 8);
    if (!bx || !c[0] || !c[1] || !c[2]) return -1;
    for (int k = 0; k < 3; k++) c[k][0] = hlo[k], c[k][1] = hhi[k], n[k] = 2;
    size_t nb = 0;
    for (size_t i = 0; i < nc; i++) {
        double lo[3], hi[3];
        if (as_box(&cut[i], lo, hi)) return 1;
        int empty = 0;
        for (int k = 0; k < 3; k++) {
            /* an opening face within 1e-9 of a host face is on it (the kernel sees them coplanar) */
            double tol = 1e-9 * mx(1.0, hhi[k] - hlo[k]);
            if (ab(lo[k] - hlo[k]) <= tol) lo[k] = hlo[k];
            if (ab(hi[k] - hhi[k]) <= tol) hi[k] = hhi[k];
            lo[k] = mx(lo[k], hlo[k]), hi[k] = mn(hi[k], hhi[k]);
            empty |= !(hi[k] > lo[k]);
        }
        if (empty) continue; /* outside the host: no change */
        for (int k = 0; k < 3; k++) bx[nb][0][k] = lo[k], bx[nb][1][k] = hi[k], c[k][n[k]++] = lo[k], c[k][n[k]++] = hi[k];
        nb++;
    }
    for (int k = 0; k < 3; k++) n[k] = uniq(c[k], n[k]);
    size_t cx = n[0] - 1, cy = n[1] - 1, cz = n[2] - 1;
    if ((double)cx * (double)cy * (double)cz > CC_PRISM_MAX_CELLS) return 1;
    unsigned char *solid = arena_alloc(a, cx * cy * cz + 1, 1);
    if (!solid) return -1;
    size_t nsolid = 0;
    for (size_t i = 0; i < cx; i++)
        for (size_t j = 0; j < cy; j++)
            for (size_t k = 0; k < cz; k++) {
                double m[3] = {(c[0][i] + c[0][i + 1]) / 2, (c[1][j] + c[1][j + 1]) / 2, (c[2][k] + c[2][k + 1]) / 2};
                int in = 0;
                for (size_t b = 0; b < nb && !in; b++)
                    in = m[0] > bx[b][0][0] && m[0] < bx[b][1][0] && m[1] > bx[b][0][1] && m[1] < bx[b][1][1] && m[2] > bx[b][0][2] &&
                         m[2] < bx[b][1][2];
                solid[(i * cy + j) * cz + k] = (unsigned char)!in;
                nsolid += !in;
            }
#define SOLID(i, j, k) ((i) < cx && (j) < cy && (k) < cz && solid[((i) * cy + (j)) * cz + (k)])
    size_t nf = 0;
    for (int pass = 0; pass < 2; pass++) {
        double(*o)[3][3] = NULL;
        if (pass) {
            o = arena_alloc(a, (2 * nf + 1) * sizeof *o, 8);
            if (!o) return -1;
            nf = 0;
        }
        for (size_t i = 0; i < cx; i++)
            for (size_t j = 0; j < cy; j++)
                for (size_t k = 0; k < cz; k++) {
                    if (!SOLID(i, j, k)) continue;
                    size_t id[3] = {i, j, k};
                    for (int ax = 0; ax < 3; ax++)
                        for (int side = 0; side < 2; side++) {
                            size_t q[3] = {i, j, k};
                            if (side) q[ax]++;
                            else if (q[ax] == 0) q[ax] = (size_t)-1;
                            else q[ax]--;
                            if (SOLID(q[0], q[1], q[2])) continue;
                            if (pass) {
                                int b = (ax + 1) % 3, cc = (ax + 2) % 3;
                                double v[4][3];
                                double fa = c[ax][id[ax] + (size_t)side];
                                double b0 = c[b][id[b]], b1 = c[b][id[b] + 1], c0 = c[cc][id[cc]], c1 = c[cc][id[cc] + 1];
                                double bb[4] = {b0, b1, b1, b0}, ccv[4] = {c0, c0, c1, c1};
                                for (int t = 0; t < 4; t++) {
                                    int s2 = side ? t : 3 - t; /* + face counter-clockwise from outside, - reversed */
                                    v[t][ax] = fa, v[t][b] = bb[s2], v[t][cc] = ccv[s2];
                                }
                                memcpy(o[nf][0], v[0], 24), memcpy(o[nf][1], v[1], 24), memcpy(o[nf][2], v[2], 24), nf++;
                                memcpy(o[nf][0], v[0], 24), memcpy(o[nf][1], v[2], 24), memcpy(o[nf][2], v[3], 24), nf++;
                            } else
                                nf++;
                        }
                }
        if (pass) *tris = o, *ntris = nf;
    }
#undef SOLID
    (void)nsolid;
    return 0;
}

/* ---- holes ---- */

static double cross2(const double *o, const double *p, const double *q) {
    return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0]);
}
static int pt_cmp(const void *a, const void *b) {
    const double *x = a, *y = b;
    return x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : x[1] < y[1] ? -1 : x[1] > y[1];
}
/* Strictly inside the polygon (even-odd), and not within tol of an edge. */
static double seg_dist2(const double *p, const double *a, const double *b) {
    double d[2] = {b[0] - a[0], b[1] - a[1]}, l2 = d[0] * d[0] + d[1] * d[1];
    double s = l2 > 0 ? ((p[0] - a[0]) * d[0] + (p[1] - a[1]) * d[1]) / l2 : 0;
    s = s < 0 ? 0 : s > 1 ? 1 : s;
    double e[2] = {a[0] + d[0] * s - p[0], a[1] + d[1] * s - p[1]};
    return e[0] * e[0] + e[1] * e[1];
}
static int inside(const double *p, const cc_ploop *l, double tol) {
    int in = 0;
    for (size_t i = 0, j = l->n - 1; i < l->n; j = i++) {
        const double *a = l->p[i], *b = l->p[j];
        if (seg_dist2(p, a, b) <= tol * tol) return 0;
        if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]) in = !in;
    }
    return in;
}
/* q on segment a-b, given it is on the line. */
static int on_seg(const double *a, const double *b, const double *q) {
    return q[0] >= mn(a[0], b[0]) && q[0] <= mx(a[0], b[0]) && q[1] >= mn(a[1], b[1]) && q[1] <= mx(a[1], b[1]);
}
/* The segments meet (cross or touch). */
static int segs_cross(const double *a, const double *b, const double *c, const double *d) {
    double d1 = cross2(c, d, a), d2 = cross2(c, d, b), d3 = cross2(a, b, c), d4 = cross2(a, b, d);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) return 1;
    return (d1 == 0 && on_seg(c, d, a)) || (d2 == 0 && on_seg(c, d, b)) || (d3 == 0 && on_seg(a, b, c)) || (d4 == 0 && on_seg(a, b, d));
}
static int loops_cross(const cc_ploop *x, const cc_ploop *y) {
    for (size_t i = 0; i < x->n; i++)
        for (size_t j = 0; j < y->n; j++)
            if (segs_cross(x->p[i], x->p[(i + 1) % x->n], y->p[j], y->p[(j + 1) % y->n])) return 1;
    return 0;
}

/* The convex prism along z through 0..depth a triangle set is: 0 with its hull (clockwise) in
 * *out; 1 not one. */
static int as_prism(const cc_tri_set *s, double depth, Arena *a, cc_ploop *out) {
    if (s->n < 8) return 1;
    double zlo = 1.0 / 0.0, zhi = -1.0 / 0.0;
    for (size_t i = 0; i < s->n; i++)
        for (int c = 0; c < 3; c++) zlo = mn(zlo, s->t[i][3 * c + 2]), zhi = mx(zhi, s->t[i][3 * c + 2]);
    double tz = 1e-9 * mx(1.0, depth);
    if (!(zlo <= tz && zhi >= depth - tz)) return 1; /* not through */
    double(*pt)[2] = arena_alloc(a, 3 * s->n * sizeof *pt, 8);
    double(*h)[2] = arena_alloc(a, (3 * s->n + 1) * sizeof *h, 8);
    if (!pt || !h) return -1;
    size_t n = 0;
    for (size_t i = 0; i < s->n; i++)
        for (int c = 0; c < 3; c++) {
            double z = s->t[i][3 * c + 2];
            if (ab(z - zlo) > 1e-9 * mx(1.0, zhi - zlo) && ab(z - zhi) > 1e-9 * mx(1.0, zhi - zlo)) return 1; /* a third level */
            pt[n][0] = s->t[i][3 * c], pt[n][1] = s->t[i][3 * c + 1], n++;
        }
    qsort(pt, n, sizeof *pt, pt_cmp);
    size_t k = 0; /* Andrew's monotone chain, counter-clockwise */
    for (size_t i = 0; i < n; i++) {
        while (k >= 2 && cross2(h[k - 2], h[k - 1], pt[i]) <= 0) k--;
        memcpy(h[k++], pt[i], sizeof *h);
    }
    for (size_t i = n - 1, t = k + 1; i-- > 0;) {
        while (k >= t && cross2(h[k - 2], h[k - 1], pt[i]) <= 0) k--;
        memcpy(h[k++], pt[i], sizeof *h);
    }
    if (k < 4) return 1;
    k--; /* the last is the first */
    double area = 0;
    for (size_t i = 0; i < k; i++) area += cross2((double[]){0, 0}, h[i], h[(i + 1) % k]) / 2.0;
    double v, ar;
    vol_area(s, &v, &ar);
    double want = area * (zhi - zlo);
    if (!(area > 0) || ab(ab(v) - want) > 1e-9 * want) return 1;
    for (size_t i = 0; i < k / 2; i++) { /* clockwise: a void loop */
        double t[2];
        memcpy(t, h[i], sizeof t), memcpy(h[i], h[k - 1 - i], sizeof t), memcpy(h[k - 1 - i], t, sizeof t);
    }
    out->p = h, out->n = k;
    return 0;
}

int cc_prism_cut_holes(const cc_ploop *loops, size_t nloops, double depth, const cc_tri_set *cut, size_t nc, Arena *a,
                       cc_ploop **holes) {
    if (!nloops || !nc || !(depth > 0) || loops[0].n < 3) return 1;
    cc_ploop *h = arena_alloc(a, nc * sizeof *h, 8);
    if (!h) return -1;
    double ext = 0;
    for (size_t i = 0; i < loops[0].n; i++) ext = mx(ext, mx(ab(loops[0].p[i][0]), ab(loops[0].p[i][1])));
    double tol = 1e-6 * mx(1.0, ext);
    for (size_t i = 0; i < nc; i++) {
        int rc = as_prism(&cut[i], depth, a, &h[i]);
        if (rc) return rc;
        for (size_t v = 0; v < h[i].n; v++)
            if (!inside(h[i].p[v], &loops[0], tol)) return 1; /* touches or leaves the outline */
        if (loops_cross(&h[i], &loops[0])) return 1;
        for (size_t l = 1; l < nloops; l++) { /* apart from the voids */
            if (loops_cross(&h[i], &loops[l]) || inside(loops[l].p[0], &h[i], 0) || inside(h[i].p[0], &loops[l], 0)) return 1;
        }
        for (size_t j = 0; j < i; j++) /* and from each other */
            if (loops_cross(&h[i], &h[j]) || inside(h[j].p[0], &h[i], 0) || inside(h[i].p[0], &h[j], 0)) return 1;
    }
    *holes = h;
    return 0;
}
