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
 * Fails on the code before 3D1: scene_bmsh.h did not exist (the lab received every copy placed).
 * SIZE2: BMSH version 4 (the blocks in the mesh codec, what a link is served as): read back
 * (cc_bmsh4_to_bmsh3) it is version 3 byte for byte on a view file's scene (a link cache's: its meshes
 * already went through the codec), and within the codec's 0.05 mm with the normals' w dropped on a
 * model's scene; it is within 5 % of the view file's geometry (ELEM) on both; damaged bytes (every
 * cut, every byte flipped, a count raised past what is there, a block length past the end) are refused
 * without a crash and without writing past the cap. Fails on the code before SIZE2: no version 4. */
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
#include "../src/viewfile.h"
#include "../session/links.h" /* cc_view_scene */
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

/* SIZE2: version 4 read back against version 3 (`exact`: byte for byte; else positions within 0.05 mm,
 * normals' xyz equal and w 0), and its size against the view file's ELEM sections (`elem`, 0: not checked). */
static void compare4(const char *what, const cc_scene *s, int exact, size_t elem) {
    Buf v3, v4, back;
    buf_init(&v3), buf_init(&v4), buf_init(&back);
    cc_error err;
    char t[200];
    int ok = cc_scene_bmsh3(s, NULL, NULL, 7, &v3, &err) == 0 && cc_scene_bmsh4(s, NULL, NULL, 7, &v4, &err) == 0;
    ok = ok && cc_bmsh4_to_bmsh3((const uint8_t *)v4.p, v4.len, (size_t)1 << 30, &back, &err) == 0;
    snprintf(t, sizeof t, "%s: version 4 written and read back (%s)", what, ok ? "" : err.msg);
    check_true(t, ok && back.len == v3.len);
    if (ok && back.len == v3.len) {
        int same = 1;
        if (exact) same = !memcmp(back.p, v3.p, v3.len);
        else {
            size_t ne = u32at(&v3, 12), nV = u32at(&v3, 16), nI = u32at(&v3, 20), nS = u32at(&v3, 24);
            size_t P = 52 + 48 * ne, I = P + 12 * nV, E = I + 4 * nI, N = E + 24 * nS, T = N + 8 * nS;
            same = !memcmp(back.p, v3.p, P) && !memcmp(back.p + I, v3.p + I, E - I) && !memcmp(back.p + T, v3.p + T, v3.len - T);
            for (size_t k = 0; same && k < 3 * nV + 6 * nS; k++) {
                float a, b;
                size_t o = k < 3 * nV ? P + 4 * k : E + 4 * (k - 3 * nV);
                memcpy(&a, back.p + o, 4), memcpy(&b, v3.p + o, 4);
                same = a - b <= 0.05f && b - a <= 0.05f;
            }
            for (size_t k = 0; same && k < 2 * nS; k++)
                same = !memcmp(back.p + N + 4 * k, v3.p + N + 4 * k, 3) && back.p[N + 4 * k + 3] == 0;
        }
        snprintf(t, sizeof t, "%s: version 4 read back is version 3 (%s)", what, exact ? "byte for byte" : "within 0.05 mm, normals' w 0");
        check_true(t, same);
    }
    printf("  %s: version 3 %zu bytes, version 4 %zu bytes, view file geometry %zu bytes\n", what, v3.len, v4.len, elem);
    if (elem) {
        snprintf(t, sizeof t, "%s: version 4 within 5 %% of the view file's geometry (%zu, %zu)", what, v4.len, elem);
        check_true(t, (double)v4.len <= 1.05 * (double)elem);
    }
    if (ok && v4.len > 60) { /* damaged copies: refused, nothing past the cap, no crash */
        size_t refused = 0, tried = 0, step = v4.len / 600 + 1;
        uint8_t *d = malloc(v4.len);
        for (size_t cut = 0; cut < v4.len; cut += step) {
            buf_clear(&back);
            tried++, refused += cc_bmsh4_to_bmsh3((const uint8_t *)v4.p, cut, (size_t)1 << 30, &back, &err) != 0;
        }
        snprintf(t, sizeof t, "%s: every cut copy refused (%zu of %zu)", what, refused, tried);
        check_true(t, refused == tried);
        size_t over = 0;
        for (size_t at = 0; d && at < v4.len; at += step) {
            memcpy(d, v4.p, v4.len);
            d[at] ^= 0xa5;
            buf_clear(&back);
            if (cc_bmsh4_to_bmsh3(d, v4.len, v3.len, &back, &err) == 0 && back.len > v3.len) over++;
        }
        snprintf(t, sizeof t, "%s: a flipped byte never writes past the cap", what);
        check_true(t, over == 0);
        if (d) { /* nV raised to 2^31: over the cap, refused before any allocation */
            memcpy(d, v4.p, v4.len);
            uint32_t big = 0x80000000u;
            memcpy(d + 16, &big, 4);
            buf_clear(&back);
            check_true("version 4: a count over the cap refused", cc_bmsh4_to_bmsh3(d, v4.len, (size_t)1 << 28, &back, &err) != 0 && back.len == 0);
            size_t ne4 = u32at(&v4, 12), np = 0;
            for (size_t k = 0; k < ne4; k++) np += u32at(&v4, 52 + 48 * ne4 + 4 * k) != 0;
            size_t lenat = 52 + 52 * ne4 + 48 * np + 4; /* the first block's length past the end */
            if (lenat + 4 <= v4.len) {
                memcpy(d, v4.p, v4.len);
                uint32_t far = 0xfffffff0u;
                memcpy(d + lenat, &far, 4);
                check_true("version 4: a block length past the end refused", cc_bmsh4_to_bmsh3(d, v4.len, (size_t)1 << 30, &back, &err) != 0);
            }
        }
        free(d);
    }
    buf_free(&v3), buf_free(&v4), buf_free(&back);
}

