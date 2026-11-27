/* Suite tiles (ticket TILE1, ADR 0078 amendment 6): the tile pyramid (src/tiles.h) on a synthetic "buildings"
 * scene made here (a grid of buildings: storey slabs and facade walls, the large elements, around many small
 * inner parts: furniture as shared shapes placed turned and mirrored, thin pipes as own meshes, glass panels),
 * built from the scene and from its view file read in place (a link's cache):
 *   - the section reads back (cc_tiles_check): every element with geometry in exactly one leaf, children inside
 *     their parent, ranges nested, the root's elements all;
 *   - every leaf's answer (TLF1, the cache's own BMG1 blocks) decodes to its elements' world meshes (cc_scene_world_mesh)
 *     within the codec's 0.05 mm;
 *   - every proxy point lies within its tile's error of the detail under it; points sampled on the detail of the
 *     surface-like elements (two sides at least 4 errors) lie within the error of the proxy (measured, printed);
 *   - each level's proxies at most a quarter of the level below's triangles; the root at most 512;
 *   - large elements high: a slab is never in a leaf deeper than a furniture part's;
 *   - deterministic: two builds, and a build in steps of 1,000 triangles, give the same bytes;
 *   - the walker: far, close and inside views: the far one a few hundred triangles, within the budget, front to
 *     back; a marked tile is descended;
 *   - the 21-bit lattice: a vertex shared by two neighbouring leaves lands on one lattice point; within 1/32 mm;
 *   - hostile sections: 4,000 random damages and a cycle, a child outside its parent, a range past the list, an
 *     element twice, absurd counts: refused or read, never a crash (ASan/UBSan in the gate);
 *   - a huge element (400,000 triangles) and non-finite vertices: bounded, ends;
 *   - cancel: a build stopped half way frees everything.
 * TILES_SYNTH_OUT=<file.bimbleview> (TILES_SYNTH="bx,by,storeys,parts"): a buildings scene of that size written there
 * (ccore/tools/tiles.mjs uses it).
 * Fails on the code before TILE1: src/tiles.h did not exist. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/arena.h"
#include "../src/buf.h"
#include "../src/math.h"
#include "../src/meshcodec.h"
#include "../src/scene.h"
#include "../src/tiles.h"
#include "../src/viewfile.h"
#include "check.h"

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- geometry helpers ---- */
typedef struct {
    float (*p)[3];
    uint32_t *i;
    uint32_t nv, ni, cv, ci;
} M;
static uint32_t mv(M *m, double x, double y, double z) {
    if (m->nv == m->cv) m->cv = m->cv ? 2 * m->cv : 64, m->p = realloc(m->p, m->cv * sizeof *m->p);
    m->p[m->nv][0] = (float)x, m->p[m->nv][1] = (float)y, m->p[m->nv][2] = (float)z;
    return m->nv++;
}
static void mt(M *m, uint32_t a, uint32_t b, uint32_t c) {
    if (m->ni + 3 > m->ci) m->ci = m->ci ? 2 * m->ci : 192, m->i = realloc(m->i, m->ci * sizeof *m->i);
    m->i[m->ni++] = a, m->i[m->ni++] = b, m->i[m->ni++] = c;
}
/* a box [lo, hi], each face an n x n grid of quads, faces outward */
static void box(M *m, const double lo[3], const double hi[3], int n) {
    for (int ax = 0; ax < 3; ax++)
        for (int side = 0; side < 2; side++) {
            int u = (ax + 1) % 3, v = (ax + 2) % 3;
            uint32_t base = m->nv;
            for (int a = 0; a <= n; a++)
                for (int b = 0; b <= n; b++) {
                    double q[3];
                    q[ax] = side ? hi[ax] : lo[ax];
                    q[u] = lo[u] + (hi[u] - lo[u]) * a / n;
                    q[v] = lo[v] + (hi[v] - lo[v]) * b / n;
                    mv(m, q[0], q[1], q[2]);
                }
            for (int a = 0; a < n; a++)
                for (int b = 0; b < n; b++) {
                    uint32_t p00 = base + a * (n + 1) + b, p10 = p00 + n + 1, p01 = p00 + 1, p11 = p10 + 1;
                    if (side) mt(m, p00, p10, p11), mt(m, p00, p11, p01);
                    else mt(m, p00, p11, p10), mt(m, p00, p01, p11);
                }
        }
}
/* a cylinder along x from x0 to x1, radius r, k sides */
static void pipe(M *m, double x0, double x1, double y, double z, double r, int k) {
    uint32_t b = m->nv;
    for (int s = 0; s < k; s++) {
        double a = 6.283185307179586 * s / k, c = 1.0 - a * a / 2 + a * a * a * a / 24, sn = a - a * a * a / 6 + a * a * a * a * a / 120;
        if (a > 1.5) c = -1 + (a - 3.14159) * (a - 3.14159) / 2, sn = -(a - 3.14159) + (a - 3.14159) * (a - 3.14159) * (a - 3.14159) / 6; /* rough */
        mv(m, x0, y + r * c, z + r * sn), mv(m, x1, y + r * c, z + r * sn);
    }
    for (int s = 0; s < k; s++) {
        uint32_t a0 = b + 2 * s, a1 = a0 + 1, c0 = b + 2 * ((s + 1) % k), c1 = c0 + 1;
        mt(m, a0, c0, c1), mt(m, a0, c1, a1);
    }
}

typedef struct {
    cc_scene s;
    cc_view_info *info;
    char *ids;
} Synth;

