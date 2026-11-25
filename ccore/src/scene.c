/* Scene::from_model in C (see scene.h). Ports, in the order Rust runs them:
 *   lib.rs     profile_points, corners_only, extrude_tris (earcut: convex CCW quads only),
 *              mesh_from_tris (weld by f64 bits), placement_matrix
 *   scene.rs   from_model, SceneBuilder::finish (origin), MeshBuilder::build (weld by f32 bits),
 *              crease_edges_with_normals (20 degrees), Placement32::mesh
 *   dedup.rs   share_copies (anchors, fit, own_frame, place, tight)
 * Every expression keeps Rust's types and evaluation order: that is what makes the bits equal.
 * Floating-point contraction (a*b+c into fma) would change bits: it is off. */
#ifdef __clang__
#pragma clang fp contract(off)
#endif
#include "scene.h"
#include <string.h>
#include "buf.h"

typedef double V[3];

/* ---- exact helpers, no libm ---- */

static uint64_t bits64(double x) { uint64_t u; memcpy(&u, &x, 8); return u; }
static double from64(uint64_t u) { double x; memcpy(&x, &u, 8); return x; }
static uint32_t bits32(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }

static double f64abs(double x) { return from64(bits64(x) & ~(1ull << 63)); }
static float f32abs(float x) {
    uint32_t u = bits32(x) & 0x7fffffffu;
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static int is_finite(double x) { return x == x && x - x == 0.0; }

/* f64::round: half away from zero, sign kept (round(-0.3) is -0.0). */
static double round64(double x) {
    if (!(f64abs(x) < 4503599627370496.0)) return x; /* 2^52 and up are integers; NaN */
    double t = (double)(int64_t)x;
    if (f64abs(x - t) >= 0.5) t += x < 0.0 ? -1.0 : 1.0;
    return from64((bits64(t) & ~(1ull << 63)) | (bits64(x) & (1ull << 63)));
}
static float round32(float x) {
    if (!(f32abs(x) < 8388608.0f)) return x; /* 2^23 */
    float t = (float)(int32_t)x;
    if (f32abs(x - t) >= 0.5f) t += x < 0.0f ? -1.0f : 1.0f;
    uint32_t u = (bits32(t) & 0x7fffffffu) | (bits32(x) & 0x80000000u);
    memcpy(&t, &u, 4);
    return t;
}

/* Correctly rounded square root: digit by digit on the 108-bit radicand m << 54. */
double cc_sqrt(double x) {
    uint64_t u = bits64(x);
    if (x != x) return x;
    if (x == 0.0 || x == 1.0 / 0.0) return x; /* ±0, +inf */
    if (u >> 63) return 0.0 / 0.0;
    int e = (int)(u >> 52);
    uint64_t m = u & ((1ull << 52) - 1);
    if (e == 0) { /* subnormal */
        e = 1;
        while (!(m & (1ull << 52))) m <<= 1, e--;
    } else {
        m |= 1ull << 52;
    }
    int ex = e - 1075; /* x = m * 2^ex */
    if (ex & 1) m <<= 1, ex--;
    uint64_t hi = m >> 10, lo = m << 54, root = 0, rem = 0;
    for (int i = 53; i >= 0; i--) {
        int b = 2 * i;
        uint64_t pair = b >= 64 ? (hi >> (b - 64)) & 3 : (lo >> b) & 3;
        rem = (rem << 2) | pair;
        uint64_t trial = (root << 2) | 1;
        if (rem >= trial) {
            rem -= trial;
            root = (root << 1) | 1;
        } else {
            root <<= 1;
        }
    }
    /* root in [2^53, 2^54): 53 bits and a guard bit; rem != 0 is the sticky bit. */
    uint64_t mant = root >> 1;
    if ((root & 1) && (rem != 0 || (mant & 1))) mant++;
    int exp = ex / 2 - 26;
    if (mant == (1ull << 53)) mant >>= 1, exp++;
    return from64((uint64_t)(exp + 52 + 1023) << 52 | (mant & ((1ull << 52) - 1)));
}

/* Rounding the exact f64 root to f32 is correctly rounded (53 >= 2 * 24 + 2). */
float cc_sqrtf(float x) { return (float)cc_sqrt((double)x); }

/* f64::total_cmp */
static int total_cmp(double a, double b) {
    int64_t x = (int64_t)bits64(a), y = (int64_t)bits64(b);
    x ^= (int64_t)((uint64_t)(x >> 63) >> 1);
    y ^= (int64_t)((uint64_t)(y >> 63) >> 1);
    return x < y ? -1 : x > y;
}

/* f64::min / f64::max (a NaN operand is ignored; -0.0 < 0.0 as aarch64 fminnm/fmaxnm do). */
static double min64(double a, double b) {
    if (a != a) return b;
    if (b != b) return a;
    if (a == b) return (bits64(a) >> 63) ? a : b;
    return a < b ? a : b;
}
static double max64(double a, double b) {
    if (a != a) return b;
    if (b != b) return a;
    if (a == b) return (bits64(a) >> 63) ? b : a;
    return a > b ? a : b;
}
static float min32(float a, float b) { return (float)min64(a, b); }
static float max32(float a, float b) { return (float)max64(a, b); }

/* f32::to_radians().cos() for 20 degrees: glibc's cosf(0x3eb2b8c2) = 0x3f708fb2, the value the
 * Rust goldens were made with. Ticket 13 must check this (and any other libm use) on wasm. */
static float cos_20_deg(void) {
    uint32_t u = 0x3f708fb2u;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* lib.rs pack_normal */
static int8_t q8(float c) {
    float x = c < -1.0f ? -1.0f : c > 1.0f ? 1.0f : c;
    return (int8_t)round32(x * 127.0f);
}
static void pack_normal(const float n[3], int8_t o[4]) {
    o[0] = q8(n[0]);
    o[1] = q8(n[1]);
    o[2] = q8(n[2]);
    o[3] = 0;
}

/* ---- hash table of indices into a key array (open addressing) ---- */

typedef struct {
    int32_t *slot;
    uint32_t mask;
} Tab;

static int tab_init(Tab *t, Arena *a, size_t n) {
    size_t cap = 16;
    while (cap < 2 * n + 2) cap <<= 1;
    t->slot = arena_alloc(a, cap * sizeof(int32_t), 4);
    if (!t->slot) return -1;
    memset(t->slot, 0xff, cap * sizeof(int32_t)); /* -1 */
    t->mask = (uint32_t)(cap - 1);
    return 0;
}

static uint32_t hash_bytes(const void *k, size_t n) {
    const unsigned char *p = k;
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

/* Index of `key` among base[0..next) (keys at `stride`), or `next` after recording it. */
static int32_t tab_find(Tab *t, const void *key, size_t ksz, const void *base, size_t stride, int32_t next) {
    uint32_t h = hash_bytes(key, ksz) & t->mask;
    for (;;) {
        int32_t s = t->slot[h];
        if (s < 0) {
            t->slot[h] = next;
            return next;
        }
        if (!memcmp((const unsigned char *)base + (size_t)s * stride, key, ksz)) return s;
        h = (h + 1) & t->mask;
    }
}

/* ---- crease_edges_with_normals (scene.rs) ---- */

typedef struct {
    int64_t k[6]; /* the hash key: quantized ends, smaller first */
    uint32_t count;
    int crease;
    float normal[3], other[3], seg[6];
} EdgeUse;

/* Lexicographic partial_cmp of the 6 flattened coordinates: a < b. */
static int seg_less(const float *a, const float *b) {
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i]) return a[i] < b[i];
    return 0;
}

/* (c * 100.0).round() as i64, saturating like Rust's `as`. */
static int64_t quant(float c) {
    float r = round32(c * 100.0f);
    if (r != r) return 0;
    if (r >= 9223372036854775807.0f) return INT64_MAX;
    if (r <= -9223372036854775808.0f) return INT64_MIN;
    return (int64_t)r;
}

static int crease_edges(cc_mesh *m, Arena *a) {
    float cos_limit = cos_20_deg();
    uint32_t nt = m->ni / 3, nu = 0;
    EdgeUse *uses = arena_alloc(a, (size_t)(3 * nt + 1) * sizeof(EdgeUse), 8);
    Tab tab;
    if (!uses || tab_init(&tab, a, 3 * (size_t)nt)) return -1;
    for (uint32_t t = 0; t < nt; t++) {
        const uint32_t *tr = &m->idx[3 * t];
        const float *a0 = m->pos[tr[0]], *b0 = m->pos[tr[1]], *c0 = m->pos[tr[2]];
        float u[3], v[3], cr[3], n[3];
        for (int k = 0; k < 3; k++) {
            u[k] = b0[k] - a0[k];
            v[k] = c0[k] - a0[k];
        }
        cr[0] = u[1] * v[2] - u[2] * v[1];
        cr[1] = u[2] * v[0] - u[0] * v[2];
        cr[2] = u[0] * v[1] - u[1] * v[0];
        float l = cc_sqrtf(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
        if (l > 1e-12f) {
            n[0] = cr[0] / l;
            n[1] = cr[1] / l;
            n[2] = cr[2] / l;
        } else {
            n[0] = 0.0f;
            n[1] = 0.0f;
            n[2] = 1.0f;
        }
        for (int k = 0; k < 3; k++) {
            const float *p = m->pos[tr[k]], *q = m->pos[tr[(k + 1) % 3]];
            int64_t ka[3], kb[3], key[6];
            for (int i = 0; i < 3; i++) ka[i] = quant(p[i]), kb[i] = quant(q[i]);
            int c = 0;
            for (int i = 0; i < 3 && !c; i++) c = ka[i] < kb[i] ? -1 : ka[i] > kb[i];
            if (!c) continue;
            memcpy(key, c < 0 ? ka : kb, 24);
            memcpy(key + 3, c < 0 ? kb : ka, 24);
            int32_t at = tab_find(&tab, key, 48, uses, sizeof(EdgeUse), (int32_t)nu);
            EdgeUse *e = &uses[at];
            if ((uint32_t)at == nu) {
                memcpy(e->k, key, 48);
                memcpy(e->normal, n, 12);
                memcpy(e->other, n, 12);
                memcpy(e->seg, p, 12);
                memcpy(e->seg + 3, q, 12);
                nu++;
            }
            if (e->count > 0) {
                float d = e->normal[0] * n[0] + e->normal[1] * n[1] + e->normal[2] * n[2];
                if (d < cos_limit) e->crease = 1;
                memcpy(e->other, n, 12);
            }
            e->count++;
        }
    }
    /* Keep boundary and crease edges, then sort (stable merge sort, like sort_by). */
    uint32_t *ord = arena_alloc(a, (size_t)(2 * nu + 1) * sizeof(uint32_t), 4), n = 0;
    if (!ord) return -1;
    for (uint32_t i = 0; i < nu; i++)
        if (uses[i].count == 1 || uses[i].crease) ord[n++] = i;
    uint32_t *tmp = ord + nu;
    for (uint32_t w = 1; w < n; w *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * w) {
            uint32_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                tmp[k++] = seg_less(uses[ord[j]].seg, uses[ord[i]].seg) ? ord[j++] : ord[i++];
            while (i < mid) tmp[k++] = ord[i++];
            while (j < hi) tmp[k++] = ord[j++];
        }
        memcpy(ord, tmp, n * sizeof(uint32_t));
    }
    m->edges = arena_alloc(a, (size_t)(n + 1) * sizeof m->edges[0], 4);
    m->normals = arena_alloc(a, (size_t)(n + 1) * sizeof m->normals[0], 1);
    if (!m->edges || !m->normals) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const EdgeUse *e = &uses[ord[i]];
        int8_t *o = m->normals[i];
        memcpy(m->edges[i], e->seg, 24);
        if (e->count == 1) {
            float neg[3] = {-e->normal[0], -e->normal[1], -e->normal[2]};
            pack_normal(e->normal, o);
            pack_normal(neg, o + 4);
        } else if (e->count == 2) {
            pack_normal(e->normal, o);
            pack_normal(e->other, o + 4);
        } /* else: non-manifold, zeros (arena memory is zeroed) */
    }
    m->ne = m->nn = n;
    return 0;
}

