/* Suite ifc_in: STEP parsing and IFC import of the subset Bimble exports, against Rust.
 * - frame.ifc (golden/ifc, made by the golden dumper) imported, saved, ids masked: the bytes of
 *   golden/ifc/frame_reimport.json (bimble_ifc::import::import_str).
 * - the six synthetic fixtures of crates/bimble-ifc/tests/fixtures: parsed, dumped in the
 *   format of the compile-speed spike's step_dump and compared with its expected dumps (by
 *   length and a BLAKE3 fingerprint, the dumps themselves live on that branch); imported, the
 *   report printed (what is outside the subset is reported, not failed).
 * - ifc/import goldens: the 17 inputs of golden/ifc/import imported (seeded ids), saved, masked
 *   and compared byte for byte with Rust's model and report (chain18: its size summary); each
 *   imported twice to check the ids are stable; time per fixture printed. With the boolean kernel
 *   (manifold, CC_HAVE_MANIFOLD) all 17 must match, booleans included (ticket C6: was a known
 *   difference). Without it (cc_boolean_set(NULL), or a build without manifold) booleans must still
 *   differ (drawn uncut, counted as unsupported): a build with the kernel checks both paths.
 * - a STEP text over 1 GiB parses (as in Rust; it was refused).
 * - robustness: truncations and byte mutations of frame.ifc (run it under ASan/UBSan too).
 * - timing: parse + import of frame.ifc. */
#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE /* MAP_ANONYMOUS */
#include <stdint.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> /* WIN1: VirtualAlloc for the 1 GiB text (reserved and committed, zero pages) */
#else
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include "../src/boolean.h"
#include "../src/file.h"
#include "../src/guid.h"
#include "../src/ifc_in.h"
#include "../src/model.h"
#include "../src/step.h"
#include "check.h"

static const char *FIXTURES = "../crates/bimble-ifc/tests/fixtures";

static int is_id_char(char c) { return c != 0 && strchr(cc_gid_alphabet, c) != NULL; }

/* ---- masking (golden/README.md): new ids -> from_u128(N) by first appearance, reload and
 * save (canonical order), then from_u128(N) -> "<new-N>". */
typedef struct {
    cc_gid *v;
    size_t n;
} Known;

static int known_has(const Known *k, const char *s) {
    for (size_t i = 0; i < k->n; i++)
        if (!memcmp(k->v[i].c, s, 22)) return 1;
    return 0;
}

static void to_u128(const char *s, size_t n, const Known *k, Buf *out) {
    char seen[64][22];
    size_t ns = 0;
    buf_clear(out);
    for (size_t i = 0; i < n;) {
        if (!is_id_char(s[i])) { buf_putc(out, s[i++]); continue; }
        size_t j = i;
        while (j < n && is_id_char(s[j])) j++;
        int all0 = 1;
        for (size_t q = i; q < j; q++) all0 &= s[q] == '0';
        if (j - i == 22 && cc_gid_valid(s + i, 22) && !all0 && !known_has(k, s + i)) {
            size_t N;
            for (N = 0; N < ns && memcmp(seen[N], s + i, 22); N++) {}
            if (N == ns && ns < 63) memcpy(seen[ns++], s + i, 22);
            buf_fill(out, '0', 21);
            buf_putc(out, cc_gid_alphabet[N + 1]);
        } else buf_append(out, s + i, j - i);
        i = j;
    }
}

static void remask(const char *s, size_t n, Buf *out) {
    buf_clear(out);
    for (size_t i = 0; i < n;) {
        if (!is_id_char(s[i])) { buf_putc(out, s[i++]); continue; }
        size_t j = i;
        while (j < n && is_id_char(s[j])) j++;
        int masked = 0;
        if (j - i == 22) {
            int zeros = 1;
            for (size_t q = i; q < i + 21; q++) zeros &= s[q] == '0';
            const char *at = zeros ? memchr(cc_gid_alphabet, s[i + 21], 64) : NULL;
            if (at && at != cc_gid_alphabet) {
                buf_puts(out, "<new-");
                buf_u64(out, (uint64_t)(at - cc_gid_alphabet));
                buf_putc(out, '>');
                masked = 1;
            }
        }
        if (!masked) buf_append(out, s + i, j - i);
        i = j;
    }
}