/* SIZE2: the view file of `s` (from model `m`) read back: its scene (as a link cache's) and its ELEM bytes. */
static void via_viewfile(const char *what, const cc_model *m, const cc_scene *s, int size_check) {
    Buf vf;
    buf_init(&vf);
    cc_viewfile v;
    cc_scene vs;
    cc_error err;
    memset(&vs, 0, sizeof vs);
    int ok = cc_viewfile_write(m, s, 0, &vf) == 0 && cc_viewfile_read((const uint8_t *)vf.p, vf.len, &v, &err) == 0;
    size_t elem = 0;
    for (size_t at = 12; ok && at + 12 <= vf.len;) {
        uint64_t len;
        memcpy(&len, vf.p + at + 4, 8);
        if (!memcmp(vf.p + at, "ELEM", 4)) elem += 12 + (size_t)len;
        at += 12 + (size_t)len;
    }
    ok = ok && cc_view_scene(&vs, &v, &err) == 0;
    check_true("a view file's scene read back", ok);
    if (ok) compare4(what, &vs, 1, size_check ? elem : 0); /* (72 tiny boxes: the rows weigh more than their geometry) */
    cc_scene_free(&vs);
    cc_viewfile_free(&v);
    buf_free(&vf);
}

/* SIZE2: `n` distinct height fields of g x g quads (own meshes, scene coordinates, unique heights), each
 * with its border as crease edges: a mesh-heavy scene, as an imported link's. */
