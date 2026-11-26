/* Suite bvh (ticket 3D2): the box tree (src/bvh.h, a port of bimble_geom::bvh) and the measure
 * tool's ray through it (session/measure.c, cc_mboxes' tree).
 *   - bvh.rs `queries_match_brute_force`: 5,000 and 700 boxes of the same generator, the same box
 *     query, the tree-vs-tree pairs and 50 rays: what the tree answers, checking every box answers.
 *   - an empty tree (no box, only empty boxes): every query answers nothing; non-finite boxes (NaN,
 *     infinite) are left out; equally near hits go to the smallest key; more than CC_BVH_MAX_ITEMS
 *     boxes is an error, 200,000 equal boxes and 200,000 in order build in bounded depth.
 *   - element_boxes (scene.rs SceneElement::bounds) on golden/frame.json and on own meshes mixed
 *     with placed copies: an own mesh's box is its points' box, a copy's holds its placed points.
 *   - the measure tool on 1,201 bipyramids (600 copies of one shape, 600 own, one with an infinite
 *     point: a box left out of the tree), one layer and two: the same snap (kind, point, normal,
 *     bit for bit) through the tree as with every box tested and as testing every element, with
 *     fewer boxes tested.
 * Fails on the code before 3D2: bvh.h did not exist, nor cc_mlayer.index. */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../session/measure.h"
#include "../src/arena.h"
#include "../src/bvh.h"
#include "../src/file.h"
#include "../src/model.h"
#include "../src/scene.h"
#include "../src/scene_build.h"
#include "check.h"

#pragma STDC FP_CONTRACT OFF /* f * 7.31 + seed rounds twice, as in Rust */

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* f32 `%` for x >= 0, y > 0 (exact: no libm). */
static float fmodp(float x, float y) {
    double X = x, Y = y, q = (double)(long long)(X / Y), r = X - q * Y;
    while (r < 0) r += Y;
    while (r >= Y) r -= Y;
    return (float)r;
}

/* bvh.rs:269 `boxes`: a grid of unit boxes with gaps, deterministic. */
static cc_aabb *gen_boxes(size_t n, float seed) {
    cc_aabb *b = malloc(n * sizeof *b);
    for (size_t i = 0; i < n; i++) {
        float f = (float)i;
        float p[3] = {fmodp(f * 7.31f + seed, 97.0f), fmodp(f * 3.17f, 89.0f), fmodp(f * 1.93f + seed, 31.0f)};
        for (int k = 0; k < 3; k++) b[i].min[k] = p[k], b[i].max[k] = p[k] + 1.0f + fmodp(f, 3.0f);
    }
    return b;
}

typedef struct {
    uint64_t *v;
    size_t n, cap;
} List;

