/* Suite scene_dedup (ticket SIZE3): which copies become one shared shape, against Rust.
 *   - Per kind of copy of a box (dedup.rs share_copies on own meshes): translated, turned about z,
 *     turned about an oblique axis, scaled, stretched along one axis, within the tolerance (a corner
 *     0.05 mm off) share; mirrored (faces turned to stay outward), faces in another order, a corner
 *     0.2 mm or 50 mm off do not (a copy's vertices are matched by order). The port of dedup.rs's test
 *     `rotated_copies_share_one_shape` (dedup.rs:276-353): a, turned, mirrored, longer, bent ->
 *     shared [1, 1, 0, 1, 0]; and every element's triangles stay where they were (0.2 mm).
 *   - scene.rs from_model (scene.rs:266-312): elements whose mesh generators hold one positions array
 *     (the binary .bimble's table, ADR 0042 decision 4) are one shape meshed once and placed, however
 *     many distinct shapes look alike: 20 shapes of the same vertex, triangle and edge counts (one
 *     dedup bucket, MAX_SHAPES_PER_BUCKET 16), each placed 3 times turned and moved: 20 shapes and
 *     60 instances, each element's world mesh within 0.2 mm of the same model meshed element by
 *     element. Fails on the code before SIZE3: from_model meshed every element alone, and dedup's
 *     bucket cap left 4 of the 20 shapes (12 elements) unshared (a local imported model of 1,749
 *     elements: 1,081 placed of Rust's 1,472, 1.67 M stored triangles of Rust's 0.30 M).
 *   - A cut element (openings) or a reference plane never shares its generator's mesh. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ifc_geom.h" /* MeshBuilder::add_polygon, as Rust's test builds its boxes */
#include "../src/arena.h"
#include "../src/file.h"
#include "../src/model.h"
#include "../src/scene.h"
#include "../src/scene_build.h"
#include "check.h"

typedef void (*Xf)(const double *p, double *o);

static void id_xf(const double *p, double *o) { o[0] = p[0], o[1] = p[1], o[2] = p[2]; }
static void moved(const double *p, double *o) { o[0] = p[0] + 9000.0, o[1] = p[1] - 4000.0, o[2] = p[2] + 700.0; }
static void turned(const double *p, double *o) { o[0] = 5000.0 - p[1], o[1] = 2000.0 + p[0], o[2] = p[2]; }
static void oblique(const double *p, double *o) { /* 30 degrees about (1, 1, 1)/sqrt(3), moved */
    static const double R[3][3] = {{0.9106836025229591, -0.24401693585629247, 0.33333333333333337},
                                   {0.33333333333333337, 0.9106836025229591, -0.24401693585629247},
                                   {-0.24401693585629247, 0.33333333333333337, 0.9106836025229591}};
    for (int i = 0; i < 3; i++) o[i] = R[i][0] * p[0] + R[i][1] * p[1] + R[i][2] * p[2] + 12000.0;
}
static void scaled(const double *p, double *o) { o[0] = 2.0 * p[0] - 7000.0, o[1] = 2.0 * p[1], o[2] = 2.0 * p[2]; }
static void longer(const double *p, double *o) { o[0] = p[0], o[1] = p[1], o[2] = p[2] * 1.5 + 9000.0; }
static void mirrored(const double *p, double *o) { o[0] = -p[0] - 9000.0, o[1] = p[1], o[2] = p[2]; }
static void bent(const double *p, double *o) { o[0] = p[0] + 20000.0, o[1] = p[1], o[2] = p[2]; }

/* dedup.rs's `boxed`: a 300 x 500 x 3000 box of six polygons through `t`; `moved_corner` mm added
 * to the top's third corner (x); `reverse`: each polygon's points in the other order (a mirrored
 * copy whose faces still point out); `rotate_faces`: the sides added before the bottom and top. */
