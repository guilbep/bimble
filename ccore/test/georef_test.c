/* Suite georef (ADR 0045, src/georef.h): the georeference against Rust, golden/geo/ (made by
 * crates/bimble-api/examples/ccore_golden.rs from the frame, three synthetic models: `geo` an
 * IFC4 map conversion to LV95 and a turn, `turned` a turn only, `survey` drawn in LV95):
 * - each model's text (v1) and CBOR (v2) byte for byte, Rust's .bimble loads to the same text;
 * - IFC4 and IFC2x3 exports byte for byte (version masked), Rust's import of each (model masked
 *   as ifc_in_test, report lines) and the display reader's scene.txt;
 * - the .bimbleview raw stream (GEOR from the model);
 * - the coordinate table: survey coordinates bit for bit, the status bar's texts byte for byte,
 *   WGS84 within 2e-9 degrees of Rust's proj4rs, swisstopo's formulas to 9 decimals;
 * - units: Georef::from_frame, the file form's errors and defaults, set_georef with undo, the
 *   verb georef_set, the queries north and coordinates. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/command.h"
#include "../src/file.h"
#include "../src/georef.h"
#include "../src/guid.h"
#include "../src/history.h"
#include "../src/ifc_in.h"
#include "../src/ifc_out.h"
#include "../src/ifc_read.h"
#include "../src/model.h"
#include "../src/quantity.h"
#include "../src/query.h"
#include "../src/scene.h"
#include "../src/verbs.h"
#include "../src/viewfile.h"
#include "check.h"

static const char *golden = "golden";

static double fabs_d(double x) { return x < 0 ? -x : x; }

static int is_id_char(char c) { return c != 0 && strchr(cc_gid_alphabet, c) != NULL; }

/* ---- masking, as ifc_in_test.c (golden/README.md) ---- */
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

static void rust_lines(const Buf *l, Buf *out) {
    buf_clear(out);
    for (size_t i = 0; i < l->len;) {
        size_t j = i;
        while (j < l->len && l->p[j] != '\n') j++;
        if (!(j - i >= 13 && !memcmp(l->p + i, "unsupported: ", 13))) buf_append(out, l->p + i, j + 1 - i);
        i = j + 1;
    }
}

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

static void check_buf(const char *what, const Buf *got, const char *want, size_t wn) {
    if (got->len == wn && !memcmp(got->p, want, wn)) {
        passed++;
        return;
    }
    size_t k = 0;
    while (k < got->len && k < wn && got->p[k] == want[k]) k++;
    size_t line = 1;
    for (size_t q = 0; q < k; q++) line += want[q] == '\n';
    char msg[512];
    snprintf(msg, sizeof msg, "%s (first difference at byte %zu, line %zu: got %.60s)", what, k, line,
             k < got->len ? got->p + k : "<end>");
    check_true(msg, 0);
}

static void hex64(Buf *b, double x) {
    uint64_t u;
    memcpy(&u, &x, 8);
    char t[20];
    snprintf(t, sizeof t, "%016llx", (unsigned long long)u);
    buf_puts(b, t);
}

/* ---- per model ---- */

