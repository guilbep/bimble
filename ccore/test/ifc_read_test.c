/* Suite ifc_read: the IFC display reader (src/ifc_read.h) against Rust's read_str.
 * - golden/ifc/read/<name>/: scene.txt and every <k>.txt byte for byte, for the 18 inputs (the
 *   six crate fixtures, the eight ifc/fixtures, frame.ifc, frame2.ifc and their IFC2x3 forms).
 *   With the boolean kernel (manifold, CC_HAVE_MANIFOLD) all 18 must match, booleans and opening
 *   included (ticket C6: was a known difference). Without it (cc_boolean_set(NULL), or a build
 *   without manifold) booleans and opening must still differ (drawn uncut) and say so in a
 *   warning: a build with the kernel checks both paths.
 * - each input read twice: the same text; the BMSH of each scene has the layout's size.
 * - robustness: truncations and byte mutations of frame.ifc, mapped_item.ifc and curves.ifc:
 *   a scene or an error, no crash (run it under ASan/UBSan too); not IFC, empty, not STEP.
 * - timing per input (mean of runs). */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "../src/boolean.h"
#include "../src/ifc_read.h"
#include "check.h"

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static const struct {
    const char *name, *dir, *file;
} G[] = {
    {"booleans", "../crates/bimble-ifc/tests/fixtures", "booleans.ifc"},
    {"chain18", "../crates/bimble-ifc/tests/fixtures", "chain18.ifc"},
    {"curves", "../crates/bimble-ifc/tests/fixtures", "curves.ifc"},
    {"cyclic_map", "../crates/bimble-ifc/tests/fixtures", "cyclic_map.ifc"},
    {"mirror", "../crates/bimble-ifc/tests/fixtures", "mirror.ifc"},
    {"tilt", "../crates/bimble-ifc/tests/fixtures", "tilt.ifc"},
    {"ishape_beam", "golden/ifc/fixtures", "ishape_beam.ifc"},
    {"circle_column", "golden/ifc/fixtures", "circle_column.ifc"},
    {"rect_hollow", "golden/ifc/fixtures", "rect_hollow.ifc"},
    {"triangulated", "golden/ifc/fixtures", "triangulated.ifc"},
    {"faceted_brep", "golden/ifc/fixtures", "faceted_brep.ifc"},
    {"mapped_item", "golden/ifc/fixtures", "mapped_item.ifc"},
    {"indexed_arc", "golden/ifc/fixtures", "indexed_arc.ifc"},
    {"opening", "golden/ifc/fixtures", "opening.ifc"},
    {"frame", "golden/ifc", "frame.ifc"},
    {"frame.ifc2x3", "golden/ifc", "frame.ifc2x3.ifc"},
    {"frame2", "golden/ifc", "frame2.ifc"},
    {"frame2.ifc2x3", "golden/ifc", "frame2.ifc2x3.ifc"},
};
/* Without a boolean kernel (Rust: manifold3d): booleans are drawn as their first operand,
 * openings are not cut. With the kernel they match. */
static const char *KNOWN_DIFFERENT[] = {"booleans", "opening"};

