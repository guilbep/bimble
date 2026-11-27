/* boolean.rs in C: apply, weld, snap (portable), and the manifold adapter (CC_HAVE_MANIFOLD). */
#include "boolean.h"
#include <stdlib.h>
#include <string.h>
#include "earcut.h"
#include "math.h"

/* ---- small helpers (no libm: wasm is freestanding) */

static double f_min(double a, double b) { return a != a ? b : b != b ? a : a < b ? a : b; } /* f64::min */
static double f_max(double a, double b) { return a != a ? b : b != b ? a : a > b ? a : b; } /* f64::max */
static double f_floor(double x) {
    double ax = x < 0 ? -x : x;
    if (!(ax < 4503599627370496.0)) return x; /* 2^52: an integer already (or NaN, inf) */
    double t = (double)(int64_t)x;
    if (t > x) t -= 1.0;
    return t == 0.0 && x < 0 ? -0.0 : t;
}
static double f_round(double x) { /* f64::round: half away from zero */
    double ax = x < 0 ? -x : x;
    if (!(ax < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x, d = x - t;
    if (d >= 0.5) t += 1.0;
    else if (d <= -0.5) t -= 1.0;
    if (t == 0.0) return x < 0 ? -0.0 : 0.0;
    return t;
}
static int64_t f_to_i64(double x) { /* `as i64`: saturating, NaN -> 0 */
    if (x != x) return 0;
    if (x >= 9223372036854775807.0) return INT64_MAX;
    if (x <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)x;
}
static uint64_t bits(double x) {
    uint64_t u;
    memcpy(&u, &x, 8);
    return u;
}
static uint64_t mix(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}
static void put_err(char *err, const char *a, const char *b) {
    if (!err) return;
    size_t n = 0;
    for (; a && *a && n + 1 < CC_BOOL_ERR; a++) err[n++] = *a;
    for (; b && *b && n + 1 < CC_BOOL_ERR; b++) err[n++] = *b;
    err[n] = 0;
}

/* ---- the kernel table */

#ifdef CC_HAVE_MANIFOLD
static const cc_boolean MANIFOLD_TABLE;
static const cc_boolean *g_kernel = &MANIFOLD_TABLE;
const cc_boolean *cc_boolean_manifold(void) { return &MANIFOLD_TABLE; }
#else
static const cc_boolean *g_kernel = NULL;
const cc_boolean *cc_boolean_manifold(void) { return NULL; }
#endif
const cc_boolean *cc_boolean_kernel(void) { return g_kernel; }
void cc_boolean_set(const cc_boolean *k) { g_kernel = k; }

/* ---- weld: shared vertices welded exactly (same bits after the shift to `origin`) */

static int weld(cc_tri_set t, const double origin[3], cc_bmesh *m) {
    memset(m, 0, sizeof *m);
    size_t np = 3 * t.n, cap = 16;
    while (cap < 2 * np) cap *= 2;
    uint32_t *slot = calloc(cap, sizeof *slot); /* vertex index + 1, 0 empty */
    m->pos = malloc((np ? np : 1) * 3 * sizeof *m->pos);
    m->idx = malloc((np ? np : 1) * sizeof *m->idx);
    if (!slot || !m->pos || !m->idx || np > 0xfffffffeu) {
        free(slot), free(m->pos), free(m->idx);
        memset(m, 0, sizeof *m);
        return -1;
    }
    for (size_t i = 0; i < np; i++) {
        const double *p = &t.t[i / 3][3 * (i % 3)];
        /* `+ 0.0` turns -0.0 into 0.0 so equal coordinates weld. */
        double q[3] = {p[0] - origin[0] + 0.0, p[1] - origin[1] + 0.0, p[2] - origin[2] + 0.0};
        uint64_t k[3] = {bits(q[0]), bits(q[1]), bits(q[2])};
        size_t h = (size_t)mix(k[0] ^ mix(k[1] ^ mix(k[2]))) & (cap - 1);
        for (;;) {
            uint32_t s = slot[h];
            if (!s) {
                memcpy(&m->pos[3 * m->nv], q, sizeof q);
                slot[h] = (uint32_t)(++m->nv);
                m->idx[i] = m->nv - 1;
                break;
            }
            const double *v = &m->pos[3 * (s - 1)];
            if (bits(v[0]) == k[0] && bits(v[1]) == k[1] && bits(v[2]) == k[2]) {
                m->idx[i] = s - 1;
                break;
            }
            h = (h + 1) & (cap - 1);
        }
    }
    m->nt = t.n;
    free(slot);
    return 0;
}
static void bmesh_free(cc_bmesh *m) {
    free(m->pos), free(m->idx);
    memset(m, 0, sizeof *m);
}

/* ---- snap */

int cc_boolean_snap(cc_tri_set t, double tol, double (**out)[9], size_t *nout) {
    *out = NULL, *nout = 0;
    size_t np = 3 * t.n, cap = 16;
    while (cap < 2 * np) cap *= 2;
    typedef struct {
        int64_t k[3];
        uint32_t head, tail; /* point index + 1 */
    } Cell;
    Cell *cells = calloc(cap, sizeof *cells);
    double (*pts)[3] = malloc((np ? np : 1) * sizeof *pts);
    uint32_t *next = malloc((np ? np : 1) * sizeof *next);
    double (*o)[9] = malloc((t.n ? t.n : 1) * sizeof *o);
    size_t npts = 0, no = 0;
    if (!cells || !pts || !next || !o || np > 0xfffffffeu) {
        free(cells), free(pts), free(next), free(o);
        return -1;
    }
    for (size_t ti = 0; ti < t.n; ti++) {
        double q[3][3];
        for (int c = 0; c < 3; c++) {
            const double *p = &t.t[ti][3 * c];
            int64_t k[3];
            for (int r = 0; r < 3; r++) k[r] = f_to_i64(f_floor(p[r] / tol));
            const double *found = NULL;
            for (int dx = -1; dx <= 1 && !found; dx++)
                for (int dy = -1; dy <= 1 && !found; dy++)
                    for (int dz = -1; dz <= 1 && !found; dz++) {
                        int64_t kk[3] = {(int64_t)((uint64_t)k[0] + (uint64_t)(int64_t)dx),
                                         (int64_t)((uint64_t)k[1] + (uint64_t)(int64_t)dy),
                                         (int64_t)((uint64_t)k[2] + (uint64_t)(int64_t)dz)};
                        size_t h = (size_t)mix((uint64_t)kk[0] ^ mix((uint64_t)kk[1] ^ mix((uint64_t)kk[2]))) & (cap - 1);
                        while (cells[h].head && memcmp(cells[h].k, kk, sizeof kk)) h = (h + 1) & (cap - 1);
                        for (uint32_t s = cells[h].head; s && !found; s = next[s - 1]) {
                            const double *v = pts[s - 1];
                            double d0 = v[0] - p[0], d1 = v[1] - p[1], d2 = v[2] - p[2];
                            if ((d0 < 0 ? -d0 : d0) <= tol && (d1 < 0 ? -d1 : d1) <= tol && (d2 < 0 ? -d2 : d2) <= tol)
                                found = v;
                        }
                    }
            if (found) {
                memcpy(q[c], found, sizeof q[c]);
                continue;
            }
            size_t h = (size_t)mix((uint64_t)k[0] ^ mix((uint64_t)k[1] ^ mix((uint64_t)k[2]))) & (cap - 1);
            while (cells[h].head && memcmp(cells[h].k, k, sizeof k)) h = (h + 1) & (cap - 1);
            memcpy(pts[npts], p, sizeof pts[npts]);
            next[npts] = 0;
            npts++;
            if (!cells[h].head) {
                memcpy(cells[h].k, k, sizeof k);
                cells[h].head = (uint32_t)npts;
            } else next[cells[h].tail - 1] = (uint32_t)npts;
            cells[h].tail = (uint32_t)npts;
            memcpy(q[c], p, sizeof q[c]);
        }
        int eq01 = q[0][0] == q[1][0] && q[0][1] == q[1][1] && q[0][2] == q[1][2];
        int eq12 = q[1][0] == q[2][0] && q[1][1] == q[2][1] && q[1][2] == q[2][2];
        int eq02 = q[0][0] == q[2][0] && q[0][1] == q[2][1] && q[0][2] == q[2][2];
        if (eq01 || eq12 || eq02) continue;
        memcpy(o[no++], q, sizeof o[0]);
    }
    free(cells), free(pts), free(next);
    *out = o, *nout = no;
    return 0;
}

/* ---- apply */

static int kernel(const cc_boolean *k, cc_bool_op op, cc_tri_set a, const cc_tri_set *b, size_t nb,
                  const double origin[3], double (**out)[9], size_t *nout, char *err) {
    cc_bmesh *m = calloc(nb + 1, sizeof *m), r;
    memset(&r, 0, sizeof r);
    int rc = m ? 0 : -1;
    for (size_t i = 0; rc == 0 && i <= nb; i++) rc = weld(i ? b[i - 1] : a, origin, &m[i]);
    if (rc == 0) {
        rc = (op == CC_BOOL_DIFFERENCE ? k->difference : op == CC_BOOL_UNION ? k->unite : k->intersection)(
            &m[0], &m[1], nb, &r, err);
    }
    for (size_t i = 0; m && i <= nb; i++) bmesh_free(&m[i]);
    free(m);
    if (rc) {
        bmesh_free(&r);
        return rc;
    }
    double (*o)[9] = malloc((r.nt ? r.nt : 1) * sizeof *o);
    if (!o) {
        bmesh_free(&r);
        return -1;
    }
    for (size_t t = 0; t < r.nt; t++)
        for (int c = 0; c < 3; c++) {
            uint64_t v = r.idx[3 * t + c];
            if (v >= r.nv) { /* a bad mesh from the kernel */
                free(o);
                bmesh_free(&r);
                put_err(err, "bad mesh from the kernel", NULL);
                return CC_BOOL_FAILED;
            }
            for (int q = 0; q < 3; q++) o[t][3 * c + q] = r.pos[3 * v + q] + origin[q];
        }
    *out = o, *nout = r.nt;
    bmesh_free(&r);
    return 0;
}

int cc_boolean_apply(cc_bool_op op, cc_tri_set a, const cc_tri_set *b, size_t nb, double (**out)[9], size_t *nout,
                     char *err) {
    *out = NULL, *nout = 0;
    if (err) err[0] = 0;
    cc_tri_set *bb = malloc((nb ? nb : 1) * sizeof *bb);
    if (!bb) return -1;
    size_t n = 0;
    for (size_t i = 0; i < nb; i++)
        if (b[i].n) bb[n++] = b[i];
    if (!n) { /* nothing to cut: the input (an intersection: nothing) */
        free(bb);
        if (op == CC_BOOL_INTERSECTION || !a.n) return 0;
        double (*o)[9] = malloc(a.n * sizeof *o);
        if (!o) return -1;
        memcpy(o, a.t, a.n * sizeof *o);
        *out = o, *nout = a.n;
        return 0;
    }
    const cc_boolean *k = g_kernel;
    if (!k) {
        free(bb);
        put_err(err, "no boolean kernel", NULL);
        return CC_BOOL_NO_KERNEL;
    }
    /* bounds over a then b: f64::min / max from the first point. */
    double lo[3], hi[3];
    int any = 0;
    for (size_t s = 0; s <= n; s++) {
        cc_tri_set t = s ? bb[s - 1] : a;
        for (size_t i = 0; i < t.n; i++)
            for (int c = 0; c < 3; c++) {
                const double *p = &t.t[i][3 * c];
                for (int q = 0; q < 3; q++) {
                    if (!any) lo[q] = hi[q] = p[q];
                    else lo[q] = f_min(lo[q], p[q]), hi[q] = f_max(hi[q], p[q]);
                }
                any = 1;
            }
    }
    if (!any) {
        free(bb);
        return 0;
    }
    /* Coordinates relative to the middle keep full precision with site coordinates. */
    double origin[3];
    for (int q = 0; q < 3; q++) origin[q] = f_round((lo[q] + hi[q]) / 2.0);
    int rc = kernel(k, op, a, bb, n, origin, out, nout, err);
    if (rc == CC_BOOL_NOT_CLOSED) {
        /* Solids exported with corners a hair apart (Revit Breps: 0.03-0.07 mm) are not closed for
         * the kernel: once more with points that close welded together, as IfcOpenShell sews them. */
        double (*sa)[9] = NULL, (**sb)[9] = calloc(n, sizeof *sb);
        size_t nsa = 0, *nsb = calloc(n, sizeof *nsb);
        rc = sb && nsb ? cc_boolean_snap(a, CC_SEW_MM, &sa, &nsa) : -1;
        for (size_t i = 0; rc == 0 && i < n; i++) rc = cc_boolean_snap(bb[i], CC_SEW_MM, &sb[i], &nsb[i]);
        if (rc == 0) {
            cc_tri_set as = {(const double(*)[9])sa, nsa};
            for (size_t i = 0; i < n; i++) bb[i].t = (const double(*)[9])sb[i], bb[i].n = nsb[i];
            if (err) err[0] = 0;
            rc = kernel(k, op, as, bb, n, origin, out, nout, err);
        }
        free(sa);
        for (size_t i = 0; sb && i < n; i++) free(sb[i]);
        free(sb), free(nsb);
    }
    free(bb);
    return rc;
}

/* ---- TESS1 (ADR 0085): a kernel result's coplanar triangles merged, triangulated again */

/* An open-addressing set of u64 keys (never 0 after the +1) to a value. */
typedef struct {
    uint64_t *key;
    size_t *val;
    size_t cap;
} U64Map;
static int u64map_init(U64Map *m, size_t n) {
    m->cap = 16;
    while (m->cap < 2 * n) m->cap *= 2;
    m->key = calloc(m->cap, sizeof *m->key);
    m->val = malloc(m->cap * sizeof *m->val);
    return m->key && m->val ? 0 : -1;
}
static void u64map_free(U64Map *m) { free(m->key), free(m->val); }
/* The slot of key k (k + 1 stored); *found 1 when present. */
static size_t u64map_slot(const U64Map *m, uint64_t k, int *found) {
    size_t h = (size_t)mix(k) & (m->cap - 1);
    while (m->key[h] && m->key[h] != k + 1) h = (h + 1) & (m->cap - 1);
    *found = m->key[h] != 0;
    return h;
}

static double v_dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void v_sub(const double *a, const double *b, double *o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static void v_cross(const double *a, const double *b, double *o) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}

#define MERGE_MAX_LOOP 65536 /* boundary points of one region: more stays as the kernel gave it */

typedef struct { /* the working arrays of cc_boolean_merge_coplanar */
    size_t n, nv, nreg;
    const double (*t)[9];
    uint32_t *vid, *nb;  /* per corner: point id; the triangle across the edge from this corner */
    double (*pos)[3], (*nrm)[3], *area;
    unsigned char *deg;
    int32_t *reg;
    uint32_t *ord, *tmp, *stack;
    uint32_t *start, *members; /* per region its triangles: members[start[r] .. start[r + 1]) */
    uint32_t *nextv;           /* per point: the next boundary point of the region in hand */
    unsigned char *walked;
    uint32_t *lv, *lstart, *ptid; /* loop points in walk order, loop starts, earcut point -> point id */
    double *flat;
    size_t *hidx;
    unsigned char *used;
    double (*nt)[9]; /* the new triangles */
    size_t nnt, ntcap;
    uint32_t *rnew, *rnew_n;
    unsigned char *gone; /* triangles with a repeated point: dropped */
    size_t ngone;
} Merge;

static uint32_t corner_next(const Merge *g, size_t e) { return g->vid[e - e % 3 + (e % 3 + 1) % 3]; }

/* One region's triangles again: 0 done (rnew set), 1 kept as given, -1 out of memory. */
static int merge_region(Merge *g, size_t r) {
    size_t m = g->start[r + 1] - g->start[r];
    const uint32_t *mem = g->members + g->start[r];
    const double *ns = NULL;
    for (size_t i = 0; i < m && !ns; i++)
        if (!g->deg[mem[i]]) ns = g->nrm[mem[i]];
    if (m < 2 || !ns) return 1;
    int ok = 1, rc = 1;
    size_t nbd = 0, nl = 0, nlp = 0;
    /* boundary edges: the next point per point; a pinch (two from one point) keeps the region */
    for (size_t i = 0; i < m && ok; i++)
        for (int k = 0; k < 3 && ok; k++) {
            size_t e = 3 * (size_t)mem[i] + k;
            if (g->reg[g->nb[e]] == (int32_t)r) continue;
            uint32_t a = g->vid[e];
            if (g->nextv[a] != UINT32_MAX) ok = 0;
            else g->nextv[a] = corner_next(g, e), nbd++;
        }
    if (nbd < 3 || nbd > MERGE_MAX_LOOP) ok = 0;
    int ax = 0;
    double best = -1.0;
    for (int k = 0; k < 3; k++) {
        double a = ns[k] < 0 ? -ns[k] : ns[k];
        if (a > best) best = a, ax = k;
    }
    int pu = ax == 0 ? 1 : ax == 1 ? 2 : 0, pv = ax == 0 ? 2 : ax == 1 ? 0 : 1;
    double sgn = ns[ax] < 0 ? -1.0 : 1.0;
    const double *o3 = g->pos[g->vid[3 * (size_t)mem[0]]]; /* a local origin: site coordinates */
    size_t outer = SIZE_MAX, nouter = 0;
    /* the loops, walked from each boundary point in member order (deterministic) */
    for (size_t i = 0; i < m && ok; i++)
        for (int k = 0; k < 3 && ok; k++) {
            size_t e = 3 * (size_t)mem[i] + k;
            if (g->reg[g->nb[e]] == (int32_t)r) continue;
            uint32_t s0 = g->vid[e], v = s0;
            if (g->walked[s0]) continue;
            size_t at = nl;
            do {
                if (nl >= nbd || g->nextv[v] == UINT32_MAX || g->walked[v]) { ok = 0; break; }
                g->walked[v] = 1;
                g->lv[nl++] = v;
                v = g->nextv[v];
            } while (v != s0);
            if (!ok) break;
            double a2 = 0.0;
            for (size_t q = at; q < nl; q++) {
                const double *pa = g->pos[g->lv[q]], *pb = g->pos[g->lv[q + 1 < nl ? q + 1 : at]];
                a2 += (pa[pu] - o3[pu]) * (pb[pv] - o3[pv]) - (pb[pu] - o3[pu]) * (pa[pv] - o3[pv]);
            }
            if (a2 * sgn > 0.0) nouter++, outer = nlp;
            g->lstart[nlp++] = (uint32_t)at;
        }
    g->lstart[nlp] = (uint32_t)nl;
    if (!ok || nouter != 1 || nl != nbd) goto reset;
    /* earcut: the outer loop first, then the holes */
    size_t fp = 0, nh = 0;
    for (size_t pass = 0; pass < 2; pass++)
        for (size_t L = 0; L < nlp; L++) {
            if ((pass == 0) != (L == outer)) continue;
            if (pass == 1) g->hidx[nh++] = fp;
            for (size_t q = g->lstart[L]; q < g->lstart[L + 1]; q++) {
                const double *p = g->pos[g->lv[q]];
                g->flat[2 * fp] = p[pu] - o3[pu], g->flat[2 * fp + 1] = p[pv] - o3[pv];
                g->ptid[fp++] = g->lv[q];
            }
        }
    uint32_t *idx = NULL;
    size_t ni = 0;
    int ec = cc_earcut(g->flat, 2 * fp, g->hidx, nh, &idx, &ni);
    if (ec == CC_EARCUT_OOM) { rc = -1; goto reset; }
    if (ec || ni % 3 || ni / 3 >= m) { free(idx); goto reset; }
    /* every point kept (a point earcut drops is still in a neighbour: the solid would open), the
     * area unchanged */
    memset(g->used, 0, fp);
    for (size_t q = 0; q < ni; q++) g->used[idx[q]] = 1;
    for (size_t q = 0; q < fp && ok; q++) ok = g->used[q];
    double a_old = 0.0, a_new = 0.0;
    for (size_t i = 0; i < m; i++) a_old += g->area[mem[i]];
    if (ok && g->nnt + ni / 3 > g->ntcap) {
        size_t c = g->ntcap ? g->ntcap : 64;
        while (c < g->nnt + ni / 3) c *= 2;
        void *p = realloc(g->nt, c * sizeof *g->nt);
        if (!p) { free(idx); rc = -1; goto reset; }
        g->nt = p, g->ntcap = c;
    }
    size_t at = g->nnt;
    for (size_t q = 0; ok && q + 3 <= ni; q += 3) {
        const double *p[3] = {g->pos[g->ptid[idx[q]]], g->pos[g->ptid[idx[q + 1]]], g->pos[g->ptid[idx[q + 2]]]};
        double e1[3], e2[3], c[3];
        v_sub(p[1], p[0], e1), v_sub(p[2], p[0], e2), v_cross(e1, e2, c);
        double d = v_dot(c, ns);
        if (d < 0) { const double *s = p[1]; p[1] = p[2], p[2] = s, d = -d; }
        a_new += d;
        for (int j = 0; j < 3; j++) memcpy(&g->nt[at + q / 3][3 * j], p[j], 3 * sizeof(double));
    }
    free(idx);
    if (!ok || !(f_max(a_old, a_new) - f_min(a_old, a_new) <= 1e-6 * a_old + 1e-9)) goto reset;
    g->rnew[r] = (uint32_t)at, g->rnew_n[r] = (uint32_t)(ni / 3);
    g->nnt = at + ni / 3;
    rc = 0;
reset:
    for (size_t i = 0; i < m; i++)
        for (int k = 0; k < 3; k++) {
            uint32_t v = g->vid[3 * (size_t)mem[i] + k];
            g->nextv[v] = UINT32_MAX, g->walked[v] = 0;
        }
    return rc;
}

static void merge_free(Merge *g) {
    free(g->vid), free(g->nb), free(g->pos), free(g->nrm), free(g->area), free(g->deg), free(g->reg);
    free(g->ord), free(g->tmp), free(g->stack), free(g->start), free(g->members), free(g->nextv);
    free(g->walked), free(g->lv), free(g->lstart), free(g->ptid), free(g->flat), free(g->hidx), free(g->used);
    free(g->nt), free(g->rnew), free(g->rnew_n), free(g->gone);
}

int cc_boolean_merge_coplanar(const double (*t)[9], size_t n, double tol, double (**out)[9], size_t *nout) {
    *out = NULL, *nout = 0;
    if (n < 2 || n > ((size_t)1 << 26) || !(tol >= 0.0)) return 1;
    size_t np = 3 * n;
    int rc = -1;
    U64Map vm = {0}, em = {0};
    Merge g;
    memset(&g, 0, sizeof g);
    g.n = n, g.t = t;
    g.vid = malloc(np * sizeof *g.vid), g.nb = malloc(np * sizeof *g.nb);
    g.pos = malloc(np * sizeof *g.pos), g.nrm = malloc(n * sizeof *g.nrm), g.area = malloc(n * sizeof *g.area);
    g.deg = calloc(n, 1), g.reg = malloc(n * sizeof *g.reg);
    g.ord = malloc(n * sizeof *g.ord), g.tmp = malloc(n * sizeof *g.tmp), g.stack = malloc(n * sizeof *g.stack);
    if (!g.vid || !g.nb || !g.pos || !g.nrm || !g.area || !g.deg || !g.reg || !g.ord || !g.tmp || !g.stack ||
        u64map_init(&vm, np) || u64map_init(&em, np))
        goto done;
    /* 1. points welded by their bits (a kernel result shares its points exactly) */
    for (size_t i = 0; i < np; i++) {
        const double *p = &t[i / 3][3 * (i % 3)];
        uint64_t k = mix(bits(p[0] + 0.0) ^ mix(bits(p[1] + 0.0) ^ mix(bits(p[2] + 0.0))));
        size_t h = (size_t)k & (vm.cap - 1);
        for (;;) {
            if (!vm.key[h]) {
                vm.key[h] = 1, vm.val[h] = g.nv;
                memcpy(g.pos[g.nv], p, sizeof g.pos[0]);
                g.vid[i] = (uint32_t)g.nv++;
                break;
            }
            const double *q = g.pos[vm.val[h]];
            if (q[0] == p[0] && q[1] == p[1] && q[2] == p[2]) {
                g.vid[i] = (uint32_t)vm.val[h];
                break;
            }
            h = (h + 1) & (vm.cap - 1);
        }
    }
    /* 2. a triangle with a repeated point (two kernel points at the same place) has no area: dropped
     * (its two edges pair its neighbours with each other). Then directed edges, each once, each with
     * its reverse (a closed 2-manifold): else left as given. */
    g.gone = calloc(n, 1);
    if (!g.gone) goto done;
    for (size_t k = 0; k < n; k++) {
        uint32_t a = g.vid[3 * k], b = g.vid[3 * k + 1], c = g.vid[3 * k + 2];
        if (a == b || b == c || a == c) g.gone[k] = 1, g.ngone++;
    }
    for (size_t i = 0; i < np; i++) {
        if (g.gone[i / 3]) continue;
        uint32_t a = g.vid[i], b = corner_next(&g, i);
        uint64_t key = ((uint64_t)a << 32) | b;
        int found;
        size_t h = u64map_slot(&em, key, &found);
        if (found) { rc = 1; goto done; }
        em.key[h] = key + 1, em.val[h] = i;
    }
    for (size_t i = 0; i < np; i++) {
        if (g.gone[i / 3]) continue;
        uint32_t a = g.vid[i], b = corner_next(&g, i);
        int found;
        size_t h = u64map_slot(&em, ((uint64_t)b << 32) | a, &found);
        if (!found) { rc = 1; goto done; }
        g.nb[i] = (uint32_t)(em.val[h] / 3);
    }
    /* 3. normals, areas; a sliver (height under 1e-5 of its longest edge) has no reliable facing */
    for (size_t k = 0; k < n; k++) {
        const double *p0 = g.pos[g.vid[3 * k]], *p1 = g.pos[g.vid[3 * k + 1]], *p2 = g.pos[g.vid[3 * k + 2]];
        double e1[3], e2[3], e3[3], c[3];
        v_sub(p1, p0, e1), v_sub(p2, p0, e2), v_sub(p2, p1, e3), v_cross(e1, e2, c);
        double l = cc_sqrt(v_dot(c, c));
        double lmax = f_max(v_dot(e1, e1), f_max(v_dot(e2, e2), v_dot(e3, e3)));
        g.area[k] = l;
        if (!(l > 0.0) || l * l <= 1e-10 * lmax * lmax) g.deg[k] = 1, g.nrm[k][0] = g.nrm[k][1] = g.nrm[k][2] = 0.0;
        else g.nrm[k][0] = c[0] / l, g.nrm[k][1] = c[1] / l, g.nrm[k][2] = c[2] / l;
        g.reg[k] = -1;
        g.ord[k] = (uint32_t)k;
        if (g.gone[k]) g.deg[k] = 1, g.area[k] = 0.0;
    }
    /* 4. regions: seeds largest first (a stable merge sort: no qsort in wasm), grown across edges
     * onto triangles whose three points lie within tol of the seed's plane and that face its way */
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
            while (i < mid && j < hi) g.tmp[k++] = g.area[g.ord[j]] > g.area[g.ord[i]] ? g.ord[j++] : g.ord[i++];
            while (i < mid) g.tmp[k++] = g.ord[i++];
            while (j < hi) g.tmp[k++] = g.ord[j++];
        }
        memcpy(g.ord, g.tmp, n * sizeof *g.ord);
    }
    for (size_t s = 0; s < n; s++) {
        uint32_t seed = g.ord[s];
        if (g.reg[seed] >= 0 || g.gone[seed]) continue;
        int32_t r = (int32_t)g.nreg++;
        g.reg[seed] = r;
        if (g.deg[seed]) continue; /* a sliver no plane took: alone */
        const double *ns = g.nrm[seed], *p0 = g.pos[g.vid[3 * (size_t)seed]];
        size_t sp = 0;
        g.stack[sp++] = seed;
        while (sp) {
            uint32_t cur = g.stack[--sp];
            for (int k = 0; k < 3; k++) {
                uint32_t u = g.nb[3 * (size_t)cur + k];
                if (g.reg[u] >= 0) continue; /* (never a gone one: no edge leads there) */
                int in = g.deg[u] || v_dot(g.nrm[u], ns) > 0.0;
                for (int c = 0; c < 3 && in; c++) {
                    double d[3];
                    v_sub(g.pos[g.vid[3 * (size_t)u + c]], p0, d);
                    double z = v_dot(d, ns);
                    in = (z < 0 ? -z : z) <= tol;
                }
                if (in) g.reg[u] = r, g.stack[sp++] = u;
            }
        }
    }
    /* 5. each region of two or more triangles again (merge_region) */
    g.start = calloc(g.nreg + 1, sizeof *g.start), g.members = malloc(n * sizeof *g.members);
    g.nextv = malloc(g.nv * sizeof *g.nextv), g.walked = calloc(g.nv, 1);
    g.lv = malloc((np + 1) * sizeof *g.lv), g.lstart = malloc((np + 2) * sizeof *g.lstart);
    g.ptid = malloc((np + 1) * sizeof *g.ptid), g.flat = malloc((2 * np + 2) * sizeof *g.flat);
    g.hidx = malloc((np + 1) * sizeof *g.hidx), g.used = malloc(np + 1);
    g.rnew = malloc(g.nreg * sizeof *g.rnew), g.rnew_n = calloc(g.nreg, sizeof *g.rnew_n);
    if (!g.start || !g.members || !g.nextv || !g.walked || !g.lv || !g.lstart || !g.ptid || !g.flat || !g.hidx ||
        !g.used || !g.rnew || !g.rnew_n)
        goto done;
    for (size_t v = 0; v < g.nv; v++) g.nextv[v] = UINT32_MAX;
    for (size_t k = 0; k < n; k++)
        if (!g.gone[k]) g.start[g.reg[k] + 1]++;
    for (size_t r = 0; r < g.nreg; r++) g.start[r + 1] += g.start[r], g.rnew[r] = UINT32_MAX;
    for (size_t k = 0; k < n; k++)
        if (!g.gone[k]) g.members[g.start[g.reg[k]] + g.rnew_n[g.reg[k]]++] = (uint32_t)k;
    for (size_t r = 0; r < g.nreg; r++) g.rnew_n[r] = 0;
    for (size_t r = 0; r < g.nreg; r++)
        if (merge_region(&g, r) < 0) goto done;
    /* 6. the output in input order: a merged region's triangles where its first one was */
    size_t no = 0;
    for (size_t k = 0; k < n; k++) {
        int32_t r = g.reg[k];
        if (!g.gone[k]) no += g.rnew[r] == UINT32_MAX ? 1 : g.members[g.start[r]] == k ? g.rnew_n[r] : 0;
    }
    if (no == n) { rc = 1; goto done; }
    double (*o)[9] = malloc((no ? no : 1) * sizeof *o);
    if (!o) goto done;
    size_t w = 0;
    for (size_t k = 0; k < n; k++) {
        int32_t r = g.reg[k];
        if (g.gone[k]) continue;
        if (g.rnew[r] == UINT32_MAX) memcpy(o[w++], t[k], sizeof o[0]);
        else if (g.members[g.start[r]] == k) {
            memcpy(o[w], g.nt[g.rnew[r]], g.rnew_n[r] * sizeof o[0]);
            w += g.rnew_n[r];
        }
    }
    *out = o, *nout = w;
    rc = 0;
done:
    u64map_free(&vm), u64map_free(&em);
    merge_free(&g);
    return rc;
}