static void import_check(const char *name, const char *ifc, size_t in) {
    char path[256], what[300];
    Known known;
    known_from_text(ifc, in, &known);
    cc_model m;
    cc_ifc_report rep;
    cc_error err;
    cc_guid_seed(1);
    int rc = cc_ifc_import(&m, &rep, ifc, in, &err);
    snprintf(what, sizeof what, "%s: imports (%s)", name, rc ? err.msg : "ok");
    check_true(what, rc == 0);
    Buf s, t, masked, lines;
    buf_init(&s), buf_init(&t), buf_init(&masked), buf_init(&lines);
    if (!rc) {
        cc_model_save(&m, &s);
        to_u128(s.p, s.len, &known, &t);
        cc_model m2;
        if (!cc_model_load(&m2, t.p, t.len, &err)) {
            buf_clear(&s);
            cc_model_save(&m2, &s);
            cc_model_free(&m2);
            remask(s.p, s.len, &masked);
        }
        size_t wn;
        snprintf(path, sizeof path, "geo/import/%s.json", name);
        char *want = slurp(golden, path, &wn);
        snprintf(what, sizeof what, "%s: import == Rust's %s", name, path);
        check_buf(what, &masked, want, wn);
        free(want);
        rust_lines(&rep.lines, &lines);
        snprintf(path, sizeof path, "geo/import/%s.report.txt", name);
        want = slurp(golden, path, &wn);
        snprintf(what, sizeof what, "%s: report == Rust's %s", name, path);
        check_buf(what, &lines, want, wn);
        free(want);
        cc_model_free(&m);
    }
    cc_ifc_report_free(&rep);
    buf_free(&s), buf_free(&t), buf_free(&masked), buf_free(&lines);
    free(known.v);

    cc_ifc_scene sc;
    rc = cc_ifc_read(ifc, in, &sc, &err);
    snprintf(what, sizeof what, "%s: display read (%s)", name, rc ? err.msg : "ok");
    check_true(what, rc == 0);
    if (!rc) {
        Buf b;
        buf_init(&b);
        cc_ifc_scene_txt(&b, &sc);
        size_t wn;
        snprintf(path, sizeof path, "geo/read/%s/scene.txt", name);
        char *want = slurp(golden, path, &wn);
        snprintf(what, sizeof what, "%s: scene.txt == Rust's %s", name, path);
        check_buf(what, &b, want, wn);
        free(want);
        buf_free(&b);
    }
    cc_ifc_scene_free(&sc);
}

/* golden/geo/<name>.coords.txt, line by line. */
static void coords_check(const char *name, const cc_model *m) {
    char path[256], what[400];
    snprintf(path, sizeof path, "geo/%s.coords.txt", name);
    size_t n;
    char *t = slurp(golden, path, &n);
    const cc_georef *g = &m->georef;
    double p[3] = {0, 0, 0};
    int points = 0, ok_texts = 0;
    double worst = 0.0;
    for (char *line = t; line < t + n;) {
        char *end = memchr(line, '\n', (size_t)(t + n - line));
        if (!end) end = t + n;
        *end = 0;
        Buf b;
        buf_init(&b);
        if (!strncmp(line, "georef ", 7)) {
            buf_puts(&b, "georef ");
            cc_georef_json(&b, g);
        } else if (!strncmp(line, "north ", 6)) {
            buf_puts(&b, "north rotation_deg "), hex64(&b, g->rotation_deg);
            buf_puts(&b, " map_rotation_deg "), hex64(&b, g->map_rotation_deg);
            buf_puts(&b, " plan_turn "), hex64(&b, cc_georef_plan_turn(g, 0));
            buf_putc(&b, ' '), hex64(&b, cc_georef_plan_turn(g, 1));
            buf_puts(&b, cc_georef_has_map(g) ? " has_map true" : " has_map false");
        } else if (!strncmp(line, "survey_plan_to_world ", 21)) {
            double deg, off[2];
            cc_georef_survey_plan_to_world(g, &deg, off);
            buf_puts(&b, "survey_plan_to_world "), hex64(&b, deg);
            buf_putc(&b, ' '), hex64(&b, off[0]), buf_putc(&b, ' '), hex64(&b, off[1]);
        } else if (!strncmp(line, "point ", 6)) {
            p[0] = strtod(line + 6, NULL);
            char *q = strchr(line + 6, ' ');
            p[1] = strtod(q + 1, NULL);
            q = strchr(q + 1, ' ');
            p[2] = strtod(q + 1, NULL);
            points++;
            buf_puts(&b, line); /* the input */
        } else if (!strncmp(line, "survey ", 7)) {
            double s[3];
            cc_georef_survey(g, p, s);
            buf_puts(&b, "survey "), hex64(&b, s[0]), buf_putc(&b, ' '), hex64(&b, s[1]), buf_putc(&b, ' '), hex64(&b, s[2]);
        } else if (!strncmp(line, "text ", 5)) {
            buf_puts(&b, "text ");
            cc_georef_text(&b, g, p, CC_COORDS_SURVEY, 0);
            ok_texts += strlen(line) == b.len && !memcmp(line, b.p, b.len);
        } else if (!strncmp(line, "text_wgs84 ", 11)) {
            buf_puts(&b, "text_wgs84 ");
            cc_georef_text(&b, g, p, CC_COORDS_WGS84, 0);
            ok_texts += strlen(line) == b.len && !memcmp(line, b.p, b.len);
        } else if (!strncmp(line, "wgs84 ", 6)) {
            double s[3], lat, lon;
            cc_georef_survey(g, p, s);
            int is95 = g->has_crs && g->crs.n == 9 && !memcmp(g->crs.p, CC_LV95, 9);
            if (!strcmp(line, "wgs84 none")) {
                if (!is95) buf_puts(&b, line);
            } else if (is95 && !cc_lv95_to_wgs84(s[0], s[1], &lat, &lon)) {
                /* Tolerance: Rust's proj4rs on glibc's libm, the C on its own (georef.c); 2e-9
                 * degrees is 0.2 mm, the status bar prints 1e-6. */
                double wl = strtod(line + 6, NULL), wo = strtod(strchr(line + 6, ' ') + 1, NULL);
                double d = fabs_d(lat - wl) > fabs_d(lon - wo) ? fabs_d(lat - wl) : fabs_d(lon - wo);
                if (d > worst) worst = d;
                if (d <= 2e-9) buf_puts(&b, line);
                else {
                    char x[96];
                    snprintf(x, sizeof x, "wgs84 %.9f %.9f", lat, lon);
                    buf_puts(&b, x);
                }
            }
        } else if (!strncmp(line, "approx ", 7)) {
            double ll[2];
            if (cc_georef_wgs84_approx(g, p, ll)) buf_puts(&b, "approx none");
            else {
                Buf x;
                buf_init(&x);
                buf_puts(&b, "approx ");
                cc_fmt_fixed(&b, ll[0], 9);
                buf_putc(&b, ' ');
                cc_fmt_fixed(&b, ll[1], 9);
                buf_free(&x);
            }
        } else buf_puts(&b, "?");
        snprintf(what, sizeof what, "%s: %s", path, line);
        check_eq(what, b.p ? b.p : "", b.len, line, strlen(line));
        buf_free(&b);
        line = end + 1;
    }
    snprintf(what, sizeof what, "%s: 12 points", path);
    check_true(what, points == 12);
    printf("  %s: %d points, %d texts identical, WGS84 within %.1e degrees of Rust's\n", name, points, ok_texts, worst);
    free(t);
}