static void push(List *l, uint64_t x) {
    if (l->n == l->cap) {
        l->cap = l->cap ? 2 * l->cap : 64;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = x;
}

static void on_item(void *ctx, uint32_t i) { push(ctx, i); }
static void on_pair(void *ctx, uint32_t i, uint32_t j) { push(ctx, (uint64_t)i << 32 | j); }

static int cmp_u64(const void *x, const void *y) {
    uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
    return a < b ? -1 : a > b;
}

static int same_list(List *got, List *want) {
    qsort(got->v, got->n, sizeof *got->v, cmp_u64);
    return got->n == want->n && (!got->n || !memcmp(got->v, want->v, got->n * sizeof *got->v));
}

typedef struct {
    const cc_aabb *a;
    const float *o, *inv;
} RayCtx;

/* bvh.rs:316: the entry distance as the "hit", the item as the key. */
static int entry_hit(void *ctx, uint32_t i, float entry, float *dist, uint64_t *key) {
    const RayCtx *c = ctx;
    (void)entry;
    float t;
    if (!cc_aabb_ray_entry(&c->a[i], c->o, c->inv, 1.0f / 0.0f, &t)) return 0;
    *dist = t, *key = i;
    return 1;
}

/* bvh.rs:288 queries_match_brute_force: the tree answers exactly what checking every box answers. */
static void brute_force(void) {
    size_t na = 5000, nb = 700;
    cc_aabb *a = gen_boxes(na, 0.0f), *b = gen_boxes(nb, 13.7f);
    Arena ar;
    arena_init(&ar, 0);
    cc_bvh ta, tb;
    check_true("brute force: both trees build", !cc_bvh_build(&ta, a, na, &ar) && !cc_bvh_build(&tb, b, nb, &ar));
    printf("  5,000 boxes: %zu nodes, depth %u; 700 boxes: %zu nodes, depth %u\n", ta.nnodes, ta.depth, tb.nnodes, tb.depth);
    check_true("brute force: every box in, depth ~log2(n / 4)", ta.nitems == na && tb.nitems == nb && ta.depth <= 11 && tb.depth <= 8);

    cc_aabb q = {{10.0f, 20.0f, 5.0f}, {40.0f, 35.0f, 12.0f}};
    List got = {0}, want = {0};
    cc_bvh_overlapping(&ta, &q, on_item, &got);
    for (uint32_t i = 0; i < na; i++)
        if (cc_aabb_overlaps(&a[i], &q)) push(&want, i);
    check_true("brute force: overlapping = every box that overlaps", want.n > 0 && same_list(&got, &want));
    printf("  overlapping: %zu boxes\n", want.n);

    got.n = 0, want.n = 0;
    cc_bvh_pairs(&ta, &tb, on_pair, &got);
    for (uint32_t i = 0; i < na; i++)
        for (uint32_t j = 0; j < nb; j++)
            if (cc_aabb_overlaps(&a[i], &b[j])) push(&want, (uint64_t)i << 32 | j);
    check_true("brute force: pairs = every pair that overlaps", want.n > 0 && same_list(&got, &want));
    printf("  pairs: %zu\n", want.n);

    /* Nearest box along rays: entry distance as the "hit". */
    int ok = 1, hits = 0;
    for (int r = 0; r < 50; r++) {
        float o[3] = {-5.0f, (float)r * 1.7f, (float)r * 0.6f};
        float d[3] = {1.0f, 0.1f * (float)(r % 5) - 0.2f, 0.0f};
        float inv[3] = {1.0f / d[0], 1.0f / d[1], 1.0f / d[2]};
        RayCtx c = {a, o, inv};
        float gd = 0, wd = 0;
        uint64_t gk = 0;
        int g = cc_bvh_ray(&ta, o, d, entry_hit, &c, &gd, &gk), w = 0;
        for (uint32_t i = 0; i < na; i++) {
            float t;
            if (cc_aabb_ray_entry(&a[i], o, inv, 1.0f / 0.0f, &t) && (!w || t < wd)) w = 1, wd = t;
        }
        hits += w;
        if (g != w || (g && gd != wd)) {
            ok = 0;
            printf("  ray %d: tree %d %g, brute %d %g\n", r, g, (double)gd, w, (double)wd);
        }
    }
    check_true("brute force: 50 rays, the nearest box as every box gives", ok && hits > 25);
    free(got.v), free(want.v), free(a), free(b);
    arena_free(&ar);
}

static int no_hit(void *ctx, uint32_t i, float entry, float *dist, uint64_t *key) {
    (void)ctx, (void)i, (void)entry, (void)dist, (void)key;
    return 1; /* never called on an empty tree */
}

static int fixed_hit(void *ctx, uint32_t i, float entry, float *dist, uint64_t *key) {
    (void)ctx, (void)entry;
    *dist = 20.0f, *key = 100 - i; /* every hit equally near (beyond every box): the smallest key wins */
    return 1;
}

static int stop_hit(void *ctx, uint32_t i, float entry, float *dist, uint64_t *key) {
    (void)ctx, (void)i, (void)entry, (void)dist, (void)key;
    return -1;
}

static int enter_all(void *ctx, const cc_aabb *b) {
    (void)ctx, (void)b;
    return 1;
}

static void edge_cases(void) {
    Arena ar;
    arena_init(&ar, 0);
    float inf = 1.0f / 0.0f, nan = inf - inf;
    float o[3] = {-10, 0.5f, 0.5f}, d[3] = {1, 0, 0}, dist = 0;
    uint64_t key = 0;
    List got = {0};

    /* empty trees */
    cc_bvh t;
    cc_aabb all = {{-inf, -inf, -inf}, {inf, inf, inf}};
    int ok = !cc_bvh_build(&t, NULL, 0, &ar) && cc_bvh_is_empty(&t) && t.nnodes == 0;
    ok &= cc_bvh_ray(&t, o, d, no_hit, NULL, &dist, &key) == 0;
    cc_bvh_overlapping(&t, &all, on_item, &got);
    cc_bvh_pairs(&t, &t, on_pair, &got);
    cc_bvh_visit(&t, enter_all, on_item, &got);
    check_true("empty: no box, no node, every query answers nothing", ok && got.n == 0);
    cc_aabb empties[3] = {cc_aabb_empty(), {{1, 0, 0}, {0, 1, 1}}, {{0, 0, 2}, {1, 1, 1}}};
    ok = !cc_bvh_build(&t, empties, 3, &ar) && cc_bvh_is_empty(&t) && t.nnodes == 0;
    ok &= cc_bvh_ray(&t, o, d, no_hit, NULL, &dist, &key) == 0;
    cc_bvh_overlapping(&t, &all, on_item, &got);
    check_true("empty: only empty boxes, an empty tree", ok && got.n == 0 && cc_aabb_is_empty(&empties[0]));

    /* non-finite boxes are left out */
    cc_aabb mixed[8];
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 3; k++) mixed[i].min[k] = (float)(2 * i), mixed[i].max[k] = (float)(2 * i + 1);
    mixed[1].min[0] = nan, mixed[3].max[2] = inf, mixed[5].min[1] = -inf, mixed[6].max[0] = nan;
    ok = !cc_bvh_build(&t, mixed, 8, &ar) && t.nitems == 4;
    for (size_t i = 0; i < t.nitems; i++) ok &= t.items[i] == 0 || t.items[i] == 2 || t.items[i] == 4 || t.items[i] == 7;
    cc_bvh_overlapping(&t, &all, on_item, &got);
    check_true("non-finite: NaN and infinite boxes left out, the 4 others in", ok && got.n == 4 && cc_aabb_is_valid(&t.nodes[0].bounds));
    float o2[3] = {-10, -10, -10}, d2[3] = {1, 1, 1};
    cc_aabb *a = mixed;
    float inv2[3] = {1, 1, 1};
    RayCtx c = {a, o2, inv2};
    ok = cc_bvh_ray(&t, o2, d2, entry_hit, &c, &dist, &key) == 1 && key == 0 && dist == 10.0f;
    check_true("non-finite: the diagonal ray's nearest box is box 0, at 10", ok);
    check_true("ray: a hit test's error stops the query", cc_bvh_ray(&t, o2, d2, stop_hit, NULL, &dist, &key) == -1);

    /* equally near hits: the smallest key, whatever the order they were found in */
    cc_aabb row[40];
    for (int i = 0; i < 40; i++)
        for (int k = 0; k < 3; k++) row[i].min[k] = 0, row[i].max[k] = 1;
    ok = !cc_bvh_build(&t, row, 40, &ar) && cc_bvh_ray(&t, o, d, fixed_hit, NULL, &dist, &key) == 1 && key == 61 && dist == 20.0f;
    check_true("ray: between equally near hits, the smallest key", ok);

    /* a ray along a face (0 x inf: NaN) keeps the other bound (an infinite one: a miss, as Rust) */
    cc_aabb unit = {{0, 0, 0}, {1, 1, 1}};
    float on_face[3] = {-1, 0, 0.5f}, on_max[3] = {-1, 1, 0.5f}, in_slab[3] = {-1, 0.5f, 0.5f}, inv[3] = {1, inf, inf}, te = -1;
    check_true("ray_entry: a ray along a face misses, a ray inside enters at 1",
               !cc_aabb_ray_entry(&unit, on_face, inv, inf, &te) && !cc_aabb_ray_entry(&unit, on_max, inv, inf, &te) &&
                   cc_aabb_ray_entry(&unit, in_slab, inv, inf, &te) && te == 1.0f);

    /* counts */
    size_t big = (size_t)CC_BVH_MAX_ITEMS + 1; /* refused before any box is read */
    check_true("count: more than CC_BVH_MAX_ITEMS boxes is an error, the tree empty",
                   cc_bvh_build(&t, mixed, big, &ar) == -1 && t.nnodes == 0 && t.nitems == 0);
    size_t n = 200000;
    cc_aabb *eq = malloc(n * sizeof *eq), *line = malloc(n * sizeof *line);
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) eq[i].min[k] = 3, eq[i].max[k] = 4, line[i].min[k] = k ? 0 : (float)i, line[i].max[k] = k ? 1 : (float)i + 0.5f;
    double t0 = now_ms();
    cc_bvh te1, te2;
    ok = !cc_bvh_build(&te1, eq, n, &ar) && !cc_bvh_build(&te2, line, n, &ar);
    double ms = now_ms() - t0;
    printf("  200,000 equal boxes: depth %u, %zu nodes; 200,000 in a row: depth %u; both built in %.0f ms\n", te1.depth, te1.nnodes,
           te2.depth, ms);
    check_true("count: 200,000 equal boxes and 200,000 in order, depth 16, nodes < n", ok && te1.depth == 16 && te2.depth == 16 &&
                                                                                           te1.nnodes < n && te2.nnodes < n);
    got.n = 0;
    cc_aabb q = {{1000.2f, 0, 0}, {1999.9f, 1, 1}};
    cc_bvh_overlapping(&te2, &q, on_item, &got);
    ok = got.n == 1000;
    for (size_t i = 0; i < got.n; i++) ok &= got.v[i] >= 1000 && got.v[i] < 2000;
    check_true("count: the row's overlapping query, 1,000 boxes", ok);
    float orow[3] = {-5, 0.5f, 0.5f}, inv1[3] = {1, inf, inf};
    RayCtx c2 = {line, orow, inv1};
    check_true("count: the row's first box is the nearest along x",
               cc_bvh_ray(&te2, orow, d, entry_hit, &c2, &dist, &key) == 1 && key == 0 && dist == 5.0f);
    free(eq), free(line), free(got.v);
    arena_free(&ar);
}