/* ---- element_mesh (lib.rs): profile, extrusion, weld ---- */

#define NOT_YET 1 /* local_mesh: unsupported generator */

typedef struct {
    double (*tris)[3][3]; /* world mm, f64 */
    uint32_t ntris;
} World;

/* lib.rs corners_only. Returns the new count. */
static size_t corners_only(double (*v)[2], size_t n) {
    for (;;) {
        if (n < 3) return n;
        size_t drop = n;
        for (size_t i = 0; i < n && drop == n; i++) {
            const double *a = v[(i + n - 1) % n], *b = v[i], *c = v[(i + 1) % n];
            double ux = c[0] - a[0], uy = c[1] - a[1], wx = b[0] - a[0], wy = b[1] - a[1];
            double len = cc_sqrt(ux * ux + uy * uy);
            int same = f64abs(b[0] - a[0]) < 1e-9 && f64abs(b[1] - a[1]) < 1e-9;
            double l2 = len * len;
            double along = (wx * ux + wy * uy) / max64(l2, 1e-30);
            if (same || (len > 0.0 && f64abs(ux * wy - uy * wx) / len < 1e-6 && along >= 0.0 && along <= 1.0))
                drop = i;
        }
        if (drop == n) return n;
        memmove(v[drop], v[drop + 1], (n - drop - 1) * sizeof v[0]);
        n--;
    }
}

