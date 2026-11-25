/* Suite ifc_out: IFC export against Rust (bimble_ifc::export_as).
 * - frame.json -> IFC4, the version masked as golden/README.md says: the bytes of
 *   golden/ifc/frame.ifc; frame2.ifc, frame.ifc2x3.ifc and frame2.ifc2x3.ifc the same way.
 * - round trip: the C export of frame, imported by the C core (cc_ifc_import), saved and masked:
 *   golden/ifc/frame_reimport.json (Rust's own round trip).
 * - `real`, `string` (quotes, backslashes, accents, CJK, emoji, controls) and iso_utc as Rust.
 * - IFC2x3: what ifc2x3.rs's own test checks, and the C import reads it back.
 * - timing: export of frame (and of out/t16-large-20000.bimble when it exists). */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "../src/file.h"
#include "../src/guid.h"
#include "../src/ifc_in.h"
#include "../src/ifc_out.h"
#include "../src/model.h"
#include "check.h"

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

/* golden/README.md ifc/: "'bimble <C version>'" -> "'bimble <version>'"; in IFC2x3 also the
 * IfcApplication's version "'<C version>'" (the same masking, applied to every occurrence). */
static void mask_version(const char *s, size_t n, Buf *out) {
    static const char *from[] = {"'bimble " CC_IFC_VERSION "'", "'" CC_IFC_VERSION "'"};
    static const char *to[] = {"'bimble <version>'", "'<version>'"};
    buf_clear(out);
    for (size_t i = 0; i < n;) {
        int hit = 0;
        for (int k = 0; k < 2 && !hit; k++) {
            size_t fl = strlen(from[k]);
            if (n - i >= fl && !memcmp(s + i, from[k], fl)) buf_puts(out, to[k]), i += fl, hit = 1;
        }
        if (!hit) buf_putc(out, s[i++]);
    }
}

static int exists(const char *dir, const char *name) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void real_is(double x, const char *want) {
    Buf b;
    buf_init(&b);
    cc_step_real(&b, x);
    check_eq("real", b.p, b.len, want, strlen(want));
    buf_free(&b);
}

static void string_is(const char *s, const char *want) {
    Buf b;
    buf_init(&b);
    cc_step_string(&b, s, strlen(s));
    check_eq("string", b.p, b.len, want, strlen(want));
    buf_free(&b);
}

static void iso_is(int64_t secs, const char *want) {
    char t[32];
    cc_iso_utc(secs, t);
    check_eq("iso_utc", t, strlen(t), want, strlen(want));
}

static void units(void) {
    /* lib.rs tests `reals`, `strings` */
    real_is(3000.0, "3000.0");
    real_is(0.5, "0.5");
    real_is(-800.0, "-800.0");
    real_is(1e-5, "1.E-5");
    real_is(1.5e20, "1.5E20");
    real_is(33000.0 * 1e6, "33000000000.0");
    real_is(1e16, "1.E16");
    real_is(0.0001, "0.0001");
    string_is("Niveau 1", "'Niveau 1'");
    string_is("l'\xc3\xa9tage", "'l''\\X2\\00E9\\X0\\tage'");
    string_is("a\\b", "'a\\\\b'");
    /* Rust: is_ascii && !is_ascii_control stays, the rest goes in one \X2\ run as UTF-16. */
    string_is("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e/\xe5\x90\x8d\xe5\x89\x8d", "'\\X2\\65E5672C8A9E\\X0\\/\\X2\\540D524D\\X0\\'");
    string_is("ok \xf0\x9f\x91\x8d!", "'ok \\X2\\D83DDC4D\\X0\\!'");
    string_is("W2\trotated 30\xc2\xb0", "'W2\\X2\\0009\\X0\\rotated 30\\X2\\00B0\\X0\\'");
    string_is("a\nb\x7f\xe2\x80\x94", "'a\\X2\\000A\\X0\\b\\X2\\007F2014\\X0\\'");
    string_is("", "''");
    /* date.rs iso_utc */
    iso_is(0, "1970-01-01T00:00:00");
    iso_is(1767225600, "2026-01-01T00:00:00");
    iso_is(951782400 + 3661, "2000-02-29T01:01:01");
    iso_is(-1, "1969-12-31T23:59:59");
}

