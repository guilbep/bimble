/* Suite mesh: Scene::from_model of the golden frame and frame2 against Rust's bytes
 * (golden/mesh/, golden/frame2/mesh/, see golden/README.md), earcut against earcutr 0.5.0,
 * outlines that need earcut and mesh generators, robustness, exact sqrt, and timings. */
#define _POSIX_C_SOURCE 199309L /* clock_gettime under -std=c11 */
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../src/model.h"
#include "../src/file.h"
#include "../src/scene.h"
#include "../src/earcut.h"
#include "../src/boolean.h"
#include "../src/ifc_in.h"
#include "check.h"

static const char *golden;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void hex32(Buf *b, float f) {
    uint32_t u;
    char s[16];
    memcpy(&u, &f, 4);
    snprintf(s, sizeof s, "%08x", (unsigned)u);
    buf_puts(b, s);
}

/* golden/mesh/<k>.txt */
static void dump_mesh(Buf *b, const cc_scene *s, size_t k) {
    const cc_scene_element *e = &s->elements[k];
    const cc_mesh *m = cc_scene_local_mesh(s, k);
    buf_clear(b);
    if (e->shape >= 0) {
        const cc_placement32 *p = &e->to_scene;
        const float *cols[4] = {p->x, p->y, p->z, p->t};
        buf_putc(b, 't');
        for (int c = 0; c < 4; c++)
            for (int i = 0; i < 3; i++) buf_putc(b, ' '), hex32(b, cols[c][i]);
        buf_putc(b, '\n');
    }
    for (uint32_t i = 0; i < m->nv; i++) {
        buf_putc(b, 'v');
        for (int j = 0; j < 3; j++) buf_putc(b, ' '), hex32(b, m->pos[i][j]);
        buf_putc(b, '\n');
    }
    for (uint32_t i = 0; i + 2 < m->ni; i += 3) {
        buf_putc(b, 'i');
        for (int j = 0; j < 3; j++) buf_putc(b, ' '), buf_u64(b, m->idx[i + j]);
        buf_putc(b, '\n');
    }
    for (uint32_t i = 0; i < m->ne; i++) {
        buf_putc(b, 'e');
        for (int j = 0; j < 6; j++) buf_putc(b, ' '), hex32(b, m->edges[i][j]);
        buf_putc(b, '\n');
    }
    for (uint32_t i = 0; i < m->nn; i++) {
        buf_putc(b, 'n');
        for (int j = 0; j < 8; j++) buf_putc(b, ' '), buf_i64(b, m->normals[i][j]);
        buf_putc(b, '\n');
    }
}

/* golden/mesh/scene.txt */
static void dump_scene(Buf *b, const cc_scene *s) {
    char t[64];
    buf_clear(b);
    buf_puts(b, "origin");
    for (int k = 0; k < 3; k++) {
        uint64_t u;
        memcpy(&u, &s->origin[k], 8);
        snprintf(t, sizeof t, " %016llx", (unsigned long long)u);
        buf_puts(b, t);
    }
    buf_puts(b, "\nelements ");
    buf_u64(b, s->nelements);
    buf_putc(b, '\n');
    for (size_t k = 0; k < s->nelements; k++) {
        const cc_scene_element *e = &s->elements[k];
        const cc_mesh *m = cc_scene_local_mesh(s, k);
        const char *cat = cc_category_name(e->category);
        snprintf(t, sizeof t, "%03zu ", k);
        buf_puts(b, t);
        cc_buf_gid(b, &e->id);
        buf_putc(b, ' ');
        buf_append(b, e->class_name.p, e->class_name.n);
        buf_putc(b, ' ');
        buf_u64(b, (uint64_t)e->category);
        buf_putc(b, ' ');
        buf_putc(b, (char)(cat[0] - 'a' + 'A')); /* Debug name: "Concrete" */
        buf_puts(b, cat + 1);
        buf_puts(b, " v=");
        buf_u64(b, m->nv);
        buf_puts(b, " i=");
        buf_u64(b, m->ni / 3);
        buf_puts(b, " e=");
        buf_u64(b, m->ne);
        buf_puts(b, " n=");
        buf_u64(b, m->nn);
        buf_putc(b, '\n');
    }
    for (size_t k = 0; k < s->nwarnings; k++) {
        buf_puts(b, "warning ");
        buf_append(b, s->warnings[k].p, s->warnings[k].n);
        buf_putc(b, '\n');
    }
}