static void boxed(Tris *mb, Xf t, double moved_corner, int reverse, int rotate_faces) {
    const double a = 300.0, b = 500.0, h = 3000.0;
    double q0[4][3] = {{0, 0, 0}, {a, 0, 0}, {a, b, 0}, {0, b, 0}}, r0[4][3] = {{0, 0, h}, {a, 0, h}, {a, b, h}, {0, b, h}};
    r0[2][0] += moved_corner;
    double q[4][3], r[4][3];
    for (int k = 0; k < 4; k++) t(q0[k], q[k]), t(r0[k], r[k]);
    double polys[6][4][3];
    int np = 0;
    if (!rotate_faces) {
        memcpy(polys[np][0], q[0], 24), memcpy(polys[np][1], q[3], 24), memcpy(polys[np][2], q[2], 24), memcpy(polys[np][3], q[1], 24), np++;
        memcpy(polys[np], r, sizeof r), np++;
    }
    for (int k = 0; k < 4; k++) {
        int j = (k + 1) % 4;
        memcpy(polys[np][0], q[k], 24), memcpy(polys[np][1], q[j], 24), memcpy(polys[np][2], r[j], 24), memcpy(polys[np][3], r[k], 24), np++;
    }
    if (rotate_faces) {
        memcpy(polys[np][0], q[0], 24), memcpy(polys[np][1], q[3], 24), memcpy(polys[np][2], q[2], 24), memcpy(polys[np][3], q[1], 24), np++;
        memcpy(polys[np], r, sizeof r), np++;
    }
    for (int p = 0; p < np; p++) {
        double o[4][3];
        for (int k = 0; k < 4; k++) memcpy(o[k], polys[p][reverse ? 3 - k : k], 24);
        add_polygon(mb, (const double(*)[3])o, 4, NULL, 0);
    }
}

/* A scene of own meshes, one per element (SceneBuilder::push then build at origin 0). */
static int scene_of(cc_scene *s, Tris *items, size_t n) {
    memset(s, 0, sizeof *s);
    arena_init(&s->arena, 0);
    Arena scratch;
    arena_init(&scratch, 0);
    s->elements = arena_alloc(&s->arena, (n + 1) * sizeof *s->elements, 8);
    if (!s->elements) return -1;
    for (size_t k = 0; k < n; k++) {
        cc_scene_element *e = &s->elements[k];
        memset(e, 0, sizeof *e);
        snprintf(e->id.c, sizeof e->id.c, "%021zu", k);
        e->shape = -1;
        e->model_index = k;
        static const double o[3] = {0, 0, 0};
        if (cc_scene_mesh_build((double(*)[3][3])items[k].t, (uint32_t)items[k].n, o, &s->arena, &scratch, &e->mesh)) return -1;
        arena_reset(&scratch);
    }
    s->nelements = n;
    arena_free(&scratch);
    return 0;
}

/* Each element's placed mesh against its own mesh before sharing: as many indices, every vertex
 * within `tol` mm. */
static int same_meshes(const cc_scene *a, const cc_scene *b, double tol) {
    if (a->nelements != b->nelements) return 0;
    Arena t;
    arena_init(&t, 0);
    int ok = 1;
    for (size_t k = 0; k < a->nelements && ok; k++) {
        cc_mesh x, y;
        if (cc_scene_world_mesh(a, k, &t, &x) || cc_scene_world_mesh(b, k, &t, &y)) ok = 0;
        else if (x.ni != y.ni || x.nv != y.nv) ok = 0;
        else {
            double ox[3], oy[3];
            for (int r = 0; r < 3; r++) ox[r] = a->origin[r], oy[r] = b->origin[r];
            for (uint32_t i = 0; i < x.nv && ok; i++) {
                double d2 = 0;
                for (int r = 0; r < 3; r++) {
                    double d = ((double)x.pos[i][r] + ox[r]) - ((double)y.pos[i][r] + oy[r]);
                    d2 += d * d;
                }
                ok = d2 < tol * tol;
            }
        }
        arena_reset(&t);
    }
    arena_free(&t);
    return ok;
}

