/* Suite scene_bmsh (ticket 3D1, ADR 0078): BMSH version 3, each shared shape once and a placement
 * per element (src/scene_bmsh.h), what the lab draws instanced.
 *   - golden/frame.json and the 20-element mixed scene below: every element of version 3, placed by
 *     its placement as cc_scene_world_mesh places it (f32, x p.x + y p.y + z p.z + t; a mirrored
 *     placement's triangles turned), gives version 1's vertices, indices, edges and normals bit for
 *     bit, and version 3 is smaller when shapes are shared.
 *   - dedup (scene.c share_copies, a port of dedup.rs) on hostile input: 6,000 elements with the same
 *     vertex, triangle and edge counts (one bucket) and no copy among them: the fits per bucket are
 *     capped (MAX_SHAPES_PER_BUCKET), so it ends in linear time (under 2 s here; all pairs would be
 *     18 million fits) and shares nothing; 6,000 translated copies share one shape.
 * Fails on the code before 3D1: scene_bmsh.h did not exist (the lab received every copy placed). */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/arena.h"
#include "../src/buf.h"
#include "../src/file.h"
#include "../src/model.h"
#include "../src/scene.h"
#include "../src/scene_bmsh.h"
#include "../src/scene_build.h"
#include "check.h"

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static uint32_t u32at(const Buf *b, size_t o) {
    uint32_t v;
    memcpy(&v, b->p + o, 4);
    return v;
}

/* Version 3 placed element by element against version 1 (both written from `s`). */
static void compare(const char *what, const cc_scene *s) {
    Buf v1, v3;
    buf_init(&v1), buf_init(&v3);
    cc_error err;
    Arena a;
    arena_init(&a, 0);
    /* version 1 as session/mesh.c writes it: cc_scene_world_mesh per element */
    int ok = cc_scene_bmsh3(s, NULL, NULL, 9, &v3, &err) == 0;
    size_t ne = s->nelements;
    uint32_t nv1 = 0, ni1 = 0, ns1 = 0;
    cc_mesh *w = arena_alloc(&a, (ne + 1) * sizeof *w, 16);
    for (size_t k = 0; k < ne && ok; k++) {
        ok &= cc_scene_world_mesh(s, k, &a, &w[k]) == 0;
        nv1 += w[k].nv, ni1 += w[k].ni, ns1 += w[k].ne;
    }
    char t[160];
    snprintf(t, sizeof t, "%s: version 3 written", what);
    check_true(t, ok && v3.len >= 52 && !memcmp(v3.p, "BMSH", 4) && u32at(&v3, 4) == 3 && u32at(&v3, 8) == 9 && u32at(&v3, 12) == ne);
    if (!ok || v3.len < 52) goto out;
    uint32_t nV = u32at(&v3, 16), nI = u32at(&v3, 20), nS = u32at(&v3, 24);
    size_t rows = 52, P = rows + 48 * ne, I = P + 12 * (size_t)nV, E = I + 4 * (size_t)nI, N = E + 24 * (size_t)nS, T = N + 8 * (size_t)nS;
    snprintf(t, sizeof t, "%s: version 3 size", what);
    check_true(t, v3.len == T + 52 * ne);
    if (v3.len != T + 52 * ne) goto out;
    int same = 1;
    for (size_t k = 0; k < ne && same; k++) {
        const unsigned char *row = (const unsigned char *)v3.p + rows + 48 * k;
        uint32_t f[6];
        memcpy(f, row + 24, 24);
        same &= !memcmp(row, s->elements[k].id.c, 22) && f[1] == w[k].nv && f[3] == w[k].ni && f[5] == w[k].ne;
        uint32_t shape = u32at(&v3, T + 52 * k);
        float pl[12];
        memcpy(pl, v3.p + T + 52 * k + 4, 48);
        same &= (shape == 0) == (s->elements[k].shape < 0);
        for (uint32_t i = 0; i < f[1] && same; i++) {
            float q[3], r[3];
            memcpy(q, v3.p + P + 12 * ((size_t)f[0] + i), 12);
            for (int j = 0; j < 3; j++) r[j] = shape ? pl[j] * q[0] + pl[3 + j] * q[1] + pl[6 + j] * q[2] + pl[9 + j] : q[j];
            same &= !memcmp(r, w[k].pos[i], 12);
        }
        float c[3] = {pl[4] * pl[8] - pl[5] * pl[7], pl[5] * pl[6] - pl[3] * pl[8], pl[3] * pl[7] - pl[4] * pl[6]};
        int mirrors = shape && pl[0] * c[0] + pl[1] * c[1] + pl[2] * c[2] < 0.0f;
        for (uint32_t i = 0; i < f[3] && same; i++) {
            uint32_t at = i % 3 == 0 || !mirrors ? i : i % 3 == 1 ? i + 1 : i - 1;
            same &= u32at(&v3, I + 4 * ((size_t)f[2] + at)) - f[0] == w[k].idx[i];
        }
        for (uint32_t i = 0; i < f[5] && same; i++) {
            float q[6], r[6];
            memcpy(q, v3.p + E + 24 * ((size_t)f[4] + i), 24);
            for (int h = 0; h < 2; h++)
                for (int j = 0; j < 3; j++)
                    r[3 * h + j] = shape ? pl[j] * q[3 * h] + pl[3 + j] * q[3 * h + 1] + pl[6 + j] * q[3 * h + 2] + pl[9 + j] : q[3 * h + j];
            same &= !memcmp(r, w[k].edges[i], 24);
            if (!shape) same &= !memcmp(v3.p + N + 8 * ((size_t)f[4] + i), w[k].normals[i], 8);
        }
        if (!same) printf("  element %zu differs\n", k);
    }
    snprintf(t, sizeof t, "%s: every element placed by version 3 is version 1's, bit for bit", what);
    check_true(t, same);
    size_t b1 = 52 + 48 * ne + 12 * (size_t)nv1 + 4 * (size_t)ni1 + 32 * (size_t)ns1;
    printf("  %s: %zu elements, %zu shapes; version 1 %zu bytes, version 3 %zu bytes\n", what, ne, s->nshapes, b1, v3.len);
    if (s->nshapes) {
        snprintf(t, sizeof t, "%s: version 3 is smaller", what);
        check_true(t, v3.len < b1);
    }
out:
    buf_free(&v1), buf_free(&v3);
    arena_free(&a);
}