/* A bipyramid: 5 vertices, 6 triangles, the tips at (x, y, +-h) and three around. */
static void bipyramid(double tris[6][3][3], const double p[5][3]) {
    static const int T[6][3] = {{0, 2, 3}, {0, 3, 4}, {0, 4, 2}, {1, 3, 2}, {1, 4, 3}, {1, 2, 4}};
    for (int t = 0; t < 6; t++)
        for (int v = 0; v < 3; v++) memcpy(tris[t][v], p[T[t][v]], 24);
}

/* n bipyramids, own meshes, on a 2 m grid 100 wide, `z` up; `copies`: translated copies of one,
 * else each its own (an LCG moves every point). As scene_bmsh_test.c. */
static int pyramids(cc_scene *s, size_t n, int copies, double z) {
    memset(s, 0, sizeof *s);
    arena_init(&s->arena, 0);
    Arena scratch;
    arena_init(&scratch, 0);
    s->elements = arena_alloc(&s->arena, n * sizeof *s->elements, 8);
    if (!s->elements) return -1;
    uint64_t r = 12345;
    for (size_t k = 0; k < n; k++) {
        double p[5][3] = {{0, 0, 500}, {0, 0, -500}, {400, 0, 0}, {-200, 350, 0}, {-200, -350, 0}};
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 3; j++) {
                r = r * 6364136223846793005ull + 1442695040888963407ull;
                if (!copies) p[i][j] += (double)(r >> 40) / 16777216.0 * 300.0;
            }
        for (int i = 0; i < 5; i++) p[i][0] += 2000.0 * (double)(k % 100), p[i][1] += 2000.0 * (double)(k / 100), p[i][2] += z;
        double tris[6][3][3];
        bipyramid(tris, (const double(*)[3])p);
        cc_scene_element *e = &s->elements[k];
        memset(e, 0, sizeof *e);
        char idt[32];
        snprintf(idt, sizeof idt, "%c%021zu", copies ? 'a' : 'b', k);
        memcpy(e->id.c, idt, 22);
        e->shape = -1;
        e->model_index = k;
        static const double o[3] = {0, 0, 0};
        if (cc_scene_mesh_build(tris, 6, o, &s->arena, &scratch, &e->mesh)) return -1;
        arena_reset(&scratch);
    }
    s->nelements = n;
    arena_free(&scratch);
    return 0;
}