static void set3(double *p, const double *q, double z) {
    p[0] = q[0];
    p[1] = q[1];
    p[2] = z;
}

/* The element's local mesh (mesh_from_tris positions and indices; its edges are not used by
 * the scene). 0 ok, NOT_YET with `why`, -1 out of memory, 2 Rust's "fewer than 3 corners". */
static int local_mesh(const cc_element *e, Arena *a, cc_mesh *out, const char **why) {
    double pts[4][2];
    const cc_loop *outline = NULL;
    double depth;
    const cc_generator *g = &e->generator;
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING:
    case CC_GEN_RECT_BEAM: {
        double xd = g->kind == CC_GEN_RECT_BEAM ? g->u.beam.width : g->u.col.width;
        double yd = g->kind == CC_GEN_RECT_BEAM ? g->u.beam.height : g->u.col.depth;
        depth = g->kind == CC_GEN_RECT_BEAM ? g->u.beam.length : g->u.col.height;
        double x = xd / 2.0, y = yd / 2.0;
        double r[4][2] = {{-x, -y}, {x, -y}, {x, y}, {-x, y}};
        memcpy(pts, r, sizeof r);
        break;
    }
    case CC_GEN_WALL: {
        double t = g->u.wall.thickness;
        double r[4][2] = {{0.0, -t / 2.0}, {g->u.wall.length, -t / 2.0}, {g->u.wall.length, t / 2.0}, {0.0, t / 2.0}};
        memcpy(pts, r, sizeof r);
        depth = g->u.wall.height;
        break;
    }
    case CC_GEN_SLAB:
        outline = &g->u.slab.outline;
        depth = g->u.slab.thickness;
        break;
    case CC_GEN_EXTRUDED_POLYGON:
        *why = "extruded_polygon needs earcut";
        return NOT_YET;
    default:
        *why = "mesh generator";
        return NOT_YET;
    }
    double (*p)[2] = pts;
    size_t n = 4;
    if (outline) {
        p = arena_alloc(a, (outline->n + 1) * sizeof p[0], 8);
        if (!p) return -1;
        memcpy(p, outline->pts, outline->n * sizeof p[0]);
        n = outline->n;
    }
    n = corners_only(p, n);
    if (n < 3) return 2;
    /* earcutr gives [2,3,0, 0,1,2] for a convex counter-clockwise quad (spike c-slab). */
    int convex = n == 4;
    for (size_t i = 0; i < n && convex; i++) {
        const double *u = p[i], *v = p[(i + 1) % n], *w = p[(i + 2) % n];
        convex = (v[0] - u[0]) * (w[1] - v[1]) - (v[1] - u[1]) * (w[0] - v[0]) > 0.0;
    }
    if (!convex) {
        *why = "slab outline that is not a convex counter-clockwise quad needs earcut";
        return NOT_YET;
    }
    static const int cap[6] = {2, 3, 0, 0, 1, 2};
    double tris[12][3][3];
    int m = 0;
    for (int pass = 0; pass < 2; pass++) { /* bottom cap reversed, top cap */
        double z = pass ? depth : 0.0;
        for (int t = 0; t < 2; t++) {
            int i0 = cap[3 * t], i1 = cap[3 * t + 1], i2 = cap[3 * t + 2];
            if (!pass) {
                int s = i1;
                i1 = i2;
                i2 = s;
            }
            set3(tris[m][0], p[i0], z);
            set3(tris[m][1], p[i1], z);
            set3(tris[m][2], p[i2], z);
            m++;
        }
    }
    for (size_t i = 0; i < 4; i++) { /* sides */
        size_t j = (i + 1) % 4;
        set3(tris[m][0], p[i], 0.0);
        set3(tris[m][1], p[j], 0.0);
        set3(tris[m][2], p[j], depth);
        m++;
        set3(tris[m][0], p[i], 0.0);
        set3(tris[m][1], p[j], depth);
        set3(tris[m][2], p[i], depth);
        m++;
    }
    /* mesh_from_tris: weld by f64 bits after + 0.0 (-0.0 becomes 0.0), positions to f32. */
    double wp[36][3];
    Tab tab;
    if (tab_init(&tab, a, 36)) return -1;
    out->idx = arena_alloc(a, 36 * sizeof(uint32_t), 4);
    out->pos = arena_alloc(a, 36 * sizeof out->pos[0], 4);
    if (!out->idx || !out->pos) return -1;
    uint32_t nv = 0;
    for (int t = 0; t < m; t++)
        for (int k = 0; k < 3; k++) {
            double q[3] = {tris[t][k][0] + 0.0, tris[t][k][1] + 0.0, tris[t][k][2] + 0.0};
            int32_t at = tab_find(&tab, q, 24, wp, sizeof wp[0], (int32_t)nv);
            if ((uint32_t)at == nv) memcpy(wp[nv++], q, 24);
            out->idx[3 * t + k] = (uint32_t)at;
        }
    for (uint32_t i = 0; i < nv; i++)
        for (int k = 0; k < 3; k++) out->pos[i][k] = (float)wp[i][k];
    out->nv = nv;
    out->ni = (uint32_t)(3 * m);
    return 0;
}