static void kinds(void) {
    static const struct {
        const char *name;
        Xf t;
        double corner;
        int reverse, rotate, shared; /* shared: what dedup.rs gives (its doc and test) */
    } K[] = {
        {"translated", moved, 0, 0, 0, 1},
        {"turned 90 degrees about z", turned, 0, 0, 0, 1},
        {"turned 30 degrees about an oblique axis", oblique, 0, 0, 0, 1},
        {"scaled 2x", scaled, 0, 0, 0, 1},
        {"stretched along z (1.5x)", longer, 0, 0, 0, 1},
        {"a corner 0.05 mm off (within 0.1 mm)", bent, 0.05, 0, 0, 1},
        {"mirrored, faces outward", mirrored, 0, 1, 0, 0},
        {"faces in another order", moved, 0, 0, 1, 0},
        {"a corner 0.2 mm off", bent, 0.2, 0, 0, 0},
        {"a corner 50 mm off", bent, 50.0, 0, 0, 0},
    };
    printf("  kind of copy                              shared (C)  dedup.rs\n");
    for (size_t k = 0; k < sizeof K / sizeof *K; k++) {
        Tris it[2];
        memset(it, 0, sizeof it);
        boxed(&it[0], id_xf, 0, 0, 0);
        boxed(&it[1], K[k].t, K[k].corner, K[k].reverse, K[k].rotate);
        cc_scene s, before;
        int ok = !it[0].oom && !it[1].oom && scene_of(&s, it, 2) == 0 && scene_of(&before, it, 2) == 0;
        ok &= cc_scene_share_copies(&s) == 0;
        int shared = ok && s.elements[0].shape >= 0 && s.elements[1].shape >= 0 && s.nshapes == 1;
        printf("  %-42s %-11s %s\n", K[k].name, shared ? "yes" : "no", K[k].shared ? "yes" : "no");
        char what[128];
        snprintf(what, sizeof what, "copy %s: shared as dedup.rs", K[k].name);
        check_true(what, ok && shared == K[k].shared);
        snprintf(what, sizeof what, "copy %s: triangles where they were", K[k].name);
        check_true(what, ok && same_meshes(&s, &before, 0.2));
        cc_scene_free(&s), cc_scene_free(&before);
        tris_free(&it[0]), tris_free(&it[1]);
    }
    /* dedup.rs:330-352: a, turned, mirrored (the same polygons mirrored), longer, bent */
    Tris it[5];
    memset(it, 0, sizeof it);
    boxed(&it[0], id_xf, 0, 0, 0);
    boxed(&it[1], turned, 0, 0, 0);
    boxed(&it[2], mirrored, 0, 0, 0);
    boxed(&it[3], longer, 0, 0, 0);
    boxed(&it[4], bent, 50.0, 0, 0);
    cc_scene s;
    int ok = scene_of(&s, it, 5) == 0 && cc_scene_share_copies(&s) == 0;
    int got[5];
    for (int k = 0; k < 5; k++) got[k] = ok && s.elements[k].shape >= 0;
    check_true("dedup.rs rotated_copies_share_one_shape: [a, turned, mirrored, longer, bent] = [1, 1, 0, 1, 0]",
               ok && got[0] && got[1] && !got[2] && got[3] && !got[4]);
    cc_scene_free(&s);
    for (int k = 0; k < 5; k++) tris_free(&it[k]);
}

/* 20 distinct bipyramids (5 points each, moved by an LCG: no affine map between two), one
 * positions array each, every one placed 3 times; `own`: each element its own copy of the arrays
 * (the JSON form). Elements replace the frame's, ids in order. */
static double (*pyr_pos)[5][3];
static uint32_t pyr_idx[18] = {0, 2, 3, 0, 3, 4, 0, 4, 2, 1, 3, 2, 1, 4, 3, 1, 2, 4};

static int shared_model(cc_model *m, int own, int extra) {
    const cc_element e0 = m->elements[0];
    size_t n = 60 + (size_t)extra;
    cc_element *el = arena_alloc(&m->arena, n * sizeof *el, 8);
    if (!el) return -1;
    for (size_t i = 0; i < n; i++) {
        cc_element *e = &el[i];
        memset(e, 0, sizeof *e);
        snprintf(e->id.c, sizeof e->id.c, "0%020zu", i);
        e->cls = CC_IFC_PROXY;
        e->storey = e0.storey;
        e->material = e0.material;
        size_t shape = i % 20, copy = i / 20;
        double ang = 0.7 * (double)copy + 0.1 * (double)shape;
        double c = 1.0 - ang * ang / 2.0 + ang * ang * ang * ang / 24.0, sn = ang - ang * ang * ang / 6.0;
        double l = c * c + sn * sn; /* not exactly 1: scaled back */
        double inv = 1.0 / l;
        e->placement.x_axis[0] = c * inv, e->placement.x_axis[1] = sn * inv, e->placement.x_axis[2] = 0.0;
        e->placement.z_axis[2] = 1.0;
        e->placement.origin[0] = 3000.0 * (double)shape, e->placement.origin[1] = 4000.0 * (double)copy;
        e->generator.kind = CC_GEN_MESH;
        e->generator.u.mesh.positions = pyr_pos[shape];
        e->generator.u.mesh.npositions = 5;
        e->generator.u.mesh.indices = pyr_idx;
        e->generator.u.mesh.nindices = 18;
        if (own) {
            double (*p)[3] = arena_alloc(&m->arena, sizeof pyr_pos[0], 8);
            if (!p) return -1;
            memcpy(p, pyr_pos[shape], sizeof pyr_pos[0]);
            e->generator.u.mesh.positions = p;
        }
        if (i >= 60) { /* extra: a reference plane and a cut element on shape 0's array */
            e->generator.u.mesh.positions = pyr_pos[0];
            if (i == 60) e->cls = CC_IFC_VIRTUAL_ELEMENT;
            else {
                static cc_opening op;
                op.placement.x_axis[0] = 1.0, op.placement.z_axis[2] = 1.0, op.placement.origin[0] = 1e6; /* far: cuts nothing */
                op.generator = e->generator;
                e->openings = &op, e->nopenings = 1;
            }
        }
    }
    m->elements = el;
    m->nelements = n;
    return 0;
}