/* Every placed point of element k inside `b`; for an own mesh, `b` is exactly its points' box. */
static int holds(const cc_scene *s, size_t k, const cc_aabb *b, Arena *a) {
    cc_mesh w;
    arena_reset(a);
    if (cc_scene_world_mesh(s, k, a, &w)) return 0;
    if (s->elements[k].shape < 0) {
        cc_aabb x = cc_aabb_of_points((const float(*)[3])w.pos, w.nv);
        return !memcmp(&x, b, sizeof x);
    }
    for (uint32_t i = 0; i < w.nv; i++)
        for (int c = 0; c < 3; c++)
            if (!(w.pos[i][c] >= b->min[c] && w.pos[i][c] <= b->max[c])) return 0;
    return 1;
}

static int mixed_scene(cc_scene *mix, size_t half) {
    cc_scene a, b;
    int ok = pyramids(&a, half, 1, 0.0) == 0 && pyramids(&b, half + 1, 0, 1200.0) == 0;
    memset(mix, 0, sizeof *mix);
    arena_init(&mix->arena, 0);
    mix->elements = arena_alloc(&mix->arena, (2 * half + 1) * sizeof *mix->elements, 8);
    ok &= mix->elements != NULL;
    if (ok) {
        memcpy(mix->elements, a.elements, half * sizeof *mix->elements);
        memcpy(mix->elements + half, b.elements, (half + 1) * sizeof *mix->elements);
        mix->nelements = 2 * half + 1;
        ok &= cc_scene_share_copies(mix) == 0;
        arena_adopt(&mix->arena, &a.arena), arena_adopt(&mix->arena, &b.arena); /* the own meshes */
    }
    cc_scene_free(&a), cc_scene_free(&b);
    return ok ? 0 : -1;
}