static void model_check(const char *name) {
    char path[256], what[300];
    size_t tn, bn, cn;
    snprintf(path, sizeof path, "geo/%s.json", name);
    char *text = slurp(golden, path, &tn);
    cc_model m;
    cc_error err;
    int rc = cc_model_load(&m, text, tn, &err);
    snprintf(what, sizeof what, "%s loads (%s)", path, rc ? err.msg : "ok");
    check_true(what, rc == 0);
    if (rc) {
        free(text);
        return;
    }
    check_true("the georeference is held", !cc_georef_is_default(&m.georef));
    Buf b;
    buf_init(&b);
    cc_model_save(&m, &b);
    snprintf(what, sizeof what, "%s: save == Rust's text", path);
    check_buf(what, &b, text, tn);
    buf_clear(&b);
    cc_model_to_cbor(&m, &b);
    snprintf(path, sizeof path, "geo/%s.cbor", name);
    char *cbor = slurp(golden, path, &cn);
    snprintf(what, sizeof what, "%s: C CBOR == Rust's %s", name, path);
    check_buf(what, &b, cbor, cn);
    snprintf(path, sizeof path, "geo/%s.bimble", name);
    char *bin = slurp(golden, path, &bn);
    cc_model mb;
    rc = cc_model_load(&mb, bin, bn, &err);
    snprintf(what, sizeof what, "%s loads (%s) and saves Rust's text", path, rc ? err.msg : "ok");
    buf_clear(&b);
    if (!rc) cc_model_save(&mb, &b), cc_model_free(&mb);
    check_true(what, rc == 0 && b.len == tn && !memcmp(b.p, text, tn));
    /* C binary save, loaded back: the same model */
    buf_clear(&b);
    cc_model_save_v2(&m, &b);
    rc = cc_model_load(&mb, b.p, b.len, &err);
    snprintf(what, sizeof what, "%s: C v2 save loads back to the same georeference", name);
    check_true(what, rc == 0 && cc_georef_eq(&mb.georef, &m.georef));
    if (!rc) cc_model_free(&mb);

    /* IFC exports */
    static const char *const sfx[2] = {"", ".ifc2x3"};
    for (int k = 0; k < 2; k++) {
        char fname[128];
        snprintf(fname, sizeof fname, "%s%s.ifc", name, sfx[k]);
        Buf ifc, masked;
        buf_init(&ifc), buf_init(&masked);
        rc = cc_ifc_export(&m, fname, "2026-01-01T00:00:00", k ? CC_IFC2X3 : CC_IFC4, &ifc);
        mask_version(ifc.p, ifc.len, &masked);
        size_t wn;
        snprintf(path, sizeof path, "geo/%s", fname);
        char *want = slurp(golden, path, &wn);
        snprintf(what, sizeof what, "%s: export == Rust's %s", name, path);
        check_true(what, rc == 0);
        check_buf(what, &masked, want, wn);
        char iname[128];
        snprintf(iname, sizeof iname, "%s%s", name, sfx[k]);
        import_check(iname, want, wn);
        free(want);
        buf_free(&ifc), buf_free(&masked);
    }

    /* .bimbleview (GEOR from the model) */
    cc_scene sc;
    if (cc_scene_build(&sc, &m, &err) == 0) {
        buf_clear(&b);
        rc = cc_viewfile_write(&m, &sc, 0, &b);
        size_t wn;
        snprintf(path, sizeof path, "geo/%s.bimbleview.raw", name);
        char *want = slurp(golden, path, &wn);
        snprintf(what, sizeof what, "%s: .bimbleview raw == Rust's %s", name, path);
        check_true(what, rc == 0);
        check_buf(what, &b, want, wn);
        free(want);
        cc_scene_free(&sc);
    } else check_true("scene builds", 0);

    coords_check(name, &m);
    buf_free(&b);
    cc_model_free(&m);
    free(text), free(cbor), free(bin);
}

