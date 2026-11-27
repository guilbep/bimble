/* Suite hlod (ticket 3D2, ADR 0078): HLOD proxies (src/hlod.h), a port of bimble-geom hlod.rs and
 * its tests, on golden/frame.json and golden/frame2.json (Rust's tests used concrete_frame 4 x 3 x 3
 * bays, which the C core has no generator for):
 *   - proxies_stay_within_their_error (hlod.rs:519): every proxy point is within its cell's error of
 *     the cell's own elements, every element thicker than twice the error is within it of the proxy,
 *     triangles drop level by level, the root's proxy under half the scene's.
 *   - proxies_fit_their_cap (hlod.rs:586), empty_scene_has_no_cells (hlod.rs:607).
 *   - 3D2: built in steps of a few triangles, the BPRX bytes are those of one build; the BPRX layout.
 *   - 3D2, hostile input: one element of 400,000 triangles and one with non-finite vertices, default
 *     options: it ends, each proxy under its cap, the cells bounded, the non-finite ones skipped.
 *   - LNK3D: BPRX read back from a link cache, untrusted (cc_hlod_bprx_check): damages refused, no crash.
 * Fails on the code before 3D2: src/hlod.h did not exist. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/arena.h"
#include "../src/buf.h"
#include "../src/file.h"
#include "../src/hlod.h"
#include "../src/math.h"
#include "../src/model.h"
#include "../src/scene.h"
#include "../src/scene_build.h"
#include "../session/link_cache.h" /* SIZE2: the proxies' budget */
#include "check.h"

typedef double P[3];

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void sub(const double *a, const double *b, double *o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static double dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* Distance from a point to a triangle (Ericson, Real-Time Collision Detection 5.1.5; hlod.rs:426). */
static double to_triangle(const double *p, const double (*t)[3]) {
    const double *a = t[0], *b = t[1], *c = t[2];
    double ab[3], ac[3], ap[3], bp[3], cp[3], q[3];
    sub(b, a, ab), sub(c, a, ac), sub(p, a, ap), sub(p, b, bp), sub(p, c, cp);
    double d1 = dot(ab, ap), d2 = dot(ac, ap), d3 = dot(ab, bp), d4 = dot(ac, bp), d5 = dot(ab, cp), d6 = dot(ac, cp);
    double va = d3 * d6 - d5 * d4, vb = d5 * d2 - d1 * d6, vc = d1 * d4 - d3 * d2;
#define AT(u, v) for (int k = 0; k < 3; k++) q[k] = a[k] + (u) * ab[k] + (v) * ac[k]
    if (d1 <= 0 && d2 <= 0) memcpy(q, a, 24);
    else if (d3 >= 0 && d4 <= d3) memcpy(q, b, 24);
    else if (d6 >= 0 && d5 <= d6) memcpy(q, c, 24);
    else if (vc <= 0 && d1 >= 0 && d3 <= 0) { AT(d1 / (d1 - d3), 0.0); }
    else if (vb <= 0 && d2 >= 0 && d6 <= 0) { AT(0.0, d2 / (d2 - d6)); }
    else if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
        double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; k++) q[k] = b[k] + w * (c[k] - b[k]);
    } else {
        double d = 1.0 / (va + vb + vc);
        AT(vb * d, vc * d);
    }
#undef AT
    double d[3];
    sub(p, q, d);
    return cc_sqrt(dot(d, d));
}

typedef struct {
    double (*t)[3][3];
    size_t n, cap;
} Tris;

static void push_mesh(Tris *ts, const cc_mesh *m) {
    for (uint32_t i = 0; i + 2 < m->ni; i += 3) {
        if (ts->n == ts->cap) ts->cap = ts->cap ? 2 * ts->cap : 256, ts->t = realloc(ts->t, ts->cap * sizeof *ts->t);
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++) ts->t[ts->n][c][k] = m->pos[m->idx[i + c]][k];
        ts->n++;
    }
}