/* `dir` is "mesh" or "frame2/mesh". */
static void check_file(const char *dir, const char *name, Buf *got) {
    size_t n;
    char path[96], what[128];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    char *want = slurp(golden, path, &n);
    snprintf(what, sizeof what, "%s/%s byte for byte", dir, name);
    check_eq(what, got->p, got->len, want, n);
    free(want);
}

static uint64_t b64(double x) {
    uint64_t u;
    memcpy(&u, &x, 8);
    return u;
}

/* The frame with one text edit (`from` replaced by `to`), loaded and built. 0 when the edit
 * applies and the model loads; the scene build's code in *rc. */
static int edit_build(const char *frame, size_t fn, const char *what, const char *from, const char *to, cc_model *m,
                      cc_scene *s, int *rc, cc_error *err) {
    char label[160];
    const char *at = strstr(frame, from);
    snprintf(label, sizeof label, "%s: fixture edit applies", what);
    check_true(label, at != NULL);
    if (!at) return -1;
    Buf t;
    buf_init(&t);
    buf_append(&t, frame, (size_t)(at - frame));
    buf_puts(&t, to);
    buf_append(&t, at + strlen(from), fn - (size_t)(at - frame) - strlen(from));
    int lr = cc_model_load(m, t.p, t.len, err);
    buf_free(&t);
    snprintf(label, sizeof label, "%s: model loads", what);
    check_true(label, lr == 0);
    if (lr) {
        printf("  %s\n", err->msg);
        return -1;
    }
    *rc = cc_scene_build(s, m, err);
    return 0;
}

/* The comb outline of the earcut checks (also in the Rust oracle, see below): (0,0), (10n,0),
 * then n+1 teeth back to x = 0. Counter-clockwise, 2n + 6 values. */
static size_t comb(double *v, size_t n) {
    size_t k = 0;
    v[k++] = 0.0;
    v[k++] = 0.0;
    v[k++] = (double)(n * 10);
    v[k++] = 0.0;
    for (size_t j = n + 1; j-- > 0;) {
        v[k++] = (double)(j * 10);
        v[k++] = (double)(100 + (j % 2) * 50 + (j * j % 7) * 3);
    }
    return k;
}

static uint32_t fnv(const uint32_t *t, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ t[i]) * 16777619u;
    return h;
}

static void earcut_case(const char *what, const double *v, size_t nv, const size_t *holes, size_t nh,
                        const uint32_t *want, size_t nwant) {
    uint32_t *t;
    size_t nt;
    int rc = cc_earcut(v, nv, holes, nh, &t, &nt);
    char label[96];
    snprintf(label, sizeof label, "earcut %s = earcutr 0.5.0", what);
    int ok = rc == 0 && nt == nwant && (nt == 0 || memcmp(t, want, nt * sizeof *t) == 0);
    check_true(label, ok);
    if (!ok) {
        printf("  rc %d, got [", rc);
        for (size_t i = 0; i < nt; i++) printf(i ? ", %u" : "%u", t[i]);
        printf("]\n");
    }
    free(t);
}

/* Expected values: earcutr 0.5.0 run on the same input (a throwaway Rust program calling
 * earcutr::earcut(v, holes, 2), not part of the repository). */