/* ---- manifold through its C binding: the calls manifold-csg makes */

#ifdef CC_HAVE_MANIFOLD
#include <manifold/manifoldc.h>

static const char *status_name(ManifoldError e) { /* manifold_csg_sys::ManifoldError, Debug */
    static const char *N[] = {"NoError", "NonFiniteVertex", "NotManifold", "VertexOutOfBounds",
                              "PropertiesWrongLength", "MissingPositionProperties", "MergeVectorsDifferentLengths",
                              "MergeIndexOutOfBounds", "TransformWrongLength", "RunIndexWrongLength",
                              "FaceIdWrongLength", "InvalidConstruction", "ResultTooLarge", "InvalidTangents",
                              "Cancelled"};
    unsigned i = (unsigned)e;
    return i < sizeof N / sizeof *N ? N[i] : "Unknown";
}

/* Manifold::from_mesh_f64 then status() (to_manifold). NULL with err set when not closed. */
static ManifoldManifold *to_manifold(const cc_bmesh *m, char *err) {
    ManifoldMeshGL64 *gl = manifold_meshgl64(manifold_alloc_meshgl64(), m->pos, m->nv, 3, m->idx, m->nt);
    ManifoldManifold *r = manifold_of_meshgl64(manifold_alloc_manifold(), gl);
    manifold_delete_meshgl64(gl);
    ManifoldError s = manifold_status(r);
    if (s != MANIFOLD_NO_ERROR) {
        put_err(err, "not a closed solid: manifold3d status: ", status_name(s));
        manifold_delete_manifold(r);
        return NULL;
    }
    return r;
}

