/* Suite shell (ticket SHELL1, ADR 0078 amendment 7): the exterior shell (src/shell.h) and its place in the tile pyramid
 * (src/tiles.h: element flag 4, tile flag 16, the shell table, the outside walk, the exterior leaf answer):
 *   - a closed box of walls and slabs with an object inside: the object is interior, the walls exterior;
 *   - the same box with a window (a non-opaque class filling a hole in a wall): the object behind it is exterior;
 *   - an open courtyard (a ring of walls, no roof over the yard): the yard's object is exterior, a closed room's not;
 *   - two nested shells: the inner shell and what it holds are interior;
 *   - the section with the shell: checks, the flags agree, the outside walk skips interior tiles and counts exterior
 *     triangles, the full walk is unchanged, a leaf's exterior answer holds only exterior elements, the table's proxies
 *     of tiles with both kinds depict their exterior elements only; damaged tables refused;
 *   - hostile links: a zero-size box, non-finite points, 100,000 tiny elements: bounded, no crash (ASan/UBSan in the gate);
 *   - determinism: the same marks and the same section bytes with 1 and 4 threads, and in steps.
 * Fails on the code before SHELL1: src/shell.h did not exist. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dirent.h>

#include "../src/arena.h"
#include "../src/ifc_read.h"
#include "../src/buf.h"
#include "../src/jobs.h"
#include "../src/scene.h"
#include "../src/shell.h"
#include "../src/tiles.h"
#include "../src/viewfile.h"
#include "check.h"

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

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
static void box(M *m, double x0, double y0, double z0, double x1, double y1, double z1) {
    double lo[3] = {x0, y0, z0}, hi[3] = {x1, y1, z1};
    for (int ax = 0; ax < 3; ax++)
        for (int side = 0; side < 2; side++) {
            int u = (ax + 1) % 3, v = (ax + 2) % 3;
            uint32_t b = m->nv;
            for (int k = 0; k < 4; k++) {
                double q[3];
                q[ax] = side ? hi[ax] : lo[ax], q[u] = k & 1 ? hi[u] : lo[u], q[v] = k & 2 ? hi[v] : lo[v];
                mv(m, q[0], q[1], q[2]);
            }
            if (side) mt(m, b, b + 1, b + 3), mt(m, b, b + 3, b + 2);
            else mt(m, b, b + 3, b + 1), mt(m, b, b + 2, b + 3);
        }
}

static size_t ecap;
static void add(cc_scene *s, const char *cls, cc_category cat, M *m) {
    if (s->nelements == ecap) {
        ecap = ecap ? 2 * ecap : 64;
        cc_scene_element *e = arena_alloc_n(&s->arena, ecap, sizeof *e, 8);
        if (s->nelements) memcpy(e, s->elements, s->nelements * sizeof *e);
        s->elements = e;
    }
    cc_scene_element *e = &s->elements[s->nelements];
    memset(e, 0, sizeof *e);
    char idb[32];
    snprintf(idb, sizeof idb, "S%021zu", s->nelements);
    memcpy(e->id.c, idb, sizeof e->id.c);
    e->class_name.p = cls, e->class_name.n = strlen(cls);
    e->category = cat, e->model_index = s->nelements, e->shape = -1;
    e->mesh.pos = arena_alloc_n(&s->arena, m->nv + 1, sizeof e->mesh.pos[0], 4);
    e->mesh.idx = arena_alloc_n(&s->arena, m->ni + 1, sizeof *e->mesh.idx, 4);
    memcpy(e->mesh.pos, m->p, m->nv * sizeof e->mesh.pos[0]), memcpy(e->mesh.idx, m->i, m->ni * sizeof *m->i);
    e->mesh.nv = m->nv, e->mesh.ni = m->ni;
    free(m->p), free(m->i);
    memset(m, 0, sizeof *m);
    s->nelements++;
}
static void scene_new(cc_scene *s) {
    memset(s, 0, sizeof *s);
    ecap = 0;
    arena_init(&s->arena, 0);
}
#define WALL(s, ...) (box(&m, __VA_ARGS__), add(s, "IfcWall", CC_CAT_MASONRY, &m))
#define SLAB(s, ...) (box(&m, __VA_ARGS__), add(s, "IfcSlab", CC_CAT_CONCRETE, &m))
#define THING(s, ...) (box(&m, __VA_ARGS__), add(s, "IfcFurnishingElement", CC_CAT_TIMBER, &m))
#define WINDOW(s, ...) (box(&m, __VA_ARGS__), add(s, "IfcWindow", CC_CAT_GLASS, &m))

/* a closed room [x0, x1] x [y0, y1] x [z0, z1], walls 200 thick; `win`: a 1 m window in the -y wall at its middle
 * (the wall in three pieces around the hole, a window element in it) */
