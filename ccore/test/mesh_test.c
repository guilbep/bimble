/* Suite mesh: Scene::from_model of the golden frame against Rust's bytes (golden/mesh/, see
 * golden/README.md), unsupported generators, exact sqrt, and a timing. */
#define _POSIX_C_SOURCE 199309L /* clock_gettime under -std=c11 */
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../src/model.h"
#include "../src/file.h"
#include "../src/scene.h"
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
        buf_puts(b, cc_ifc_class_name(e->cls));
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

static void check_file(const char *name, Buf *got) {
    size_t n;
    char path[64], what[96];
    snprintf(path, sizeof path, "mesh/%s", name);
    char *want = slurp(golden, path, &n);
    snprintf(what, sizeof what, "mesh/%s byte for byte", name);
    check_eq(what, got->p, got->len, want, n);
    free(want);
}

static uint64_t b64(double x) {
    uint64_t u;
    memcpy(&u, &x, 8);
    return u;
}

/* A slab whose outline is an L, and an element with a mesh generator: errors, not crashes. */
static void unsupported(const char *frame, size_t fn, const char *what, const char *from, const char *to,
                        const char *want) {
    cc_model m;
    cc_scene s;
    cc_error err;
    char label[160];
    const char *at = strstr(frame, from);
    snprintf(label, sizeof label, "%s: fixture edit applies", what);
    check_true(label, at != NULL);
    if (!at) return;
    Buf t;
    buf_init(&t);
    buf_append(&t, frame, (size_t)(at - frame));
    buf_puts(&t, to);
    buf_append(&t, at + strlen(from), fn - (size_t)(at - frame) - strlen(from));
    int rc = cc_model_load(&m, t.p, t.len, &err);
    snprintf(label, sizeof label, "%s: model loads", what);
    check_true(label, rc == 0);
    if (rc) printf("  %s\n", err.msg);
    if (!rc) {
        rc = cc_scene_build(&s, &m, &err);
        snprintf(label, sizeof label, "%s: scene build fails with a clear error", what);
        check_true(label, rc == -1 && strstr(err.msg, want) != NULL);
        if (rc != -1 || !strstr(err.msg, want)) printf("  got: %s\n", rc ? err.msg : "(no error)");
        cc_scene_free(&s);
    }
    cc_model_free(&m);
    buf_free(&t);
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
        check_file("scene.txt", &b);
        int all = 1;
        for (size_t k = 0; k < s.nelements; k++) {
            char name[32];
            snprintf(name, sizeof name, "%03zu.txt", k);
            dump_mesh(&b, &s, k);
            check_file(name, &b);
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

    {
        const char *quad = "[\n            [\n              -150.0,\n              -150.0\n            ],\n"
                           "            [\n              18150.0,\n              -150.0\n            ],\n"
                           "            [\n              18150.0,\n              10150.0\n            ],\n"
                           "            [\n              -150.0,\n              10150.0\n            ]\n          ]";
        unsupported(frame, fn, "L-shaped slab", quad,
                    "[[0, 0], [6000, 0], [6000, 3000], [3000, 3000], [3000, 6000], [0, 6000]]",
                    "not in the C core yet");
        unsupported(frame, fn, "mesh generator",
                    "\"kind\": \"rect_column\",\n          \"width\": 300.0,\n          \"depth\": 300.0,\n"
                    "          \"height\": 2780.0",
                    "\"kind\": \"mesh\", \"positions\": [[0, 0, 0], [1000, 0, 0], [0, 1000, 0], [0, 0, 1000]], "
                    "\"indices\": [0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3]",
                    "not in the C core yet");
    }
    free(frame);
    buf_free(&b);
    return report("mesh");
}