static void earcut_checks(void) {
    static const double sq[] = {0, 0, 10, 0, 10, 10, 0, 10};
    static const uint32_t sq_t[] = {2, 3, 0, 0, 1, 2};
    earcut_case("square", sq, 8, NULL, 0, sq_t, 6);
    static const double l[] = {0, 0, 6000, 0, 6000, 2000, 2000, 2000, 2000, 5000, 0, 5000};
    static const uint32_t l_t[] = {4, 5, 0, 0, 1, 2, 3, 4, 0, 0, 2, 3};
    earcut_case("L", l, 12, NULL, 0, l_t, 12);
    static const double ho[] = {0, 0, 4000, 0, 4000, 3000, 0, 3000, 1000, 1000, 1000, 2200, 2500.5, 2200, 2500.5, 1000};
    static const size_t ho_h[] = {4};
    static const uint32_t ho_t[] = {3, 0, 4, 7, 4, 0, 3, 4, 5, 7, 0, 1, 2, 3, 5, 6, 7, 1, 2, 5, 6, 6, 1, 2};
    earcut_case("square with a hole", ho, 16, ho_h, 1, ho_t, 24);
    static const double co[] = {0, 0, 1, 0, 2, 0, 3, 0};
    earcut_case("collinear outline (no triangle)", co, 8, NULL, 0, NULL, 0);
    static const double sm[] = {0, 0, 5, 0, 10, 0, 10, 10, 0, 10};
    static const uint32_t sm_t[] = {3, 4, 0, 1, 2, 3, 3, 0, 1};
    earcut_case("square with a collinear point", sm, 10, NULL, 0, sm_t, 9);
    static const double bt[] = {0, 0, 10, 10, 10, 0, 0, 10};
    static const uint32_t bt_t[] = {3, 2, 1};
    earcut_case("bow tie (self-intersecting)", bt, 8, NULL, 0, bt_t, 3);
    static const double sx[] = {0, 0, 10, 0, 0, 10, 10, 10, 5, -5};
    static const uint32_t sx_t[] = {1, 0, 4, 4, 3, 2};
    earcut_case("self-intersecting pentagon", sx, 10, NULL, 0, sx_t, 6);
    static const double tr[] = {0, 0, 300, 0, 0, 300};
    static const uint32_t tr_t[] = {1, 2, 0};
    earcut_case("triangle", tr, 6, NULL, 0, tr_t, 3);
    {   /* 53 points: z-order hashing (80 values or more) */
        static double c[200];
        size_t nc = comb(c, 50);
        static const uint32_t want[] = {
            51, 52, 0,  0,  1,  2,  2,  3,  4,  4,  5,  6,  6,  7,  8,  8,  9,  10, 10, 11, 12, 12, 13, 14, 14, 15,
            16, 16, 17, 18, 18, 19, 20, 20, 21, 22, 22, 23, 24, 24, 25, 26, 26, 27, 28, 28, 29, 30, 30, 31, 32, 32,
            33, 34, 34, 35, 36, 36, 37, 38, 38, 39, 40, 40, 41, 42, 42, 43, 44, 44, 45, 46, 46, 47, 48, 48, 49, 50,
            50, 51, 0,  0,  2,  4,  6,  8,  10, 10, 12, 14, 20, 22, 24, 24, 26, 28, 34, 36, 38, 38, 40, 42, 48, 50,
            0,  0,  4,  6,  10, 14, 16, 18, 20, 24, 24, 28, 30, 32, 34, 38, 38, 42, 44, 46, 48, 0,  0,  6,  10, 10,
            16, 18, 24, 30, 32, 38, 44, 46, 0,  10, 18, 24, 32, 38, 38, 46, 0,  0,  18, 24, 24, 38, 0};
        earcut_case("comb of 53 points (hashed)", c, nc, NULL, 0, want, sizeof want / sizeof want[0]);
    }
    {   /* hashed, with two holes */
        static double c[200];
        size_t nc = comb(c, 30), h[2];
        static const double holes[] = {100, 20, 100, 60, 200, 60, 200, 20, 250, 20, 250, 60, 260, 60, 260, 20};
        h[0] = nc / 2;
        for (int i = 0; i < 8; i++) c[nc++] = holes[i];
        h[1] = nc / 2;
        for (int i = 8; i < 16; i++) c[nc++] = holes[i];
        static const uint32_t want[] = {
            0,  33, 34, 35, 36, 37, 40, 33, 0,  1,  2,  3,  4,  5,  6,  6,  7,  8,  8,  9,  10, 10, 11, 12,
            12, 13, 14, 14, 15, 16, 16, 17, 18, 18, 19, 20, 20, 21, 22, 22, 23, 24, 24, 25, 26, 26, 27, 28,
            28, 29, 30, 30, 31, 32, 32, 0,  34, 35, 37, 38, 40, 0,  1,  1,  3,  4,  4,  6,  8,  14, 16, 18,
            18, 20, 22, 28, 30, 32, 32, 34, 35, 39, 40, 1,  1,  4,  8,  12, 14, 18, 18, 22, 24, 26, 28, 32,
            32, 35, 38, 39, 1,  8,  10, 12, 18, 18, 24, 26, 26, 32, 38, 38, 39, 8,  18, 26, 38, 38, 8,  10,
            10, 18, 38};
        earcut_case("comb of 33 points with two holes (hashed)", c, nc, h, 2, want, sizeof want / sizeof want[0]);
    }
    {   /* 10,001 points: same triangles as earcutr (count and FNV-1a of the indices) */
        size_t cap = 2 * 10010;
        double *c = malloc(cap * sizeof *c);
        size_t nc = comb(c, 9998);
        uint32_t *t;
        size_t nt;
        double t0 = now_ms();
        int rc = cc_earcut(c, nc, NULL, 0, &t, &nt);
        double ms = now_ms() - t0;
        check_true("earcut comb of 10,001 points = earcutr 0.5.0 (29997 indices, fnv 2de6c797)",
                   rc == 0 && nt == 29997 && fnv(t, nt) == 0x2de6c797u);
        printf("  earcut 10,001 points: %.3f ms\n", ms);
        free(t);
        free(c);
    }
    {   /* malformed input: errors, never a crash */
        static const double v[] = {0, 0, 1, 0, 1, 1};
        static const size_t bad[] = {5};
        static const size_t zero[] = {0};
        static const size_t huge[] = {(size_t)-1};
        uint32_t *t;
        size_t nt;
        check_true("earcut odd value count: error", cc_earcut(v, 5, NULL, 0, &t, &nt) == CC_EARCUT_UNKNOWN);
        check_true("earcut hole past the end: error", cc_earcut(v, 6, bad, 1, &t, &nt) == CC_EARCUT_UNKNOWN);
        check_true("earcut hole at 0: error", cc_earcut(v, 6, zero, 1, &t, &nt) == CC_EARCUT_UNKNOWN);
        check_true("earcut hole index overflow: error", cc_earcut(v, 6, huge, 1, &t, &nt) == CC_EARCUT_UNKNOWN);
        check_true("earcut empty: no triangle", cc_earcut(v, 0, NULL, 0, &t, &nt) == 0 && nt == 0);
    }
}