static void room(cc_scene *s, double x0, double y0, double z0, double x1, double y1, double z1, int win, int roof) {
    M m = {0};
    SLAB(s, x0, y0, z0, x1, y1, z0 + 200);
    if (roof) SLAB(s, x0, y0, z1 - 200, x1, y1, z1);
    double xm = (x0 + x1) / 2, zm = (z0 + z1) / 2;
    if (win) {
        WALL(s, x0, y0, z0 + 200, xm - 500, y0 + 200, z1 - 200);
        WALL(s, xm + 500, y0, z0 + 200, x1, y0 + 200, z1 - 200);
        WALL(s, xm - 500, y0, z0 + 200, xm + 500, y0 + 200, zm - 500);
        WALL(s, xm - 500, y0, zm + 500, xm + 500, y0 + 200, z1 - 200);
        WINDOW(s, xm - 500, y0 + 50, zm - 500, xm + 500, y0 + 150, zm + 500);
    } else WALL(s, x0, y0, z0 + 200, x1, y0 + 200, z1 - 200);
    WALL(s, x0, y1 - 200, z0 + 200, x1, y1, z1 - 200);
    WALL(s, x0, y0 + 200, z0 + 200, x0 + 200, y1 - 200, z1 - 200);
    WALL(s, x1 - 200, y0 + 200, z0 + 200, x1, y1 - 200, z1 - 200);
}

typedef struct {
    Buf sec, shelled;
    uint8_t *ext;
    cc_shell_stats st;
    cc_tiles_shell_info info;
} Out;

static int classify(const cc_scene *s, uint32_t views, uint32_t res, uint64_t step, Out *o) {
    memset(o, 0, sizeof *o);
    buf_init(&o->sec), buf_init(&o->shelled);
    cc_tiles_src src = {s, NULL};
    cc_tiles t;
    cc_error e;
    cc_tiles_opts to;
    cc_tiles_default(&to);
    to.leaf_tris = 1000; /* several leaves even on small scenes */
    to.shell_views = views, to.shell_res = res;
    if (cc_tiles_build(&t, &src, &to, &e)) return printf("build: %s\n", e.msg), cc_tiles_free(&t), -1;
    o->sec = t.out, buf_init(&t.out);
    cc_tiles_free(&t);
    cc_tiles_toc toc;
    if (cc_tiles_check((const uint8_t *)o->sec.p, o->sec.len, 1, (uint32_t)s->nelements, &toc, &e)) return printf("check: %s\n", e.msg), -1;
    cc_shell *sh;
    cc_shell_opts so = {views, res, to.shell_tol, 0};
    if (cc_shell_start(&sh, &src, &toc, &so, &e)) return printf("start: %s\n", e.msg), -1;
    int st;
    while ((st = cc_shell_step(sh, step, &e)) == 0) {}
    if (st < 0) return printf("step: %s\n", e.msg), cc_shell_free(sh), -1;
    o->ext = malloc(s->nelements + 1);
    memcpy(o->ext, cc_shell_exterior(sh), s->nelements);
    cc_shell_get_stats(sh, &o->st);
    int rc = cc_tiles_shell_apply((const uint8_t *)o->sec.p, o->sec.len, &src, &to, o->ext, NULL, &o->shelled, &o->info, &e);
    if (rc) printf("apply: %s\n", e.msg);
    cc_shell_free(sh);
    return rc;
}
static void out_free(Out *o) { buf_free(&o->sec), buf_free(&o->shelled), free(o->ext); }

static void closed_box(void) {
    cc_scene s;
    scene_new(&s);
    room(&s, 0, 0, 0, 10000, 8000, 3500, 0, 1);
    size_t inner = s.nelements;
    M m = {0};
    THING(&s, 4000, 3000, 200, 5000, 4000, 1200);
    Out o;
    check_true("closed box: classified", !classify(&s, 64, 256, UINT64_MAX, &o));
    check_true("closed box: the object inside is interior", o.ext && !o.ext[inner]);
    int walls = 1;
    for (size_t k = 0; o.ext && k < inner; k++) walls &= o.ext[k];
    check_true("closed box: walls and slabs exterior", walls);
    out_free(&o);
    cc_scene_free(&s);
}