/* Exports `json` (a .bimble text) in `schema`; 0 ok. */
static int export_text(const char *json, size_t n, const char *name, cc_ifc_schema schema, Buf *out) {
    cc_model m;
    cc_error err;
    int rc = cc_model_load(&m, json, n, &err);
    if (rc) printf("load: %s\n", err.msg);
    else rc = cc_ifc_export(&m, name, "2026-01-01T00:00:00", schema, out);
    cc_model_free(&m);
    return rc;
}

/* `json` exported in `schema`, masked, equals golden/ifc/<golden_name> when that file exists. */
static void against_golden(const char *golden, const char *json_name, const char *file_name, cc_ifc_schema schema,
                           const char *golden_name, int required) {
    char what[256];
    snprintf(what, sizeof what, "%s -> ifc/%s", json_name, golden_name);
    char gpath[300];
    snprintf(gpath, sizeof gpath, "ifc/%s", golden_name);
    if (!exists(golden, gpath)) {
        printf("%s: golden not there yet%s\n", what, required ? " (required)" : ", not compared");
        check_true(what, !required);
        return;
    }
    size_t jn, wn;
    char *json = slurp(golden, json_name, &jn), *want = slurp(golden, gpath, &wn);
    Buf got, masked;
    buf_init(&got), buf_init(&masked);
    check_true(what, export_text(json, jn, file_name, schema, &got) == 0);
    mask_version(got.p, got.len, &masked);
    check_eq(what, masked.p, masked.len, want, wn);
    buf_free(&got), buf_free(&masked), free(json), free(want);
}