/* ---- step_dump of the compile-speed spike */
static void dump_value(Buf *o, const cc_sv *v) {
    static const char HEX[] = "0123456789abcdef";
    switch (v->kind) {
    case CC_SV_NULL: buf_putc(o, '$'); break;
    case CC_SV_DERIVED: buf_putc(o, '*'); break;
    case CC_SV_INT: buf_i64(o, v->u.i); break;
    case CC_SV_REAL: {
        uint64_t bits;
        memcpy(&bits, &v->u.r, 8);
        buf_putc(o, 'r');
        for (int k = 60; k >= 0; k -= 4) buf_putc(o, HEX[(bits >> k) & 15]);
        break;
    }
    case CC_SV_STR:
        buf_putc(o, 's');
        for (size_t k = 0; k < v->u.s.n; k++) {
            unsigned char c = (unsigned char)v->u.s.p[k];
            buf_putc(o, HEX[c >> 4]);
            buf_putc(o, HEX[c & 15]);
        }
        break;
    case CC_SV_ENUM: buf_putc(o, '.'); buf_puts(o, v->u.s.p); buf_putc(o, '.'); break;
    case CC_SV_REF: buf_putc(o, '#'); buf_u64(o, v->u.ref); break;
    case CC_SV_LIST:
        buf_putc(o, '(');
        for (size_t k = 0; k < v->u.list.n; k++) {
            if (k) buf_putc(o, ',');
            dump_value(o, &v->u.list.items[k]);
        }
        buf_putc(o, ')');
        break;
    case CC_SV_TYPED:
        buf_puts(o, v->u.typed.name);
        buf_putc(o, '(');
        dump_value(o, v->u.typed.inner);
        buf_putc(o, ')');
        break;
    }
}

static void dump(const cc_step_file *f, Buf *o) {
    /* schemas sorted by bytes */
    size_t ord[64], ns = f->nschemas < 64 ? f->nschemas : 64;
    for (size_t k = 0; k < ns; k++) ord[k] = k;
    for (size_t k = 1; k < ns; k++)
        for (size_t j = k; j && strcmp(f->schemas[ord[j - 1]], f->schemas[ord[j]]) > 0; j--) {
            size_t t = ord[j];
            ord[j] = ord[j - 1], ord[j - 1] = t;
        }
    buf_puts(o, "SCHEMAS ");
    for (size_t k = 0; k < ns; k++) {
        if (k) buf_putc(o, ',');
        buf_puts(o, f->schemas[ord[k]]);
    }
    buf_puts(o, "\nSKIPPED ");
    buf_u64(o, f->skipped);
    buf_putc(o, '\n');
    for (size_t k = 0; k < f->n; k++) {
        const cc_step_entity *e = &f->ents[k];
        buf_putc(o, '#');
        buf_u64(o, e->id);
        buf_putc(o, '=');
        buf_puts(o, e->name);
        buf_putc(o, '(');
        for (size_t j = 0; j < e->nargs; j++) {
            if (j) buf_putc(o, ',');
            dump_value(o, &e->args[j]);
        }
        buf_puts(o, ");\n");
    }
}

static void fingerprint(const char *s, size_t n, char out[23]) {
    cc_guid g = cc_guid_derived(s, n);
    memcpy(out, g.c, 22);
    out[22] = 0;
}

/* Expected dumps of spikes/compile-speed/corpus/expected/<name>.dump (branch spike/compile-speed):
 * entity lines, bytes, and the dump's GlobalId::derived fingerprint. */
static const struct {
    const char *name;
    size_t entities, bytes;
    const char *fp;
} EXPECTED[] = {
#include "ifc_in_expected.inc"
};

/* ---- parser unit checks */
/* A text over 1 GiB parses (Rust's parser has no cap; read.rs refuses only over 64 GiB): the
 * IFC4 export of a large imported model is that big. Was refused ("file too large for the C
 * core"). The text is a small file then zero bytes after its ENDSEC, in an anonymous mapping:
 * untouched pages cost no memory. */
static void over_1gib(void) {
#if SIZE_MAX > 0xffffffffu
    const char *t = "ISO-10303-21;\nHEADER;FILE_SCHEMA(('IFC4'));ENDSEC;\nDATA;\n#1=IFCX(1);\nENDSEC;\n";
    size_t n = ((size_t)1 << 30) + 4096;
#ifdef _WIN32
    char *m = VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!m) { check_true("over 1 GiB: mmap", 0); return; }
#else
    char *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { check_true("over 1 GiB: mmap", 0); return; }
#endif
    memcpy(m, t, strlen(t));
    cc_step_file f;
    cc_error err;
    int rc = cc_step_parse(&f, m, n, &err);
    if (rc) printf("over 1 GiB: %s\n", err.msg);
    check_true("a text over 1 GiB parses", rc == 0 && f.n == 1);
    cc_step_free(&f);
#ifdef _WIN32
    VirtualFree(m, 0, MEM_RELEASE);
#else
    munmap(m, n);
#endif
#endif
}