static void window_box(void) {
    cc_scene s;
    scene_new(&s);
    room(&s, 0, 0, 0, 10000, 8000, 3500, 1, 1);
    M m = {0};
    size_t behind = s.nelements;
    THING(&s, 4600, 1500, 1300, 5400, 2500, 2100); /* in front of the window, inside */
    size_t corner = s.nelements;
    THING(&s, 300, 300, 300, 700, 700, 500); /* low in a corner of the window's wall: no line through the window reaches it */
    Out o;
    check_true("window: classified", !classify(&s, 256, 512, UINT64_MAX, &o));
    check_true("window: the object behind the window is exterior", o.ext && o.ext[behind]);
    check_true("window: the object low in the window wall's corner is interior", o.ext && !o.ext[corner]);
    check_true("window: the window is exterior", o.ext && o.ext[behind - 1]);
    out_free(&o);
    cc_scene_free(&s);
}

static void courtyard(void) {
    cc_scene s;
    scene_new(&s);
    M m = {0};
    /* a ring building around a 10 m yard, 3 storeys, a roof on the ring only */
    SLAB(&s, 0, 0, 0, 30000, 30000, 200);
    WALL(&s, 0, 0, 200, 30000, 200, 9000), WALL(&s, 0, 29800, 200, 30000, 30000, 9000);
    WALL(&s, 0, 200, 200, 200, 29800, 9000), WALL(&s, 29800, 200, 200, 30000, 29800, 9000);
    WALL(&s, 10000, 10000, 200, 20000, 10200, 9000), WALL(&s, 10000, 19800, 200, 20000, 20000, 9000);
    WALL(&s, 10000, 10200, 200, 10200, 19800, 9000), WALL(&s, 19800, 10200, 200, 20000, 19800, 9000);
    SLAB(&s, 0, 0, 9000, 30000, 10000, 9200), SLAB(&s, 0, 20000, 9000, 30000, 30000, 9200);
    SLAB(&s, 0, 10000, 9000, 10000, 20000, 9200), SLAB(&s, 20000, 10000, 9000, 30000, 20000, 9200);
    size_t yard = s.nelements;
    THING(&s, 14000, 14000, 200, 16000, 16000, 1000);
    size_t ring = s.nelements;
    THING(&s, 3000, 3000, 200, 4000, 4000, 1000);
    Out o;
    check_true("courtyard: classified", !classify(&s, 64, 256, UINT64_MAX, &o));
    check_true("courtyard: the yard's object is exterior", o.ext && o.ext[yard]);
    check_true("courtyard: the ring's room object is interior", o.ext && !o.ext[ring]);
    out_free(&o);
    cc_scene_free(&s);
}

static void nested(void) {
    cc_scene s;
    scene_new(&s);
    room(&s, 0, 0, 0, 20000, 20000, 10000, 0, 1);
    size_t in0 = s.nelements;
    room(&s, 5000, 5000, 1000, 15000, 15000, 8000, 0, 1);
    size_t in1 = s.nelements;
    M m = {0};
    THING(&s, 9000, 9000, 1200, 11000, 11000, 2000);
    Out o;
    check_true("nested: classified", !classify(&s, 64, 256, UINT64_MAX, &o));
    int inner = 1;
    for (size_t k = in0; o.ext && k <= in1; k++) inner &= !o.ext[k];
    check_true("nested: the inner shell and its object are interior", o.ext && inner);
    check_true("nested: counted", o.st.exterior == in0 && o.info.exterior == in0);
    out_free(&o);
    cc_scene_free(&s);
}