/* ---- units ---- */

static void load_err(const char *what, const char *text, const char *want) {
    cc_model m;
    cc_error err;
    int rc = cc_model_load(&m, text, strlen(text), &err);
    char msg[700];
    snprintf(msg, sizeof msg, "%s: refused with \"%s\" (got %s)", what, want, rc ? err.msg : "loaded");
    check_true(msg, rc != 0 && strstr(err.msg, want) != NULL);
    if (!rc) cc_model_free(&m);
}

static void file_units(void) {
    size_t n;
    char *frame = slurp(golden, "frame.json", &n);
    /* "georef": {} is the default: loads, saves without the key (existing files keep their bytes). */
    const char *at = strstr(frame, "\n  }\n}");
    check_true("frame.json ends with the model's last key", at != NULL);
    Buf t;
    buf_init(&t);
    char probe[600];
    #define WITH(geo) (buf_clear(&t), buf_append(&t, frame, (size_t)(at - frame)), buf_puts(&t, ",\n    \"georef\": " geo), buf_puts(&t, at), buf_putc(&t, 0), t.len--, t.p)
    cc_model m;
    cc_error err;
    WITH("{}");
    int rc = cc_model_load(&m, t.p, t.len, &err);
    Buf s;
    buf_init(&s);
    if (!rc) cc_model_save(&m, &s), cc_model_free(&m);
    check_true("\"georef\": {} loads and saves as frame.json (left out when the default)",
               rc == 0 && s.len == n && !memcmp(s.p, frame, n));
    WITH("{\"rotation_deg\": -0.0, \"crs\": null, \"east_m\": 0}");
    rc = cc_model_load(&m, t.p, t.len, &err);
    buf_clear(&s);
    if (!rc) cc_model_save(&m, &s), cc_model_free(&m);
    check_true("zeros and a null crs are the default", rc == 0 && s.len == n && !memcmp(s.p, frame, n));
    WITH("{\"crs\": \"\"}");
    rc = cc_model_load(&m, t.p, t.len, &err);
    check_true("an empty crs is kept (Some(\"\"), not the default)", rc == 0 && m.georef.has_crs && !m.georef.crs.n);
    if (!rc) {
        buf_clear(&s);
        cc_model_save(&m, &s);
        check_true("... and saved", strstr(s.p, "\"georef\": {\n      \"crs\": \"\"\n    }") != NULL);
        cc_model_free(&m);
    }
    load_err("unknown field", WITH("{\"x\": 1}"),
             "unknown field `x`, expected one of `rotation_deg`, `crs`, `east_m`, `north_m`, `height_m`, "
             "`map_rotation_deg`");
    load_err("null", WITH("null"), "invalid type: null, expected struct Georef");
    load_err("crs a number", WITH("{\"crs\": 1}"), "invalid type: integer `1`, expected a string");
    load_err("rotation a string", WITH("{\"rotation_deg\": \"30\"}"), "invalid type: string \"30\", expected f64");
    load_err("duplicate", WITH("{\"east_m\": 1, \"east_m\": 2}"), "duplicate field `east_m`");
    /* CBOR can hold NaN (JSON cannot): refused, as every other number of the model. */
    if (!cc_model_load(&m, frame, n, &err)) {
        m.georef.east_m = 0.0 / 0.0;
        Buf v2;
        buf_init(&v2);
        cc_model_save_v2(&m, &v2);
        cc_model_free(&m);
        rc = cc_model_load(&m, v2.p, v2.len, &err);
        snprintf(probe, sizeof probe, "a NaN in a binary file's georef refused (got %s)", rc ? err.msg : "loaded");
        check_true(probe, rc != 0 && strstr(err.msg, "number out of range") != NULL);
        if (!rc) cc_model_free(&m);
        buf_free(&v2);
    }
    #undef WITH
    buf_free(&t), buf_free(&s);
    free(frame);
    (void)probe;
}