static void parser_units(void) {
    const char *t = "ISO-10303-21;\nHEADER;FILE_SCHEMA(('IFC4'));ENDSEC;\nDATA;\n"
                    "#1=IFCX('it''s',\"0FF\",.t.,$,*,-12,+3,1.E-5,2.5e+2,-0.0,(1,(2,(#3))),IFCLABEL('a'),"
                    "IFCX2((1,2)),'\\X\\E9t\\X2\\00E9D83DDE00\\X0\\\\\\\\S\\a');\n"
                    "#2=BAD(1,;\n#2=IFCY();\n/* c */ #3 = ( A(1) B(2) );\n#4=IFCZ(1.0.0);\nENDSEC;\n";
    cc_step_file f;
    cc_error err;
    int rc = cc_step_parse(&f, t, strlen(t), &err);
    check_true("parse units", rc == 0);
    check_true("units: 4 entities", f.n == 3);
    check_true("units: 2 skipped", f.skipped == 2);
    Buf o;
    buf_init(&o);
    dump(&f, &o);
    const char *want =
        "SCHEMAS IFC4\nSKIPPED 2\n"
        "#1=IFCX(s69742773,s304646,.T.,$,*,-12,3,r3ee4f8b588e368f1,r406f400000000000,r8000000000000000,"
        "(1,(2,(#3))),IFCLABEL(s61),IFCX2((1,2)),sc3a974c3a9f09f98805cc3a1);\n"
        "#2=IFCY();\n#3=A(1);\n";
    check_eq("units dump", o.p, o.len, want, strlen(want));
    cc_step_free(&f);
    /* depth 64 nested lists parse, 70 do not (the statement is skipped) */
    Buf d;
    buf_init(&d);
    for (int depth = 62; depth <= 70; depth += 8) {
        buf_clear(&d);
        buf_puts(&d, "ISO-10303-21;\nDATA;\n#1=X(");
        for (int k = 0; k < depth; k++) buf_putc(&d, '(');
        for (int k = 0; k < depth; k++) buf_putc(&d, ')');
        buf_puts(&d, ");\nENDSEC;\n");
        rc = cc_step_parse(&f, d.p, d.len, &err);
        check_true(depth == 62 ? "depth 62 kept" : "depth 70 skipped",
                   rc == 0 && f.n == (depth == 62 ? 1u : 0u) && f.skipped == (depth == 62 ? 0u : 1u));
        cc_step_free(&f);
    }
    rc = cc_step_parse(&f, "hello", 5, &err);
    check_true("not STEP", rc != 0 && strstr(err.msg, "line 1: not a STEP file") == err.msg);
    cc_step_free(&f);
    rc = cc_step_parse(&f, "ISO-10303-21;\nHEADER;\n", 22, &err);
    check_true("no DATA", rc != 0 && !strcmp(err.msg, "line 1: no DATA section"));
    cc_step_free(&f);
    buf_free(&o);
    buf_free(&d);
    over_1gib();
}

static void sqrt_units(void) {
#if defined(__clang__) && (defined(__aarch64__) || defined(__x86_64__))
    uint64_t s = 0x9E3779B97F4A7C15ull;
    int bad = 0;
    for (int k = 0; k < 200000; k++) {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        uint64_t bits = s & 0x7FFFFFFFFFFFFFFFull;
        double x;
        memcpy(&x, &bits, 8);
        if (x != x || x - x != 0) continue;
        double a = cc_sqrt_soft(x), b;
#if defined(__aarch64__)
        __asm__("fsqrt %d0, %d1" : "=w"(b) : "w"(x));
#else
        __asm__("sqrtsd %1, %0" : "=x"(b) : "x"(x));
#endif
        bad += memcmp(&a, &b, 8) != 0;
    }
    check_true("soft sqrt == hardware sqrt (200k)", bad == 0);
#else
    check_true("soft sqrt of 2.25", cc_sqrt_soft(2.25) == 1.5);
#endif
}

static int import_mask(const char *text, size_t n, const Known *known, Buf *out, cc_ifc_report *rep, cc_error *err) {
    cc_model m;
    int rc = cc_ifc_import(&m, rep, text, n, err);
    Buf s, t;
    buf_init(&s);
    buf_init(&t);
    if (!rc) rc = cc_model_save(&m, &s);
    cc_model_free(&m);
    if (!rc) {
        to_u128(s.p, s.len, known, &t);
        cc_model m2;
        rc = cc_model_load(&m2, t.p, t.len, err);
        buf_clear(&s);
        if (!rc) rc = cc_model_save(&m2, &s);
        cc_model_free(&m2);
        if (!rc) remask(s.p, s.len, out);
    }
    buf_free(&s);
    buf_free(&t);
    return rc;
}

/* Known ids for ifc/import: every token of the source IFC text that is a valid GlobalId. */
static void known_from_text(const char *t, size_t n, Known *k) {
    k->n = 0;
    k->v = NULL;
    size_t cap = 0;
    for (size_t i = 0; i < n;) {
        if (!is_id_char(t[i])) { i++; continue; }
        size_t j = i;
        while (j < n && is_id_char(t[j])) j++;
        if (j - i == 22 && cc_gid_valid(t + i, 22)) {
            if (k->n == cap) {
                cap = cap ? cap * 2 : 64;
                k->v = realloc(k->v, cap * sizeof *k->v);
            }
            memcpy(k->v[k->n++].c, t + i, 22);
        }
        i = j;
    }
}

/* Report lines with ids masked by the model's numbering is not needed: no report line of
 * these goldens holds a new id. The C-only "unsupported: " lines are not Rust's. */
static void rust_lines(const Buf *l, Buf *out) {
    buf_clear(out);
    for (size_t i = 0; i < l->len;) {
        size_t j = i;
        while (j < l->len && l->p[j] != '\n') j++;
        if (!(j - i >= 13 && !memcmp(l->p + i, "unsupported: ", 13))) buf_append(out, l->p + i, j + 1 - i);
        i = j + 1;
    }
}