/* many rooms with things: the section with its shell, the outside walk, the answers */
static void pyramid(void) {
    cc_scene s;
    scene_new(&s);
    M m = {0};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 3; j++) {
            double x = i * 15000.0, y = j * 12000.0;
            room(&s, x, y, 0, x + 10000, y + 8000, 3500, (i + j) % 2, 1);
            for (int k = 0; k < 40; k++) THING(&s, x + 500 + 220 * k, y + 1000 + 150 * (k % 9), 200, x + 650 + 220 * k, y + 1100 + 150 * (k % 9), 900);
        }
    Out o;
    check_true("pyramid: classified", !classify(&s, 64, 256, UINT64_MAX, &o));
    cc_tiles_toc a, b;
    cc_error e;
    int ok = !cc_tiles_check((const uint8_t *)o.sec.p, o.sec.len, 1, (uint32_t)s.nelements, &a, &e) &&
             !cc_tiles_check((const uint8_t *)o.shelled.p, o.shelled.len, 1, (uint32_t)s.nelements, &b, &e);
    check_true("pyramid: both sections check", ok);
    if (!ok) return printf("  %s\n", e.msg), out_free(&o), cc_scene_free(&s);
    check_true("pyramid: baked flag and table", (b.bake & CC_TILES_BAKE_SHELL) && b.shell && !(a.bake & CC_TILES_BAKE_SHELL) && !a.shell);
    check_true("pyramid: the table of contents is the same size", a.toc == b.toc && a.ntiles == b.ntiles && a.nocc == b.nocc);
    check_true("pyramid: some tiles have both kinds", o.info.mixed > 0 && o.info.shell_build_ok);
    printf("  pyramid: %u of %u elements exterior, %u tiles, %u with both kinds, %llu bytes added\n", o.info.exterior, o.info.elements, b.ntiles,
           o.info.mixed, (unsigned long long)o.info.added);
    /* flags agree with the marks */
    int fl = 1;
    uint64_t ext_tris = 0;
    for (uint32_t i = 0; i < b.nlist; i++) {
        uint32_t el = cc_tiles_list(&b, i);
        fl &= !!(cc_tiles_el_flags(&b, i) & CC_TILES_EL_EXTERIOR) == !!o.ext[el];
        fl &= (cc_tiles_el_flags(&b, i) & 3) == cc_tiles_el_flags(&a, i);
        if (o.ext[el]) ext_tris += s.elements[el].mesh.ni / 3;
    }
    cc_tile_shell r0;
    cc_tiles_shell_get(&b, 0, &r0);
    check_true("pyramid: element flags and the root's exterior triangles", fl && r0.ext == ext_tris);
    /* the walks: the full walk unchanged, the outside walk within it */
    cc_tiles_view v;
    memset(&v, 0, sizeof v);
    v.eye[0] = -60000, v.eye[1] = -50000, v.eye[2] = 30000, v.k = 1080 / (2 * 0.577), v.nplanes = 0;
    uint32_t *ia = malloc(b.ntiles * sizeof *ia), *ib = malloc(b.ntiles * sizeof *ib), *ic = malloc(b.ntiles * sizeof *ic);
    uint64_t ta, tb, tc;
    uint32_t na = cc_tiles_select(&a, &v, 1e-9, UINT64_MAX, CC_TILES_SEL_LEAVES, NULL, ia, a.ntiles, &ta);
    uint32_t nb = cc_tiles_select(&b, &v, 1e-9, UINT64_MAX, CC_TILES_SEL_LEAVES, NULL, ib, b.ntiles, &tb);
    uint32_t nc = cc_tiles_select(&b, &v, 1e-9, UINT64_MAX, CC_TILES_SEL_LEAVES | CC_TILES_SEL_SHELL, NULL, ic, b.ntiles, &tc);
    check_true("pyramid: the full walk is unchanged by the shell", na == nb && ta == tb && !memcmp(ia, ib, na * sizeof *ia));
    check_true("pyramid: the outside walk of every leaf draws the exterior triangles", tc == ext_tris && nc <= nb);
    uint32_t nd = cc_tiles_select(&a, &v, 1e-9, UINT64_MAX, CC_TILES_SEL_LEAVES | CC_TILES_SEL_SHELL, NULL, ic, a.ntiles, &tc);
    check_true("pyramid: without the shell the flag is ignored", nd == na && tc == ta);
    /* the outside walk with levels: proxies of tiles with both kinds are the table's */
    v.eye[0] = -600000, v.eye[1] = -500000, v.eye[2] = 300000;
    nc = cc_tiles_select(&b, &v, 1, UINT64_MAX, CC_TILES_SEL_SHELL, NULL, ic, b.ntiles, &tc);
    nb = cc_tiles_select(&b, &v, 1, UINT64_MAX, 0, NULL, ib, b.ntiles, &tb);
    check_true("pyramid: far outside walk, a few tiles", nc >= 1 && tc <= tb);
    /* the leaf answers: exterior only */
    cc_tiles_src src = {&s, NULL};
    int la = 1;
    uint32_t leaves = 0;
    for (uint32_t i = 0; i < b.ntiles; i++) {
        cc_tile t;
        cc_tiles_get(&b, i, &t);
        if (!(t.flags & CC_TILE_LEAF)) continue;
        Buf x, y;
        buf_init(&x), buf_init(&y);
        la &= !cc_tiles_leaf_answer_ex(&src, &b, i, CC_TILES_ANS_SHELL, &x, &e) && !cc_tiles_leaf_answer(&src, &b, i, &y, &e);
        uint32_t n = 0;
        for (uint32_t k = 0; k < t.nel; k++) n += !!(cc_tiles_el_flags(&b, t.el0 + k) & CC_TILES_EL_EXTERIOR);
        la &= x.len >= 16 && (uint32_t)(((uint8_t)x.p[8]) | ((uint8_t)x.p[9]) << 8) == n && y.len >= x.len;
        la &= !!(t.flags & CC_TILE_EXTERIOR) == (n > 0);
        Arena ar;
        arena_init(&ar, 0);
        cc_mesh lm;
        uint32_t *rows;
        if (!cc_tiles_leaf_mesh((const uint8_t *)x.p, x.len, &ar, &lm, &rows))
            for (uint32_t q = 0; q < lm.ni / 3; q++) la &= o.ext[rows[q]] != 0;
        else la = 0;
        arena_free(&ar);
        buf_free(&x), buf_free(&y);
        leaves++;
    }
    check_true("pyramid: the exterior leaf answers hold the exterior elements only", la && leaves > 2);
    /* the shell proxies: each mixed tile's depicts only exterior geometry (no point inside an interior-only element's box) */
    int px = 1, nmix = 0;
    for (uint32_t i = 0; i < b.ntiles; i++) {
        cc_tile t;
        cc_tile_shell r;
        cc_tiles_get(&b, i, &t), cc_tiles_shell_get(&b, i, &r);
        if (!(r.flags & CC_TILE_SHELL_PROXY) || r.off == t.off) continue;
        nmix++;
        px &= r.tris <= t.tris || r.tris <= r.ext;
        Arena ar;
        arena_init(&ar, 0);
        cc_mesh pm;
        uint8_t *cat;
        px &= !cc_tiles_proxy_mesh(b.p + b.blob_at + r.off, r.len, t.box, &ar, &pm, &cat);
        arena_free(&ar);
    }
    check_true("pyramid: tiles with both kinds have their exterior proxy", px && nmix > 0);
    /* damaged tables refused, never a crash */
    unsigned seed = 7;
    int refused = 0;
    for (int k = 0; k < 2000; k++) {
        Buf d;
        buf_init(&d);
        buf_append(&d, o.shelled.p, o.shelled.len);
        seed = seed * 1103515245u + 12345u;
        size_t at = o.shelled.len - 16 - 32 - 32 * (size_t)b.ntiles + (seed >> 4) % (32 + 32 * (size_t)b.ntiles + 16);
        seed = seed * 1103515245u + 12345u;
        d.p[at] = (char)(d.p[at] ^ (1u << ((seed >> 7) % 8)));
        cc_tiles_toc z;
        if (cc_tiles_check((const uint8_t *)d.p, d.len, 1, (uint32_t)s.nelements, &z, NULL)) refused++;
        else { /* read: the walks run */
            uint64_t tt;
            cc_tiles_select(&z, &v, 1, UINT64_MAX, CC_TILES_SEL_SHELL, NULL, ic, z.ntiles, &tt);
        }
        buf_free(&d);
    }
    printf("  pyramid: %d of 2000 damaged tables refused\n", refused);
    check_true("pyramid: damaged tables mostly refused", refused > 1000);
    free(ia), free(ib), free(ic);
    out_free(&o);
    cc_scene_free(&s);
}