static int mf_op(int op, const cc_bmesh *a, const cc_bmesh *b, size_t nb, cc_bmesh *out, char *err) {
    memset(out, 0, sizeof *out);
    ManifoldManifold *ma = to_manifold(a, err), **cut = calloc(nb ? nb : 1, sizeof *cut);
    int rc = ma && cut ? 0 : cut ? CC_BOOL_NOT_CLOSED : -1;
    for (size_t i = 0; rc == 0 && i < nb; i++)
        if (!(cut[i] = to_manifold(&b[i], err))) rc = CC_BOOL_NOT_CLOSED;
    ManifoldManifold *u = NULL, *r = NULL;
    if (rc == 0) {
        u = manifold_copy(manifold_alloc_manifold(), cut[0]);
        for (size_t i = 1; i < nb; i++) {
            ManifoldManifold *v = manifold_union(manifold_alloc_manifold(), u, cut[i]);
            manifold_delete_manifold(u);
            u = v;
        }
        r = op == 0   ? manifold_difference(manifold_alloc_manifold(), ma, u)
            : op == 1 ? manifold_union(manifold_alloc_manifold(), ma, u)
                      : manifold_intersection(manifold_alloc_manifold(), ma, u);
        ManifoldError s = manifold_status(r);
        if (s != MANIFOLD_NO_ERROR) {
            put_err(err, "boolean failed: manifold3d status: ", status_name(s));
            rc = CC_BOOL_FAILED;
        }
    }
    if (rc == 0) {
        ManifoldMeshGL64 *gl = manifold_get_meshgl64(manifold_alloc_meshgl64(), r);
        size_t np = manifold_meshgl64_num_prop(gl), nv = manifold_meshgl64_num_vert(gl);
        size_t nt = manifold_meshgl64_num_tri(gl);
        double *props = malloc((manifold_meshgl64_vert_properties_length(gl) + 1) * sizeof *props);
        out->pos = malloc((nv * 3 + 1) * sizeof *out->pos);
        out->idx = malloc((manifold_meshgl64_tri_length(gl) + 1) * sizeof *out->idx);
        if (!props || !out->pos || !out->idx || np < 3) {
            free(out->pos), free(out->idx);
            memset(out, 0, sizeof *out);
            rc = np < 3 && props ? CC_BOOL_FAILED : -1;
            if (rc == CC_BOOL_FAILED) put_err(err, "bad mesh from the kernel", NULL);
        } else {
            manifold_meshgl64_vert_properties(props, gl);
            manifold_meshgl64_tri_verts(out->idx, gl);
            for (size_t i = 0; i < nv; i++)
                for (int q = 0; q < 3; q++) out->pos[3 * i + q] = props[i * np + q];
            out->nv = nv, out->nt = nt;
        }
        free(props);
        manifold_delete_meshgl64(gl);
    }
    if (r) manifold_delete_manifold(r);
    if (u) manifold_delete_manifold(u);
    for (size_t i = 0; cut && i < nb; i++)
        if (cut[i]) manifold_delete_manifold(cut[i]);
    free(cut);
    if (ma) manifold_delete_manifold(ma);
    return rc;
}
static int mf_difference(const cc_bmesh *a, const cc_bmesh *b, size_t nb, cc_bmesh *out, char *err) {
    return mf_op(0, a, b, nb, out, err);
}
static int mf_union(const cc_bmesh *a, const cc_bmesh *b, size_t nb, cc_bmesh *out, char *err) {
    return mf_op(1, a, b, nb, out, err);
}
static int mf_intersection(const cc_bmesh *a, const cc_bmesh *b, size_t nb, cc_bmesh *out, char *err) {
    return mf_op(2, a, b, nb, out, err);
}
static int mf_is_closed(const cc_bmesh *m) {
    ManifoldManifold *r = to_manifold(m, NULL);
    if (r) manifold_delete_manifold(r);
    return r != NULL;
}
static const cc_boolean MANIFOLD_TABLE = {mf_difference, mf_intersection, mf_union, mf_is_closed};
#endif