static int file_exists(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* Every file of the folder matches: 1, else 0 with the first difference in `why`. */
static int compare(const char *name, const cc_ifc_scene *s, int rc, const cc_error *err, char *why, size_t wn) {
    char dir[256];
    snprintf(dir, sizeof dir, "golden/ifc/read/%s", name);
    Buf b;
    buf_init(&b);
    if (rc) cc_ifc_error_txt(&b, err);
    else cc_ifc_scene_txt(&b, s);
    size_t n;
    char *want = slurp(dir, "scene.txt", &n);
    int ok = b.len == n && !memcmp(b.p, want, n);
    if (!ok) {
        size_t k = 0, line = 1;
        while (k < b.len && k < n && b.p[k] == want[k]) line += want[k++] == '\n';
        size_t e = k;
        while (e < b.len && b.p[e] != '\n' && e - k < 160) e++;
        snprintf(why, wn, "scene.txt line %zu: got \"%.*s\"", line, (int)(e - k), b.p + k);
    }
    free(want);
    size_t ne = rc ? 0 : s->scene.nelements;
    int meshes = !rc && s->triangles <= 20000;
    for (size_t k = 0; ok && meshes && k < ne; k++) {
        char f[32];
        snprintf(f, sizeof f, "%03zu.txt", k);
        if (!file_exists(dir, f)) {
            ok = 0;
            snprintf(why, wn, "%s: no golden file", f);
            break;
        }
        buf_clear(&b);
        cc_ifc_mesh_txt(&b, s, k);
        want = slurp(dir, f, &n);
        if (b.len != n || memcmp(b.p, want, n)) {
            ok = 0;
            size_t q = 0, line = 1;
            while (q < b.len && q < n && b.p[q] == want[q]) line += want[q++] == '\n';
            snprintf(why, wn, "%s line %zu differs", f, line);
        }
        free(want);
    }
    if (ok) {
        char f[32];
        snprintf(f, sizeof f, "%03zu.txt", meshes ? ne : 0);
        if (file_exists(dir, f)) ok = 0, snprintf(why, wn, "golden has %s, the C scene has no such element", f);
    }
    buf_free(&b);
    return ok;
}

static int bmsh_size_ok(const cc_ifc_scene *s) {
    Buf b;
    buf_init(&b);
    cc_error err;
    int rc = cc_ifc_scene_bmsh(s, 7, &b, &err);
    size_t ne = s->scene.nelements, nv = 0, ni = 0, ns = 0;
    for (size_t k = 0; k < ne; k++) {
        const cc_mesh *m = cc_scene_local_mesh(&s->scene, k);
        nv += m->nv, ni += m->ni, ns += m->ne;
    }
    int ok = rc == 0 && b.len == 52 + 48 * ne + 12 * nv + 4 * ni + 32 * ns && !memcmp(b.p, "BMSH", 4);
    buf_free(&b);
    return ok;
}

static void robustness(const char *dir, const char *file, uint64_t seed) {
    size_t in;
    char *t = slurp(dir, file, &in);
    char *mut = malloc(in + 1);
    uint64_t s = seed;
    size_t scenes = 0, errs = 0;
    for (int k = 0; k < 300; k++) {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        size_t len = in;
        memcpy(mut, t, in);
        if (k < 150) len = (size_t)(s % in);
        else {
            static const char pool[] = "#()$*,;'.=\\/0123456789EeX\n";
            for (int j = 0; j < 1 + (int)(s % 8); j++) {
                s ^= s << 13, s ^= s >> 7, s ^= s << 17;
                mut[s % in] = (s >> 40) & 1 ? pool[(s >> 20) % (sizeof pool - 1)] : (char)(s >> 32);
            }
        }
        cc_ifc_scene sc;
        cc_error err;
        int rc = cc_ifc_read(mut, len, &sc, &err);
        if (rc) errs++;
        else {
            scenes++;
            Buf b;
            buf_init(&b);
            cc_ifc_scene_txt(&b, &sc);
            for (size_t e = 0; e < sc.scene.nelements; e++) cc_ifc_mesh_txt(&b, &sc, e);
            buf_free(&b);
        }
        cc_ifc_scene_free(&sc);
    }
    printf("robustness %s: 150 truncations + 150 mutations: %zu scenes, %zu errors, no crash\n", file, scenes, errs);
    check_true("robustness: 300 runs", scenes + errs == 300);
    free(mut);
    free(t);
}

int main(void) {
    size_t matched = 0;
    printf("%-14s %-9s %8s %9s  %s\n", "input", "result", "elements", "ms", "note");
    for (size_t g = 0; g < sizeof G / sizeof *G; g++) {
        size_t tn;
        char *t = slurp(G[g].dir, G[g].file, &tn);
        cc_ifc_scene s, s2;
        cc_error err, err2;
        int rc = cc_ifc_read(t, tn, &s, &err);
        char why[256] = "";
        int ok = compare(G[g].name, &s, rc, &err, why, sizeof why);
        int known = 0;
        for (size_t k = 0; k < sizeof KNOWN_DIFFERENT / sizeof *KNOWN_DIFFERENT; k++)
            known |= !strcmp(KNOWN_DIFFERENT[k], G[g].name);
        known &= !cc_boolean_kernel();
        /* Twice: the same text. */
        int rc2 = cc_ifc_read(t, tn, &s2, &err2);
        Buf a, b;
        buf_init(&a), buf_init(&b);
        if (!rc) cc_ifc_scene_txt(&a, &s);
        if (!rc2) cc_ifc_scene_txt(&b, &s2);
        for (size_t k = 0; !rc && k < s.scene.nelements; k++) cc_ifc_mesh_txt(&a, &s, k);
        for (size_t k = 0; !rc2 && k < s2.scene.nelements; k++) cc_ifc_mesh_txt(&b, &s2, k);
        check_true("read twice: identical", rc == rc2 && a.len == b.len && !memcmp(a.p, b.p, a.len));
        buf_free(&a), buf_free(&b);
        cc_ifc_scene_free(&s2);
        if (!rc) check_true("BMSH size", bmsh_size_ok(&s));
        /* Timing: mean of runs (at least 5, about 200 ms). */
        int runs = 0;
        double t0 = now_ms(), el = 0;
        while (runs < 5 || (el < 200 && runs < 1000)) {
            cc_ifc_scene x;
            cc_error e;
            cc_ifc_read(t, tn, &x, &e);
            cc_ifc_scene_free(&x);
            runs++;
            el = now_ms() - t0;
        }
        printf("%-14s %-9s %8zu %9.3f  %s%s\n", G[g].name, ok ? "MATCHES" : known ? "differs*" : "DIFFERS",
               rc ? 0 : s.scene.nelements, el / runs, rc ? err.msg : "", ok ? "" : why);
        check_true(G[g].name, known ? !ok : ok);
        matched += ok;
        cc_ifc_scene_free(&s);
        free(t);
    }
    printf("ifc/read: %zu of %zu goldens match (* = known difference: no boolean kernel)\n", matched,
           sizeof G / sizeof *G);
    printf("ifc/read: boolean kernel %s\n", cc_boolean_kernel() ? "manifold" : "none (uncut)");

    /* The uncut path in a build with the kernel: turned off, booleans and opening differ again
     * and the scene says why. */
    if (cc_boolean_kernel()) {
        const cc_boolean *k = cc_boolean_kernel();
        cc_boolean_set(NULL);
        for (size_t g = 0; g < sizeof G / sizeof *G; g++) {
            int known = 0;
            for (size_t j = 0; j < sizeof KNOWN_DIFFERENT / sizeof *KNOWN_DIFFERENT; j++)
                known |= !strcmp(KNOWN_DIFFERENT[j], G[g].name);
            if (!known) continue;
            size_t tn;
            char *t = slurp(G[g].dir, G[g].file, &tn);
            cc_ifc_scene s;
            cc_error err;
            int rc = cc_ifc_read(t, tn, &s, &err);
            char why[256] = "";
            int ok = compare(G[g].name, &s, rc, &err, why, sizeof why);
            Buf a;
            buf_init(&a);
            if (!rc) cc_ifc_scene_txt(&a, &s);
            int said = !rc && strstr(buf_cstr(&a), "(no boolean kernel in the C core)") != NULL;
            printf("%-14s no kernel: %s, %s\n", G[g].name, ok ? "MATCHES" : "differs (uncut)",
                   said ? "warned" : "NOT WARNED");
            check_true("no kernel: differs", !ok);
            check_true("no kernel: warned", said);
            buf_free(&a);
            cc_ifc_scene_free(&s);
            free(t);
        }
        cc_boolean_set(k);
    }

    /* Errors. */
    {
        cc_ifc_scene s;
        cc_error err;
        const char *notifc = "ISO-10303-21;\nHEADER;FILE_SCHEMA(('CONFIG_CONTROL_DESIGN'));ENDSEC;\nDATA;\nENDSEC;\n";
        int rc = cc_ifc_read(notifc, strlen(notifc), &s, &err);
        check_true("not IFC", rc != 0 && !strcmp(err.msg, "not an IFC file (schema \"CONFIG_CONTROL_DESIGN\")"));
        cc_ifc_scene_free(&s);
        rc = cc_ifc_read("hello", 5, &s, &err);
        check_true("not STEP", rc != 0 && !strncmp(err.msg, "parse: line 1: ", 15));
        cc_ifc_scene_free(&s);
        rc = cc_ifc_read("", 0, &s, &err);
        check_true("empty: an error", rc != 0);
        cc_ifc_scene_free(&s);
        const char *empty = "ISO-10303-21;\nHEADER;FILE_SCHEMA(('IFC4'));ENDSEC;\nDATA;\nENDSEC;\n";
        rc = cc_ifc_read(empty, strlen(empty), &s, &err);
        check_true("IFC without products: an empty scene", rc == 0 && s.scene.nelements == 0);
        cc_ifc_scene_free(&s);
    }
    robustness("golden/ifc", "frame.ifc", 12345);
    robustness("golden/ifc/fixtures", "mapped_item.ifc", 777);
    robustness("../crates/bimble-ifc/tests/fixtures", "curves.ifc", 4242);
    return report("ifc_read");
}