static void hostile(void) {
    M m = {0};
    { /* a zero-size link: every point the same */
        cc_scene s;
        scene_new(&s);
        for (int k = 0; k < 5; k++) mv(&m, 7, 7, 7);
        mt(&m, 0, 1, 2), mt(&m, 2, 3, 4);
        add(&s, "IfcWall", CC_CAT_MASONRY, &m);
        Out o;
        int rc = classify(&s, 64, 64, UINT64_MAX, &o);
        check_true("hostile: a zero-size link ends", rc == 0 || rc == -1);
        out_free(&o);
        cc_scene_free(&s);
    }
    { /* non-finite points among good ones */
        cc_scene s;
        scene_new(&s);
        room(&s, 0, 0, 0, 5000, 5000, 3000, 0, 1);
        box(&m, 1000, 1000, 1000, 2000, 2000, 2000);
        m.p[0][0] = 0.0f / 0.0f, m.p[3][1] = 1.0f / 0.0f;
        add(&s, "IfcFurnishingElement", CC_CAT_TIMBER, &m);
        Out o;
        int rc = classify(&s, 64, 128, 1000, &o);
        check_true("hostile: non-finite points end", rc == 0 || rc == -1);
        out_free(&o);
        cc_scene_free(&s);
    }
    { /* 100,000 tiny elements: bounded */
        cc_scene s;
        scene_new(&s);
        room(&s, 0, 0, 0, 50000, 50000, 3000, 0, 1);
        for (int k = 0; k < 100000; k++) {
            double x = 300 + (k % 316) * 150.0, y = 300 + (k / 316) * 150.0;
            mv(&m, x, y, 500), mv(&m, x + 1, y, 500), mv(&m, x, y + 1, 500);
            mt(&m, 0, 1, 2);
            add(&s, "IfcFurnishingElement", CC_CAT_TIMBER, &m);
        }
        double t0 = now_ms();
        Out o;
        int rc = classify(&s, 64, 256, UINT64_MAX, &o);
        uint32_t ext = 0;
        for (size_t k = 12; o.ext && k < s.nelements; k++) ext += o.ext[k];
        printf("  hostile: 100,000 tiny elements in %.0f ms, %u exterior, peak %zu bytes\n", now_ms() - t0, ext, o.st.peak_bytes);
        check_true("hostile: 100,000 tiny elements, inside a closed room, interior", rc == 0 && ext == 0);
        check_true("hostile: bounded memory", o.st.peak_bytes < ((size_t)160 << 20));
        out_free(&o);
        cc_scene_free(&s);
    }
}