static cc_mesh to_mesh(Arena *a, M *m) {
    cc_mesh o;
    memset(&o, 0, sizeof o);
    o.pos = arena_alloc_n(a, m->nv + 1, sizeof o.pos[0], 4);
    o.idx = arena_alloc_n(a, m->ni + 1, sizeof *o.idx, 4);
    memcpy(o.pos, m->p, m->nv * sizeof o.pos[0]), memcpy(o.idx, m->i, m->ni * sizeof *o.idx);
    o.nv = m->nv, o.ni = m->ni;
    free(m->p), free(m->i);
    memset(m, 0, sizeof *m);
    return o;
}

static size_t ecap;
static void add_el(Synth *y, const char *cls, cc_category cat, int shape, cc_mesh mesh, const cc_placement32 *pl) {
    cc_scene *s = &y->s;
    if (s->nelements == ecap) {
        ecap = ecap ? 2 * ecap : 256;
        cc_scene_element *e = arena_alloc_n(&s->arena, ecap, sizeof *e, 8);
        if (s->nelements) memcpy(e, s->elements, s->nelements * sizeof *e);
        s->elements = e;
    }
    cc_scene_element *e = &s->elements[s->nelements];
    memset(e, 0, sizeof *e);
    char idb[32];
    snprintf(idb, sizeof idb, "T%021zu", s->nelements);
    memcpy(e->id.c, idb, sizeof e->id.c);
    e->class_name.p = cls, e->class_name.n = strlen(cls);
    e->category = cat, e->model_index = s->nelements, e->shape = shape;
    if (shape < 0) e->mesh = mesh;
    else e->to_scene = *pl;
    s->nelements++;
}

/* bx x by buildings of `st` storeys; `parts` furniture per storey */
static void synth(Synth *y, int bx, int by, int st, int parts) {
    memset(y, 0, sizeof *y);
    ecap = 0;
    arena_init(&y->s.arena, 0);
    cc_scene *s = &y->s;
    /* shapes: a chair-like part (subdivided box, 2 sizes) */
    s->nshapes = 2;
    s->shapes = arena_alloc_n(&s->arena, 2, sizeof *s->shapes, 8);
    M m = {0};
    box(&m, (double[3]){0, 0, 0}, (double[3]){500, 450, 900}, 3);
    s->shapes[0] = to_mesh(&s->arena, &m);
    box(&m, (double[3]){-800, -400, 0}, (double[3]){800, 400, 740}, 2);
    s->shapes[1] = to_mesh(&s->arena, &m);
    unsigned seed = 12345;
    for (int i = 0; i < bx; i++)
        for (int j = 0; j < by; j++) {
            double ox = i * 40000.0, oy = j * 32000.0;
            for (int k = 0; k < st; k++) {
                double z = k * 3500.0;
                box(&m, (double[3]){ox, oy, z}, (double[3]){ox + 20000, oy + 15000, z + 250}, 4);
                add_el(y, "IfcSlab", CC_CAT_CONCRETE, -1, to_mesh(&s->arena, &m), NULL);
                double w[4][6] = {{ox, oy, z + 250, ox + 20000, oy + 300, z + 3500},
                                  {ox, oy + 14700, z + 250, ox + 20000, oy + 15000, z + 3500},
                                  {ox, oy + 300, z + 250, ox + 300, oy + 14700, z + 3500},
                                  {ox + 19700, oy + 300, z + 250, ox + 20000, oy + 14700, z + 3500}};
                for (int q = 0; q < 4; q++) {
                    box(&m, w[q], w[q] + 3, 6);
                    add_el(y, "IfcWall", CC_CAT_MASONRY, -1, to_mesh(&s->arena, &m), NULL);
                }
                box(&m, (double[3]){ox + 5000, oy - 50, z + 1000}, (double[3]){ox + 7000, oy - 20, z + 2500}, 1);
                add_el(y, "IfcPlate", CC_CAT_GLASS, -1, to_mesh(&s->arena, &m), NULL);
                for (int q = 0; q < parts; q++) {
                    seed = seed * 1103515245u + 12345u;
                    double px = ox + 1000 + (seed >> 8) % 17000, py = oy + 1000 + (seed >> 4) % 12000;
                    int sh = q % 3 == 2;
                    cc_placement32 pl;
                    memset(&pl, 0, sizeof pl);
                    int turn = q % 4;
                    float c = turn == 1 ? 0.0f : turn == 2 ? -1.0f : 1.0f, sn = turn == 1 ? 1.0f : 0.0f;
                    pl.x[0] = c, pl.x[1] = sn, pl.y[0] = -sn, pl.y[1] = c, pl.z[2] = 1;
                    if (q % 7 == 3) pl.x[0] = -pl.x[0], pl.x[1] = -pl.x[1]; /* mirrored */
                    pl.t[0] = (float)px, pl.t[1] = (float)py, pl.t[2] = (float)(z + 250);
                    add_el(y, "IfcFurnishingElement", CC_CAT_TIMBER, sh, (cc_mesh){0}, &pl);
                }
                pipe(&m, ox + 500, ox + 19500, oy + 7000, z + 3200, 40, 12);
                add_el(y, "IfcPipeSegment", CC_CAT_STEEL, -1, to_mesh(&s->arena, &m), NULL);
            }
        }
    y->info = calloc(s->nelements + 1, sizeof *y->info);
    for (size_t k = 0; k < s->nelements; k++) {
        y->info[k].global_id.p = s->elements[k].id.c, y->info[k].global_id.n = 22;
        y->info[k].name.p = "", y->info[k].storey.p = "", y->info[k].material.p = "";
    }
}

static void synth_free(Synth *y) {
    cc_scene_free(&y->s);
    free(y->info);
}

static int write_view(Synth *y, Buf *out) {
    cc_viewfile_meta mt;
    cc_viewfile_meta_init(&mt);
    mt.info = y->info, mt.ninfo = y->s.nelements;
    int rc = cc_viewfile_write_meta(&mt, &y->s, 0, out);
    cc_viewfile_meta_free(&mt);
    return rc;
}