/* from_model: local mesh through placement_matrix into world f64 triangles (add_triangle). */
static int to_world(const cc_mesh *l, const cc_placement *pl, double elevation, Arena *a, World *w) {
    const double *x = pl->x_axis, *z = pl->z_axis, *o = pl->origin;
    double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    double t[4][3] = {{x[0], x[1], x[2]}, {y[0], y[1], y[2]}, {z[0], z[1], z[2]}, {o[0], o[1], o[2] + elevation}};
    w->tris = arena_alloc(a, (l->ni / 3 + 1) * sizeof w->tris[0], 8);
    if (!w->tris) return -1;
    w->ntris = 0;
    for (uint32_t i = 0; i < l->ni / 3; i++) {
        double (*d)[3] = w->tris[w->ntris];
        int finite = 1;
        for (int k = 0; k < 3; k++) {
            const float *f = l->pos[l->idx[3 * i + k]];
            double p[3] = {f[0], f[1], f[2]};
            for (int r = 0; r < 3; r++) {
                d[k][r] = t[0][r] * p[0] + t[1][r] * p[1] + t[2][r] * p[2] + t[3][r];
                finite &= is_finite(d[k][r]);
            }
        }
        if (finite) w->ntris++;
    }
    return 0;
}

/* MeshBuilder::build: drop degenerate triangles, positions relative to the origin as f32,
 * weld by f32 bits, crease edges. */
static int build_mesh(const World *w, const double origin[3], Arena *a, cc_mesh *m) {
    memset(m, 0, sizeof *m);
    m->pos = arena_alloc(a, (3 * (size_t)w->ntris + 1) * sizeof m->pos[0], 4);
    m->idx = arena_alloc(a, (3 * (size_t)w->ntris + 1) * sizeof(uint32_t), 4);
    Tab tab;
    if (!m->pos || !m->idx || tab_init(&tab, a, 3 * (size_t)w->ntris)) return -1;
    for (uint32_t i = 0; i < w->ntris; i++) {
        double (*t)[3] = w->tris[i];
        double u[3], v[3];
        for (int r = 0; r < 3; r++) u[r] = t[1][r] - t[0][r], v[r] = t[2][r] - t[0][r];
        double c[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        double l = cc_sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        if (l != l || l <= 0.0) continue;
        for (int k = 0; k < 3; k++) {
            float p[3] = {(float)(t[k][0] - origin[0]), (float)(t[k][1] - origin[1]), (float)(t[k][2] - origin[2])};
            int32_t at = tab_find(&tab, p, 12, m->pos, sizeof m->pos[0], (int32_t)m->nv);
            if ((uint32_t)at == m->nv) memcpy(m->pos[m->nv++], p, 12);
            m->idx[m->ni++] = (uint32_t)at;
        }
    }
    return crease_edges(m, a);
}

/* ---- dedup.rs share_copies ---- */

#define TOLERANCE_MM 0.1
#define MAX_SHAPES_PER_BUCKET 16

static void sub(const double *a, const double *b, double *o) {
    o[0] = a[0] - b[0];
    o[1] = a[1] - b[1];
    o[2] = a[2] - b[2];
}
static double dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross(const double *a, const double *b, double *o) {
    double r[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    memcpy(o, r, 24);
}
static int unit(const double *a, double *o) {
    double l = cc_sqrt(dot(a, a));
    if (!(l > 1e-6)) return 0;
    o[0] = a[0] / l;
    o[1] = a[1] / l;
    o[2] = a[2] / l;
    return 1;
}
static void p64(const float *p, double *o) {
    o[0] = p[0];
    o[1] = p[1];
    o[2] = p[2];
}

typedef long Anchors[4]; /* [first, second, third, fourth or -1 for a flat mesh] */

/* Iterator::max_by with total_cmp: the last of equal maxima. */
typedef double (*Score)(const double *q, const void *ctx);
static long far_index(const cc_mesh *m, Score score, const void *ctx) {
    long best = -1;
    double bs = 0.0;
    for (uint32_t k = 0; k < m->nv; k++) {
        double q[3];
        p64(m->pos[k], q);
        double s = score(q, ctx);
        if (best < 0 || total_cmp(s, bs) >= 0) best = (long)k, bs = s;
    }
    return best;
}
typedef struct {
    V p0, u, n;
} AnchorCtx;
static double score_dist(const double *q, const void *c) {
    const AnchorCtx *x = c;
    double d[3];
    sub(q, x->p0, d);
    return dot(d, d);
}
static double score_line(const double *q, const void *c) {
    const AnchorCtx *x = c;
    double d[3], cr[3];
    sub(q, x->p0, d);
    cross(x->u, d, cr);
    return dot(cr, cr);
}
static double score_plane(const double *q, const void *c) {
    const AnchorCtx *x = c;
    double d[3];
    sub(q, x->p0, d);
    return f64abs(dot(x->n, d));
}

static int anchors(const cc_mesh *m, Anchors out) {
    AnchorCtx c;
    double d[3], q[3];
    if (!m->nv) return 0;
    p64(m->pos[0], c.p0);
    long k1 = far_index(m, score_dist, &c);
    p64(m->pos[k1], q);
    sub(q, c.p0, d);
    double size = cc_sqrt(dot(d, d));
    if (!unit(d, c.u)) return 0;
    long k2 = far_index(m, score_line, &c);
    p64(m->pos[k2], q);
    sub(q, c.p0, d);
    cross(c.u, d, q);
    if (!unit(q, c.n)) return 0;
    long k3 = far_index(m, score_plane, &c);
    p64(m->pos[k3], q);
    sub(q, c.p0, d);
    int solid = f64abs(dot(c.n, d)) > 1e-3 * size;
    out[0] = 0;
    out[1] = k1;
    out[2] = k2;
    out[3] = solid ? k3 : -1;
    return 1;
}

static int edges_of(const cc_mesh *m, const Anchors a, V e[3]) {
    double p0[3], q[3];
    p64(m->pos[a[0]], p0);
    p64(m->pos[a[1]], q);
    sub(q, p0, e[0]);
    p64(m->pos[a[2]], q);
    sub(q, p0, e[1]);
    if (a[3] < 0) {
        double c[3];
        cross(e[0], e[1], c);
        return unit(c, e[2]);
    }
    p64(m->pos[a[3]], q);
    sub(q, p0, e[2]);
    return 1;
}

static int inverse(V c[3], V o[3]) {
    double c12[3], det;
    cross(c[1], c[2], c12);
    det = dot(c[0], c12);
    if (f64abs(det) < 1e-12) return 0;
    V r[3];
    cross(c[1], c[2], r[0]);
    cross(c[2], c[0], r[1]);
    cross(c[0], c[1], r[2]);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i][j] = r[i][j] / det;
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) o[j][i] = r[i][j];
    return 1;
}