static void from_model(const char *frame, size_t fn) {
    cc_error err;
    pyr_pos = malloc(20 * sizeof *pyr_pos);
    if (!pyr_pos) return;
    uint64_t r = 777;
    for (int s = 0; s < 20; s++) {
        double p[5][3] = {{0, 0, 500}, {0, 0, -500}, {400, 0, 0}, {-200, 350, 0}, {-200, -350, 0}};
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 3; j++) {
                r = r * 6364136223846793005ull + 1442695040888963407ull;
                p[i][j] += (double)(r >> 40) / 16777216.0 * 300.0;
            }
        memcpy(pyr_pos[s], p, sizeof p);
    }
    cc_model a, b;
    if (cc_model_load(&a, frame, fn, &err) || cc_model_load(&b, frame, fn, &err)) {
        check_true("frame.json loads", 0);
        return;
    }
    int ok = shared_model(&a, 0, 0) == 0 && shared_model(&b, 1, 0) == 0;
    cc_scene sa, sb;
    ok &= cc_scene_build(&sa, &a, &err) == 0;
    ok &= cc_scene_build(&sb, &b, &err) == 0;
    size_t inst = 0, binst = 0;
    for (size_t k = 0; ok && k < sa.nelements; k++) inst += sa.elements[k].shape >= 0;
    for (size_t k = 0; ok && k < sb.nelements; k++) binst += sb.elements[k].shape >= 0;
    printf("  20 shapes x 3 (one bucket): one positions array per shape: %zu shapes, %zu of %zu placed; each "
           "element its own arrays: %zu shapes, %zu placed\n",
           sa.nshapes, inst, sa.nelements, sb.nshapes, binst);
    check_true("shared positions: 20 shapes, 60 instances", ok && sa.nelements == 60 && sa.nshapes == 20 && inst == 60);
    check_true("shared positions: each element where meshing it alone puts it (0.2 mm)", ok && same_meshes(&sa, &sb, 0.2));
    check_true("own arrays: dedup bounded by its bucket cap (fewer than 20 shapes, some not placed)", ok && sb.nshapes < 20 && binst < 60);
    cc_scene_free(&sa), cc_scene_free(&sb);
    cc_model_free(&a), cc_model_free(&b);

    /* a reference plane and a cut element on shape 0's array: neither placed by it */
    cc_model c;
    if (cc_model_load(&c, frame, fn, &err)) return;
    ok = shared_model(&c, 0, 2) == 0;
    cc_scene sc;
    ok &= cc_scene_build(&sc, &c, &err) == 0;
    int cut_own = 0, plane_absent = 1;
    for (size_t k = 0; ok && k < sc.nelements; k++) {
        size_t mi = sc.elements[k].model_index;
        if (mi == 60) plane_absent = 0;
        if (mi == 61) cut_own = sc.elements[k].shape < 0 || sc.elements[k].shape != sc.elements[0].shape;
    }
    check_true("a reference plane is not drawn, a cut element is not placed by the uncut ones' shape", ok && plane_absent && cut_own);
    cc_scene_free(&sc);
    cc_model_free(&c);
    free(pyr_pos);
}

int main(int argc, char **argv) {
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    size_t fn;
    char *frame = slurp(dir, "frame.json", &fn);
    if (!frame) {
        printf("frame.json missing\n");
        return 1;
    }
    kinds();
    from_model(frame, fn);
    free(frame);
    return report("scene_dedup");
}
