/* cut.rs and crop.rs of bimble-drawing, byte for byte (draw_cut.h). */
#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */
#include "draw_cut.h"
#include <stdlib.h>
#include <string.h>

static double f64_bits(uint64_t u) {
    double x;
    memcpy(&x, &u, 8);
    return x;
}

/* Constants by their bits (no compiler strtod). */
#define WELD_MM f64_bits(0x3f1a36e2eb1c432dull)   /* 1e-4 */
#define TINY f64_bits(0x3d719799812dea11ull)      /* 1e-12 */
#define FACING_MIN_NZ f64_bits(0x3f947ae147ae147bull) /* 0.02 */

static double fmax_rs(double a, double b) { return a != a ? b : b != b ? a : a > b ? a : b; } /* f64::max */
static double fmin_rs(double a, double b) { return a != a ? b : b != b ? a : a < b ? a : b; } /* f64::min */
static double fabs_(double x) { return x < 0 ? -x : x; }

/* (x / WELD_MM).round() as i64: half away from zero, saturating, NaN -> 0. */
static int64_t weld(double x) {
    double q = x / WELD_MM;
    if (q != q) return 0;
    if (q >= 9223372036854775808.0) return INT64_MAX;
    if (q <= -9223372036854775808.0) return INT64_MIN;
    if (fabs_(q) >= 4503599627370496.0) return (int64_t)q; /* 2^52: already whole */
    int64_t t = (int64_t)q;                                 /* towards zero */
    double f = q - (double)t;                               /* exact */
    if (f >= 0.5) t++;
    else if (f <= -0.5) t--;
    return t;
}

typedef struct {
    int64_t k[2];
} Key;

static Key key(const double p[2]) {
    Key k = {{weld(p[0]), weld(p[1])}};
    return k;
}

static int key_eq(Key a, Key b) { return a.k[0] == b.k[0] && a.k[1] == b.k[1]; }

/* ---- chain ---- */

typedef struct {
    Key k;
    size_t order; /* 2 * segment + end: the order Rust pushes into its map's lists */
} Entry;

static int entry_cmp(const void *x, const void *y) {
    const Entry *a = x, *b = y;
    for (int i = 0; i < 2; i++)
        if (a->k.k[i] != b->k.k[i]) return a->k.k[i] < b->k.k[i] ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}

typedef struct {
    const double (*segs)[2][2];
    size_t n;
    Entry *at; /* 2n entries sorted by key, then order */
    unsigned char *used;
    double (*pts)[2]; /* scratch for one walk: at most n + 1 points */
    size_t npts;
} Chain;

