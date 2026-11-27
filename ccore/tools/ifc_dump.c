/* ifc_dump: the C side of `ccore_golden --dump-read` / `--dump-import` (crates/bimble-api/
 * examples/ccore_golden.rs), for comparing the C core with Rust on any IFC file (files that are
 * not goldens). Same formats, same file names, nothing masked.
 *
 *   ifc_dump read <in.ifc> <outdir> [--detail exact|standard|light]
 *       cc_ifc_read: scene.txt, meshes.txt (per element "<k> <bytes> <FNV-1a 64 hex>" of its
 *       <k>.txt text), <k>.txt when the scene has at most 20000 triangles (the golden rule),
 *       time.txt ("read_ms <ms>": the read alone), counts.txt (products with a body, entities
 *       parsed, elements drawn, triangles, "empty <ENTITY> <n>" per class of the products drawn
 *       empty: C only, for bench/corpus.sh). --detail: the reader's detail (ifc_read.h, ticket
 *       TESS1; default standard; exact is Rust's reader).
 *   ifc_dump import <in.ifc> <outdir> [<export.ifc>]
 *       cc_ifc_import: report.txt (the report's lines, the C-only "unsupported: " lines
 *       included, or "error <text>"), model.size.txt, model.json (at most 1 GiB), time.txt.
 *       With <export.ifc>: the model exported as IFC4 there, imported again, and export.txt
 *       ("export bytes <n> ms <ms>", then "reimport elements <n>" or "reimport error <text>").
 *
 * Both modes add "peak_rss_kb <VmHWM>" to time.txt. Build: build.sh native (out/native/ifc_dump). */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "../src/file.h"
#include "../src/guid.h"
#include "../src/ifc_in.h"
#include "../src/ifc_out.h"
#include "../src/ifc_read.h"

#define DUMP_MODEL_MAX_BYTES ((size_t)1 << 30)
#define READ_MESH_MAX_TRIANGLES 20000

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static uint64_t fnv64(const char *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)p[i]) * 0x100000001b3ull;
    return h;
}

static char *slurp_path(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); exit(2); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc((size_t)len + 1);
    if (!p || fread(p, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "%s: read failed\n", path); exit(2); }
    fclose(f);
    p[len] = 0;
    *n = (size_t)len;
    return p;
}

static void put(const char *dir, const char *name, const char *p, size_t n) {
    char path[4096];
    snprintf(path, sizeof path, "%s%s%s", dir, *dir ? "/" : "", name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n || fclose(f)) { fprintf(stderr, "%s: write failed\n", path); exit(2); }
}

static long peak_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long kb = -1;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "VmHWM:", 6)) kb = strtol(line + 6, NULL, 10);
    if (f) fclose(f);
    return kb;
}

static void put_time(const char *dir, const char *what, double ms) {
    char t[128];
    int n = snprintf(t, sizeof t, "%s %.1f\npeak_rss_kb %ld\n", what, ms, peak_rss_kb());
    put(dir, "time.txt", t, (size_t)n);
}

static int dump_read(const char *in, const char *out, int detail) {
    size_t n;
    char *text = slurp_path(in, &n);
    cc_ifc_scene s;
    cc_error err;
    double t0 = now_ms();
    cc_ifc_read_opts opts = {(cc_ifc_detail)detail};
    int rc = cc_ifc_read_with(text, n, &opts, &s, &err, NULL);
    double ms = now_ms() - t0;
    free(text);
    Buf b, m, lines;
    buf_init(&b);
    buf_init(&m);
    buf_init(&lines);
    if (rc) cc_ifc_error_txt(&b, &err);
    else cc_ifc_scene_txt(&b, &s);
    put(out, "scene.txt", b.p, b.len);
    if (!rc) {
        int meshes = s.triangles <= READ_MESH_MAX_TRIANGLES;
        for (size_t k = 0; k < s.scene.nelements; k++) {
            buf_clear(&m);
            cc_ifc_mesh_txt(&m, &s, k);
            char l[96];
            int ln = snprintf(l, sizeof l, "%03zu %zu %016llx\n", k, m.len, (unsigned long long)fnv64(m.p, m.len));
            buf_append(&lines, l, (size_t)ln);
            if (meshes) {
                char name[32];
                snprintf(name, sizeof name, "%03zu.txt", k);
                put(out, name, m.p, m.len);
            }
        }
        put(out, "meshes.txt", lines.p, lines.len);
    }
    put_time(out, "read_ms", ms);
    if (!rc) { /* counts for corpus reports (bench/corpus.sh); no Rust twin */
        char t[160];
        int tn = snprintf(t, sizeof t, "products %zu\nentities %zu\nelements %zu\ntriangles %zu\n", s.products,
                          s.entities, s.scene.nelements, s.triangles);
        Buf cb;
        buf_init(&cb);
        buf_append(&cb, t, (size_t)tn);
        for (size_t i = 0; i < s.empty_classes.n;) { /* "empty <ENTITY> <count>" lines */
            const char *nl = memchr(s.empty_classes.p + i, '\n', s.empty_classes.n - i);
            size_t e = nl ? (size_t)(nl - s.empty_classes.p) + 1 : s.empty_classes.n;
            buf_puts(&cb, "empty ");
            buf_append(&cb, s.empty_classes.p + i, e - i);
            i = e;
        }
        put(out, "counts.txt", cb.p, cb.len);
        buf_free(&cb);
    }
    buf_free(&b);
    buf_free(&m);
    buf_free(&lines);
    cc_ifc_scene_free(&s);
    return 0;
}