/* ---- C-own import goldens (ticket IFC3): golden/ifc_c/fixtures, the types past Rust's reader
 * (IFC2, IFC3), through the importer. golden/ifc_c/import/<name>.json (the model, ids masked)
 * and <name>.report.txt (every report line, the C-only "unsupported: " ones included) are the C
 * importer's own output (CCORE_WRITE_GOLDEN=1 writes them again); each model's volume is checked
 * against the hand value (the same numbers as the reader's, test/ifc_read_test.c). */

static double loop_area(const cc_loop *l) {
    double a = 0;
    for (size_t i = 0; i < l->n; i++) {
        const double *p = l->pts[i], *q = l->pts[(i + 1) % l->n];
        a += p[0] * q[1] - q[0] * p[1];
    }
    return (a < 0 ? -a : a) / 2;
}

/* Volume (mm^3) of an element's generator, openings not cut. */
static double gen_volume(const cc_generator *g) {
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING: return g->u.col.width * g->u.col.depth * g->u.col.height;
    case CC_GEN_RECT_BEAM: return g->u.beam.width * g->u.beam.height * g->u.beam.length;
    case CC_GEN_SLAB: return loop_area(&g->u.slab.outline) * g->u.slab.thickness;
    case CC_GEN_WALL: return g->u.wall.length * g->u.wall.thickness * g->u.wall.height;
    case CC_GEN_EXTRUDED_POLYGON: {
        double a = loop_area(&g->u.poly.outline);
        for (size_t k = 0; k < g->u.poly.nvoids; k++) a -= loop_area(&g->u.poly.voids[k]);
        return a * g->u.poly.height;
    }
    case CC_GEN_MESH: {
        double v = 0;
        for (size_t t = 0; t + 2 < g->u.mesh.nindices; t += 3) {
            const double *a = g->u.mesh.positions[g->u.mesh.indices[t]], *b = g->u.mesh.positions[g->u.mesh.indices[t + 1]],
                         *c = g->u.mesh.positions[g->u.mesh.indices[t + 2]];
            v += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
        }
        return v;
    }
    default: return 0;
    }
}

/* Expected by hand: elements, the first element's generator, the volume of all elements. */
static const struct {
    const char *name;
    size_t elements;
    const char *kind;
    double volume, rel_tol;
} OWN_IMPORT[] = {
    /* IfcColumn 300 x 300 x 3000 under an IfcProductRepresentation: parametric */
    {"product_rep", 1, "rect_column", 270e6, 1e-9},
    /* IfcMember 200 x 100 x 2000 + IfcBuildingElementProxy 1000^3, both under an
     * IfcProductRepresentation (skipped before IFC3): 4e7 + 1e9 */
    {"product_rep_member", 2, "extruded_polygon", 1.04e9, 1e-9},
    /* IfcMember whose Body is two extrusions, 200 x 200 x 1000 and 100 x 100 x 500 at x 1000:
     * 4e7 + 5e6 (before IFC3 the importer kept the first, 4e7, without a word): a mesh */
    {"multi_item", 1, "mesh", 4.5e7, 1e-9},
    /* The profiles: extruded polygons, the reader's hand areas x 1000 */
    {"lshape", 1, "extruded_polygon", 1.9e6, 1e-9},
    {"lshape_fillet", 1, "extruded_polygon", 1910730.1, 2e-3},
    {"tshape", 1, "extruded_polygon", 4.1e6, 1e-9},
    {"ushape", 1, "extruded_polygon", 3.328e6, 1e-9},
    {"cshape", 1, "extruded_polygon", 1.164e6, 1e-9},
    {"zshape", 1, "extruded_polygon", 3.424e6, 1e-9},
    {"asym_ishape", 1, "extruded_polygon", 9.9e6, 1e-9},
    {"trapezium", 1, "extruded_polygon", 25e6, 1e-9},
    {"ellipse", 1, "extruded_polygon", 62831853.1, 5e-3},
    {"rounded_rect", 1, "extruded_polygon", 77853981.6, 2e-3},
    /* The sloped profiles (ticket IFC3): the reader's hand areas x 1000 */
    {"ishape_slope", 1, "extruded_polygon", 13411040.8, 1e-6},
    {"ushape_slope", 1, "extruded_polygon", 3270207.2, 1e-6},
    {"ushape_slope_fillet", 1, "extruded_polygon", 3295600.4, 2e-3},
    {"tshape_slope", 1, "extruded_polygon", 3998768.5, 1e-6},
    {"lshape_slope", 1, "extruded_polygon", 1795060.3, 1e-6},
    /* Centre line, mirrored (ticket IFC3): extruded polygons (the closed centre line: a hole) */
    {"centreline", 1, "extruded_polygon", 3e7, 1e-9},
    {"centreline_closed", 1, "extruded_polygon", 8e7, 1e-9},
    {"mirrored", 1, "extruded_polygon", 2e7, 1e-9},
    /* A composite of two profiles is two regions: a mesh */
    {"composite", 1, "mesh", 4e7, 1e-9},
    /* The solids the importer cannot parametrise: meshes (the reader's triangles) */
    {"revolved", 1, "mesh", 14137166.9, 3e-3},
    {"revolved_full", 1, "mesh", 56548667.8, 3e-3},
    {"tapered", 1, "mesh", 23333333.3, 1e-6},
    {"csg_block", 1, "mesh", 1.24e8, 1e-6},
    {"csg_round", 1, "mesh", 38746309.4, 1.5e-2},
};