/* A bipyramid: 5 vertices, 6 triangles, 9 edges, the tips at (x, y, +-h) and three around. */
static void bipyramid(double tris[6][3][3], const double p[5][3]) {
    static const int T[6][3] = {{0, 2, 3}, {0, 3, 4}, {0, 4, 2}, {1, 3, 2}, {1, 4, 3}, {1, 2, 4}};
    for (int t = 0; t < 6; t++)
        for (int v = 0; v < 3; v++) memcpy(tris[t][v], p[T[t][v]], 24);
}

/* A scene of n bipyramids, own meshes; `copies`: translated copies of one, else each its own shape
 * (an LCG moves every point). */
static int pyramids(cc_scene *s, size_t n, int copies) {
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
        for (int i = 0; i < 5; i++) p[i][0] += 2000.0 * (double)(k % 100), p[i][1] += 2000.0 * (double)(k / 100);
        double tris[6][3][3];
        bipyramid(tris, (const double(*)[3])p);
        cc_scene_element *e = &s->elements[k];
        memset(e, 0, sizeof *e);
        char idt[32];
        snprintf(idt, sizeof idt, "%022zu", k);
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

int main(int argc, char **argv) {
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    cc_error err;
    size_t fn;
    char *frame = slurp(dir, "frame.json", &fn);
    cc_model m;
    if (cc_model_load(&m, frame, fn, &err)) {
        printf("frame.json does not load: %s\n", err.msg);
        return 1;
    }
    cc_scene s;
    check_true("frame scene builds", cc_scene_build(&s, &m, &err) == 0);
    compare("frame", &s);
    cc_scene_free(&s);
    cc_model_free(&m);
    free(frame);

    /* own meshes and shapes mixed: 20 bipyramids, 10 copies of one and 10 of their own */
    {
        cc_scene a, b;
        int ok = pyramids(&a, 10, 1) == 0 && pyramids(&b, 10, 0) == 0;
        cc_scene mix;
        memset(&mix, 0, sizeof mix);
        arena_init(&mix.arena, 0);
        mix.elements = arena_alloc(&mix.arena, 20 * sizeof *mix.elements, 8);
        ok &= mix.elements != NULL;
        if (ok) {
            memcpy(mix.elements, a.elements, 10 * sizeof *mix.elements);
            memcpy(mix.elements + 10, b.elements, 10 * sizeof *mix.elements);
            for (size_t k = 10; k < 20; k++) mix.elements[k].id.c[0] = 'b';
            mix.nelements = 20;
            ok &= cc_scene_share_copies(&mix) == 0;
        }
        check_true("mixed scene: copies shared", ok && mix.nshapes == 1);
        if (ok) compare("mixed", &mix);
        cc_scene_free(&mix), cc_scene_free(&a), cc_scene_free(&b);
    }

    /* dedup's work is bounded on one bucket of distinct meshes */
    {
        cc_scene h;
        int ok = pyramids(&h, 6000, 0) == 0;
        double t0 = now_ms();
        ok &= cc_scene_share_copies(&h) == 0;
        double ms = now_ms() - t0;
        printf("  6,000 distinct bipyramids (one bucket): share_copies %.1f ms, %zu shapes\n", ms, h.nshapes);
        check_true("hostile bucket: no shape, linear time (under 2 s)", ok && h.nshapes == 0 && ms < 2000.0);
        cc_scene_free(&h);
        cc_scene c;
        ok = pyramids(&c, 6000, 1) == 0 && cc_scene_share_copies(&c) == 0;
        check_true("6,000 translated copies share one shape", ok && c.nshapes == 1);
        cc_scene_free(&c);
    }
    return report("scene_bmsh");
}