static double nearest(const double *p, const Tris *ts) {
    double best = 1e300;
    for (size_t i = 0; i < ts->n; i++) {
        double lb = 0; /* the distance to the triangle's box: a lower bound (the test's speed only) */
        for (int k = 0; k < 3; k++) {
            double lo = ts->t[i][0][k], hi = lo;
            for (int c = 1; c < 3; c++) lo = ts->t[i][c][k] < lo ? ts->t[i][c][k] : lo, hi = ts->t[i][c][k] > hi ? ts->t[i][c][k] : hi;
            double o = p[k] < lo ? lo - p[k] : p[k] > hi ? p[k] - hi : 0;
            lb += o * o;
        }
        if (lb >= best * best) continue;
        double d = to_triangle(p, (const double(*)[3])ts->t[i]);
        if (d < best) best = d;
    }
    return best;
}

/* The worst distance of the samples of `from` (corners, edge midpoints, centroids; hlod.rs:466) to `to`. */
static double worst(const Tris *from, const Tris *to) {
    static const double W[7][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {.5, .5, 0}, {0, .5, .5}, {.5, 0, .5}, {1. / 3, 1. / 3, 1. / 3}};
    double w = 0;
    for (size_t i = 0; i < from->n; i++)
        for (int s = 0; s < 7; s++) {
            double p[3];
            for (int k = 0; k < 3; k++) p[k] = W[s][0] * from->t[i][0][k] + W[s][1] * from->t[i][1][k] + W[s][2] * from->t[i][2][k];
            double d = nearest(p, to);
            if (d > w) w = d;
        }
    return w;
}

static uint64_t scene_tris(const cc_scene *s) {
    uint64_t n = 0;
    for (size_t k = 0; k < s->nelements; k++) n += cc_scene_local_mesh(s, k)->ni / 3;
    return n;
}