static void determinism(void) {
    cc_scene s;
    scene_new(&s);
    M m = {0};
    for (int i = 0; i < 3; i++) {
        room(&s, i * 12000.0, 0, 0, i * 12000.0 + 10000, 8000, 3500, i != 1, i != 2);
        for (int k = 0; k < 30; k++) THING(&s, i * 12000.0 + 500 + 300 * k, 1000 + 200 * (k % 7), 200, i * 12000.0 + 700 + 300 * k, 1200 + 200 * (k % 7), 900);
    }
    Out a, b, c;
    cc_jobs_set_threads(1);
    int ra = classify(&s, 100, 200, UINT64_MAX, &a);
    cc_jobs_set_threads(4);
    int rb = classify(&s, 100, 200, UINT64_MAX, &b);
    int rc = classify(&s, 100, 200, 777, &c);
    cc_jobs_set_threads(0);
    check_true("determinism: classified", !ra && !rb && !rc);
    check_true("determinism: the same marks with 1 and 4 threads and in steps",
               a.ext && b.ext && c.ext && !memcmp(a.ext, b.ext, s.nelements) && !memcmp(a.ext, c.ext, s.nelements));
    check_true("determinism: the same section bytes", a.shelled.len == b.shelled.len && !memcmp(a.shelled.p, b.shelled.p, a.shelled.len) &&
                                                        a.shelled.len == c.shelled.len && !memcmp(a.shelled.p, c.shelled.p, a.shelled.len));
    out_free(&a), out_free(&b), out_free(&c);
    cc_scene_free(&s);
}

static void raster(void) { /* the rasteriser: a square covers its pixels; a sliver is found conservatively only */
    cc_shell_cam c;
    double dir[3] = {0, 0, -1}, ctr[3] = {0, 0, 0};
    cc_shell_cam_ortho(&c, dir, ctr, 100, 100);
    cc_shell_zb z;
    check_true("raster: buffer", !cc_shell_zb_init(&z, &c));
    float q1[9] = {-50, -50, 10, 50, -50, 10, 50, 50, 10}, q2[9] = {-50, -50, 10, 50, 50, 10, -50, 50, 10};
    cc_shell_draw(&z, q1), cc_shell_draw(&z, q2);
    int covered = 0;
    for (int i = 0; i < 100 * 100; i++) covered += z.z[i] < 1e30f;
    check_true("raster: a square covers its quarter", covered == 50 * 50);
    cc_shell_zb_blocks(&z);
    float below[9] = {-10, -10, 0, 10, -10, 0, 0, 10, 0}, above[9] = {-10, -10, 20, 10, -10, 20, 0, 10, 20};
    float sliver[9] = {60.1f, 60.1f, 0, 60.3f, 60.1f, 0, 60.2f, 60.3f, 0}; /* inside one pixel, off the centre */
    check_true("raster: under the square: hidden", !cc_shell_test(&z, below, 0, 1));
    check_true("raster: over it: seen", cc_shell_test(&z, above, 0, 1));
    check_true("raster: a sliver: conservative finds it, exact not", cc_shell_test(&z, sliver, 0, 1) && !cc_shell_test(&z, sliver, 0, 0));
    float bx[6] = {-10, -10, -5, 10, 10, 0}, bx2[6] = {-10, -10, 15, 10, 10, 18};
    check_true("raster: boxes", cc_shell_box_hidden(&z, bx, 0) && !cc_shell_box_hidden(&z, bx2, 0));
    cc_shell_zb_free(&z);
    double d[3 * 300];
    cc_shell_dirs(300, d);
    int unit = 1;
    for (int i = 0; i < 300; i++) {
        double l = d[3 * i] * d[3 * i] + d[3 * i + 1] * d[3 * i + 1] + d[3 * i + 2] * d[3 * i + 2];
        unit &= l > 0.999999 && l < 1.000001;
    }
    check_true("raster: unit directions", unit);
}


/* the cross-link pass: a room in one scene, a thing in another placed inside it (turned and moved): alone the thing is
 * exterior, together interior; the room's walls stay exterior */