static void element_boxes(const char *dir) {
    size_t fn;
    char *frame = slurp(dir, "frame.json", &fn);
    cc_model m;
    cc_error err;
    cc_scene s;
    int ok = !cc_model_load(&m, frame, fn, &err) && !cc_scene_build(&s, &m, &err);
    check_true("element_boxes: golden/frame.json's scene builds", ok);
    if (!ok) return;
    Arena a;
    arena_init(&a, 0);
    cc_aabb *b = malloc((s.nelements + 1) * sizeof *b);
    cc_element_boxes(&s, b);
    int in = 1;
    for (size_t k = 0; k < s.nelements; k++) in &= holds(&s, k, &b[k], &a) && cc_aabb_is_valid(&b[k]);
    check_true("element_boxes: frame.json, each element's box", in && s.nelements == 72);
    free(b);
    cc_scene_free(&s);
    cc_model_free(&m);
    free(frame);

    cc_scene mix;
    ok = !mixed_scene(&mix, 20);
    check_true("element_boxes: 20 copies of one shape and 21 own meshes", ok && mix.nshapes == 1);
    if (ok) {
        b = malloc(mix.nelements * sizeof *b);
        cc_element_boxes(&mix, b);
        int placed = 0;
        in = 1;
        for (size_t k = 0; k < mix.nelements; k++) in &= holds(&mix, k, &b[k], &a), placed += mix.elements[k].shape >= 0;
        check_true("element_boxes: an own mesh's box is its points', a copy's holds its placed points", in && placed == 20);
        free(b);
    }
    cc_scene_free(&mix);
    arena_free(&a);
}