static int has_element(const cc_scene *s, uint32_t nv, uint32_t ntri) {
    for (size_t k = 0; k < s->nelements; k++) {
        const cc_mesh *m = cc_scene_local_mesh(s, k);
        if (m->nv == nv && m->ni == 3 * ntri) return 1;
    }
    return 0;
}

/* A model element with an opening (ticket C6): golden/ifc/fixtures/opening.ifc imported (the
 * opening kept as a Mesh on the wall, as import.rs), then Scene::from_model, which cuts it
 * (element_tris). No model-level Rust golden holds this mesh: it is compared with the read golden
 * of the same IFC (golden/ifc/read/opening, Rust's read_str cut): the same triangle and vertex
 * counts and the same world points within 0.01 mm. Without a kernel: uncut (12 triangles), with
 * the scene warning. */
static int hexval(const char *p, int n, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return 0;
        v = v << 4 | (uint64_t)d;
    }
    *out = v;
    return 1;
}
static double f64_of(uint64_t u) {
    double d;
    memcpy(&d, &u, 8);
    return d;
}
static float f32_of(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static int has_text(const char *p, size_t n, const char *t) {
    size_t m = strlen(t);
    for (size_t i = 0; i + m <= n; i++)
        if (!memcmp(p + i, t, m)) return 1;
    return 0;
}
static void opening_checks(void) {
    size_t tn, sn, mn;
    char *ifc = slurp(golden, "ifc/fixtures/opening.ifc", &tn);
    char *st = slurp(golden, "ifc/read/opening/scene.txt", &sn);
    char *mt = slurp(golden, "ifc/read/opening/000.txt", &mn);
    /* The read golden's world points: origin + f32 position. */
    double org[3] = {0, 0, 0}, want[64][3];
    size_t nw = 0;
    const char *o = strstr(st, "origin ");
    for (int k = 0; o && k < 3; k++) {
        uint64_t u;
        if (hexval(o + 7 + 17 * k, 16, &u)) org[k] = f64_of(u);
    }
    for (const char *p = mt; p && *p && nw < 64; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        if (p[0] != 'v' || p[1] != ' ') continue;
        uint64_t u;
        for (int k = 0; k < 3; k++) want[nw][k] = hexval(p + 2 + 9 * k, 8, &u) ? org[k] + f32_of((uint32_t)u) : 0.0;
        nw++;
    }
    check_true("opening: read golden has 16 points", nw == 16);
    const cc_boolean *kernel = cc_boolean_kernel();
    for (int pass = 0; pass < (kernel ? 2 : 1); pass++) {
        if (pass) cc_boolean_set(NULL);
        int cut = cc_boolean_kernel() != NULL;
        cc_model m;
        cc_ifc_report rep;
        cc_error err;
        int rc = cc_ifc_import(&m, &rep, ifc, tn, &err);
        check_true("opening: imports", rc == 0);
        check_true("opening: one element with one opening", !rc && m.nelements == 1 && m.elements[0].nopenings == 1);
        cc_scene s;
        rc = rc ? rc : cc_scene_build(&s, &m, &err);
        check_true("opening: scene builds", rc == 0);
        if (!rc) {
            Arena a;
            arena_init(&a, 0);
            cc_mesh w;
            int ok = s.nelements == 1 && !cc_scene_world_mesh(&s, 0, &a, &w);
            printf("opening model scene (%s): %u triangles, %u vertices, %zu warning(s)\n",
                   cut ? "kernel" : "no kernel", ok ? w.ni / 3 : 0, ok ? w.nv : 0, s.nwarnings);
            if (cut) {
                check_true("opening: cut like Rust (32 triangles, 16 vertices)", ok && w.ni == 96 && w.nv == 16);
                check_true("opening: no warning", s.nwarnings == 0);
                /* Every point of each side has one of the other within 0.01 mm. */
                double worst = 0;
                for (int dir = 0; ok && dir < 2; dir++) {
                    size_t na = dir ? nw : w.nv, nb = dir ? w.nv : nw;
                    for (size_t i = 0; i < na; i++) {
                        double best = 1e300;
                        for (size_t j = 0; j < nb; j++) {
                            double d = 0;
                            for (int k = 0; k < 3; k++) {
                                double pa = dir ? want[i][k] : s.origin[k] + w.pos[i][k];
                                double pb = dir ? s.origin[k] + w.pos[j][k] : want[j][k];
                                double e = pa - pb;
                                d = d > (e < 0 ? -e : e) ? d : (e < 0 ? -e : e);
                            }
                            best = best < d ? best : d;
                        }
                        worst = worst > best ? worst : best;
                    }
                }
                printf("opening model scene vs read golden: worst point distance %.3g mm\n", worst);
                check_true("opening: same points as Rust's read (0.01 mm)", ok && worst <= 0.01);
            } else {
                check_true("opening: uncut without a kernel (12 triangles)", ok && w.ni == 36);
                check_true("opening: warned without a kernel",
                           s.nwarnings == 1 && has_text(s.warnings[0].p, s.warnings[0].n, "opening(s) not cut (no boolean kernel"));
            }
            arena_free(&a);
            cc_scene_free(&s);
        }
        cc_model_free(&m);
        cc_ifc_report_free(&rep);
    }
    cc_boolean_set(kernel);
    free(ifc), free(st), free(mt);
}

int main(int argc, char **argv) {
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    golden = dir;
    Buf b;
    buf_init(&b);
    cc_error err;

    /* exact sqrt */
    {
        static const double xs[] = {0.0, 1.0, 2.0, 3.0, 4.0, 0.5, 1e-300, 4.9e-324, 1e300, 123456.789, 0.1,
                                    1.7976931348623157e308, 2.2250738585072014e-308, 90000.0, 1e-12};
        static const uint64_t want[] = {
            0x0000000000000000ull, 0x3ff0000000000000ull, 0x3ff6a09e667f3bcdull, 0x3ffbb67ae8584caaull,
            0x4000000000000000ull, 0x3fe6a09e667f3bcdull, 0x20ca2fe76a3f9475ull, 0x1e60000000000000ull,
            0x5f138d352e5096afull, 0x4075f5d3b16948f6ull, 0x3fd43d136248490full, 0x5fefffffffffffffull,
            0x2000000000000000ull, 0x4072c00000000000ull, 0x3eb0c6f7a0b5ed8dull};
        int ok = 1;
        for (size_t i = 0; i < sizeof xs / sizeof xs[0]; i++) {
            uint64_t g = b64(cc_sqrt(xs[i]));
            if (g != want[i]) {
                ok = 0;
                printf("  sqrt(%.17g) = %016llx, want %016llx\n", xs[i], (unsigned long long)g,
                       (unsigned long long)want[i]);
            }
        }
        check_true("cc_sqrt is correctly rounded (table)", ok);
        /* (x*x) round trips: sqrt(n^2) == n for integers, and squares of a sweep stay ordered */
        ok = 1;
        for (double x = 1.0; x < 3e5; x += 7.25) ok &= cc_sqrt(x * x) == x;
        check_true("cc_sqrt(x*x) == x", ok);
        check_true("cc_sqrt(-1) is NaN", cc_sqrt(-1.0) != cc_sqrt(-1.0));
    }

    size_t fn;
    char *frame = slurp(golden, "frame.json", &fn);
    cc_model m;
    if (cc_model_load(&m, frame, fn, &err)) {
        printf("frame.json does not load: %s\n", err.msg);
        return 1;
    }
    cc_scene s;
    int rc = cc_scene_build(&s, &m, &err);
    check_true("frame scene builds", rc == 0);
    if (rc) printf("  %s\n", err.msg);
    if (!rc) {
        dump_scene(&b, &s);
        check_file("mesh", "scene.txt", &b);
        int all = 1;
        for (size_t k = 0; k < s.nelements; k++) {
            char name[32];
            snprintf(name, sizeof name, "%03zu.txt", k);
            dump_mesh(&b, &s, k);
            check_file("mesh", name, &b);
            all &= s.elements[k].shape >= 0;
        }
        check_true("72 elements, all instances of shared shapes", s.nelements == 72 && all);
        printf("  %zu elements, %zu shared shapes\n", s.nelements, s.nshapes);

        /* world meshes: every placed vertex lies within 0.2 mm of the element's own scene mesh box */
        Arena a;
        arena_init(&a, 0);
        int ok = 1;
        for (size_t k = 0; k < s.nelements; k++) {
            cc_mesh w;
            ok &= cc_scene_world_mesh(&s, k, &a, &w) == 0 && w.nv == 8 && w.ni == 36 && w.ne == 12;
        }
        check_true("world meshes", ok);
        arena_free(&a);
        cc_scene_free(&s);
    }

    /* timing: scene build of the frame */
    {
        int reps = 100;
        double t0 = now_ms();
        for (int i = 0; i < reps; i++) {
            cc_scene_build(&s, &m, &err);
            cc_scene_free(&s);
        }
        printf("  frame scene build: %.3f ms (mean of %d)\n", (now_ms() - t0) / reps, reps);
    }
    cc_model_free(&m);

    /* Requirement change (ticket T5b): an L-shaped slab and a mesh generator now mesh, as in Rust. */
    {
        const char *quad = "[\n            [\n              -150.0,\n              -150.0\n            ],\n"
                           "            [\n              18150.0,\n              -150.0\n            ],\n"
                           "            [\n              18150.0,\n              10150.0\n            ],\n"
                           "            [\n              -150.0,\n              10150.0\n            ]\n          ]";
        const char *column = "\"kind\": \"rect_column\",\n          \"width\": 300.0,\n          \"depth\": 300.0,\n"
                             "          \"height\": 2780.0";
        cc_model em;
        cc_scene es;
        int rc;
        if (!edit_build(frame, fn, "L-shaped slab", quad,
                        "[[0, 0], [6000, 0], [6000, 3000], [3000, 3000], [3000, 6000], [0, 6000]]", &em, &es, &rc,
                        &err)) {
            check_true("L-shaped slab: scene builds, 72 elements, no warning, one 12 v / 20 tri mesh",
                       rc == 0 && es.nelements == 72 && es.nwarnings == 0 && has_element(&es, 12, 20));
            if (rc) printf("  %s\n", err.msg);
            cc_scene_free(&es);
            cc_model_free(&em);
        }
        if (!edit_build(frame, fn, "mesh generator", column,
                        "\"kind\": \"mesh\", \"positions\": [[0, 0, 0], [1000, 0, 0], [0, 1000, 0], [0, 0, 1000]], "
                        "\"indices\": [0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3]",
                        &em, &es, &rc, &err)) {
            check_true("mesh generator: scene builds, 72 elements, no warning, one 4 v / 4 tri mesh",
                       rc == 0 && es.nelements == 72 && es.nwarnings == 0 && has_element(&es, 4, 4));
            if (rc) printf("  %s\n", err.msg);
            cc_scene_free(&es);
            cc_model_free(&em);
        }
        /* Robustness: a self-intersecting slab outline, and a 10,001-point one. A result or an
         * error, never a crash (the suite also runs under ASan/UBSan). */
        if (!edit_build(frame, fn, "self-intersecting outline", quad,
                        "[[0, 0], [6000, 0], [6000, 6000], [2000, -2000], [0, 6000]]", &em, &es, &rc, &err)) {
            printf("  self-intersecting outline: rc %d, %zu elements, %zu warnings\n", rc, es.nelements,
                   es.nwarnings);
            if (es.nwarnings) printf("  %.*s\n", (int)es.warnings[0].n, es.warnings[0].p);
            check_true("self-intersecting outline: a result or a clear error", rc == 0 || err.msg[0]);
            cc_scene_free(&es);
            cc_model_free(&em);
        }
        {
            size_t cap = 2 * 10010;
            double *c = malloc(cap * sizeof *c);
            size_t nc = comb(c, 9998);
            Buf o;
            buf_init(&o);
            buf_putc(&o, '[');
            for (size_t i = 0; i < nc; i += 2) {
                char t[64];
                snprintf(t, sizeof t, "%s[%.1f, %.1f]", i ? ", " : "", c[i], c[i + 1]);
                buf_puts(&o, t);
            }
            buf_putc(&o, ']');
            free(c);
            if (!edit_build(frame, fn, "10,001-point outline", quad, buf_cstr(&o), &em, &es, &rc, &err)) {
                check_true("10,001-point outline: scene builds, 72 elements, no warning",
                           rc == 0 && es.nelements == 72 && es.nwarnings == 0);
                if (rc) printf("  %s\n", err.msg);
                double t0 = now_ms();
                cc_scene_free(&es);
                cc_scene_build(&es, &em, &err);
                printf("  scene build with a 10,001-point slab: %.3f ms\n", now_ms() - t0);
                cc_scene_free(&es);
                cc_model_free(&em);
            }
            buf_free(&o);
        }
    }

    earcut_checks();
    opening_checks();

    /* frame2: rotated wall, L slab, slab with a void, a plate with z_axis -y, a tetrahedron mesh */
    {
        size_t f2n;
        char *f2 = slurp(golden, "frame2.json", &f2n);
        cc_model m2;
        if (cc_model_load(&m2, f2, f2n, &err)) {
            printf("frame2.json does not load: %s\n", err.msg);
            return 1;
        }
        cc_scene s2;
        int rc2 = cc_scene_build(&s2, &m2, &err);
        check_true("frame2 scene builds", rc2 == 0);
        if (rc2) printf("  %s\n", err.msg);
        if (!rc2) {
            dump_scene(&b, &s2);
            check_file("frame2/mesh", "scene.txt", &b);
            int before = failed;
            for (size_t k = 0; k < s2.nelements; k++) {
                char name[32];
                snprintf(name, sizeof name, "%03zu.txt", k);
                dump_mesh(&b, &s2, k);
                check_file("frame2/mesh", name, &b);
            }
            check_true("frame2: 11 elements, all meshes byte for byte", s2.nelements == 11 && failed == before);
            printf("  frame2: %zu elements, %zu shared shapes\n", s2.nelements, s2.nshapes);
            cc_scene_free(&s2);
        }
        int reps = 100;
        double t0 = now_ms();
        for (int i = 0; i < reps; i++) {
            cc_scene_build(&s2, &m2, &err);
            cc_scene_free(&s2);
        }
        printf("  frame2 scene build: %.3f ms (mean of %d)\n", (now_ms() - t0) / reps, reps);
        cc_model_free(&m2);
        free(f2);
    }
    free(frame);
    buf_free(&b);
    return report("mesh");
}