static void unit_frame(void) {
    const double a = 30.0 * (3.14159265358979323846 / 180.0);
    double x[2] = {0, 0}, at[3] = {2600000000.0, 1200000000.0, 0.0};
    /* cos, sin of 30 degrees as Rust's from_frame test gives them */
    x[0] = 0.8660254037844387, x[1] = 0.49999999999999994;
    (void)a;
    cc_georef g;
    cc_georef_from_frame(&g, x, at);
    check_true("from_frame: 30 degrees", g.rotation_deg == 30.0);
    check_true("from_frame: LV95 in Switzerland", g.has_crs && g.crs.n == 9 && !memcmp(g.crs.p, "EPSG:2056", 9));
    double w[3] = {2600123450.0, 1200000000.0, 432100.0}, s[3];
    cc_georef_survey(&g, w, s);
    check_true("survey: world / 1000", fabs_d(s[0] - 2600123.45) < 1e-6 && fabs_d(s[2] - 432.1) < 1e-9);
    double x0[2] = {1, 0}, o[3] = {0, 0, 0};
    cc_georef_from_frame(&g, x0, o);
    check_true("from_frame elsewhere, unturned: the default", cc_georef_is_default(&g));
    /* coords.rs test */
    cc_georef_from_frame(&g, x0, at);
    Buf b;
    buf_init(&b);
    double p[3] = {2600123450.0, 1200000000.0, 432100.0};
    cc_georef_text(&b, &g, p, CC_COORDS_SURVEY, 0);
    check_eq("coords.rs: the Swiss way", b.p, b.len, "E 2'600'123.45 N 1'200'000.00 \xc2\xb7 432.10 m (LV95)",
             strlen("E 2'600'123.45 N 1'200'000.00 \xc2\xb7 432.10 m (LV95)"));
    buf_clear(&b);
    cc_georef_text(&b, &g, p, CC_COORDS_WGS84, 0);
    check_true("coords.rs: WGS84 starts with 46.951", b.len > 6 && !memcmp(b.p, "46.951", 6));
    buf_clear(&b);
    int rc = cc_georef_text(&b, &g, p, CC_COORDS_UTM, 0);
    check_true("UTM refused with a clear error", rc == -1 && strstr(b.p, "not converted by the C core") != NULL);
    cc_georef none;
    memset(&none, 0, sizeof none);
    double q[3] = {-1500.0, 20.0, 0.0};
    buf_clear(&b);
    rc = cc_georef_text(&b, &none, q, CC_COORDS_WGS84, 0);
    check_eq("coords.rs: no survey system, model metres", b.p, b.len, "x -1.500 y 0.020 \xc2\xb7 0.00 m",
             strlen("x -1.500 y 0.020 \xc2\xb7 0.00 m"));
    /* another system Rust knows: refused; one it does not know: its own coordinates, as Rust */
    cc_georef ny = none;
    ny.has_crs = 1, ny.crs.p = "EPSG:2263", ny.crs.n = 9;
    buf_clear(&b);
    rc = cc_georef_text(&b, &ny, q, CC_COORDS_WGS84, 0);
    check_true("EPSG:2263 to WGS84: refused (Swiss first)", rc == -1 && strstr(b.p, "EPSG:2263") != NULL);
    ny.crs.p = "EPSG:9", ny.crs.n = 6;
    buf_clear(&b);
    rc = cc_georef_text(&b, &ny, q, CC_COORDS_WGS84, 0);
    check_true("an unknown system: its own coordinates", rc == 0 && strstr(b.p, "(EPSG:9)") != NULL);
    buf_free(&b);
    /* georef.rs tests: the origin and swisstopo's worked example */
    double lat, lon, ll[2];
    check_true("LV95 origin -> WGS84 (proj4rs)", !cc_lv95_to_wgs84(2600000.0, 1200000.0, &lat, &lon) &&
                                                     fabs_d(lat - 46.9510827718711) < 1e-5 &&
                                                     fabs_d(lon - 7.43863242087181) < 1e-5);
    cc_lv95_to_wgs84_approx(2700000.0, 1100000.0, ll);
    check_true("swisstopo's worked example", fabs_d(ll[0] - (46.0 + 2.0 / 60 + 38.87 / 3600)) < 1e-4 &&
                                                 fabs_d(ll[1] - (8.0 + 43.0 / 60 + 49.79 / 3600)) < 1e-4);
    check_true("cc_crs_known", cc_crs_known(2056) && cc_crs_known(21781) && !cc_crs_known(9));
}