static void mul(V a[3], V b[3], V o[3]) {
    V r[3];
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 3; i++) r[c][i] = a[0][i] * b[c][0] + a[1][i] * b[c][1] + a[2][i] * b[c][2];
    memcpy(o, r, sizeof r);
}

static void lin(V r[3], const double *v, double *o) {
    double x[3];
    for (int i = 0; i < 3; i++) x[i] = r[0][i] * v[0] + r[1][i] * v[1] + r[2][i] * v[2];
    memcpy(o, x, 24);
}

static int fit(const cc_mesh *from, const cc_mesh *to, const Anchors a, V r[3], double t[3]) {
    V et[3], ef[3], inv[3];
    if (!edges_of(to, a, et) || !edges_of(from, a, ef) || !inverse(ef, inv)) return 0;
    mul(et, inv, r);
    double x[3], y[3], lx[3], d[3], yt[3];
    p64(to->pos[0], y);
    p64(from->pos[0], x);
    lin(r, x, lx);
    sub(y, lx, t);
    for (uint32_t k = 0; k < from->nv; k++) {
        p64(from->pos[k], x);
        p64(to->pos[k], y);
        lin(r, x, lx);
        sub(y, t, yt);
        sub(lx, yt, d);
        if (!(dot(d, d) <= TOLERANCE_MM * TOLERANCE_MM)) return 0;
    }
    return 1;
}

