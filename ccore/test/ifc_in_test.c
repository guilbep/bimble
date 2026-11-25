/* Suite ifc_in: STEP parsing and IFC import of the subset Bimble exports, against Rust.
 * - frame.ifc (golden/ifc, made by the golden dumper) imported, saved, ids masked: the bytes of
 *   golden/ifc/frame_reimport.json (bimble_ifc::import::import_str).
 * - the six synthetic fixtures of crates/bimble-ifc/tests/fixtures: parsed, dumped in the
 *   format of the compile-speed spike's step_dump and compared with its expected dumps (by
 *   length and a BLAKE3 fingerprint, the dumps themselves live on that branch); imported, the
 *   report printed (what is outside the subset is reported, not failed).
 * - robustness: truncations and byte mutations of frame.ifc (run it under ASan/UBSan too).
 * - timing: parse + import of frame.ifc. */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <string.h>
#include <time.h>
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