/* ---- distances (hlod_test.c) ---- */
static void sub(const double *a, const double *b, double *o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static double dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
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

/* ---- the checks ---- */

static double max_leaf_dev, max_err_in, max_err_out_ratio, out_frac;

/* every leaf decodes to its elements' world meshes; the proxies honour their error */
static void check_tiles(const char *what, const cc_tiles_src *src, const cc_scene *ref, const Buf *sec) {
    char w[160];
    cc_tiles_toc toc;
    cc_error e;
    int ok = cc_tiles_check((const uint8_t *)sec->p, sec->len, 1, (uint32_t)ref->nelements, &toc, &e) == 0;
    snprintf(w, sizeof w, "%s: the section reads back (%s)", what, ok ? "ok" : e.msg);
    check_true(w, ok);
    if (!ok) return;
    /* each element with geometry in exactly one leaf */
    uint32_t *hits = calloc(ref->nelements + 1, sizeof *hits);
    size_t want = 0;
    for (size_t k = 0; k < ref->nelements; k++) want += cc_scene_local_mesh(ref, k)->ni > 0;
    int leaf_ok = 1;
    max_leaf_dev = 0;
    Arena a;
    arena_init(&a, 0);
    uint32_t deep_slab = 0, shallow_part = 99;
    for (uint32_t i = 0; i < toc.ntiles; i++) {
        cc_tile t;
        cc_tiles_get(&toc, i, &t);
        if (!(t.flags & CC_TILE_LEAF)) continue;
        for (uint32_t q = 0; q < t.nel; q++) {
            uint32_t el = cc_tiles_list(&toc, t.el0 + q);
            hits[el]++;
            cc_str c = ref->elements[el].class_name;
            if (c.n == 7 && !memcmp(c.p, "IfcSlab", 7) && t.level > deep_slab) deep_slab = t.level;
            if (c.n == 20 && !memcmp(c.p, "IfcFurnishingElement", 20) && t.level < shallow_part) shallow_part = t.level;
        }
        Buf ans;
        buf_init(&ans);
        arena_reset(&a);
        cc_mesh lm;
        uint32_t *rows;
        if (cc_tiles_leaf_answer(src, &toc, i, &ans, &e) || cc_tiles_leaf_mesh((const uint8_t *)ans.p, ans.len, &a, &lm, &rows)) {
            leaf_ok = 0;
            buf_free(&ans);
            continue;
        }
        /* compare with the world meshes in list order */
        uint32_t vat = 0, iat = 0;
        uint64_t tris = 0;
        for (uint32_t q = 0; q < t.nel && leaf_ok; q++) {
            uint32_t el = cc_tiles_list(&toc, t.el0 + q);
            cc_mesh wm;
            if (cc_scene_world_mesh(ref, el, &a, &wm)) { leaf_ok = 0; break; }
            tris += wm.ni / 3;
            /* a mirrored copy's world mesh turns its triangles (the answer keeps them: the reader turns the front face) */
            for (uint32_t k = 0; k + 2 < wm.ni && iat + k + 2 < lm.ni; k += 3) {
                float A[3][3], B[3][3];
                for (int z = 0; z < 3; z++) memcpy(A[z], lm.pos[lm.idx[iat + k + z]], 12), memcpy(B[z], wm.pos[wm.idx[k + z]], 12);
                for (int z = 0; z < 3; z++) { /* the corner of B nearest each corner of A */
                    double best = 1e300;
                    for (int u = 0; u < 3; u++) {
                        double d = 0;
                        for (int j = 0; j < 3; j++) {
                            double q = (double)A[z][j] - B[u][j];
                            d = q < 0 ? (d > -q ? d : -q) : (d > q ? d : q);
                        }
                        if (d < best) best = d;
                    }
                    if (best > max_leaf_dev) max_leaf_dev = best;
                }
            }
            for (uint32_t k = 0; k < wm.ni / 3; k++)
                if (rows[iat / 3 + k] != el) leaf_ok = 0;
            iat += wm.ni, vat += wm.nv;
        }
        if (iat != lm.ni || tris != t.tris) leaf_ok = 0;
        buf_free(&ans);
    }
    size_t once = 0, twice = 0;
    for (size_t k = 0; k < ref->nelements; k++) once += hits[k] == 1, twice += hits[k] > 1;
    snprintf(w, sizeof w, "%s: every element with geometry in exactly one leaf (%zu of %zu, %zu twice)", what, once, want, twice);
    check_true(w, once == want && !twice);
    snprintf(w, sizeof w, "%s: every leaf's answer is its elements' world meshes (max %.4f mm)", what, max_leaf_dev);
    check_true(w, leaf_ok && max_leaf_dev <= CC_MESHCODEC_TOLERANCE_MM + 1e-3);
    snprintf(w, sizeof w, "%s: large elements high (slabs at level <= %u, furniture from level %u)", what, deep_slab, shallow_part);
    check_true(w, deep_slab <= shallow_part);
    /* proxies: points within their error */
    max_err_in = 0, max_err_out_ratio = 0;
    uint64_t samples = 0, outside = 0;
    int prox_ok = 1;
    const uint8_t *blobs = toc.p + toc.blob_at;
    for (uint32_t i = 0; i < toc.ntiles; i++) {
        cc_tile t;
        cc_tiles_get(&toc, i, &t);
        if (!(t.flags & CC_TILE_PROXY)) continue;
        arena_reset(&a);
        cc_mesh pm;
        uint8_t *cat;
        if (cc_tiles_proxy_mesh(blobs + t.off, t.len, t.box, &a, &pm, &cat)) { prox_ok = 0; continue; }
        /* the detail: every triangle under it (sampled elements to keep the test short) */
        double err = t.error, tol = err + 1e-3 * (1 + t.box[3] - t.box[0]) / 65535.0 * 4;
        uint32_t step = t.nel > 400 ? t.nel / 400 : 1;
        for (uint32_t pv = 0; pv < pm.nv; pv += 1 + pm.nv / 64) { /* proxy point -> nearest detail vertex */
            double best = 1e300, p[3] = {pm.pos[pv][0], pm.pos[pv][1], pm.pos[pv][2]};
            for (uint32_t q = 0; q < t.nel; q++) {
                cc_mesh wm;
                Arena b;
                arena_init(&b, 0);
                cc_scene_world_mesh(ref, cc_tiles_list(&toc, t.el0 + q), &b, &wm);
                for (uint32_t v = 0; v < wm.nv; v++) {
                    double d[3] = {p[0] - wm.pos[v][0], p[1] - wm.pos[v][1], p[2] - wm.pos[v][2]}, dd = dot(d, d);
                    if (dd < best) best = dd;
                }
                arena_free(&b);
            }
            best = cc_sqrt(best);
            if (best > max_err_in * 1.0 && err > 0 && best / err > max_err_in) max_err_in = best / err;
            if (best > tol) prox_ok = 0;
        }
        /* detail of surface-like elements -> the proxy */
        for (uint32_t q = 0; q < t.nel; q += step) {
            uint32_t el = cc_tiles_list(&toc, t.el0 + q);
            cc_mesh wm;
            Arena b;
            arena_init(&b, 0);
            cc_scene_world_mesh(ref, el, &b, &wm);
            double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
            for (uint32_t v = 0; v < wm.nv; v++)
                for (int j = 0; j < 3; j++) {
                    if (wm.pos[v][j] < lo[j]) lo[j] = wm.pos[v][j];
                    if (wm.pos[v][j] > hi[j]) hi[j] = wm.pos[v][j];
                }
            double ex[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
            int big = (ex[0] >= 4 * err) + (ex[1] >= 4 * err) + (ex[2] >= 4 * err);
            if (big >= 2)
                for (uint32_t tr = 0; tr < wm.ni / 3; tr += 1 + wm.ni / 3 / 8) {
                    double c[3];
                    for (int j = 0; j < 3; j++) c[j] = (wm.pos[wm.idx[3 * tr]][j] + wm.pos[wm.idx[3 * tr + 1]][j] + wm.pos[wm.idx[3 * tr + 2]][j]) / 3.0;
                    double best = 1e300;
                    for (uint32_t pt = 0; pt < pm.ni / 3; pt++) {
                        double T[3][3];
                        for (int z = 0; z < 3; z++)
                            for (int j = 0; j < 3; j++) T[z][j] = pm.pos[pm.idx[3 * pt + z]][j];
                        double d = to_triangle(c, (const double(*)[3])T);
                        if (d < best) best = d;
                    }
                    samples++;
                    if (err > 0 && best / err > max_err_out_ratio) max_err_out_ratio = best / err;
                    if (best > err) outside++;
                }
            arena_free(&b);
        }
    }
    out_frac = samples ? (double)outside / (double)samples : 0;
    printf("  %s: proxy points to the detail: worst %.3f x the error; detail to the proxy: %llu samples, %.2f %% outside, worst %.3f x\n", what,
           max_err_in, (unsigned long long)samples, 100 * out_frac, max_err_out_ratio);
    snprintf(w, sizeof w, "%s: every proxy point within its error of the detail (worst %.2f of the error)", what, max_err_in);
    check_true(w, prox_ok);
    snprintf(w, sizeof w, "%s: surface-like detail within the error of the proxy: %llu samples, %.2f %% outside, worst %.2f x the error", what,
             (unsigned long long)samples, 100 * out_frac, max_err_out_ratio);
    check_true(w, samples > 0 && out_frac <= 0.01 && max_err_out_ratio <= 2.0);
    /* levels shrink */
    uint64_t lv[CC_TILES_MAX_LEVELS] = {0}, below[CC_TILES_MAX_LEVELS] = {0};
    int shrink = 1;
    for (uint32_t i = 0; i < toc.ntiles; i++) {
        cc_tile t;
        cc_tiles_get(&toc, i, &t);
        lv[t.level] += t.tris;
        if (t.flags & CC_TILE_PROXY) {
            uint64_t ch = 0;
            for (uint32_t c = i + 1, q = 0; q < t.nchildren; q++) {
                cc_tile u;
                cc_tiles_get(&toc, c, &u);
                ch += u.tris;
                c += u.skip;
            }
            if (4 * (uint64_t)t.tris > ch && t.tris > 1) shrink = 0;
            below[t.level] += ch;
        }
    }
    if (getenv("TILES_DUMP"))
        for (uint32_t i = 0; i < toc.ntiles; i++) {
            cc_tile t;
            cc_tiles_get(&toc, i, &t);
            printf("    tile %u level %u flags %u parent %d kids %u err %.0f tris %u box %.0f..%.0f %.0f..%.0f %.0f..%.0f nel %u\n", i, t.level, t.flags,
                   (int)t.parent, t.nchildren, t.error, t.tris, t.box[0], t.box[3], t.box[1], t.box[4], t.box[2], t.box[5], t.nel);
        }
    printf("  %s: %u tiles, %u levels; triangles per level:", what, toc.ntiles, toc.levels);
    for (uint32_t l = 0; l < toc.levels; l++) printf(" %llu", (unsigned long long)lv[l]);
    printf("\n");
    cc_tile r;
    cc_tiles_get(&toc, 0, &r);
    snprintf(w, sizeof w, "%s: each proxy at most a quarter of its children; the root %u triangles", what, r.tris);
    check_true(w, shrink && r.tris <= CC_TILES_ROOT_TRIS);
    snprintf(w, sizeof w, "%s: proxies keep to their error (%s)", what, prox_ok ? "ok" : "no");
    free(hits);
    arena_free(&a);
}

static void frustum(cc_tiles_view *v, const double eye[3], const double at[3], double fov_deg, double aspect, double h_px) {
    memset(v, 0, sizeof *v);
    memcpy(v->eye, eye, sizeof v->eye);
    double f[3], up[3] = {0, 0, 1}, r[3], u[3];
    sub(at, eye, f);
    double l = cc_sqrt(dot(f, f));
    for (int k = 0; k < 3; k++) f[k] /= l;
    r[0] = f[1] * up[2] - f[2] * up[1], r[1] = f[2] * up[0] - f[0] * up[2], r[2] = f[0] * up[1] - f[1] * up[0];
    l = cc_sqrt(dot(r, r));
    for (int k = 0; k < 3; k++) r[k] /= l;
    u[0] = r[1] * f[2] - r[2] * f[1], u[1] = r[2] * f[0] - r[0] * f[2], u[2] = r[0] * f[1] - r[1] * f[0];
    double th = fov_deg * 3.141592653589793 / 360.0, ty = th + th * th * th / 3 + 2 * th * th * th * th * th / 15 + 17 * th * th * th * th * th * th * th / 315, tx = ty * aspect; /* tan */
    v->k = h_px / (2 * ty);
    double n[4][3];
    for (int k = 0; k < 3; k++) {
        n[0][k] = f[k] * tx + r[k], n[1][k] = f[k] * tx - r[k]; /* left / right planes' normals */
        n[2][k] = f[k] * ty + u[k], n[3][k] = f[k] * ty - u[k];
    }
    for (int p = 0; p < 4; p++) {
        double d = -dot(n[p], eye);
        v->planes[p][0] = n[p][0], v->planes[p][1] = n[p][1], v->planes[p][2] = n[p][2], v->planes[p][3] = d;
    }
    v->planes[4][0] = f[0], v->planes[4][1] = f[1], v->planes[4][2] = f[2], v->planes[4][3] = -dot(f, eye) - 100; /* near 100 mm */
    v->nplanes = 5;
}

static void walker(const char *what, const Buf *sec) {
    cc_tiles_toc toc;
    cc_error e;
    if (cc_tiles_check((const uint8_t *)sec->p, sec->len, 1, CC_TILES_NONE, &toc, &e)) return;
    cc_tile r;
    cc_tiles_get(&toc, 0, &r);
    double c[3] = {(r.box[0] + r.box[3]) / 2, (r.box[1] + r.box[4]) / 2, (r.box[2] + r.box[5]) / 2};
    double size = r.box[3] - r.box[0] > r.box[4] - r.box[1] ? r.box[3] - r.box[0] : r.box[4] - r.box[1];
    uint32_t *out = malloc(toc.ntiles * sizeof *out);
    struct {
        const char *name;
        double eye[3];
    } views[3] = {{"far", {c[0] - 40 * size, c[1] - 40 * size, c[2] + 20 * size}},
                  {"close", {c[0] - 0.8 * size, c[1] - 0.8 * size, c[2] + 0.4 * size}},
                  {"inside", {r.box[0] + 3000, r.box[1] + 3000, r.box[2] + 1700}}};
    uint64_t tris[3] = {0};
    char w[200];
    for (int q = 0; q < 3; q++) {
        cc_tiles_view v;
        double at[3] = {c[0], c[1], q == 2 ? r.box[2] + 1700 : c[2]};
        frustum(&v, views[q].eye, at, 60, 1.6, 1000);
        uint32_t n = cc_tiles_select(&toc, &v, 1.0, 50000000, NULL, out, toc.ntiles, &tris[q]);
        int front = 1;
        double last = -1;
        for (uint32_t i = 0; i < n; i++) {
            cc_tile t;
            cc_tiles_get(&toc, out[i], &t);
            double d2 = 0;
            for (int k = 0; k < 3; k++) {
                double d = views[q].eye[k] < t.box[k] ? t.box[k] - views[q].eye[k] : views[q].eye[k] > t.box[3 + k] ? views[q].eye[k] - t.box[3 + k] : 0;
                d2 += d * d;
            }
            if (d2 < last) front = 0;
            last = d2;
        }
        printf("  %s, %s view at 1 px: %u tiles, %llu triangles (of %u)\n", what, views[q].name, n, (unsigned long long)tris[q], r.detail);
        snprintf(w, sizeof w, "%s: the %s view front to back", what, views[q].name);
        check_true(w, front);
        /* a budget is kept */
        uint64_t bt;
        cc_tiles_select(&toc, &v, 0.0, 20000, NULL, out, toc.ntiles, &bt);
        snprintf(w, sizeof w, "%s: the %s view within a budget of 20,000 (%llu)", what, views[q].name, (unsigned long long)bt);
        check_true(w, bt <= 20000 || bt <= r.tris);
    }
    snprintf(w, sizeof w, "%s: far (60 sizes away) %llu <= close %llu triangles; far within 2,000", what, (unsigned long long)tris[0], (unsigned long long)tris[1]);
    check_true(w, tris[0] <= tris[1] && tris[0] <= 2000);
    /* a marked tile is descended */
    uint8_t *mark = calloc(toc.ntiles, 1);
    cc_tiles_view v;
    frustum(&v, views[0].eye, c, 60, 1.6, 1000);
    uint64_t t0;
    uint32_t n0 = cc_tiles_select(&toc, &v, 1.0, 50000000, NULL, out, toc.ntiles, &t0);
    int was_root = n0 == 1 && out[0] == 0;
    mark[0] = 1;
    uint64_t t1;
    uint32_t n1 = cc_tiles_select(&toc, &v, 1.0, 50000000, mark, out, toc.ntiles, &t1);
    int has_root = 0;
    for (uint32_t i = 0; i < n1; i++) has_root |= out[i] == 0;
    snprintf(w, sizeof w, "%s: a tile marked (hidden or selected under it) is descended (%u -> %u tiles)", what, n0, n1);
    check_true(w, !has_root && (was_root ? n1 >= 1 : 1));
    free(mark), free(out);
}

static void hostile(const Buf *sec, uint32_t ne) {
    cc_tiles_toc toc;
    cc_error e;
    unsigned seed = 99;
    int refused = 0, read = 0;
    uint8_t *c = malloc(sec->len);
    for (int i = 0; i < 4000; i++) {
        memcpy(c, sec->p, sec->len);
        int k = 1 + i % 4;
        for (int q = 0; q < k; q++) {
            seed = seed * 1103515245u + 12345u;
            size_t at = (seed >> 3) % (sec->len < 4096 ? sec->len : (i % 2 ? sec->len : 4096));
            c[at] = (uint8_t)(seed >> 20);
        }
        size_t n = i % 9 == 0 ? (seed >> 5) % sec->len : sec->len;
        if (cc_tiles_check(c, n, 1, ne, &toc, &e)) refused++;
        else {
            read++;
            uint32_t out[64];
            uint64_t t;
            cc_tiles_view v;
            memset(&v, 0, sizeof v);
            v.k = 1000;
            cc_tiles_select(&toc, &v, 1, 100000, NULL, out, 64, &t);
        }
    }
    printf("  hostile: %d damaged sections refused, %d read (a harmless byte)\n", refused, read);
    check_true("hostile: 4,000 damaged sections, no crash", refused > 0);
    if (cc_tiles_check((const uint8_t *)sec->p, sec->len, 1, ne, &toc, &e) || toc.ntiles < 3) return;
    /* targeted damages */
    struct {
        const char *what;
        size_t at;
        uint32_t v;
    } d[] = {
        {"a cycle (a child's parent is itself)", CC_TILES_HEAD + CC_TILES_REC * 1, 1},
        {"the root with a parent", CC_TILES_HEAD, 0},
        {"a subtree size of 0", CC_TILES_HEAD + 4, 0},
        {"a range past the list", CC_TILES_HEAD + 44, 0x7fffffff},
        {"a box not inside its parent", CC_TILES_HEAD + CC_TILES_REC * 1 + 12, 0xc8000000u /* -131072 */},
        {"absurd tile count", 8, 0xfffffff0u},
        {"absurd element list", 16, 0xfffffff0u},
        {"a proxy past the blobs", CC_TILES_HEAD + 56, 0x7fffffff},
        {"an element twice", 0, 0},
    };
    for (size_t i = 0; i < sizeof d / sizeof *d; i++) {
        memcpy(c, sec->p, sec->len);
        size_t at = d[i].at;
        uint32_t v = d[i].v;
        if (!strcmp(d[i].what, "an element twice")) {
            at = (size_t)(toc.list - toc.p) + 4;
            v = cc_tiles_list(&toc, 0);
        }
        memcpy(c + at, &v, 4);
        char w[120];
        snprintf(w, sizeof w, "hostile: %s refused", d[i].what);
        check_true(w, cc_tiles_check(c, sec->len, 1, ne, &toc, &e) != 0);
    }
    free(c);
}

static int build(const cc_tiles_src *src, Buf *out, uint64_t step, double *ms) {
    cc_tiles t;
    cc_error e;
    double t0 = now_ms();
    int rc = cc_tiles_start(&t, src, &(cc_tiles_opts){CC_TILES_LEAF_TRIS, CC_TILES_PROXY_TRIS, CC_TILES_ROOT_TRIS, CC_TILES_RES}, &e);
    while (!rc) {
        int st = cc_tiles_step(&t, step, &e);
        if (st < 0) rc = -1;
        if (st) break;
    }
    if (ms) *ms = now_ms() - t0;
    if (rc) printf("  build failed: %s\n", e.msg);
    buf_init(out);
    if (!rc) buf_append(out, t.out.p, t.out.len);
    cc_tiles_free(&t);
    return rc;
}

/* the reference occlusion: a wall's occluders hide a box behind it, not one in front, beside or half covered */
static void occlusion(void) {
    cc_tiles_occ z;
    double eye[3] = {0, -20000, 1500}, at[3] = {0, 0, 1500}, o[3] = {0, 0, 0};
    int ok = cc_tiles_occ_init(&z, eye, at, 0.57735, 1920, 1080, 320) == 0;
    /* a section of one tile whose occluders are a wall 20 m wide, 6 m tall at y = 0 (two triangles) */
    uint8_t sec[CC_TILES_HEAD + CC_TILES_REC + 8 + 72];
    memset(sec, 0, sizeof sec);
    memcpy(sec, "TIL1", 4);
    uint32_t u[] = {1, 1, 0, 0, 2, CC_TILES_HEAD + CC_TILES_REC};
    memcpy(sec + 4, u, sizeof u);
    uint8_t *r = sec + CC_TILES_HEAD;
    uint32_t none = CC_TILES_NONE, one = 1;
    memcpy(r, &none, 4), memcpy(r + 4, &one, 4);
    r[11] = CC_TILE_LEAF | CC_TILE_OCC;
    float bx[6] = {-10000, -10, -1500, 10000, 10, 4500};
    memcpy(r + 12, bx, 24);
    uint16_t two = 2;
    memcpy(r + 72, &two, 2);
    float tr[18] = {-10000, 0, -1500, 10000, 0, -1500, 10000, 0, 4500, -10000, 0, -1500, 10000, 0, 4500, -10000, 0, 4500};
    memcpy(sec + CC_TILES_HEAD + CC_TILES_REC, tr, sizeof tr);
    cc_tiles_toc toc;
    cc_error e;
    ok = ok && cc_tiles_check(sec, CC_TILES_HEAD + CC_TILES_REC + 72, 0, CC_TILES_NONE, &toc, &e) == 0 && toc.occ;
    if (ok) cc_tiles_occ_draw(&z, &toc, 0, 1, 0, o);
    if (getenv("TILES_DUMP")) {
        int nset = 0;
        for (int i = 0; i < (z.w + 1) * (z.h + 1); i++) nset += z.depth[i] < 1e30f;
        printf("  occlusion: %d of %d cells set, ok %d, nocc %u, centre %g, k %g, w %d h %d\n", nset, z.w * z.h, ok, toc.nocc,
               z.depth[(size_t)(z.h / 2) * (z.w + 1) + z.w / 2], z.k, z.w, z.h);
    }
    float behind[6] = {-2000, 3000, 0, 2000, 6000, 3000}, front[6] = {-2000, -6000, 0, 2000, -3000, 3000};
    float beside[6] = {15000, 3000, 0, 19000, 6000, 3000}, half[6] = {8000, 3000, 0, 12000, 6000, 3000}, above[6] = {-2000, 3000, 4000, 2000, 6000, 9000};
    check_true("occlusion: a box behind the wall is hidden", ok && cc_tiles_occ_hidden(&z, behind, 1, 0, o));
    check_true("occlusion: a box in front of it, beside it, half covered or above it is not",
               ok && !cc_tiles_occ_hidden(&z, front, 1, 0, o) && !cc_tiles_occ_hidden(&z, beside, 1, 0, o) && !cc_tiles_occ_hidden(&z, half, 1, 0, o) &&
                   !cc_tiles_occ_hidden(&z, above, 1, 0, o));
    double o2[3] = {0, 30000, 0}; /* the wall placed 30 m away (a link's placement): the box now in front of it */
    cc_tiles_occ_free(&z);
    cc_tiles_occ_init(&z, eye, at, 0.57735, 1920, 1080, 320);
    if (ok) cc_tiles_occ_draw(&z, &toc, 0, 1, 0, o2);
    check_true("occlusion: through a placement", ok && !cc_tiles_occ_hidden(&z, behind, 1, 0, o));
    cc_tiles_occ_free(&z);
}

static void lattice(void) {
    float a[6] = {0, 0, 0, 30000, 20000, 3500}, b[6] = {30000, 0, 0, 61000, 20000, 3500};
    int ka, kb;
    int64_t oa[3], ob[3];
    cc_tiles_lattice(a, &ka, oa), cc_tiles_lattice(b, &kb, ob);
    float p[3] = {30000.0f, 12345.678f, 1749.97f};
    uint32_t qa[3], qb[3];
    cc_tiles_pack21(p, ka, oa, qa), cc_tiles_pack21(p, kb, ob, qb);
    int same = 1;
    double dev = 0;
    for (int j = 0; j < 3; j++) {
        if ((double)(oa[j] + ((int64_t)qa[j] << ka)) != (double)(ob[j] + ((int64_t)qb[j] << kb))) same = 0;
        double back = (double)(oa[j] + ((int64_t)qa[j] << ka)) / 16.0, d = back - p[j];
        if (d < 0) d = -d;
        if (d > dev) dev = d;
    }
    check_true("lattice: a vertex on the border of two leaves lands on one lattice point", ka == 0 && kb == 0 && same);
    check_true("lattice: within 1/32 mm", dev <= 1.0 / 32 + 1e-4);
    float big[6] = {0, 0, 0, 300000, 1000, 1000};
    int kk;
    int64_t ob2[3];
    cc_tiles_lattice(big, &kk, ob2);
    uint32_t q2[3];
    float far[3] = {300000, 1000, 1000};
    cc_tiles_pack21(far, kk, ob2, q2);
    check_true("lattice: a 300 m tile takes a shift of 2 (1/4 mm) and stays within 21 bits", kk == 2 && q2[0] < (1u << 21));
}

int main(void) {
    Synth y;
    synth(&y, 3, 2, 4, 60);
    cc_tiles_src src = {&y.s, NULL};
    Buf a, b, c;
    double ms;
    check_true("buildings: built", build(&src, &a, UINT64_MAX, &ms) == 0);
    uint64_t tot = 0;
    for (size_t k = 0; k < y.s.nelements; k++) tot += cc_scene_local_mesh(&y.s, k)->ni / 3;
    printf("  buildings: %zu elements, %llu triangles (copies counted), section %zu bytes, built in %.1f ms\n", y.s.nelements,
           (unsigned long long)tot, a.len, ms);
    check_tiles("buildings", &src, &y.s, &a);
    walker("buildings", &a);
    check_true("buildings: deterministic (two builds)", build(&src, &b, UINT64_MAX, NULL) == 0 && a.len == b.len && !memcmp(a.p, b.p, a.len));
    check_true("buildings: a build in steps of 1,000 triangles gives the same bytes",
               build(&src, &c, 1000, NULL) == 0 && a.len == c.len && !memcmp(a.p, c.p, a.len));
    hostile(&a, (uint32_t)y.s.nelements);
    buf_free(&b), buf_free(&c);
    /* the same from its view file read in place (a link's cache) */
    Buf vb;
    buf_init(&vb);
    check_true("buildings: view file written", write_view(&y, &vb) == 0);
    const char *out = getenv("TILES_SYNTH_OUT"), *dims = getenv("TILES_SYNTH"); /* ccore/tools/tiles.mjs: "bx,by,storeys,parts" */
    if (out) {
        Synth z;
        int bx = 3, by = 2, st = 4, pa = 60;
        if (dims) sscanf(dims, "%d,%d,%d,%d", &bx, &by, &st, &pa);
        synth(&z, bx, by, st, pa);
        Buf zb;
        buf_init(&zb);
        FILE *f = write_view(&z, &zb) == 0 ? fopen(out, "wb") : NULL;
        if (f) fwrite(zb.p, 1, zb.len, f), fclose(f);
        printf("  wrote %s: %zu elements, %zu bytes\n", out, z.s.nelements, zb.len);
        buf_free(&zb);
        synth_free(&z);
    }
    cc_viewfile vf;
    cc_error e;
    check_true("buildings: its view file read in place", cc_viewfile_read_mapped((const uint8_t *)vb.p, vb.len, &vf, &e) == 0 && vf.lazy);
    cc_tiles_src vsrc = {NULL, &vf};
    Buf v1, v2;
    double vms;
    check_true("buildings (view file): built", build(&vsrc, &v1, UINT64_MAX, &vms) == 0);
    printf("  buildings (view file, in place): section %zu bytes, built in %.1f ms\n", v1.len, vms);
    check_tiles("buildings (view file)", &vsrc, &y.s, &v1);
    check_true("buildings (view file): deterministic in steps", build(&vsrc, &v2, 777, NULL) == 0 && v1.len == v2.len && !memcmp(v1.p, v2.p, v1.len));
    buf_free(&v1), buf_free(&v2);
    cc_viewfile_free(&vf);
    buf_free(&vb);
    /* cancel half way: everything freed (ASan) */
    {
        cc_tiles t;
        cc_tiles_start(&t, &src, &(cc_tiles_opts){CC_TILES_LEAF_TRIS, CC_TILES_PROXY_TRIS, CC_TILES_ROOT_TRIS, CC_TILES_RES}, &e);
        int st = cc_tiles_step(&t, 5000, &e);
        check_true("cancel: a build stopped after a step frees everything", st == 0);
        cc_tiles_free(&t);
    }
    buf_free(&a);
    synth_free(&y);
    /* a huge element and non-finite vertices */
    {
        Synth h;
        memset(&h, 0, sizeof h);
        arena_init(&h.s.arena, 0);
        ecap = 0;
        M m = {0};
        box(&m, (double[3]){0, 0, 0}, (double[3]){100000, 80000, 5000}, 182); /* 6 x 2 x 182^2 = 397,488 triangles */
        add_el(&h, "IfcBuildingElementProxy", CC_CAT_OTHER, -1, to_mesh(&h.s.arena, &m), NULL);
        box(&m, (double[3]){0, 0, 0}, (double[3]){1000, 1000, 1000}, 1);
        cc_mesh bad = to_mesh(&h.s.arena, &m);
        bad.pos[0][0] = 1.0f / 0.0f, bad.pos[1][1] = 0.0f / 0.0f;
        add_el(&h, "IfcWall", CC_CAT_CONCRETE, -1, bad, NULL);
        cc_tiles_src hs = {&h.s, NULL};
        Buf hb;
        double hms;
        int ok = build(&hs, &hb, 50000, &hms) == 0;
        cc_tiles_toc toc;
        ok = ok && cc_tiles_check((const uint8_t *)hb.p, hb.len, 1, 2, &toc, &e) == 0;
        uint32_t maxp = 0;
        for (uint32_t i = 0; ok && i < toc.ntiles; i++) {
            cc_tile t;
            cc_tiles_get(&toc, i, &t);
            if ((t.flags & CC_TILE_PROXY) && t.tris > maxp) maxp = t.tris;
        }
        printf("  huge: %u tiles, %.0f ms, largest proxy %u triangles\n", ok ? toc.ntiles : 0, hms, maxp);
        check_true("huge element and non-finite vertices: built, bounded, read back", ok && maxp <= CC_TILES_PROXY_TRIS);
        buf_free(&hb);
        cc_scene_free(&h.s);
    }
    /* many elements as large as the site (all the root's own): grouped, no tile with more than 16 children */
    {
        Synth h;
        memset(&h, 0, sizeof h);
        arena_init(&h.s.arena, 0);
        ecap = 0;
        for (int i = 0; i < 1200; i++) {
            M m = {0};
            double y0 = (i % 40) * 1000.0, z0 = (i / 40) * 400.0;
            box(&m, (double[3]){0, y0, z0}, (double[3]){100000, y0 + 100, z0 + 100}, 6); /* 432 triangles, 100 m long */
            add_el(&h, "IfcBeam", CC_CAT_STEEL, -1, to_mesh(&h.s.arena, &m), NULL);
        }
        cc_tiles_src hs = {&h.s, NULL};
        Buf hb;
        cc_tiles_toc toc;
        int ok = build(&hs, &hb, UINT64_MAX, NULL) == 0 && cc_tiles_check((const uint8_t *)hb.p, hb.len, 1, 1200, &toc, &e) == 0;
        uint32_t most = 0, own = 0;
        for (uint32_t i = 0; ok && i < toc.ntiles; i++) {
            cc_tile t;
            cc_tiles_get(&toc, i, &t);
            if (t.nchildren > most) most = t.nchildren;
            own += (t.flags & CC_TILE_OWN) != 0;
        }
        char w[160];
        snprintf(w, sizeof w, "many site-long elements: %u own leaves, at most %u children a tile", own, most);
        check_true(w, ok && own > 8 && most <= 16);
        buf_free(&hb);
        cc_scene_free(&h.s);
    }
    /* an empty scene */
    {
        cc_scene s0;
        memset(&s0, 0, sizeof s0);
        arena_init(&s0.arena, 0);
        cc_tiles_src es = {&s0, NULL};
        Buf eb;
        cc_tiles_toc toc;
        check_true("empty scene: a section with no tile", build(&es, &eb, UINT64_MAX, NULL) == 0 &&
                                                              cc_tiles_check((const uint8_t *)eb.p, eb.len, 1, 0, &toc, &e) == 0 && toc.ntiles == 0);
        buf_free(&eb);
        cc_scene_free(&s0);
    }
    lattice();
    occlusion();
    return report("tiles");
}