/* The measure tool through the tree: the same snap as every box tested, and as every element. */
static void measure(void) {
    cc_scene sc;
    int ok = !mixed_scene(&sc, 600);
    size_t n = sc.nelements;
    if (ok) {
        /* one own mesh with an infinite point: its box is not finite, left out of the tree */
        cc_scene_element *e = &sc.elements[n - 1];
        ok &= e->shape < 0 && e->mesh.nv > 0;
        if (ok) e->mesh.pos[0][2] = 1.0f / 0.0f;
    }
    cc_mboxes bx;
    memset(&bx, 0, sizeof bx);
    ok = ok && !cc_mboxes_build(&bx, &sc);
    check_true("measure: 1,201 elements, the tree over 1,200 boxes, 1 left out", ok && n == 1201 && bx.tree.nitems == 1200 && bx.nodd == 1 &&
                                                                                   bx.odd[0] == 1200);
    if (!ok) return;
    const double *o = sc.origin;
    size_t work_tree = 0, work_box = 0, nrays = 0;
    int same = 1, hits = 0, kinds[3] = {0, 0, 0};
    for (int nl = 1; nl <= 2; nl++) {
        cc_mlayer L[2] = {{&sc, {0, 0, 0}, NULL, (const float(*)[6])bx.box, &bx}, {&sc, {500, 0, 0}, NULL, (const float(*)[6])bx.box, &bx}};
        cc_mlayer P[2] = {L[0], L[1]};
        P[0].index = P[1].index = NULL;
        for (int i = 0; i < 12 && same; i++)
            for (int j = 0; j < 12 && same; j++) {
                double from[3] = {-3000 - o[0], -3000 - o[1], 7000 - o[2]};
                double to[3] = {1703.0 * i + 13 - o[0], 1101.0 * j + 7 - o[1], 300.0 * ((i + j) % 4) - o[2]};
                double rr[6] = {from[0], from[1], from[2], to[0] - from[0], to[1] - from[1], to[2] - from[2]};
                double rt[6] = {from[0] + 30, from[1] - 30, from[2], rr[3], rr[4], rr[5]};
                cc_mwork wt = {0, 0, 0}, wb = {0, 0, 0};
                cc_mview v = {L, (size_t)nl, NULL, NULL, o, 0.0f, &wt, 0};
                cc_msnap s1, s2, s3;
                memset(&s1, 0, sizeof s1), memset(&s2, 0, sizeof s2), memset(&s3, 0, sizeof s3);
                int r1 = cc_measure_snap(&v, rr, rt, 12.0, 0, &s1);
                v.layers = P, v.work = &wb;
                int r2 = cc_measure_snap(&v, rr, rt, 12.0, 0, &s2);
                v.brute = 1, v.work = NULL;
                int r3 = cc_measure_snap(&v, rr, rt, 12.0, 0, &s3);
                same = r1 == r2 && r2 == r3 && (r1 != 1 || (!memcmp(&s1, &s2, sizeof s1) && !memcmp(&s1, &s3, sizeof s1)));
                if (!same) printf("  %d layers, ray %d %d: tree %d kind %d, boxes %d kind %d, every element %d kind %d\n", nl, i, j, r1, s1.kind, r2, s2.kind, r3, s3.kind);
                hits += r1 == 1, kinds[s1.kind] += r1 == 1;
                work_tree += wt.boxes, work_box += wb.boxes, nrays++;
            }
    }
    printf("  %zu rays: %d hits (%d corners, %d edge points, %d faces); boxes tested: %zu through the tree, %zu each (%.1f%%)\n", nrays,
           hits, kinds[0], kinds[1], kinds[2], work_tree, work_box, 100.0 * (double)work_tree / (double)work_box);
    check_true("measure: 288 rays, one layer and two: the same snap (kind, point, normal) through the tree, each box, every element",
               same && nrays == 288 && hits > 100 && kinds[0] + kinds[1] > 0 && kinds[2] > 0);
    check_true("measure: the tree tests under a quarter of the boxes", work_tree * 4 < work_box);
    cc_mboxes_free(&bx);
    cc_scene_free(&sc);
}

int main(int argc, char **argv) {
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    brute_force();
    edge_cases();
    element_boxes(dir);
    measure();
    return report("bvh");
}