static int fields(cc_scene *s, size_t n, uint32_t g) {
    memset(s, 0, sizeof *s);
    arena_init(&s->arena, 0);
    s->elements = arena_alloc(&s->arena, n * sizeof *s->elements, 8);
    if (!s->elements) return -1;
    for (size_t k = 0; k < n; k++) {
        cc_scene_element *e = &s->elements[k];
        memset(e, 0, sizeof *e);
        snprintf(e->id.c, sizeof e->id.c, "F%020zu", k);
        e->shape = -1, e->category = (cc_category)(k % 8);
        cc_mesh *m = &e->mesh;
        m->nv = (g + 1) * (g + 1), m->ni = 6 * g * g, m->ne = 4 * g, m->nn = 4 * g;
        m->pos = arena_alloc(&s->arena, m->nv * sizeof *m->pos, 4);
        m->idx = arena_alloc(&s->arena, m->ni * sizeof *m->idx, 4);
        m->edges = arena_alloc(&s->arena, m->ne * sizeof *m->edges, 4);
        m->normals = arena_alloc(&s->arena, m->nn * sizeof *m->normals, 1);
        if (!m->pos || !m->idx || !m->edges || !m->normals) return -1;
        for (uint32_t i = 0; i <= g; i++)
            for (uint32_t j = 0; j <= g; j++) {
                float *p = m->pos[i * (g + 1) + j];
                p[0] = 5000.0f * (float)(k % 10) + 100.0f * (float)i, p[1] = 5000.0f * (float)(k / 10) + 100.0f * (float)j;
                p[2] = (float)((i * 7 + j * 13 + k * 5) % 97) * 3.0625f;
            }
        uint32_t t = 0;
        for (uint32_t i = 0; i < g; i++)
            for (uint32_t j = 0; j < g; j++) {
                uint32_t a = i * (g + 1) + j, b = a + 1, c = a + g + 1, d = c + 1;
                m->idx[t++] = a, m->idx[t++] = b, m->idx[t++] = d, m->idx[t++] = a, m->idx[t++] = d, m->idx[t++] = c;
            }
        for (uint32_t q = 0; q < 4 * g; q++) { /* the border, as segments between vertices */
            uint32_t side = q / g, r = q % g, a, b;
            if (side == 0) a = r, b = r + 1;
            else if (side == 1) a = g * (g + 1) + r, b = a + 1;
            else if (side == 2) a = r * (g + 1), b = a + g + 1;
            else a = r * (g + 1) + g, b = a + g + 1;
            memcpy(m->edges[q], m->pos[a], 12), memcpy(m->edges[q] + 3, m->pos[b], 12);
            int8_t nn[8] = {0, 0, 127, 0, (int8_t)(q % 3), 0, 127, 0};
            memcpy(m->normals[q], nn, 8);
        }
    }
    s->nelements = n;
    return 0;
}

/* SIZE2: the view file of a scene without a model (META from its ids) read back, then compare4. */
static void via_viewfile_meta(const char *what, const cc_scene *s) {
    cc_viewfile_meta mt;
    cc_viewfile_meta_init(&mt);
    cc_view_info *info = calloc(s->nelements + 1, sizeof *info);
    for (size_t k = 0; info && k < s->nelements; k++) info[k].global_id.p = s->elements[k].id.c, info[k].global_id.n = 22;
    mt.info = info, mt.ninfo = s->nelements;
    Buf vf;
    buf_init(&vf);
    cc_viewfile v;
    cc_scene vs;
    cc_error err;
    memset(&vs, 0, sizeof vs);
    memset(&v, 0, sizeof v);
    int ok = info && cc_viewfile_write_meta(&mt, s, 0, &vf) == 0 && cc_viewfile_read((const uint8_t *)vf.p, vf.len, &v, &err) == 0;
    size_t elem = 0;
    for (size_t at = 12; ok && at + 12 <= vf.len;) {
        uint64_t len;
        memcpy(&len, vf.p + at + 4, 8);
        if (!memcmp(vf.p + at, "ELEM", 4)) elem += 12 + (size_t)len;
        at += 12 + (size_t)len;
    }
    ok = ok && cc_view_scene(&vs, &v, &err) == 0;
    check_true("a heavy view file's scene read back", ok);
    if (ok) compare4(what, &vs, 1, elem);
    cc_scene_free(&vs);
    if (ok) cc_viewfile_free(&v);
    buf_free(&vf);
    free(info);
    cc_viewfile_meta_free(&mt);
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
    compare4("frame (model)", &s, 0, 0);
    via_viewfile("frame (view file)", &m, &s, 0);
    cc_scene_free(&s);
    cc_model_free(&m);
    free(frame);

    { /* SIZE2: a mesh-heavy scene (40 distinct fields of 1,800 triangles), through a view file as a link cache */
        cc_scene h;
        if (fields(&h, 40, 30) == 0) {
            compare4("heavy (built)", &h, 0, 0);
            via_viewfile_meta("heavy (view file)", &h);
        } else check_true("heavy scene built", 0);
        cc_scene_free(&h);
    }

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