static void cross(void) {
    cc_scene a, b;
    scene_new(&a);
    room(&a, 0, 0, 0, 10000, 8000, 3500, 0, 1);
    size_t na = a.nelements;
    cc_scene_element *ae = a.elements;
    ecap = 0;
    scene_new(&b);
    M m = {0};
    THING(&b, -500, -500, 200, 500, 500, 1200); /* placed at (5000, 4000, 0), turned 90 degrees: inside the room */
    THING(&b, 20000, 0, 0, 21000, 1000, 1000);  /* far outside it (turned: at (5000, 24000)) */
    (void)ae;
    cc_tiles_src sa = {&a, NULL}, sb = {&b, NULL};
    cc_tiles ta, tb;
    cc_error e;
    cc_tiles_opts to;
    cc_tiles_default(&to);
    int ok = !cc_tiles_build(&ta, &sa, &to, &e) && !cc_tiles_build(&tb, &sb, &to, &e);
    cc_shell_part pp[2];
    memset(pp, 0, sizeof pp);
    ok = ok && !cc_tiles_check((const uint8_t *)ta.out.p, ta.out.len, 1, (uint32_t)a.nelements, &pp[0].toc, &e) &&
         !cc_tiles_check((const uint8_t *)tb.out.p, tb.out.len, 1, (uint32_t)b.nelements, &pp[1].toc, &e);
    pp[0].src = sa, pp[0].c = 1;
    pp[1].src = sb, pp[1].c = 0, pp[1].s = 1, pp[1].o[0] = 5000, pp[1].o[1] = 4000;
    cc_shell_opts so = {64, 256, 100, 0};
    cc_shell *x = NULL, *y = NULL;
    ok = ok && !cc_shell_start_parts(&x, pp, 2, &so, &e) && !cc_shell_start(&y, &sb, &pp[1].toc, &so, &e);
    while (ok && x && cc_shell_step(x, UINT64_MAX, &e) == 0) {}
    while (ok && y && cc_shell_step(y, UINT64_MAX, &e) == 0) {}
    check_true("cross: classified", ok && x && y);
    if (ok && x && y) {
        const uint8_t *xa = cc_shell_exterior_of(x, 0), *xb = cc_shell_exterior_of(x, 1), *yb = cc_shell_exterior(y);
        int walls = 1;
        for (size_t k = 0; k < na; k++) walls &= xa[k];
        check_true("cross: alone the thing is exterior; with the room around it, interior", yb[0] && !xb[0]);
        check_true("cross: the thing outside the room stays exterior, the room's walls too", xb[1] && walls);
        check_true("cross: part 0's marks are the whole's start", cc_shell_exterior(x) == xa && !cc_shell_exterior_of(x, 2));
    }
    cc_shell_free(x), cc_shell_free(y);
    cc_tiles_free(&ta), cc_tiles_free(&tb);
    cc_scene_free(&a), cc_scene_free(&b);
}

/* ---- back-face culling proven per element (the pyramid's element flag 1, "closed and outward") ---- */
static int mirrored(const cc_placement32 *p) {
    float c[3] = {p->y[1] * p->z[2] - p->y[2] * p->z[1], p->y[2] * p->z[0] - p->y[0] * p->z[2], p->y[0] * p->z[1] - p->y[1] * p->z[0]};
    return p->x[0] * c[0] + p->x[1] * c[1] + p->x[2] * c[2] < 0.0f;
}
static int proof_flagged, proof_failed;
/* every element of `sc` the pyramid flags closed: its triangles placed as a renderer culls them (a mirrored copy's winding
 * flipped, the contract's rule) drawn alone from 26 directions with and without culling: the same pixels and depths */