/* Index of the first entry with key k, or 2n. */
static size_t find_key(const Chain *c, Key k) {
    size_t lo = 0, hi = 2 * c->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const Key *m = &c->at[mid].k;
        int less = m->k[0] < k.k[0] || (m->k[0] == k.k[0] && m->k[1] < k.k[1]);
        if (less) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Entries under key(p): 1 when exactly one (a free end). */
static int is_free(const Chain *c, const double p[2]) {
    Key k = key(p);
    size_t i = find_key(c, k), n = 0;
    while (i < 2 * c->n && key_eq(c->at[i].k, k) && n < 2) {
        i++;
        n++;
    }
    return n == 1;
}

/* Walks from segment `start`; 1 if the chain closed. Points in c->pts. */
static int walk(Chain *c, size_t start, int reverse) {
    c->used[start] = 1;
    const double(*s)[2] = c->segs[start];
    memcpy(c->pts[0], s[reverse ? 1 : 0], sizeof c->pts[0]);
    memcpy(c->pts[1], s[reverse ? 0 : 1], sizeof c->pts[1]);
    c->npts = 2;
    Key first = key(c->pts[0]);
    for (;;) {
        Key end = key(c->pts[c->npts - 1]);
        size_t i = find_key(c, end), j = (size_t)-1;
        for (; i < 2 * c->n && key_eq(c->at[i].k, end); i++) {
            size_t cand = c->at[i].order / 2;
            if (!c->used[cand]) {
                j = cand;
                break;
            }
        }
        if (j == (size_t)-1) break;
        c->used[j] = 1;
        const double(*t)[2] = c->segs[j];
        const double *p = key_eq(key(t[0]), end) ? t[1] : t[0];
        if (key_eq(key(p), first)) return 1;
        memcpy(c->pts[c->npts++], p, sizeof c->pts[0]);
    }
    return 0;
}

typedef struct {
    cc_ring *v;
    size_t n, cap;
} Rings;

static int rings_push(Rings *r, const double (*pts)[2], size_t n, Arena *a) {
    if (r->n == r->cap) {
        size_t cap = r->cap ? 2 * r->cap : 8;
        cc_ring *v = arena_alloc(a, cap * sizeof *v, _Alignof(cc_ring));
        if (!v) return -1;
        if (r->n) memcpy(v, r->v, r->n * sizeof *v);
        r->v = v;
        r->cap = cap;
    }
    double(*p)[2] = arena_dup(a, pts, n * sizeof *pts);
    if (n && !p) return -1;
    r->v[r->n].p = p;
    r->v[r->n].n = n;
    r->n++;
    return 0;
}

int cc_cut_chain(const double (*segs)[2][2], size_t n, Arena *a, cc_section *out) {
    memset(out, 0, sizeof *out);
    if (n == 0) return 0;
    Chain c = {segs, n, malloc(2 * n * sizeof(Entry)), calloc(n, 1), malloc((n + 1) * sizeof(double[2])), 0};
    Rings loops = {0}, open = {0};
    int rc = -1;
    if (!c.at || !c.used || !c.pts) goto done;
    for (size_t i = 0; i < n; i++)
        for (int e = 0; e < 2; e++) {
            c.at[2 * i + (size_t)e].k = key(segs[i][e]);
            c.at[2 * i + (size_t)e].order = 2 * i + (size_t)e;
        }
    qsort(c.at, 2 * n, sizeof *c.at, entry_cmp);
    /* Open chains first, each from one of its free ends. */
    for (size_t i = 0; i < n; i++) {
        if (c.used[i]) continue;
        int reverse;
        if (is_free(&c, segs[i][0])) reverse = 0;
        else if (is_free(&c, segs[i][1])) reverse = 1;
        else continue;
        int closed = walk(&c, i, reverse);
        if (rings_push(closed ? &loops : &open, (const double(*)[2])c.pts, c.npts, a)) goto done;
    }
    for (size_t i = 0; i < n; i++) {
        if (c.used[i]) continue;
        int closed = walk(&c, i, 0);
        if (rings_push(closed ? &loops : &open, (const double(*)[2])c.pts, c.npts, a)) goto done;
    }
    out->loops = loops.v;
    out->nloops = loops.n;
    out->open = open.v;
    out->nopen = open.n;
    rc = 0;
done:
    free(c.at);
    free(c.used);
    free(c.pts);
    return rc;
}

/* ---- section ---- */

/* Lexicographic a <= b of 3 doubles (Rust's array PartialOrd, no NaN). */
static int le3(const double a[3], const double b[3]) {
    for (int i = 0; i < 3; i++) {
        if (a[i] < b[i]) return 1;
        if (a[i] > b[i]) return 0;
    }
    return 1;
}

static void edge_point(const double *a, const double *b, double h, double out[2]) {
    if (!le3(a, b)) {
        const double *t = a;
        a = b;
        b = t;
    }
    double t = (h - a[2]) / (b[2] - a[2]);
    out[0] = a[0] + (b[0] - a[0]) * t;
    out[1] = a[1] + (b[1] - a[1]) * t;
}

int cc_cut_section(const cc_mesh *m, double h, Arena *a, cc_section *out) {
    size_t nt = m->ni / 3, n = 0;
    double(*segs)[2][2] = malloc((nt ? nt : 1) * sizeof *segs);
    if (!segs) return -1;
    for (size_t t = 0; t < nt; t++) {
        double p[3][3];
        int above[3], na = 0;
        for (int k = 0; k < 3; k++) {
            const float *q = m->pos[m->idx[3 * t + (size_t)k]];
            for (int c = 0; c < 3; c++) p[k][c] = (double)q[c];
            above[k] = p[k][2] >= h;
            na += above[k];
        }
        if (na == 0 || na == 3) continue;
        double pts[2][2];
        int np = 0;
        for (int k = 0; k < 3; k++) {
            int i = k, j = (k + 1) % 3;
            if (above[i] != above[j] && np < 2) edge_point(p[i], p[j], h, pts[np++]);
        }
        if (np == 2 && !key_eq(key(pts[0]), key(pts[1]))) {
            memcpy(segs[n], pts, sizeof pts);
            n++;
        }
    }
    int rc = cc_cut_chain((const double(*)[2][2])segs, n, a, out);
    free(segs);
    return rc;
}

/* ---- facing_up_region ---- */

typedef struct {
    int64_t k[6]; /* the edge's two 3D keys, smaller first */
    double s[2][2]; /* the segment as first seen */
    size_t order;
} Edge;

static int edge_cmp(const void *x, const void *y) {
    const Edge *a = x, *b = y;
    for (int i = 0; i < 6; i++)
        if (a->k[i] != b->k[i]) return a->k[i] < b->k[i] ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}

/* f64::total_cmp as a signed integer order. */
static int64_t total_key(double x) {
    int64_t v;
    memcpy(&v, &x, 8);
    return v ^ (int64_t)(((uint64_t)(v >> 63)) >> 1);
}

static int seg_total_cmp(const void *x, const void *y) {
    const double *a = x, *b = y;
    for (int i = 0; i < 4; i++) {
        int64_t p = total_key(a[i]), q = total_key(b[i]);
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

int cc_cut_facing_up(const cc_mesh *m, Arena *a, cc_ring **loops, size_t *nloops) {
    *loops = NULL;
    *nloops = 0;
    size_t nt = m->ni / 3, ne = 0;
    Edge *e = malloc((3 * nt ? 3 * nt : 1) * sizeof *e);
    if (!e) return -1;
    for (size_t t = 0; t < nt; t++) {
        double p[3][3];
        for (int k = 0; k < 3; k++) {
            const float *q = m->pos[m->idx[3 * t + (size_t)k]];
            for (int c = 0; c < 3; c++) p[k][c] = (double)q[c];
        }
        double u[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
        double v[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
        double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        double len = cc_sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < TINY || n[2] / len <= FACING_MIN_NZ) continue;
        for (int k = 0; k < 3; k++) {
            const double *pa = p[k], *pb = p[(k + 1) % 3];
            int64_t ka[3] = {weld(pa[0]), weld(pa[1]), weld(pa[2])};
            int64_t kb[3] = {weld(pb[0]), weld(pb[1]), weld(pb[2])};
            int a_first = 1;
            for (int i = 0; i < 3; i++)
                if (ka[i] != kb[i]) {
                    a_first = ka[i] < kb[i];
                    break;
                }
            Edge *x = &e[ne];
            memcpy(x->k, a_first ? ka : kb, sizeof ka);
            memcpy(x->k + 3, a_first ? kb : ka, sizeof ka);
            x->s[0][0] = pa[0];
            x->s[0][1] = pa[1];
            x->s[1][0] = pb[0];
            x->s[1][1] = pb[1];
            x->order = ne++;
        }
    }
    qsort(e, ne, sizeof *e, edge_cmp);
    double(*segs)[2][2] = malloc((ne ? ne : 1) * sizeof *segs);
    if (!segs) {
        free(e);
        return -1;
    }
    size_t ns = 0;
    for (size_t i = 0; i < ne;) {
        size_t j = i + 1;
        while (j < ne && memcmp(e[j].k, e[i].k, sizeof e[i].k) == 0) j++;
        if (j - i == 1 && !key_eq(key(e[i].s[0]), key(e[i].s[1]))) memcpy(segs[ns++], e[i].s, sizeof e[i].s);
        i = j;
    }
    free(e);
    qsort(segs, ns, sizeof *segs, seg_total_cmp);
    cc_section s;
    int rc = cc_cut_chain((const double(*)[2][2])segs, ns, a, &s);
    free(segs);
    if (rc) return rc;
    *loops = s.loops;
    *nloops = s.nloops;
    return 0;
}

/* ---- small ones ---- */

int cc_cut_clip_z(const double a[3], const double b[3], double z0, double z1, double out[2][2]) {
    double t0 = 0.0, t1 = 1.0, dz = b[2] - a[2];
    if (fabs_(dz) < TINY) {
        if (a[2] < z0 || a[2] > z1) return 0;
    } else {
        double ta = (z0 - a[2]) / dz, tb = (z1 - a[2]) / dz;
        double lo = ta < tb ? ta : tb, hi = ta < tb ? tb : ta;
        t0 = fmax_rs(t0, lo);
        t1 = fmin_rs(t1, hi);
        if (t0 > t1) return 0;
    }
    out[0][0] = a[0] + (b[0] - a[0]) * t0;
    out[0][1] = a[1] + (b[1] - a[1]) * t0;
    out[1][0] = a[0] + (b[0] - a[0]) * t1;
    out[1][1] = a[1] + (b[1] - a[1]) * t1;
    return 1;
}

int cc_cut_edge_seen_from_above(const int8_t n[8]) {
    int unknown0 = n[0] == 0 && n[1] == 0 && n[2] == 0, unknown1 = n[4] == 0 && n[5] == 0 && n[6] == 0;
    if (unknown0 || unknown1) return 1;
    return n[2] > 2 || n[6] > 2;
}

double cc_cut_area(const cc_ring *l) {
    double s = -0.0;
    for (size_t i = 0; i < l->n; i++) {
        const double *a = l->p[i], *b = l->p[(i + 1) % l->n];
        s += a[0] * b[1] - b[0] * a[1];
    }
    return s / 2.0;
}

/* ---- crop ---- */

int cc_crop_contains(const cc_crop_rect *r, const double p[2]) {
    for (int k = 0; k < 2; k++)
        if (!(p[k] >= r->lo[k] && p[k] <= r->hi[k])) return 0;
    return 1;
}

int cc_crop_polygon(const cc_crop_rect *r, const cc_ring *poly, Arena *a, cc_ring *out) {
    out->p = NULL;
    out->n = 0;
    /* Each side adds at most one point per edge: n + 4 bounds every pass. */
    size_t cap = poly->n * 2 + 8;
    double(*cur)[2] = malloc(cap * sizeof *cur), (*nxt)[2] = malloc(cap * sizeof *nxt);
    if (!cur || !nxt) {
        free(cur);
        free(nxt);
        return -1;
    }
    size_t n = poly->n;
    if (n) memcpy(cur, poly->p, n * sizeof *cur);
    const struct {
        int k;
        int hi;
    } sides[4] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
    for (int s = 0; s < 4; s++) {
        if (n == 0) break;
        int k = sides[s].k, below = sides[s].hi;
        double v = below ? r->hi[k] : r->lo[k];
        size_t m = 0;
        for (size_t i = 0; i < n; i++) {
            const double *b = cur[i], *pa = cur[(i + n - 1) % n];
            int ia = below ? pa[k] <= v : pa[k] >= v, ib = below ? b[k] <= v : b[k] >= v;
            if (ia && ib) {
                memcpy(nxt[m++], b, sizeof nxt[0]);
            } else if (ia != ib) {
                double t = (v - pa[k]) / (b[k] - pa[k]);
                double q[2] = {pa[0] + (b[0] - pa[0]) * t, pa[1] + (b[1] - pa[1]) * t};
                q[k] = v;
                memcpy(nxt[m++], q, sizeof q);
                if (ib) memcpy(nxt[m++], b, sizeof nxt[0]);
            }
            if (m + 2 > cap) break; /* cannot happen: at most 2 points per input point */
        }
        double(*t)[2] = cur;
        cur = nxt;
        nxt = t;
        n = m;
    }
    int rc = 0;
    if (n >= 3) {
        out->p = arena_dup(a, cur, n * sizeof *cur);
        if (!out->p) rc = -1;
        else out->n = n;
    }
    free(cur);
    free(nxt);
    return rc;
}

int cc_crop_segment(const cc_crop_rect *r, const double s[2][2], double out[2][2]) {
    double d[2] = {s[1][0] - s[0][0], s[1][1] - s[0][1]};
    double t0 = 0.0, t1 = 1.0;
    for (int k = 0; k < 2; k++) {
        double pq[2][2] = {{-d[k], s[0][k] - r->lo[k]}, {d[k], r->hi[k] - s[0][k]}};
        for (int i = 0; i < 2; i++) {
            double p = pq[i][0], q = pq[i][1];
            if (p == 0.0) {
                if (q < 0.0) return 0;
            } else {
                double rr = q / p;
                if (p < 0.0) t0 = fmax_rs(t0, rr);
                else t1 = fmin_rs(t1, rr);
            }
        }
    }
    if (!(t0 <= t1)) return 0;
    out[0][0] = s[0][0] + d[0] * t0;
    out[0][1] = s[0][1] + d[1] * t0;
    out[1][0] = s[0][0] + d[0] * t1;
    out[1][1] = s[0][1] + d[1] * t1;
    return 1;
}

int cc_crop_polyline(const cc_crop_rect *r, const cc_ring *line, Arena *a, cc_ring **pieces, size_t *npieces) {
    *pieces = NULL;
    *npieces = 0;
    if (line->n < 2) return 0;
    /* Pieces share one scratch array of points: each segment adds at most 2. */
    double(*pts)[2] = malloc(2 * line->n * sizeof *pts);
    size_t *start = malloc(line->n * sizeof *start), np = 0, npc = 0;
    if (!pts || !start) {
        free(pts);
        free(start);
        return -1;
    }
    for (size_t i = 0; i + 1 < line->n; i++) {
        double seg[2][2], c[2][2];
        memcpy(seg[0], line->p[i], sizeof seg[0]);
        memcpy(seg[1], line->p[i + 1], sizeof seg[1]);
        if (!cc_crop_segment(r, (const double(*)[2])seg, c)) continue;
        if (npc && pts[np - 1][0] == c[0][0] && pts[np - 1][1] == c[0][1]) {
            memcpy(pts[np++], c[1], sizeof c[1]);
        } else {
            start[npc++] = np;
            memcpy(pts[np++], c[0], sizeof c[0]);
            memcpy(pts[np++], c[1], sizeof c[1]);
        }
    }
    int rc = 0;
    if (npc) {
        cc_ring *v = arena_alloc(a, npc * sizeof *v, _Alignof(cc_ring));
        if (!v) rc = -1;
        for (size_t k = 0; !rc && k < npc; k++) {
            size_t e = k + 1 < npc ? start[k + 1] : np;
            v[k].n = e - start[k];
            v[k].p = arena_dup(a, pts + start[k], v[k].n * sizeof *pts);
            if (!v[k].p) rc = -1;
        }
        if (!rc) {
            *pieces = v;
            *npieces = npc;
        }
    }
    free(pts);
    free(start);
    return rc;
}