static void write_file(const char *path, const char *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f) fwrite(p, 1, n, f), fclose(f);
}

static void own_imports(void) {
    int write = getenv("CCORE_WRITE_GOLDEN") != NULL;
    if (write) mkdir("golden/ifc_c/import", 0777);
    for (size_t g = 0; g < sizeof OWN_IMPORT / sizeof *OWN_IMPORT; g++) {
        const char *nm = OWN_IMPORT[g].name;
        char file[160], m[256];
        size_t tn;
        snprintf(file, sizeof file, "%s.ifc", nm);
        char *t = slurp("golden/ifc_c/fixtures", file, &tn);
        Known kk;
        known_from_text(t, tn, &kk);
        Buf a, a2;
        buf_init(&a), buf_init(&a2);
        cc_ifc_report rp;
        cc_error err;
        cc_guid_seed(1);
        int rc = import_mask(t, tn, &kk, &a, &rp, &err);
        Buf lines;
        buf_init(&lines);
        buf_append(&lines, rp.lines.p ? rp.lines.p : "", rp.lines.len);
        size_t unsup = rp.unsupported;
        cc_ifc_report_free(&rp);
        cc_guid_seed(1);
        int rc2 = import_mask(t, tn, &kk, &a2, &rp, &err);
        cc_ifc_report_free(&rp);
        char jp[256], rpth[256];
        snprintf(jp, sizeof jp, "golden/ifc_c/import/%s.json", nm);
        snprintf(rpth, sizeof rpth, "golden/ifc_c/import/%s.report.txt", nm);
        if (write && !rc) write_file(jp, a.p, a.len), write_file(rpth, lines.p ? lines.p : "", lines.len);
        size_t jn = 0, rn = 0;
        snprintf(file, sizeof file, "%s.json", nm);
        FILE *probe = fopen(jp, "rb");
        char *wj = probe ? (fclose(probe), slurp("golden/ifc_c/import", file, &jn)) : NULL;
        snprintf(file, sizeof file, "%s.report.txt", nm);
        probe = fopen(rpth, "rb");
        char *wr = probe ? (fclose(probe), slurp("golden/ifc_c/import", file, &rn)) : NULL;
        int model_ok = !rc && wj && a.len == jn && !memcmp(a.p, wj, jn);
        int rep_ok = !rc && wr && lines.len == rn && (!rn || !memcmp(lines.p, wr, rn));
        /* The hand numbers, on the unmasked model. */
        cc_model cm;
        cc_ifc_report rq;
        cc_guid_seed(1);
        int rc3 = cc_ifc_import(&cm, &rq, t, tn, &err);
        double vol = 0;
        const char *kind = "?";
        size_t nel = rc3 ? 0 : cm.nelements;
        for (size_t k = 0; k < nel; k++) vol += gen_volume(&cm.elements[k].generator);
        if (nel) kind = cc_gen_kind_name(cm.elements[0].generator.kind);
        double rel = (vol - OWN_IMPORT[g].volume) / OWN_IMPORT[g].volume;
        printf("import ifc_c/%-18s %-8s elements %zu %s volume %.6g (hand %.6g, %+.2e), %zu unsupported%s%s\n", nm,
               model_ok && rep_ok ? "MATCHES" : "DIFFERS", nel, kind, vol, OWN_IMPORT[g].volume, rel, unsup,
               rc ? ": " : "", rc ? err.msg : "");
        snprintf(m, sizeof m, "import ifc_c/%s: model matches its C-own golden", nm);
        check_true(m, model_ok);
        snprintf(m, sizeof m, "import ifc_c/%s: report matches its C-own golden", nm);
        check_true(m, rep_ok);
        snprintf(m, sizeof m, "import ifc_c/%s: two runs identical", nm);
        check_true(m, rc == rc2 && a.len == a2.len && !memcmp(a.p, a2.p, a.len));
        snprintf(m, sizeof m, "import ifc_c/%s: %zu element(s), the first a %s", nm, OWN_IMPORT[g].elements,
                 OWN_IMPORT[g].kind);
        check_true(m, nel == OWN_IMPORT[g].elements && !strcmp(kind, OWN_IMPORT[g].kind));
        snprintf(m, sizeof m, "import ifc_c/%s: volume within %g of the hand value", nm, OWN_IMPORT[g].rel_tol);
        check_true(m, (rel < 0 ? -rel : rel) <= OWN_IMPORT[g].rel_tol);
        snprintf(m, sizeof m, "import ifc_c/%s: nothing skipped or unsupported", nm);
        check_true(m, !rc3 && unsup == 0 && rq.elements == nel && !strstr(buf_cstr(&lines), "skipped") &&
                          !strstr(buf_cstr(&lines), "can't read"));
        if (!rc3) cc_model_free(&cm);
        cc_ifc_report_free(&rq);
        free(wj), free(wr), free(t), free(kk.v);
        buf_free(&a), buf_free(&a2), buf_free(&lines);
    }
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char *golden = "golden";
    parser_units();
    sqrt_units();

    /* Known ids: those of frame.json. */
    size_t fn;
    char *fj = slurp(golden, "frame.json", &fn);
    cc_model fm;
    cc_error err;
    if (cc_model_load(&fm, fj, fn, &err)) { printf("frame.json: %s\n", err.msg); return 1; }
    Known known;
    known.n = 1 + fm.nstoreys + fm.nmaterials + fm.nelements;
    known.v = malloc(known.n * sizeof *known.v);
    size_t kn = 0;
    known.v[kn++] = fm.project_id;
    for (size_t i = 0; i < fm.nstoreys; i++) known.v[kn++] = fm.storeys[i].id;
    for (size_t i = 0; i < fm.nmaterials; i++) known.v[kn++] = fm.materials[i].id;
    for (size_t i = 0; i < fm.nelements; i++) known.v[kn++] = fm.elements[i].id;
    cc_model_free(&fm);
    free(fj);

    /* frame.ifc -> frame_reimport.json */
    char path[512];
    snprintf(path, sizeof path, "%s/ifc/frame.ifc", golden);
    FILE *probe = fopen(path, "rb");
    if (!probe) {
        printf("missing %s: it is not committed (*.ifc is ignored); make it with\n"
               "  cargo run -p bimble-api --example ccore_golden\n", path);
        return 1;
    }
    fclose(probe);
    size_t in, wn;
    char *ifc = slurp(golden, "ifc/frame.ifc", &in);
    char *want = slurp(golden, "ifc/frame_reimport.json", &wn);
    Buf got;
    buf_init(&got);
    cc_guid_seed(1);
    cc_ifc_report rep;
    int rc = import_mask(ifc, in, &known, &got, &rep, &err);
    if (rc) printf("frame.ifc: %s\n", err.msg);
    check_true("frame.ifc imports", rc == 0);
    check_eq("frame_reimport.json", got.p, got.len, want, wn);
    printf("frame.ifc report:\n%.*s", (int)rep.lines.len, rep.lines.p);
    check_true("frame.ifc: 72 elements, nothing unsupported", rep.elements == 72 && rep.unsupported == 0);
    cc_ifc_report_free(&rep);

    /* Fixtures: dump vs the spike's expected dumps; import and report. */
    static const char *fx[] = {"booleans", "chain18", "curves", "cyclic_map", "mirror", "tilt"};
    for (size_t k = 0; k < 6; k++) {
        char name[64];
        snprintf(name, sizeof name, "%s.ifc", fx[k]);
        size_t n;
        char *t = slurp(FIXTURES, name, &n);
        cc_step_file f;
        rc = cc_step_parse(&f, t, n, &err);
        check_true(name, rc == 0);
        Buf d;
        buf_init(&d);
        dump(&f, &d);
        char fp[23];
        fingerprint(d.p, d.len, fp);
        int ok = 0;
        for (size_t e = 0; e < sizeof EXPECTED / sizeof *EXPECTED; e++)
            if (!strcmp(EXPECTED[e].name, fx[k]))
                ok = EXPECTED[e].entities == f.n && EXPECTED[e].bytes == d.len && !strcmp(EXPECTED[e].fp, fp);
        printf("%s: %zu entities, %zu skipped, dump %zu bytes %s: %s\n", fx[k], f.n, f.skipped, d.len, fp,
               ok ? "matches the spike's expected dump" : "DIFFERS");
        check_true("dump matches the spike's expected dump", ok);
        buf_free(&d);
        cc_step_free(&f);
        cc_model m;
        rc = cc_ifc_import(&m, &rep, t, n, &err);
        printf("%s import: %s\n%.*s", fx[k], rc ? err.msg : "ok", (int)rep.lines.len, rep.lines.p);
        if (!rc) {
            Buf s;
            buf_init(&s);
            check_true("fixture model saves", cc_model_save(&m, &s) == 0);
            buf_free(&s);
        }
        cc_model_free(&m);
        cc_ifc_report_free(&rep);
        free(t);
    }

    /* ifc/import goldens (Rust's import_str), byte for byte; and stable over two runs. */
    {
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
            {"same_level", "golden/ifc/fixtures", "same_level.ifc"},
            {"frame2", "golden/ifc", "frame2.ifc"},
            {"frame2.ifc2x3", "golden/ifc", "frame2.ifc2x3.ifc"},
        };
        /* booleans: Rust cuts with the manifold3d kernel; without a kernel it is drawn uncut. */
        static const char *KNOWN_DIFFERENT[] = {"booleans"};
        const cc_boolean *kernel = cc_boolean_kernel();
        printf("ifc/import: boolean kernel %s\n", kernel ? "manifold" : "none (uncut)");
        for (int pass = 0; pass < (kernel ? 2 : 1); pass++) {
        if (pass) cc_boolean_set(NULL); /* the uncut path, in a build with the kernel */
        size_t matched = 0, total = 0;
        for (size_t g = 0; g < sizeof G / sizeof *G; g++) {
            int listed = 0;
            for (size_t k = 0; k < sizeof KNOWN_DIFFERENT / sizeof *KNOWN_DIFFERENT; k++)
                listed |= !strcmp(KNOWN_DIFFERENT[k], G[g].name);
            if (pass && !listed) continue;
            char wn_[128];
            size_t tn, rn;
            char *t = slurp(G[g].dir, G[g].file, &tn);
            Known kk;
            known_from_text(t, tn, &kk);
            Buf a, a2, lines;
            buf_init(&a);
            buf_init(&a2);
            buf_init(&lines);
            cc_ifc_report rp;
            cc_guid_seed(1);
            double t0 = now();
            int rc1 = import_mask(t, tn, &kk, &a, &rp, &err);
            double ms = (now() - t0) * 1e3;
            rust_lines(&rp.lines, &lines);
            size_t unsup = rp.unsupported;
            cc_ifc_report_free(&rp);
            cc_guid_seed(1);
            int rc2 = import_mask(t, tn, &kk, &a2, &rp, &err);
            cc_ifc_report_free(&rp);
            int known_diff = listed && !cc_boolean_kernel();
            snprintf(wn_, sizeof wn_, "ifc/import/%s.report.txt", G[g].name);
            char *wr = slurp(golden, wn_, &rn);
            int rep_ok = rc1 == 0 && lines.len == rn && !memcmp(lines.p, wr, rn);
            int model_ok;
            char detail[160] = "";
            if (!strcmp(G[g].name, "chain18")) {
                /* chain18.size.txt: bytes, elements, one line per element */
                Buf z;
                buf_init(&z);
                cc_model cm;
                cc_guid_seed(1);
                cc_ifc_report rq;
                int rc = cc_ifc_import(&cm, &rq, t, tn, &err);
                cc_ifc_report_free(&rq);
                buf_puts(&z, "bytes ");
                buf_u64(&z, a.len);
                buf_puts(&z, "\nelements ");
                buf_u64(&z, rc ? 0 : cm.nelements);
                buf_putc(&z, '\n');
                for (size_t k = 0; !rc && k < cm.nelements; k++) {
                    const cc_element *el = &cm.elements[k];
                    buf_append(&z, el->id.c, 22);
                    buf_putc(&z, ' ');
                    buf_puts(&z, el->has_ifc_entity ? "" : cc_ifc_class_name(el->cls));
                    if (el->has_ifc_entity) buf_append(&z, el->ifc_entity.p, el->ifc_entity.n);
                    buf_putc(&z, ' ');
                    buf_puts(&z, cc_gen_kind_name(el->generator.kind));
                    int mesh = el->generator.kind == CC_GEN_MESH;
                    buf_puts(&z, " v=");
                    buf_u64(&z, mesh ? el->generator.u.mesh.npositions : 0);
                    buf_puts(&z, " i=");
                    buf_u64(&z, mesh ? el->generator.u.mesh.nindices / 3 : 0);
                    buf_putc(&z, '\n');
                }
                cc_model_free(&cm);
                size_t sn;
                char *ws = slurp(golden, "ifc/import/chain18.size.txt", &sn);
                model_ok = rc1 == 0 && z.len == sn && !memcmp(z.p, ws, sn);
                if (!model_ok) snprintf(detail, sizeof detail, " (got %.*s)", (int)(z.len < 120 ? z.len : 120), z.p);
                free(ws);
                buf_free(&z);
            } else {
                size_t jn;
                snprintf(wn_, sizeof wn_, "ifc/import/%s.json", G[g].name);
                char *wj = slurp(golden, wn_, &jn);
                model_ok = rc1 == 0 && a.len == jn && !memcmp(a.p, wj, jn);
                if (!model_ok && rc1 == 0) {
                    size_t k = 0;
                    while (k < a.len && k < jn && a.p[k] == wj[k]) k++;
                    size_t line = 1;
                    for (size_t q = 0; q < k; q++) line += wj[q] == '\n';
                    snprintf(detail, sizeof detail, " (first difference at byte %zu, line %zu)", k, line);
                } else if (rc1) snprintf(detail, sizeof detail, " (%s)", err.msg);
                free(wj);
            }
            int stable = rc1 == rc2 && a.len == a2.len && !memcmp(a.p, a2.p, a.len);
            /* The imported model through the binary form (v2) and back: the same text. */
            if (strcmp(G[g].name, "chain18")) {
                cc_model bm, bm2;
                cc_ifc_report rq;
                cc_guid_seed(1);
                int rc = cc_ifc_import(&bm, &rq, t, tn, &err);
                cc_ifc_report_free(&rq);
                Buf bin, t1, t2;
                buf_init(&bin), buf_init(&t1), buf_init(&t2);
                int ok2 = !rc && !cc_model_save(&bm, &t1) && !cc_model_save_v2(&bm, &bin) &&
                          !cc_model_load(&bm2, bin.p, bin.len, &err);
                if (ok2) ok2 = !cc_model_save(&bm2, &t2) && t1.len == t2.len && !memcmp(t1.p, t2.p, t1.len), cc_model_free(&bm2);
                check_true("imported model: binary save and load give the same model", ok2);
                cc_model_free(&bm);
                buf_free(&bin), buf_free(&t1), buf_free(&t2);
            }
            int ok = model_ok && rep_ok;
            total++;
            matched += ok;
            printf("import %-14s %-9s model %s, report %s, %zu unsupported, two runs %s, %.3f ms%s\n", G[g].name,
                   ok ? "MATCHES" : known_diff ? "differs*" : "DIFFERS", model_ok ? "ok" : "differs",
                   rep_ok ? "ok" : "differs", unsup, stable ? "identical" : "DIFFER", ms, detail);
            if (!rep_ok && !known_diff) printf("%.*s---- want\n%.*s", (int)lines.len, lines.p, (int)rn, wr);
            check_true(G[g].name, known_diff ? !ok : ok);
            if (known_diff) check_true("no kernel: uncut boolean counted as unsupported", unsup > 0);
            check_true("import twice: identical", stable);
            free(wr);
            free(t);
            free(kk.v);
            buf_free(&a);
            buf_free(&a2);
            buf_free(&lines);
        }
        printf("ifc/import%s: %zu of %zu goldens match (* = known difference)\n", pass ? " (no kernel)" : "", matched,
               total);
        }
        cc_boolean_set(kernel);
    }

    own_imports();

    /* Derived ids, unmasked (masked in the goldens): import.rs derives material ids as
     * GlobalId::derived("material/<name>") and the synthetic storey as derived("storey/synthetic");
     * cc_guid_derived itself is checked against Rust in the guid suite (golden/guid.txt). */
    {
        size_t tn;
        char *t = slurp("golden/ifc/fixtures", "opening.ifc", &tn);
        cc_model dm;
        cc_ifc_report dr;
        int rc = cc_ifc_import(&dm, &dr, t, tn, &err);
        cc_guid want = cc_guid_derived("material/C30/37", 15);
        check_true("opening: material id = derived(\"material/C30/37\")",
                   !rc && dm.nmaterials == 1 && !memcmp(dm.materials[0].id.c, want.c, 22));
        printf("derived: material/C30/37 -> %.22s\n", rc ? "?" : dm.materials[0].id.c);
        cc_model_free(&dm);
        cc_ifc_report_free(&dr);
        free(t);
        t = slurp("../crates/bimble-ifc/tests/fixtures", "tilt.ifc", &tn);
        rc = cc_ifc_import(&dm, &dr, t, tn, &err);
        want = cc_guid_derived("storey/synthetic", 16);
        check_true("tilt: synthetic storey id = derived(\"storey/synthetic\")",
                   !rc && dm.nstoreys == 1 && !memcmp(dm.storeys[0].id.c, want.c, 22));
        cc_model_free(&dm);
        cc_ifc_report_free(&dr);
        free(t);
    }

    /* Robustness: truncations and byte mutations of frame.ifc. */
    {
        uint64_t s = 12345;
        size_t ok = 0, errs = 0;
        char *mut = malloc(in);
        for (int k = 0; k < 400; k++) {
            s ^= s << 13, s ^= s >> 7, s ^= s << 17;
            size_t len = in;
            memcpy(mut, ifc, in);
            if (k < 200) len = (size_t)(s % in);
            else {
                static const char pool[] = "#()$*,;'.=\\/0123456789EeX\n";
                for (int j = 0; j < 1 + (int)(s % 8); j++) {
                    s ^= s << 13, s ^= s >> 7, s ^= s << 17;
                    mut[s % in] = (s >> 40) & 1 ? pool[(s >> 20) % (sizeof pool - 1)] : (char)(s >> 32);
                }
            }
            cc_model m;
            rc = cc_ifc_import(&m, &rep, mut, len, &err);
            if (rc) errs++;
            else ok++;
            cc_model_free(&m);
            cc_ifc_report_free(&rep);
        }
        free(mut);
        printf("robustness: 200 truncations + 200 mutations: %zu models, %zu errors, no crash\n", ok, errs);
        check_true("robustness: 400 runs", ok + errs == 400);
    }

    /* Timing: parse + import of frame.ifc. */
    {
        double t0 = now();
        for (int k = 0; k < 100; k++) {
            cc_model m;
            cc_ifc_import(&m, &rep, ifc, in, &err);
            cc_model_free(&m);
            cc_ifc_report_free(&rep);
        }
        printf("timing: parse+import of frame.ifc (%zu bytes): %.3f ms mean of 100\n", in, (now() - t0) * 1e3 / 100);
    }
    buf_free(&got);
    free(ifc);
    free(want);
    free(known.v);
    return report("ifc_in");
}