static void size_txt(Buf *o, const cc_model *m, const Buf *text) {
    char l[256];
    int ln = snprintf(l, sizeof l, "bytes %zu\nfnv %016llx\nelements %zu\n", text->len,
                      (unsigned long long)fnv64(text->p, text->len), m->nelements);
    buf_append(o, l, (size_t)ln);
    for (size_t i = 0; i < m->nelements; i++) {
        const cc_element *e = &m->elements[i];
        size_t v = 0, t = 0;
        if (e->generator.kind == CC_GEN_MESH) {
            v = e->generator.u.mesh.npositions;
            t = e->generator.u.mesh.nindices / 3;
        }
        /* Element::class_name: the IFC entity when kept, else the class. */
        buf_append(o, e->id.c, 22);
        buf_putc(o, ' ');
        if (e->has_ifc_entity) buf_append(o, e->ifc_entity.p, e->ifc_entity.n);
        else buf_puts(o, cc_ifc_class_name(e->cls));
        ln = snprintf(l, sizeof l, " %s v=%zu i=%zu\n", cc_gen_kind_name(e->generator.kind), v, t);
        buf_append(o, l, (size_t)ln);
    }
}

static int dump_import(const char *in, const char *out, const char *export_path) {
    size_t n;
    char *text = slurp_path(in, &n);
    cc_model m;
    cc_ifc_report rep;
    cc_error err;
    double t0 = now_ms();
    int rc = cc_ifc_import(&m, &rep, text, n, &err);
    double ms = now_ms() - t0;
    free(text);
    Buf b, s;
    buf_init(&b);
    buf_init(&s);
    if (rc) {
        buf_puts(&b, "error ");
        buf_puts(&b, err.msg);
        buf_putc(&b, '\n');
        put(out, "report.txt", b.p, b.len);
    } else {
        put(out, "report.txt", rep.lines.p, rep.lines.len);
        if (cc_model_save(&m, &s)) { fprintf(stderr, "save failed\n"); return 1; }
        size_txt(&b, &m, &s);
        put(out, "model.size.txt", b.p, b.len);
        if (s.len <= DUMP_MODEL_MAX_BYTES) put(out, "model.json", s.p, s.len);
    }
    put_time(out, "import_ms", ms);
    if (!rc && export_path) {
        Buf x, e;
        buf_init(&x);
        buf_init(&e);
        double t1 = now_ms();
        int xr = cc_ifc_export(&m, "export.ifc", "2026-01-01T00:00:00", CC_IFC4, &x);
        double xms = now_ms() - t1;
        char l[640];
        int ln = snprintf(l, sizeof l, "export %s bytes %zu ms %.1f\n", xr ? "failed" : "ok", x.len, xms);
        buf_append(&e, l, (size_t)ln);
        if (!xr) {
            put("", export_path, x.p, x.len);
            cc_model m2;
            cc_ifc_report r2;
            int ir = cc_ifc_import(&m2, &r2, x.p, x.len, &err);
            if (ir) ln = snprintf(l, sizeof l, "reimport error %s\n", err.msg);
            else ln = snprintf(l, sizeof l, "reimport elements %zu of %zu\n", m2.nelements, m.nelements);
            buf_append(&e, l, (size_t)ln);
            cc_model_free(&m2);
            cc_ifc_report_free(&r2);
        }
        put(out, "export.txt", e.p, e.len);
        buf_free(&x);
        buf_free(&e);
    }
    buf_free(&b);
    buf_free(&s);
    cc_model_free(&m);
    cc_ifc_report_free(&rep);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && (!strcmp(argv[1], "read") || !strcmp(argv[1], "import"))) {
#ifdef _WIN32
#define mkdir(path, mode) mkdir(path) /* WIN1: no mode argument there */
#endif
        if (mkdir(argv[3], 0755) && errno != EEXIST) { fprintf(stderr, "%s: %s\n", argv[3], strerror(errno)); return 2; }
        if (!strcmp(argv[1], "read") && argc == 4) return dump_read(argv[2], argv[3], CC_IFC_DETAIL_STANDARD);
        if (!strcmp(argv[1], "read") && argc == 6 && !strcmp(argv[4], "--detail")) { /* TESS1 */
            const char *d = argv[5];
            int detail = !strcmp(d, "exact") ? CC_IFC_DETAIL_EXACT : !strcmp(d, "standard") ? CC_IFC_DETAIL_STANDARD
                         : !strcmp(d, "light") ? CC_IFC_DETAIL_LIGHT : -1;
            if (detail >= 0) return dump_read(argv[2], argv[3], detail);
        }
        if (!strcmp(argv[1], "import") && argc <= 5) return dump_import(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
    }
    fprintf(stderr, "usage: ifc_dump read <in.ifc> <outdir> [--detail exact|standard|light] | import <in.ifc> <outdir> [<export.ifc>]\n");
    return 2;
}