static void command_units(void) {
    size_t n;
    char *frame = slurp(golden, "frame.json", &n);
    cc_model m;
    cc_error err;
    if (cc_model_load(&m, frame, n, &err)) { check_true("frame loads", 0); free(frame); return; }
    cc_history h;
    cc_history_init(&h);
    cc_cmds l;
    cc_cmds_init(&l);
    Buf e;
    buf_init(&e);
    const char *args = "{\"rotation_deg\": 12.5, \"crs\": \"EPSG:2056\", \"east_m\": 2600123.456, \"north_m\": "
                       "1199876.5, \"height_m\": 432.1, \"map_rotation_deg\": -3.25}";
    int rc = cc_verb_run(&m, "georef_set", args, strlen(args), &l, &e);
    check_true("verb georef_set: one command", rc == 0 && l.n == 1 && l.v[0].op == CC_CMD_SET_GEOREF);
    Buf j;
    buf_init(&j);
    cc_commands_json(l.v, l.n, &j);
    const char *want = "[{\"op\":\"set_georef\",\"georef\":{\"rotation_deg\":12.5,\"crs\":\"EPSG:2056\",\"east_m\":"
                       "2600123.456,\"north_m\":1199876.5,\"height_m\":432.1,\"map_rotation_deg\":-3.25}}]";
    check_eq("set_georef as JSON", j.p, j.len, want, strlen(want));
    /* JSON round trip */
    cc_cmds l2;
    cc_cmds_init(&l2);
    rc = cc_commands_parse(j.p, j.len, &l2, &e);
    check_true("set_georef parses back", rc == 0 && l2.n == 1 && cc_georef_eq(&l2.v[0].georef, &l.v[0].georef));
    cc_cmds_free(&l2);
    rc = cc_history_apply(&h, &m, l.v, l.n, &e);
    check_true("set_georef applies", rc == 0);
    /* The model is now geo.json */
    size_t gn;
    char *geo = slurp(golden, "geo/geo.json", &gn);
    Buf s;
    buf_init(&s);
    cc_model_save(&m, &s);
    check_buf("frame + georef_set == Rust's geo/geo.json", &s, geo, gn);
    /* queries */
    Buf q;
    buf_init(&q);
    int st = cc_query(&m, "north", "", 0, &q);
    const char *wn = "{\"crs\":\"EPSG:2056\",\"has_map\":true,\"map_rotation_deg\":-3.25,\"plan_turn_project\":"
                     "0.2181661564992912,\"plan_turn_true\":0.05672320068981571,\"rotation_deg\":12.5}";
    check_true("query north: 200", st == 200);
    check_eq("query north", q.p, q.len, wn, strlen(wn));
    buf_clear(&q);
    const char *ca = "{\"at\":[0,0,0],\"system\":\"wgs84\"}";
    st = cc_query(&m, "coordinates", ca, strlen(ca), &q);
    check_true("query coordinates: 200, WGS84 text",
               st == 200 && strstr(q.p, "\"text\":\"46.949972\xc2\xb0 N, 7.440254\xc2\xb0 E \xc2\xb7 432.10 m (WGS84)\"") != NULL);
    buf_clear(&q);
    ca = "{\"at\":[0,0,0],\"system\":\"utm\"}";
    st = cc_query(&m, "coordinates", ca, strlen(ca), &q);
    check_true("query coordinates in UTM: 400 with the reason", st == 400 && strstr(q.p, "not converted") != NULL);
    buf_clear(&q);
    st = cc_query(&m, "nope", "", 0, &q);
    check_true("unknown query text unchanged (Rust's)",
               st == 400 && strstr(q.p, "known: storeys, materials, elements, element, project, views, sheets, schedules") != NULL);
    /* nothing changes: no command */
    cc_cmds_free(&l);
    cc_cmds_init(&l);
    const char *a125 = "{\"rotation_deg\": 12.5}";
    rc = cc_verb_run(&m, "georef_set", a125, strlen(a125), &l, &e);
    check_true("georef_set with the same values: no command", rc == 0 && l.n == 0);
    const char *a400 = "{\"rotation_deg\": 400}";
    rc = cc_verb_run(&m, "georef_set", a400, strlen(a400), &l, &e);
    check_true("georef_set: 400 degrees refused", rc != 0 && strstr(e.p, "at most 360 degrees") != NULL);
    rc = cc_verb_run(&m, "georef_set", "{\"north\": 1}", strlen("{\"north\": 1}"), &l, &e);
    check_true("georef_set: unknown field refused", rc != 0 && strstr(e.p, "unknown field `north`") != NULL);
    cc_cmds_free(&l);
    cc_cmds_init(&l);
    rc = cc_verb_run(&m, "georef_set", "{\"clear\": true}", strlen("{\"clear\": true}"), &l, &e);
    check_true("georef_set clear: one command", rc == 0 && l.n == 1 && cc_georef_is_default(&l.v[0].georef));
    Buf d;
    buf_init(&d);
    if (l.n) cc_command_describe(&l.v[0], &m, &d);
    check_eq("describe", d.p ? d.p : "", d.len, "clear the georeference", 22);
    rc = cc_history_apply(&h, &m, l.v, l.n, &e);
    buf_clear(&s);
    cc_model_save(&m, &s);
    check_true("cleared: frame.json's bytes again", rc == 0 && s.len == n && !memcmp(s.p, frame, n));
    cc_history_undo(&h, &m);
    buf_clear(&s);
    cc_model_save(&m, &s);
    check_buf("undo: geo.json again", &s, geo, gn);
    cc_history_undo(&h, &m);
    buf_clear(&s);
    cc_model_save(&m, &s);
    check_true("undo: frame.json again", s.len == n && !memcmp(s.p, frame, n));
    /* a non-finite value in a command is refused */
    cc_command c;
    memset(&c, 0, sizeof c);
    c.op = CC_CMD_SET_GEOREF;
    c.georef.east_m = 1.0 / 0.0;
    cc_command inv;
    buf_clear(&e);
    rc = cc_command_apply(&m, &c, &inv, &e);
    check_true("set_georef: non-finite refused", rc != 0 && strstr(e.p, "non-finite") != NULL);
    buf_free(&d), buf_free(&q), buf_free(&s), buf_free(&j), buf_free(&e);
    cc_cmds_free(&l);
    cc_history_free(&h);
    cc_model_free(&m);
    free(frame), free(geo);
}

int main(void) {
    file_units();
    unit_frame();
    command_units();
    static const char *const MODELS[] = {"geo", "turned", "survey"};
    for (int k = 0; k < 3; k++) model_check(MODELS[k]);
    return report("georef");
}