/* dedup.rs own_frame */
static void own_frame(const cc_mesh *m, V f[3]) {
    static const V id[3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
    double best[3], d[3], u[3], v[3];
    long bi = -1;
    for (uint32_t k = 0; k < m->ne; k++) {
        double a[3], b[3];
        p64(m->edges[k], a);
        p64(m->edges[k] + 3, b);
        sub(b, a, d);
        if (bi < 0 || total_cmp(dot(d, d), dot(best, best)) >= 0) memcpy(best, d, 24), bi = k;
    }
    if (bi < 0 || !unit(best, u)) {
        memcpy(f, id, sizeof id);
        return;
    }
    bi = -1;
    for (uint32_t k = 0; k < m->ne; k++) {
        double a[3], b[3], ud[3];
        p64(m->edges[k], a);
        p64(m->edges[k] + 3, b);
        sub(b, a, d);
        if (!unit(d, ud) || !(f64abs(dot(ud, u)) < 0.9)) continue;
        if (bi < 0 || total_cmp(dot(d, d), dot(best, best)) >= 0) memcpy(best, d, 24), bi = k;
    }
    int have = 0;
    if (bi >= 0) {
        double du = dot(best, u), s[3] = {u[0] * du, u[1] * du, u[2] * du}, w[3];
        sub(best, s, w);
        have = unit(w, v);
    }
    if (!have) {
        double ax[3] = {1.0, 0.0, 0.0}, c[3];
        if (!(f64abs(u[0]) < 0.9)) ax[0] = 0.0, ax[1] = 1.0;
        cross(u, ax, c);
        if (!unit(c, v)) v[0] = 0.0, v[1] = 1.0, v[2] = 0.0;
    }
    memcpy(f[0], u, 24);
    memcpy(f[1], v, 24);
    cross(u, v, f[2]);
}

static void to_frame(V f[3], const double *v, double *o) {
    double r[3] = {dot(f[0], v), dot(f[1], v), dot(f[2], v)};
    memcpy(o, r, 24);
}

/* f32 Aabb::of_points: the half diagonal of an element's own mesh box (tight). */
static double half_diagonal(const cc_mesh *m) {
    float lo[3] = {1.0f / 0.0f, 1.0f / 0.0f, 1.0f / 0.0f}, hi[3] = {-1.0f / 0.0f, -1.0f / 0.0f, -1.0f / 0.0f};
    for (uint32_t i = 0; i < m->nv; i++)
        for (int k = 0; k < 3; k++) lo[k] = min32(lo[k], m->pos[i][k]), hi[k] = max32(hi[k], m->pos[i][k]);
    double a[3], b[3], d[3];
    p64(lo, a);
    p64(hi, b);
    sub(b, a, d);
    return 2.0 * cc_sqrt(dot(d, d)) / 2.0;
}

typedef struct {
    int32_t bucket_next; /* next member of its bucket, -1 */
    int32_t shape_of;    /* the shape element it copies, -1 */
    int32_t copy_next;   /* next copy of the same shape, -1 */
    V r[3];
    double t[3];
} Dedup;

typedef struct {
    uint32_t nv, ni, ne;
    int32_t head, tail;
} Bucket;

/* place(r, t): the copy's Placement32 and its placed box's longest half-diagonal. */
static void place(V r[3], const double *t, V f[3], const double *centre, const double *half,
                  cc_placement32 *pl, double *reach) {
    V mf[3];
    mul(r, f, mf);
    double tc[3];
    lin(mf, centre, tc);
    static const double s[4][2] = {{1.0, 1.0}, {1.0, -1.0}, {-1.0, 1.0}, {-1.0, -1.0}};
    double acc = 0.0;
    for (int i = 0; i < 4; i++) {
        double h[3] = {half[0], s[i][0] * half[1], s[i][1] * half[2]}, d[3];
        lin(mf, h, d);
        acc = max64(acc, cc_sqrt(dot(d, d)));
    }
    *reach = acc;
    for (int i = 0; i < 3; i++) {
        pl->x[i] = (float)mf[0][i];
        pl->y[i] = (float)mf[1][i];
        pl->z[i] = (float)mf[2][i];
        pl->t[i] = (float)(tc[i] + t[i]);
    }
}

static int share_copies(cc_scene *s) {
    size_t n = s->nelements;
    Arena *a = &s->arena;
    if (n < 2) return 0;
    Dedup *dd = arena_alloc(a, n * sizeof *dd, 8);
    Bucket *bk = arena_alloc(a, n * sizeof *bk, 8);
    int32_t *order = arena_alloc(a, n * sizeof *order, 4); /* shape elements, in found order */
    Tab tab;
    if (!dd || !bk || !order || tab_init(&tab, a, n)) return -1;
    int32_t nb = 0, ns = 0;
    for (size_t i = 0; i < n; i++) {
        dd[i].bucket_next = dd[i].shape_of = dd[i].copy_next = -1;
        const cc_mesh *m = &s->elements[i].mesh;
        if (s->elements[i].shape >= 0 || m->nv < 3) continue;
        uint32_t key[3] = {m->nv, m->ni, m->ne};
        memcpy(&bk[nb], key, 12); /* candidate slot: keys are the first 12 bytes */
        int32_t b = tab_find(&tab, key, 12, bk, sizeof *bk, nb);
        if (b == nb) bk[nb].head = -1, nb++;
        if (bk[b].head < 0) bk[b].head = (int32_t)i;
        else dd[bk[b].tail].bucket_next = (int32_t)i;
        bk[b].tail = (int32_t)i;
    }
    for (int32_t b = 0; b < nb; b++) {
        if (dd[bk[b].head].bucket_next < 0) continue; /* one member */
        int32_t shapes[MAX_SHAPES_PER_BUCKET], tails[MAX_SHAPES_PER_BUCKET];
        Anchors an[MAX_SHAPES_PER_BUCKET];
        int nsh = 0;
        for (int32_t i = bk[b].head; i >= 0; i = dd[i].bucket_next) {
            const cc_mesh *m = &s->elements[i].mesh;
            int found = 0;
            for (int k = 0; k < nsh && !found; k++) {
                if (!fit(&s->elements[shapes[k]].mesh, m, an[k], dd[i].r, dd[i].t)) continue;
                found = 1;
                dd[i].shape_of = shapes[k];
                dd[tails[k]].copy_next = i;
                tails[k] = i;
            }
            if (!found && nsh < MAX_SHAPES_PER_BUCKET && anchors(m, an[nsh])) {
                shapes[nsh] = tails[nsh] = i;
                nsh++;
            }
        }
        for (int k = 0; k < nsh; k++)
            if (dd[shapes[k]].copy_next >= 0) order[ns++] = shapes[k];
    }
    s->shapes = arena_alloc(a, ((size_t)ns + 1) * sizeof *s->shapes, 8);
    cc_placement32 *pls = arena_alloc(a, n * sizeof *pls, 4);
    int8_t *placed = arena_alloc(a, n, 1);
    if (!s->shapes || !pls || !placed) return -1;
    static const V identity[3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
    for (int32_t g = 0; g < ns; g++) {
        int32_t first = order[g];
        const cc_mesh *m = &s->elements[first].mesh;
        V f[3];
        own_frame(m, f);
        double lo[3] = {1.0 / 0.0, 1.0 / 0.0, 1.0 / 0.0}, hi[3] = {-1.0 / 0.0, -1.0 / 0.0, -1.0 / 0.0};
        for (uint32_t i = 0; i < m->nv; i++) {
            double p[3], q[3];
            p64(m->pos[i], p);
            to_frame(f, p, q);
            for (int k = 0; k < 3; k++) lo[k] = min64(lo[k], q[k]), hi[k] = max64(hi[k], q[k]);
        }
        double centre[3], half[3];
        for (int k = 0; k < 3; k++) centre[k] = (lo[k] + hi[k]) / 2.0, half[k] = (hi[k] - lo[k]) / 2.0;
        int any = 0;
        for (int32_t i = dd[first].copy_next; i >= 0; i = dd[i].copy_next) {
            double reach;
            place(dd[i].r, dd[i].t, f, centre, half, &pls[i], &reach);
            placed[i] = reach <= half_diagonal(&s->elements[i].mesh) + 1.0;
            any |= placed[i];
        }
        if (!any) continue;
        cc_mesh *sh = &s->shapes[s->nshapes];
        *sh = *m; /* indices shared */
        sh->pos = arena_alloc(a, ((size_t)m->nv + 1) * sizeof sh->pos[0], 4);
        sh->edges = arena_alloc(a, ((size_t)m->ne + 1) * sizeof sh->edges[0], 4);
        sh->normals = arena_alloc(a, ((size_t)m->nn + 1) * sizeof sh->normals[0], 1);
        if (!sh->pos || !sh->edges || !sh->normals) return -1;
        for (uint32_t i = 0; i < m->nv + 2 * m->ne; i++) {
            const float *src = i < m->nv ? m->pos[i] : m->edges[(i - m->nv) / 2] + 3 * ((i - m->nv) % 2);
            float *dst = i < m->nv ? sh->pos[i] : sh->edges[(i - m->nv) / 2] + 3 * ((i - m->nv) % 2);
            double p[3], q[3];
            p64(src, p);
            to_frame(f, p, q);
            for (int k = 0; k < 3; k++) dst[k] = (float)(q[k] - centre[k]);
        }
        for (uint32_t i = 0; i < 2 * m->nn; i++) {
            const int8_t *nv = m->normals[i / 2] + 4 * (i % 2);
            int8_t *o = sh->normals[i / 2] + 4 * (i % 2);
            if (!nv[0] && !nv[1] && !nv[2] && !nv[3]) continue;
            double v[3] = {(double)nv[0] / 127.0, (double)nv[1] / 127.0, (double)nv[2] / 127.0}, q[3];
            to_frame(f, v, q);
            float qf[3] = {(float)q[0], (float)q[1], (float)q[2]};
            pack_normal(qf, o);
        }
        double zero[3] = {0.0, 0.0, 0.0}, reach;
        V idm[3];
        memcpy(idm, identity, sizeof idm);
        place(idm, zero, f, centre, half, &s->elements[first].to_scene, &reach);
        s->elements[first].shape = (int32_t)s->nshapes;
        for (int32_t i = dd[first].copy_next; i >= 0; i = dd[i].copy_next) {
            if (!placed[i]) continue;
            s->elements[i].to_scene = pls[i];
            s->elements[i].shape = (int32_t)s->nshapes;
        }
        s->nshapes++;
    }
    /* Own meshes of elements now drawn as instances are no longer theirs. */
    for (size_t i = 0; i < n; i++)
        if (s->elements[i].shape >= 0) memset(&s->elements[i].mesh, 0, sizeof(cc_mesh));
    return 0;
}

/* ---- Scene::from_model ---- */

static int fail(cc_error *err, const char *a, const char *b, const char *c) {
    Buf t;
    buf_init(&t);
    buf_puts(&t, a);
    buf_puts(&t, b);
    buf_puts(&t, c);
    const char *s = buf_cstr(&t);
    size_t n = strlen(s);
    if (n >= sizeof err->msg) n = sizeof err->msg - 1;
    memcpy(err->msg, s, n);
    err->msg[n] = 0;
    err->offset = CC_NO_OFFSET;
    buf_free(&t);
    return -1;
}

int cc_scene_build(cc_scene *s, const cc_model *m, cc_error *err) {
    memset(s, 0, sizeof *s);
    arena_init(&s->arena, 0);
    Arena *a = &s->arena;
    size_t n = m->nelements;
    World *w = arena_alloc(a, (n + 1) * sizeof *w, 8);
    s->elements = arena_alloc(a, (n + 1) * sizeof *s->elements, 8);
    s->warnings = arena_alloc(a, (n + 1) * sizeof *s->warnings, 8);
    if (!w || !s->elements || !s->warnings) return fail(err, "out of memory", "", "");
    for (size_t i = 0; i < n; i++) {
        const cc_element *e = &m->elements[i];
        const cc_storey *st = cc_model_storey(m, &e->storey);
        const cc_material *mat = cc_model_material(m, &e->material);
        if (!st || !mat) return fail(err, "invalid model: element without its storey or material", "", "");
        cc_mesh l;
        const char *why = "";
        memset(&l, 0, sizeof l);
        int rc = local_mesh(e, a, &l, &why);
        if (rc == NOT_YET) {
            Buf t;
            buf_init(&t);
            buf_puts(&t, cc_ifc_class_name(e->cls));
            buf_putc(&t, ' ');
            buf_append(&t, e->name.p, e->name.n);
            int r = fail(err, buf_cstr(&t), ": not in the C core yet (", why);
            size_t k = strlen(err->msg);
            if (k + 1 < sizeof err->msg) err->msg[k] = ')', err->msg[k + 1] = 0;
            buf_free(&t);
            return r;
        }
        if (rc < 0) return fail(err, "out of memory", "", "");
        if (rc == 2) { /* Rust: a warning, the element is left out */
            Buf t;
            buf_init(&t);
            buf_puts(&t, cc_ifc_class_name(e->cls));
            buf_putc(&t, ' ');
            buf_append(&t, e->name.p, e->name.n);
            buf_puts(&t, ": could not mesh (Triangulation(\"profile has fewer than 3 corners\"))");
            char *c = arena_dup(a, t.p, t.len);
            int oom = t.oom || !c;
            s->warnings[s->nwarnings].p = c;
            s->warnings[s->nwarnings++].n = t.len;
            buf_free(&t);
            if (oom) return fail(err, "out of memory", "", "");
            w[i].ntris = 0;
            continue;
        }
        if (to_world(&l, &e->placement, st->elevation, a, &w[i])) return fail(err, "out of memory", "", "");
    }
    /* SceneBuilder::finish: the origin is the middle of the world box, rounded to mm. */
    double lo[3] = {1.0 / 0.0, 1.0 / 0.0, 1.0 / 0.0}, hi[3] = {-1.0 / 0.0, -1.0 / 0.0, -1.0 / 0.0};
    for (size_t i = 0; i < n; i++)
        for (uint32_t t = 0; t < w[i].ntris; t++)
            for (int k = 0; k < 3; k++)
                for (int r = 0; r < 3; r++) lo[r] = min64(lo[r], w[i].tris[t][k][r]), hi[r] = max64(hi[r], w[i].tris[t][k][r]);
    for (int r = 0; r < 3; r++) s->origin[r] = is_finite(lo[0]) ? round64((lo[r] + hi[r]) / 2.0) : 0.0;
    for (size_t i = 0; i < n; i++) {
        if (!w[i].ntris) continue; /* push: no triangles, no element */
        const cc_element *e = &m->elements[i];
        cc_scene_element *se = &s->elements[s->nelements];
        if (build_mesh(&w[i], s->origin, a, &se->mesh)) return fail(err, "out of memory", "", "");
        if (!se->mesh.ni) continue; /* all degenerate */
        se->id = e->id;
        se->cls = e->cls;
        se->category = cc_model_material(m, &e->material)->category;
        se->model_index = i;
        se->shape = -1;
        s->nelements++;
    }
    if (share_copies(s)) return fail(err, "out of memory", "", "");
    return 0;
}

void cc_scene_free(cc_scene *s) {
    arena_free(&s->arena);
    memset(s, 0, sizeof *s);
}

const cc_mesh *cc_scene_local_mesh(const cc_scene *s, size_t k) {
    const cc_scene_element *e = &s->elements[k];
    return e->shape >= 0 ? &s->shapes[e->shape] : &e->mesh;
}

/* Placement32::mesh. Not covered by the goldens (they hold local meshes). */
int cc_scene_world_mesh(const cc_scene *s, size_t k, Arena *a, cc_mesh *out) {
    const cc_scene_element *e = &s->elements[k];
    if (e->shape < 0) {
        *out = e->mesh;
        return 0;
    }
    const cc_mesh *sh = &s->shapes[e->shape];
    const cc_placement32 *p = &e->to_scene;
    *out = *sh;
    out->pos = arena_alloc(a, ((size_t)sh->nv + 1) * sizeof out->pos[0], 4);
    out->idx = arena_alloc(a, ((size_t)sh->ni + 1) * sizeof(uint32_t), 4);
    out->edges = arena_alloc(a, ((size_t)sh->ne + 1) * sizeof out->edges[0], 4);
    out->normals = arena_alloc(a, ((size_t)sh->nn + 1) * sizeof out->normals[0], 1);
    if (!out->pos || !out->idx || !out->edges || !out->normals) return -1;
    float c[3] = {p->y[1] * p->z[2] - p->y[2] * p->z[1], p->y[2] * p->z[0] - p->y[0] * p->z[2],
                  p->y[0] * p->z[1] - p->y[1] * p->z[0]};
    int mirrors = p->x[0] * c[0] + p->x[1] * c[1] + p->x[2] * c[2] < 0.0f;
    memcpy(out->idx, sh->idx, sh->ni * sizeof(uint32_t));
    if (mirrors)
        for (uint32_t t = 0; t + 2 < sh->ni; t += 3) {
            uint32_t x = out->idx[t + 1];
            out->idx[t + 1] = out->idx[t + 2];
            out->idx[t + 2] = x;
        }
    for (uint32_t i = 0; i < sh->nv + 2 * sh->ne; i++) {
        const float *q = i < sh->nv ? sh->pos[i] : sh->edges[(i - sh->nv) / 2] + 3 * ((i - sh->nv) % 2);
        float *o = i < sh->nv ? out->pos[i] : out->edges[(i - sh->nv) / 2] + 3 * ((i - sh->nv) % 2);
        float r[3];
        for (int j = 0; j < 3; j++) r[j] = p->x[j] * q[0] + p->y[j] * q[1] + p->z[j] * q[2] + p->t[j];
        memcpy(o, r, 12);
    }
    for (uint32_t i = 0; i < 2 * sh->nn; i++) {
        const int8_t *nq = sh->normals[i / 2] + 4 * (i % 2);
        int8_t *o = out->normals[i / 2] + 4 * (i % 2);
        if (!nq[0] && !nq[1] && !nq[2] && !nq[3]) continue;
        float u[3] = {(float)nq[0] / 127.0f, (float)nq[1] / 127.0f, (float)nq[2] / 127.0f}, v[3];
        for (int j = 0; j < 3; j++) v[j] = p->x[j] * u[0] + p->y[j] * u[1] + p->z[j] * u[2];
        float l = cc_sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        l = max32(l, 1e-12f);
        float nn[3] = {v[0] / l, v[1] / l, v[2] / l};
        pack_normal(nn, o);
    }
    return 0;
}