static int contains(const Buf *b, const char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= b->len; i++)
        if (!memcmp(b->p + i, s, n)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char *golden = "golden";
    units();

    /* IFC4 and IFC2x3 against Rust (golden/README.md, ifc/ exports). */
    against_golden(golden, "frame.json", "frame.ifc", CC_IFC4, "frame.ifc", 1);
    against_golden(golden, "frame2.json", "frame2.ifc", CC_IFC4, "frame2.ifc", 1);
    against_golden(golden, "frame.json", "frame.ifc2x3.ifc", CC_IFC2X3, "frame.ifc2x3.ifc", 1);
    against_golden(golden, "frame2.json", "frame2.ifc2x3.ifc", CC_IFC2X3, "frame2.ifc2x3.ifc", 1);

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

    /* Round trip: C export -> C import = Rust's round trip. */
    Buf ifc, again, got;
    buf_init(&ifc), buf_init(&again), buf_init(&got);
    check_true("export frame", cc_ifc_export(&fm, "frame.ifc", "2026-01-01T00:00:00", CC_IFC4, &ifc) == 0);
    check_true("export frame again", cc_ifc_export(&fm, "frame.ifc", "2026-01-01T00:00:00", CC_IFC4, &again) == 0);
    check_eq("deterministic", ifc.p, ifc.len, again.p, again.len);
    size_t wn;
    char *want = slurp(golden, "ifc/frame_reimport.json", &wn);
    cc_ifc_report rep;
    cc_guid_seed(1);
    int rc = import_mask(ifc.p, ifc.len, &known, &got, &rep, &err);
    if (rc) printf("reimport: %s\n", err.msg);
    check_true("C export imports", rc == 0);
    check_eq("C export -> C import = frame_reimport.json", got.p, got.len, want, wn);
    check_true("reimport: 72 elements, nothing unsupported", rep.elements == 72 && rep.unsupported == 0);
    cc_ifc_report_free(&rep);
    free(want);

    /* IFC2x3 (ifc2x3.rs test the_frame_written_down_to_ifc2x3). */
    Buf x3;
    buf_init(&x3);
    check_true("export frame IFC2x3", cc_ifc_export(&fm, "t.ifc", "2026-11-11T10:00:00", CC_IFC2X3, &x3) == 0);
    check_true("IFC2x3: schema", contains(&x3, "FILE_SCHEMA(('IFC2X3'));"));
    check_true("IFC2x3: view", contains(&x3, "'ViewDefinition [CoordinationView_V2.0]'"));
    check_true("IFC2x3: owner history", contains(&x3, "IFCOWNERHISTORY("));
    check_true("IFC2x3: no material properties", !contains(&x3, "IFCMATERIALPROPERTIES("));
    check_true("IFC2x3: no PredefinedType on columns", !contains(&x3, ",.COLUMN.)"));
    check_true("IFC2x3: rectangle profile placed", contains(&x3, "IFCRECTANGLEPROFILEDEF(.AREA.,$,#"));
    cc_model back;
    rc = cc_ifc_import(&back, &rep, x3.p, x3.len, &err);
    if (rc) printf("IFC2x3 import: %s\n", err.msg);
    check_true("IFC2x3 reads back, element by element", rc == 0 && back.nelements == fm.nelements);
    int same = rc == 0 && back.nelements == fm.nelements;
    for (size_t i = 0; same && i < fm.nelements; i++) same = !memcmp(back.elements[i].id.c, fm.elements[i].id.c, 22);
    check_true("IFC2x3: the same element ids", same);
    cc_model_free(&back);
    cc_ifc_report_free(&rep);

    /* frame2: every escape and generator kind; both schemas export and the C import reads them. */
    size_t f2n;
    char *f2 = slurp(golden, "frame2.json", &f2n);
    for (int s = 0; s < 2; s++) {
        Buf t;
        buf_init(&t);
        check_true("frame2 exports", export_text(f2, f2n, "frame2.ifc", s ? CC_IFC2X3 : CC_IFC4, &t) == 0);
        check_true("frame2: escaped quote and backslash", contains(&t, "\\\\") && contains(&t, "''"));
        check_true("frame2: faceted brep", contains(&t, "IFCFACETEDBREP("));
        check_true("frame2: profile with voids", contains(&t, "IFCARBITRARYPROFILEDEFWITHVOIDS("));
        check_true("frame2: UTF-16 run", contains(&t, "\\X2\\"));
        rc = cc_ifc_import(&back, &rep, t.p, t.len, &err);
        if (rc) printf("frame2 import: %s\n", err.msg);
        check_true("frame2 reads back", rc == 0);
        cc_model_free(&back);
        cc_ifc_report_free(&rep);
        buf_free(&t);
    }
    free(f2);

    /* Timing. */
    double t0 = now();
    int runs = 50;
    for (int r = 0; r < runs; r++) {
        buf_clear(&again);
        cc_ifc_export(&fm, "frame.ifc", "2026-01-01T00:00:00", CC_IFC4, &again);
    }
    printf("export frame (72 elements) IFC4: %.3f ms, %zu bytes\n", (now() - t0) * 1e3 / runs, again.len);
    t0 = now();
    for (int r = 0; r < runs; r++) {
        buf_clear(&again);
        cc_ifc_export(&fm, "frame.ifc", "2026-01-01T00:00:00", CC_IFC2X3, &again);
    }
    printf("export frame IFC2x3: %.3f ms, %zu bytes\n", (now() - t0) * 1e3 / runs, again.len);
    if (exists("out", "t16-large-20000.bimble")) {
        size_t ln;
        char *lj = slurp("out", "t16-large-20000.bimble", &ln);
        cc_model lm;
        if (cc_model_load(&lm, lj, ln, &err)) printf("t16-large-20000: %s\n", err.msg);
        else {
            for (int s = 0; s < 2; s++) {
                buf_clear(&again);
                t0 = now();
                rc = cc_ifc_export(&lm, "large.ifc", "2026-01-01T00:00:00", s ? CC_IFC2X3 : CC_IFC4, &again);
                printf("export t16-large-20000 (%zu elements) %s: %.1f ms, %zu bytes\n", lm.nelements,
                       s ? "IFC2x3" : "IFC4", (now() - t0) * 1e3, again.len);
                check_true("large export", rc == 0);
            }
        }
        cc_model_free(&lm);
        free(lj);
    }

    buf_free(&ifc), buf_free(&again), buf_free(&got), buf_free(&x3);
    cc_model_free(&fm);
    free(fj);
    free(known.v);
    return report("ifc_out");
}