static void cull_proofs(const char *what, const cc_tiles_src *src) {
    cc_tiles t;
    cc_error e;
    cc_tiles_opts to;
    cc_tiles_default(&to);
    to.shell_views = 0;
    size_t ne = cc_tiles_src_count(src);
    if (cc_tiles_build(&t, src, &to, &e)) {
        printf("  %s: %s\n", what, e.msg), cc_tiles_free(&t);
        return;
    }
    cc_tiles_toc toc;
    Arena ar;
    arena_init(&ar, 0);
    if (t.out.len && !cc_tiles_check((const uint8_t *)t.out.p, t.out.len, 1, (uint32_t)ne, &toc, &e))
        for (uint32_t i = 0; i < toc.nlist; i++) {
            if (!(cc_tiles_el_flags(&toc, i) & CC_TILES_EL_CLOSED)) continue;
            uint32_t el = cc_tiles_list(&toc, i);
            cc_mesh mm;
            arena_reset(&ar);
            size_t bl = cc_tiles_src_el_block(src, el);
            if (bl == SIZE_MAX || cc_tiles_src_block(src, bl, &ar, &mm)) continue;
            const cc_mesh *m = &mm;
            const cc_placement32 *pl = cc_tiles_src_place(src, el);
            int mir = pl && mirrored(pl);
            float(*tr)[9] = malloc(((size_t)m->ni / 3 + 1) * sizeof *tr);
            uint32_t n = 0;
            for (uint32_t k = 0; tr && k + 2 < m->ni; k += 3) {
                uint32_t ix[3] = {m->idx[k], m->idx[k + (mir ? 2 : 1)], m->idx[k + (mir ? 1 : 2)]};
                if (ix[0] >= m->nv || ix[1] >= m->nv || ix[2] >= m->nv) continue;
                for (int c = 0; c < 3; c++) {
                    const float *q = m->pos[ix[c]];
                    for (int j = 0; j < 3; j++) tr[n][3 * c + j] = pl ? pl->x[j] * q[0] + pl->y[j] * q[1] + pl->z[j] * q[2] + pl->t[j] : q[j];
                }
                n++;
            }
            uint32_t v = 0;
            proof_flagged++;
            if (tr && !cc_shell_cull_proof((const float(*)[9])tr, n, 26, 96, &v)) {
                cc_str cls = src->scene ? src->scene->elements[el].class_name : src->vf->elements[el].class_name;
                if (proof_failed++ < 12)
                    printf("  %s: element %u (%.*s, %s, %u triangles%s) flagged closed and outward: culling changes view %u\n", what, el, (int)cls.n,
                           cls.p ? cls.p : "", pl ? "a shape's copy" : "its own mesh", n, mir ? ", mirrored" : "", v);
            }
            free(tr);
        }
    arena_free(&ar);
    cc_tiles_free(&t);
}

static void cull_corpus(void) {
    /* SHELL_PROOF_DIR=<dir>: its IFC files proven too (tools: the synthetic links, a local folder) */
    const char *DIRS[3] = {"golden/ifc", "golden/ifc_c/fixtures", getenv("SHELL_PROOF_DIR")};
    for (int d = 0; d < 3; d++) {
        if (!DIRS[d]) continue;
        DIR *dir = opendir(DIRS[d]);
        if (!dir) continue;
        struct dirent *de;
        char *names[256];
        int nn = 0;
        while ((de = readdir(dir)) && nn < 256) {
            size_t l = strlen(de->d_name);
            if (l > 4 && !strcmp(de->d_name + l - 4, ".ifc")) names[nn] = malloc(l + 1), strcpy(names[nn++], de->d_name);
        }
        closedir(dir);
        for (int k = 0; k < nn; k++) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s", DIRS[d], names[k]);
            FILE *f = fopen(path, "rb");
            char *txt = NULL;
            long len = 0;
            if (f && !fseek(f, 0, SEEK_END) && (len = ftell(f)) > 0 && len < (256l << 20) && !fseek(f, 0, SEEK_SET) && (txt = malloc((size_t)len + 1)) &&
                fread(txt, 1, (size_t)len, f) == (size_t)len) {
                txt[len] = 0;
                cc_ifc_scene sc;
                cc_error e;
                if (!cc_ifc_read(txt, (size_t)len, &sc, &e)) {
                    cc_tiles_src src = {&sc.scene, NULL};
                    cull_proofs(path, &src); /* the reader's meshes */
                    cc_viewfile_meta mt; /* and the cache's (BMG1 on its 1/16 mm lattice), read in place as the bake reads them */
                    Buf vb;
                    buf_init(&vb);
                    cc_viewfile vf;
                    if (!cc_ifc_scene_meta(&sc, &mt) && !cc_viewfile_write_meta(&mt, &sc.scene, 0, &vb) && !cc_viewfile_read_mapped((const uint8_t *)vb.p, vb.len, &vf, &e)) {
                        cc_tiles_src vs = {NULL, &vf};
                        char w2[600];
                        snprintf(w2, sizeof w2, "%s (cache)", path);
                        cull_proofs(w2, &vs);
                        cc_viewfile_free(&vf);
                    }
                    cc_viewfile_meta_free(&mt);
                    buf_free(&vb);
                }
                cc_ifc_scene_free(&sc);
            }
            if (f) fclose(f);
            free(txt), free(names[k]);
        }
    }
}

int main(void) {
    raster();
    closed_box();
    window_box();
    courtyard();
    nested();
    pyramid();
    hostile();
    determinism();
    cross();
    cull_corpus();
    printf("  culling proofs: %d elements flagged closed and outward, %d where culling changes the picture\n", proof_flagged, proof_failed);
    check_true("every element flagged closed and outward looks the same culled (26 directions, alone)", proof_flagged > 0 && proof_failed == 0);
    return report("shell");
}