/* hlod.rs:519 proxies_stay_within_their_error */
static void within_error(const char *what, const cc_scene *s) {
    cc_hlod_opts o;
    cc_hlod_default(&o);
    o.leaf_triangles = 64, o.resolution = 16, o.max_ratio = 1.0;
    cc_hlod h;
    cc_error err;
    char t[200];
    snprintf(t, sizeof t, "%s: the tree builds", what);
    check_true(t, cc_hlod_build(&h, s, &o, &err) == 0);
    uint32_t deepest = 0;
    for (uint32_t i = 0; i < h.ncells; i++)
        if (h.cells[i].depth > deepest) deepest = h.cells[i].depth;
    snprintf(t, sizeof t, "%s: %u levels (at least 3)", what, deepest);
    check_true(t, deepest >= 3);
    Arena a;
    arena_init(&a, 0);
    int bad = 0;
    double worst_proxy = 0;
    for (uint32_t i = 0; i < h.ncells && bad < 3; i++) {
        const cc_hlod_cell *c = &h.cells[i];
        Tris proxy = {0}, original = {0};
        for (uint32_t k = 0; k < c->nproxy; k++) push_mesh(&proxy, &c->proxy[k].mesh);
        if (!proxy.n) { printf("  cell %u has no proxy\n", i); bad++; continue; }
        double err_mm = (double)c->error_mm + 1e-3;
        arena_reset(&a);
        for (uint32_t j = 0; j < c->nel; j++) {
            cc_mesh m;
            if (cc_scene_world_mesh(s, h.elements[c->el0 + j], &a, &m) == 0) push_mesh(&original, &m);
        }
        double wp = worst(&proxy, &original);
        if (wp > worst_proxy) worst_proxy = wp;
        if (wp > err_mm) { printf("  cell %u: proxy %g mm off, error %g\n", i, wp, err_mm); bad++; }
        for (uint32_t j = 0; j < c->nel; j++) {
            cc_mesh m;
            if (cc_scene_world_mesh(s, h.elements[c->el0 + j], &a, &m)) continue;
            float b[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
            for (uint32_t v = 0; v < m.nv; v++)
                for (int k = 0; k < 3; k++) {
                    if (m.pos[v][k] < b[k]) b[k] = m.pos[v][k];
                    if (m.pos[v][k] > b[3 + k]) b[3 + k] = m.pos[v][k];
                }
            int thin = 0;
            for (int k = 0; k < 3; k++) thin |= (double)(b[3 + k] - b[k]) < 2.0 * err_mm;
            if (thin) continue;
            Tris one = {0};
            push_mesh(&one, &m);
            double we = worst(&one, &proxy);
            if (we > err_mm) { printf("  cell %u: element %u %g mm from the proxy, error %g\n", i, h.elements[c->el0 + j], we, err_mm); bad++; }
            free(one.t);
        }
        free(proxy.t), free(original.t);
    }
    snprintf(t, sizeof t, "%s: every proxy within its error and every thick element within it of the proxy (%u cells, worst %.1f mm)", what, h.ncells, worst_proxy);
    check_true(t, !bad);
    printf("  %s\n", t);
    /* what is drawn when every cell at depth d (or a shallower leaf) draws its proxy */
    uint64_t prev = 0;
    int mono = 1;
    for (uint32_t d = 0; d <= deepest; d++) {
        uint64_t n = 0;
        for (uint32_t i = 0; i < h.ncells; i++)
            if (h.cells[i].depth == d || (h.cells[i].depth < d && !h.cells[i].nchildren)) n += cc_hlod_proxy_triangles(&h.cells[i]);
        if (d == 0) {
            snprintf(t, sizeof t, "%s: the root's proxy (%llu triangles) under half the scene's (%llu)", what, (unsigned long long)n, (unsigned long long)scene_tris(s));
            check_true(t, n < scene_tris(s) / 2);
        } else mono &= prev <= n;
        prev = n;
    }
    snprintf(t, sizeof t, "%s: triangles drop level by level", what);
    check_true(t, mono);
    arena_free(&a);
    cc_hlod_free(&h);
}

/* hlod.rs:586 proxies_fit_their_cap */
static void fit_cap(const cc_scene *s) {
    cc_hlod_opts o = {UINT64_MAX, 4096, 100, 1.0};
    cc_hlod h;
    cc_error err;
    check_true("cap: builds", cc_hlod_build(&h, s, &o, &err) == 0);
    check_true("cap: one cell", h.ncells == 1);
    if (h.ncells != 1) { cc_hlod_free(&h); return; }
    const cc_hlod_cell *r = &h.cells[0];
    check_true("cap: at most 100 triangles", cc_hlod_proxy_triangles(r) <= 100 && cc_hlod_proxy_triangles(r) > 0);
    check_true("cap: every element under the root", r->nel == s->nelements);
    float side = 0;
    for (int k = 0; k < 3; k++)
        if (r->bounds[3 + k] - r->bounds[k] > side) side = r->bounds[3 + k] - r->bounds[k];
    check_true("cap: the error says the grid coarsened", r->error_mm > side / 4096.0f * 1.7320508f);
    cc_hlod_free(&h);
}

static uint32_t u32at(const Buf *b, size_t o) {
    uint32_t v;
    memcpy(&v, b->p + o, 4);
    return v;
}

/* 3D2: steps of a few triangles give the bytes of one build; the BPRX layout adds up. */
/* LNK3D (ADR 0078 amendment 3): BPRX read back from a link cache is untrusted (hlod.h cc_hlod_bprx_check):
 * the bytes of a build pass; each targeted damage is refused; 20,000 random byte changes and every cut
 * end without a crash (ASan, UBSan suites) and a cut is always refused. */
static uint32_t rnd(uint64_t *x) { *x = *x * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(*x >> 33); }
static void put32(char *p, uint32_t v) { memcpy(p, &v, 4); }
static void bprx_hostile(const Buf *one, uint32_t ne) {
    cc_error err;
    const uint8_t *p = (const uint8_t *)one->p;
    size_t n = one->len;
    check_true("BPRX of a build: accepted", n >= 48 && cc_hlod_bprx_check(p, n, ne, &err) == 0);
    check_true("BPRX of another scene (nE): refused", cc_hlod_bprx_check(p, n, ne + 1, &err) == -1);
    char *c = malloc(n ? n : 1);
    if (!c || n < 48) { free(c); check_true("BPRX hostile: a buffer", 0); return; }
    uint32_t nC = u32at(one, 16), nK = u32at(one, 20), nL = u32at(one, 24), nP = u32at(one, 28), nV = u32at(one, 32);
    size_t kids = 48 + 64 * (size_t)nC, els = kids + 4 * (size_t)nK, parts = els + 4 * (size_t)nL, idx = parts + 28 * (size_t)nP + 12 * (size_t)nV;
    struct { const char *what; size_t at; uint32_t v; } dmg[] = {
        {"the magic", 0, 0x58525042u ^ 1}, {"the version", 4, 2}, {"a count over the bytes (cells)", 16, nC + 1},
        {"a count huge (cells)", 16, 0xffffffffu}, {"the vertices' count", 32, nV + 7},
        {"a cell's first child past the list", 48 + 32, nK + 1}, {"a cell's elements past the list", 48 + 40, nL},
        {"a cell's depth over 16", 48, 99},
        {"a child that is its own parent (a cycle)", kids, 0}, {"an element past nE", els, ne},
        {"a part's vertices past the end", parts + 4, nV}, {"an index outside its part", idx, 0xfffffff0u},
    };
    for (size_t i = 0; i < sizeof dmg / sizeof dmg[0]; i++) {
        if (dmg[i].at + 4 > n || (dmg[i].at >= kids && dmg[i].at < els && !nK) || (dmg[i].at >= parts && !nP)) continue;
        memcpy(c, p, n);
        put32(c + dmg[i].at, dmg[i].v);
        char t[160];
        snprintf(t, sizeof t, "BPRX damaged (%s): refused", dmg[i].what);
        check_true(t, cc_hlod_bprx_check((const uint8_t *)c, n, ne, &err) == -1);
    }
    float inf = 1.0f / 0.0f;
    memcpy(c, p, n);
    memcpy(c + 48 + 4, &inf, 4);
    check_true("BPRX damaged (an infinite error): refused", cc_hlod_bprx_check((const uint8_t *)c, n, ne, &err) == -1);
    int cuts = 1;
    for (size_t k = 0; k < n; k += 1 + k / 64)
        if (cc_hlod_bprx_check(p, k, ne, &err) != -1) cuts = 0;
    check_true("every cut BPRX: refused", cuts);
    uint64_t x = 42;
    for (int i = 0; i < 20000; i++) { /* random changes: an answer, never a crash */
        memcpy(c, p, n);
        for (int j = 0; j < 4; j++) c[rnd(&x) % n] = (char)rnd(&x);
        (void)cc_hlod_bprx_check((const uint8_t *)c, n, ne, &err);
    }
    check_true("20,000 random changes of BPRX: no crash", 1);
    free(c);
}

static void steps(const char *what, const cc_scene *s, uint64_t budget) {
    cc_hlod_opts o;
    cc_hlod_default(&o);
    o.leaf_triangles = 200;
    cc_hlod a, b;
    cc_error err;
    Buf one, many;
    buf_init(&one), buf_init(&many);
    int ok = cc_hlod_build(&a, s, &o, &err) == 0 && cc_hlod_bprx(&a, 5, (uint32_t)s->nelements, &one, &err) == 0;
    ok &= cc_hlod_start(&b, s, &o, &err) == 0;
    /* 3D2: the tree alone, before any step: the built tree's cells, children and elements, no part */
    Buf tree;
    buf_init(&tree);
    if (ok && cc_hlod_bprx_tree(&b, 5, (uint32_t)s->nelements, &tree, &err) == 0 && tree.len >= 48 && one.len >= 48) {
        uint32_t nC = u32at(&tree, 16), nK = u32at(&tree, 20), nL = u32at(&tree, 24);
        int same = !memcmp(tree.p, "BPRX", 4) && nC == u32at(&one, 16) && nK == u32at(&one, 20) && nL == u32at(&one, 24) &&
                   !u32at(&tree, 28) && !u32at(&tree, 32) && !u32at(&tree, 36) && !u32at(&tree, 40) &&
                   tree.len == 48 + 64 * (size_t)nC + 4 * ((size_t)nK + nL);
        for (uint32_t c = 0; same && c < nC; c++) {
            const char *x = tree.p + 48 + 64 * (size_t)c, *y = one.p + 48 + 64 * (size_t)c;
            /* depth, bounds, children, elements, original triangles alike; error 0 and no part in the tree */
            same &= !memcmp(x, y, 4) && !memcmp(x + 8, y + 8, 40) && !memcmp(x + 56, y + 56, 8) && !u32at(&tree, 48 + 64 * (size_t)c + 52);
        }
        size_t lists = 48 + 64 * (size_t)nC;
        same &= !memcmp(tree.p + lists, one.p + lists, 4 * ((size_t)nK + nL));
        check_true("the tree alone, before any step: the built tree's cells, children and elements, no proxy", same);
    } else check_true("the tree alone, before any step", 0);
    buf_free(&tree);
    int n = 0, r = 0;
    while (ok && (r = cc_hlod_step(&b, budget, &err)) == 0) {
        n++;
        Buf early;
        buf_init(&early);
        if (n == 1) check_true("BPRX refused before the end", cc_hlod_bprx(&b, 5, 0, &early, &err) == -1);
        buf_free(&early);
    }
    ok &= r == 1 && cc_hlod_bprx(&b, 5, (uint32_t)s->nelements, &many, &err) == 0;
    char t[200];
    snprintf(t, sizeof t, "%s: %d steps of %llu triangles give the bytes of one build (%zu bytes, %u cells)", what, n + 1,
             (unsigned long long)budget, one.len, a.ncells);
    check_true(t, ok && n > 2 && one.len == many.len && !memcmp(one.p, many.p, one.len));
    check_true("read all", b.read == b.total && b.total > 0);
    /* layout */
    if (one.len >= 48) {
        uint32_t nC = u32at(&one, 16), nK = u32at(&one, 20), nL = u32at(&one, 24), nP = u32at(&one, 28), nV = u32at(&one, 32),
                 nI = u32at(&one, 36), nS = u32at(&one, 40);
        size_t want = 48 + 64 * (size_t)nC + 4 * ((size_t)nK + nL) + 28 * (size_t)nP + 12 * (size_t)nV + 4 * (size_t)nI + 32 * (size_t)nS;
        check_true("BPRX: header and size", !memcmp(one.p, "BPRX", 4) && u32at(&one, 4) == 1 && u32at(&one, 8) == 5 &&
                                               u32at(&one, 12) == s->nelements && nC == a.ncells && nL == a.nelements && one.len == want);
        int idx_ok = 1;
        size_t I = 48 + 64 * (size_t)nC + 4 * ((size_t)nK + nL) + 28 * (size_t)nP + 12 * (size_t)nV;
        for (uint32_t i = 0; i < nI; i++) idx_ok &= u32at(&one, I + 4 * (size_t)i) < nV;
        check_true("BPRX: indices in range", idx_ok);
    }
    bprx_hostile(&one, (uint32_t)s->nelements);
    buf_free(&one), buf_free(&many);
    cc_hlod_free(&a), cc_hlod_free(&b);
}

/* 3D2: hostile input. One element of 2 x 447 x 447 triangles (a wavy sheet, 10 m), one with
 * non-finite vertices, and 200 small ones. */
static void hostile(void) {
    cc_scene s;
    memset(&s, 0, sizeof s);
    arena_init(&s.arena, 0);
    size_t ne = 202;
    s.elements = arena_alloc(&s.arena, ne * sizeof *s.elements, 8);
    const uint32_t N = 447;
    cc_mesh *big = &s.elements[0].mesh;
    big->nv = (N + 1) * (N + 1), big->ni = 6 * N * N;
    big->pos = arena_alloc(&s.arena, (size_t)big->nv * 12, 4);
    big->idx = arena_alloc(&s.arena, (size_t)big->ni * 4, 4);
    for (uint32_t y = 0; y <= N; y++)
        for (uint32_t x = 0; x <= N; x++) {
            float *p = big->pos[y * (N + 1) + x];
            p[0] = (float)x * 22.0f, p[1] = (float)y * 22.0f, p[2] = (float)((x * 7 + y * 13) % 17) * 3.0f;
        }
    uint32_t k = 0;
    for (uint32_t y = 0; y < N; y++)
        for (uint32_t x = 0; x < N; x++) {
            uint32_t a = y * (N + 1) + x;
            uint32_t q[6] = {a, a + 1, a + N + 2, a, a + N + 2, a + N + 1};
            memcpy(big->idx + k, q, sizeof q), k += 6;
        }
    cc_mesh *nan = &s.elements[1].mesh;
    nan->nv = 4, nan->ni = 6;
    nan->pos = arena_alloc(&s.arena, 48, 4);
    nan->idx = arena_alloc(&s.arena, 24, 4);
    float inf = 1e30f * 1e30f;
    float np[4][3] = {{0, 0, 0}, {inf, 0, 0}, {0, -inf, 0}, {inf - inf, 1, 1}};
    memcpy(nan->pos, np, sizeof np);
    uint32_t ni[6] = {0, 1, 2, 0, 2, 3};
    memcpy(nan->idx, ni, sizeof ni);
    for (size_t e = 2; e < ne; e++) {
        cc_mesh *m = &s.elements[e].mesh;
        m->nv = 3, m->ni = 3;
        m->pos = arena_alloc(&s.arena, 36, 4);
        m->idx = arena_alloc(&s.arena, 12, 4);
        float p[3][3] = {{(float)e * 50, 0, 0}, {(float)e * 50 + 40, 0, 0}, {(float)e * 50, 40, 9}};
        memcpy(m->pos, p, sizeof p);
        m->idx[0] = 0, m->idx[1] = 1, m->idx[2] = 2;
    }
    for (size_t e = 0; e < ne; e++) s.elements[e].shape = -1, s.elements[e].category = (cc_category)(e % CC_CAT_COUNT);
    s.nelements = ne;
    cc_hlod_opts o;
    cc_hlod_default(&o);
    cc_hlod h;
    cc_error err;
    double t0 = now_ms();
    int ok = cc_hlod_build(&h, &s, &o, &err) == 0;
    double ms = now_ms() - t0;
    uint64_t most = 0;
    int finite = 1;
    for (uint32_t i = 0; ok && i < h.ncells; i++) {
        uint64_t n = cc_hlod_proxy_triangles(&h.cells[i]);
        if (n > most) most = n;
        for (uint32_t p = 0; p < h.cells[i].nproxy; p++) {
            const cc_mesh *m = &h.cells[i].proxy[p].mesh;
            for (uint32_t v = 0; v < m->nv; v++)
                for (int c = 0; c < 3; c++) finite &= m->pos[v][c] - m->pos[v][c] == 0.0f;
        }
    }
    char t[200];
    snprintf(t, sizeof t, "hostile: built in %.0f ms, %u cells (at most 2 per element), largest proxy %llu triangles (cap %llu)", ms,
             ok ? h.ncells : 0, (unsigned long long)most, (unsigned long long)o.max_proxy_triangles);
    printf("  %s\n", t);
    check_true(t, ok && h.ncells <= 2 * ne && most <= o.max_proxy_triangles && ms < 20000);
    check_true("hostile: no non-finite proxy vertex", ok && finite);
    /* 3D2: steps of 5,000 triangles cut the large element into chunks: the same bytes as one build */
    if (ok) {
        cc_hlod b;
        Buf one, many;
        buf_init(&one), buf_init(&many);
        int n = 0, r = 0, same = cc_hlod_bprx(&h, 1, (uint32_t)ne, &one, &err) == 0 && cc_hlod_start(&b, &s, &o, &err) == 0;
        while (same && (r = cc_hlod_step(&b, 5000, &err)) == 0) n++;
        same &= r == 1 && cc_hlod_bprx(&b, 1, (uint32_t)ne, &many, &err) == 0 && one.len == many.len && !memcmp(one.p, many.p, one.len);
        snprintf(t, sizeof t, "hostile: %d steps of 5,000 triangles (the large element in chunks) give the bytes of one build", n + 1);
        check_true(t, same && n > 50);
        cc_hlod_free(&b);
        buf_free(&one), buf_free(&many);
    }
    if (ok) cc_hlod_free(&h);
    arena_free(&s.arena);
}

/* `s` tiled nx x ny times side by side (own meshes, positions moved), into `out` (a larger frame, as
 * Rust's tests built 4 x 3 x 3 bays). */
static int tiled(const cc_scene *s, int nx, int ny, cc_scene *out) {
    memset(out, 0, sizeof *out);
    arena_init(&out->arena, 0);
    float b[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    Arena a;
    arena_init(&a, 0);
    for (size_t k = 0; k < s->nelements; k++) {
        cc_mesh m;
        if (cc_scene_world_mesh(s, k, &a, &m)) return -1;
        for (uint32_t v = 0; v < m.nv; v++)
            for (int c = 0; c < 3; c++) {
                if (m.pos[v][c] < b[c]) b[c] = m.pos[v][c];
                if (m.pos[v][c] > b[3 + c]) b[3 + c] = m.pos[v][c];
            }
    }
    size_t n = s->nelements * (size_t)(nx * ny);
    out->elements = arena_alloc(&out->arena, n * sizeof *out->elements, 8);
    for (int i = 0; i < nx * ny; i++)
        for (size_t k = 0; k < s->nelements; k++) {
            cc_scene_element *e = &out->elements[out->nelements++];
            *e = s->elements[k];
            e->shape = -1;
            if (cc_scene_world_mesh(s, k, &out->arena, &e->mesh)) return -1;
            cc_mesh *m = &e->mesh;
            float(*p)[3] = arena_alloc(&out->arena, ((size_t)m->nv + 1) * 12, 4);
            float(*g)[6] = arena_alloc(&out->arena, ((size_t)m->ne + 1) * 24, 4);
            float dx = (float)(i % nx) * (b[3] - b[0] + 1000.0f), dy = (float)(i / nx) * (b[4] - b[1] + 1000.0f);
            for (uint32_t v = 0; v < m->nv; v++) p[v][0] = m->pos[v][0] + dx, p[v][1] = m->pos[v][1] + dy, p[v][2] = m->pos[v][2];
            for (uint32_t v = 0; v < m->ne; v++)
                for (int c = 0; c < 6; c++) g[v][c] = m->edges[v][c] + (c % 3 == 0 ? dx : c % 3 == 1 ? dy : 0.0f);
            m->pos = p, m->edges = g;
        }
    arena_free(&a);
    return 0;
}

static int load(const char *dir, const char *name, cc_model *m, cc_scene *s) {
    size_t n;
    char *text = slurp(dir, name, &n);
    cc_error err;
    int rc = cc_model_load(m, text, n, &err);
    free(text);
    if (rc) { printf("%s does not load: %s\n", name, err.msg); return -1; }
    if (cc_scene_build(s, m, &err)) { printf("%s: no scene: %s\n", name, err.msg); return -1; }
    return 0;
}

/* SIZE2 (link_cache.h cc_link_hlod_budget): a link's proxies cut to a byte budget, coarsest cells kept:
 * what is kept fits the budget, no kept cell is deeper than a cell left without, the BPRX still checks. */
static void budget(const char *what, const cc_scene *s) {
    cc_hlod h;
    cc_hlod_opts o;
    cc_hlod_default(&o);
    o.leaf_triangles = 64, o.resolution = 16, o.max_ratio = 1.0; /* a deep tree from a small scene (within_error's) */
    cc_error err;
    char t[160];
    int ok = cc_hlod_build(&h, s, &o, &err) == 0;
    uint64_t total = 0;
    for (uint32_t i = 0; ok && i < h.ncells; i++) total += cc_hlod_proxy_bytes(&h.cells[i]);
    uint32_t had = 0;
    uint8_t *had_proxy = ok ? calloc(h.ncells + 1, 1) : NULL;
    for (uint32_t i = 0; ok && i < h.ncells; i++) had += h.cells[i].nproxy > 0, had_proxy ? had_proxy[i] = h.cells[i].nproxy > 0 : 0;
    uint32_t dropped = ok ? cc_link_hlod_budget(&h, total / 3) : 0;
    uint64_t kept = 0;
    uint32_t deepest_kept = 0;
    for (uint32_t i = 0; ok && i < h.ncells; i++) {
        const cc_hlod_cell *c = &h.cells[i];
        kept += cc_hlod_proxy_bytes(c);
        if (c->nproxy && c->depth > deepest_kept) deepest_kept = c->depth;
    }
    snprintf(t, sizeof t, "%s: proxies cut to a third (%llu -> %llu bytes, %u of %u cells dropped)", what, (unsigned long long)total,
             (unsigned long long)kept, dropped, had);
    check_true(t, ok && kept <= total / 3 && dropped > 0);
    /* the coarsest first: every cell that had a proxy and is shallower than the deepest kept one kept it */
    int order = 1;
    for (uint32_t i = 0; ok && i < h.ncells; i++)
        if (had_proxy && had_proxy[i] && !h.cells[i].nproxy && h.cells[i].depth < deepest_kept) order = 0;
    free(had_proxy);
    Buf b;
    buf_init(&b);
    ok = ok && cc_hlod_bprx(&h, 0, (uint32_t)s->nelements, &b, &err) == 0 && cc_hlod_bprx_check((const uint8_t *)b.p, b.len, (uint32_t)s->nelements, &err) == 0;
    snprintf(t, sizeof t, "%s: the cut BPRX still checks, coarsest kept (deepest kept depth %u)", what, deepest_kept);
    check_true(t, ok && order);
    buf_free(&b);
    cc_hlod_free(&h);
    check_true("the budget: 15 % of the geometry within its bounds", cc_link_hlod_cap(0) == CC_LINK_HLOD_MIN && cc_link_hlod_cap((uint64_t)1 << 40) == CC_LINK_HLOD_MAX &&
                                                                    cc_link_hlod_cap((uint64_t)100 << 20) == ((uint64_t)100 << 20) / 100 * 15);
}

int main(int argc, char **argv) {
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    const char *names[2] = {"frame.json", "frame2.json"};
    for (int i = 0; i < 2; i++) {
        cc_model m;
        cc_scene s;
        if (load(dir, names[i], &m, &s)) return 1;
        cc_scene big;
        char what[64];
        snprintf(what, sizeof what, "%s tiled 3 x 3", names[i]);
        if (tiled(&s, 3, 3, &big) == 0) within_error(what, &big);
        else check_true("tiled", 0);
        cc_scene_free(&big);
        if (i == 0) fit_cap(&s), steps(names[i], &s, 40);
        if (i == 0) { /* SIZE2 */
            cc_scene big2;
            if (tiled(&s, 3, 3, &big2) == 0) budget("frame.json tiled 3 x 3", &big2);
            cc_scene_free(&big2);
        }
        cc_scene_free(&s);
        cc_model_free(&m);
    }
    cc_scene empty;
    memset(&empty, 0, sizeof empty);
    cc_hlod_opts o;
    cc_hlod_default(&o);
    cc_hlod h;
    cc_error err;
    check_true("an empty scene has no cells", cc_hlod_build(&h, &empty, &o, &err) == 0 && h.ncells == 0);
    cc_hlod_free(&h);
    hostile();
    return report("hlod");
}
