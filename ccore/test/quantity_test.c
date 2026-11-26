/* Suite quantity: measures and schedule tables (src/quantity.c) against Rust's bytes
 * (golden/schedule/, see golden/README.md "schedule/"): every file of that folder is produced
 * here and compared byte for byte. Then the exact fixed-point formatter against Rust's
 * format!("{:.p$}"), the natural order, the effective schedules and robustness. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <string.h>
#include <stdint.h>
#include "../src/model.h"
#include "../src/file.h"
#include "../src/scene.h"
#include "../src/viewfile.h"
#include "../src/ifc_read.h"
#include "../src/json_w.h"
#include "../src/fmt.h"
#include "../src/quantity.h"
#include "../src/command.h" /* WAVE1B (B1) */
#include "../src/verbs.h"   /* WAVE1B (B1) */
#include "check.h"

static const char *golden;
static size_t produced; /* files compared, to match the folder's count */

static void compare(const char *rel, const Buf *got) {
    size_t n;
    char path[512];
    snprintf(path, sizeof path, "schedule/%s", rel);
    char *want = slurp(golden, path, &n);
    char label[600];
    snprintf(label, sizeof label, "golden %s", path);
    check_eq(label, got->p ? got->p : "", got->len, want, n);
    free(want);
    produced++;
}

/* schedule_slug: lower case (ASCII here), spaces as '-'. */
static void slug(char *d, size_t cap, cc_str name) {
    size_t n = name.n < cap - 1 ? name.n : cap - 1;
    for (size_t i = 0; i < n; i++) {
        char c = name.p[i];
        d[i] = c == ' ' ? '-' : c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    d[n] = 0;
}

static void debug_f64(Buf *b, double x) { fmt_f64_debug(b, x); }

/* golden measures.txt */
static int measures_txt(Buf *b, const cc_scene *s, const cc_viewfile_meta *mt) {
    Arena a;
    arena_init(&a, 0);
    for (size_t k = 0; k < s->nelements; k++) {
        char head[64];
        snprintf(head, sizeof head, "%03zu ", k);
        buf_puts(b, head);
        buf_append(b, mt->info[k].global_id.p, mt->info[k].global_id.n);
        cc_measures m;
        arena_reset(&a);
        int r = cc_measure(s, mt, k, &a, &m);
        if (r < 0) {
            arena_free(&a);
            return -1;
        }
        if (r == 0) {
            buf_puts(b, " none\n");
            continue;
        }
        buf_puts(b, " volume="), debug_f64(b, m.volume_m3);
        buf_puts(b, " area="), debug_f64(b, m.area_m2);
        buf_puts(b, " length="), debug_f64(b, m.length);
        buf_puts(b, " width="), debug_f64(b, m.width);
        buf_puts(b, " height="), debug_f64(b, m.height);
        buf_puts(b, " mass=");
        double rho;
        if (cc_density(mt, mt->info[k].material, &rho)) debug_f64(b, m.volume_m3 * rho);
        else buf_putc(b, '-');
        buf_puts(b, " centre="), debug_f64(b, m.centre[0]), buf_putc(b, ','), debug_f64(b, m.centre[1]);
        buf_puts(b, " section=");
        json_escape(b, m.section.p, m.section.n);
        buf_putc(b, '\n');
    }
    arena_free(&a);
    return 0;
}

/* The two schedules golden_schedules adds (ccore_golden.rs). */
static cc_str EXTRA_CLS[] = {{"IfcColumn", 9}, {"ifcbeam", 7}};
static uint8_t ALL_FIELDS[CC_FIELD_COUNT];
static uint8_t SUMS[] = {CC_FIELD_VOLUME, CC_FIELD_AREA, CC_FIELD_MASS, CC_FIELD_LENGTH};

static void schedules_of(const char *dirname, const cc_scene *s, const cc_viewfile_meta *mt, const cc_schedule *stored,
                         size_t nstored, int builtins) {
    const cc_schedule *eff[64];
    size_t ne = 0;
    if (builtins) ne = cc_schedules_effective(stored, nstored, eff, 62);
    for (int i = 0; i < CC_FIELD_COUNT; i++) ALL_FIELDS[i] = (uint8_t)i;
    cc_schedule every = {{"Every field", 11}, {NULL, 0}, 0, 0, ALL_FIELDS, CC_FIELD_COUNT, 0, 1};
    cc_schedule sums = {{"Sums only", 9}, {EXTRA_CLS, 2}, 0, 0, SUMS, 4, 1, 0};
    eff[ne++] = &every;
    eff[ne++] = &sums;
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < ne; i++) {
        cc_sched_table t;
        cc_error err;
        char sl[128], rel[300];
        slug(sl, sizeof sl, eff[i]->name);
        if (cc_schedule_table(&t, s, mt, eff[i], &err)) {
            printf("FAIL %s/%s: %s\n", dirname, sl, err.msg);
            failed++;
            cc_sched_table_free(&t);
            continue;
        }
        buf_clear(&b);
        cc_schedule_json(&b, &t, mt, 50);
        buf_putc(&b, '\n');
        snprintf(rel, sizeof rel, "%s/%s.json", dirname, sl);
        compare(rel, &b);
        buf_clear(&b);
        cc_schedule_csv(&b, &t);
        snprintf(rel, sizeof rel, "%s/%s.csv", dirname, sl);
        compare(rel, &b);
        cc_sched_table_free(&t);
    }
    buf_clear(&b);
    if (measures_txt(&b, s, mt)) printf("FAIL %s: measures out of memory\n", dirname), failed++;
    char rel[300];
    snprintf(rel, sizeof rel, "%s/measures.txt", dirname);
    compare(rel, &b);
    buf_free(&b);
}

static void one_model(const char *name) {
    char path[256];
    size_t n;
    snprintf(path, sizeof path, "%s.json", name);
    char *text = slurp(golden, path, &n);
    cc_model m;
    cc_scene s;
    cc_error err;
    if (cc_model_load(&m, text, n, &err)) {
        printf("FAIL %s does not load: %s\n", path, err.msg);
        failed++;
        free(text);
        return;
    }
    if (cc_scene_build(&s, &m, &err)) {
        printf("FAIL %s: scene: %s\n", name, err.msg);
        failed++;
    } else {
        cc_viewfile_meta mt;
        if (cc_viewfile_meta_from_model(&mt, &m, &s)) printf("FAIL %s: meta\n", name), failed++;
        else schedules_of(name, &s, &mt, m.schedules, m.nschedules, 1);
        cc_viewfile_meta_free(&mt);
    }
    cc_scene_free(&s);
    cc_model_free(&m);
    free(text);
}

/* An IFC read for display (cc_ifc_read), as the viewer opens it: the same tables. */
static void one_ifc(const char *name) {
    char path[256], dirname[256];
    size_t n;
    snprintf(path, sizeof path, "ifc/fixtures/%s.ifc", name);
    snprintf(dirname, sizeof dirname, "ifc-%s", name);
    char *text = slurp(golden, path, &n);
    cc_ifc_scene s;
    cc_error err;
    if (cc_ifc_read(text, n, &s, &err)) {
        printf("FAIL %s: %s\n", path, err.msg);
        failed++;
    } else {
        cc_viewfile_meta mt;
        if (cc_ifc_scene_meta(&s, &mt)) printf("FAIL %s: meta\n", name), failed++;
        else schedules_of(dirname, &s.scene, &mt, NULL, 0, 0);
        cc_viewfile_meta_free(&mt);
    }
    cc_ifc_scene_free(&s);
    free(text);
}

/* Every file of golden/schedule/ was compared: none is left unchecked. */
static size_t count_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t k = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        /* Q2: schedule/session/ holds the session calls' texts: test/schedules_test.mjs compares
         * them, and checks that it compared every one. */
        if (!strcmp(e->d_name, "session") && strstr(dir, "/schedule") && !strcmp(strstr(dir, "/schedule"), "/schedule")) continue;
        char sub[4096];
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        DIR *x = opendir(sub);
        if (x) closedir(x), k += count_files(sub);
        else k++;
    }
    closedir(d);
    return k;
}

static void fixed(double x, int p, const char *want) {
    Buf b;
    buf_init(&b);
    cc_fmt_fixed(&b, x, p);
    char label[128];
    snprintf(label, sizeof label, "{:.%d} of %.17g", p, x);
    check_eq(label, b.p ? b.p : "", b.len, want, strlen(want));
    buf_free(&b);
}

/* Rust 1.100 format!("{v:.d$}"): ties to even on the exact binary value, the sign of -0.0 kept. */
static void formatter(void) {
    fixed(0.25, 1, "0.2");
    fixed(0.5, 0, "0");
    fixed(1.5, 0, "2");
    fixed(2.5, 0, "2");
    fixed(-0.0, 3, "-0.000");
    fixed(-0.0001, 3, "-0.000");
    fixed(0.125, 2, "0.12");
    fixed(0.375, 2, "0.38");
    fixed(1e21, 0, "1000000000000000000000");
    fixed(123.4565, 3, "123.457");
    fixed(5e-324, 3, "0.000");
    fixed(2.675, 2, "2.67");
    fixed(0.0, 3, "0.000");
    fixed(30.0, 1, "30.0");
    fixed(1.0 / 0.0, 2, "inf");
    fixed(-1.0 / 0.0, 2, "-inf");
    fixed(0.0 / 0.0, 2, "NaN");
    fixed(1.7976931348623157e308, 2,
          "17976931348623157081452742373170435679807056752584499659891747680315726078002853876058955863276687817154045"
          "89535143824642343213268894641827684675467035375169860499105765512820762454900903893289440758685084551339423"
          "04583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368.00");
}

/* Natural order and the effective list: a stored schedule replaces the built-in one of its
 * name (any case) in place; a new one comes after. */
static void definitions(void) {
    size_t nb;
    const cc_schedule *b = cc_schedule_builtins(&nb);
    check_true("ten built-in schedules", nb == 10 && b[9].classes.n == 0);
    uint8_t f[] = {CC_FIELD_NAME};
    cc_schedule stored[2] = {
        {{"steel BEAMS", 11}, {NULL, 0}, 0, 0, f, 1, 0, 0},
        {{"Rebar check", 11}, {NULL, 0}, 0, 0, f, 1, 0, 0},
    };
    const cc_schedule *out[16];
    size_t n = cc_schedules_effective(stored, 2, out, 16);
    check_true("effective: 11 schedules", n == 11);
    check_true("effective: Steel beams replaced in place", out[4] == &stored[0]);
    check_true("effective: Rebar check last", out[10] == &stored[1]);
    check_true("effective: a short `out` still counts", cc_schedules_effective(stored, 2, out, 3) == 11);
}

/* A table of nothing: no rows, totals of zero (never "-0.000"), labels where Rust puts them. */
static void empty_and_order(void) {
    size_t n;
    char *text = slurp(golden, "frame.json", &n);
    cc_model m;
    cc_scene s;
    cc_error err;
    if (cc_model_load(&m, text, n, &err)) {
        failed++;
        free(text);
        return;
    }
    if (cc_scene_build(&s, &m, &err) == 0) {
        cc_viewfile_meta mt;
        if (cc_viewfile_meta_from_model(&mt, &m, &s) == 0) {
            size_t nb;
            const cc_schedule *b = cc_schedule_builtins(&nb);
            cc_sched_table t;
            Buf csv;
            buf_init(&csv);
            check_true("steel beams: table", cc_schedule_table(&t, &s, &mt, &b[4], &err) == 0);
            cc_schedule_csv(&csv, &t);
            check_true("no steel in the frame: count 0", t.count == 0 && t.ngroups == 0);
            check_true("empty totals never -0", !strstr(buf_cstr(&csv), "-0"));
            cc_sched_table_free(&t);
            /* Natural order: P1.2 before P1.10 (numbers, not text). */
            uint8_t f[] = {CC_FIELD_NAME};
            cc_schedule names = {{"n", 1}, {NULL, 0}, 0, 0, f, 1, 0, 0};
            check_true("names: table", cc_schedule_table(&t, &s, &mt, &names, &err) == 0);
            int sorted = t.ngroups == 1 && t.groups[0].nrows == s.nelements && !t.total;
            check_true("names: one group, no total", sorted);
            cc_sched_table_free(&t);
            buf_free(&csv);
        }
        cc_viewfile_meta_free(&mt);
    }
    cc_scene_free(&s);
    cc_model_free(&m);
    free(text);
}

int main(int argc, char **argv) {
    static char dir[4096], sched[4200];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    golden = dir;
    one_model("frame");
    one_model("frame2");
    static const char *const IFC[] = {"circle_column", "ishape_beam", "rect_hollow", "triangulated",
                                      "faceted_brep",  "mapped_item", "indexed_arc"};
    for (size_t i = 0; i < sizeof IFC / sizeof IFC[0]; i++) one_ifc(IFC[i]);
    snprintf(sched, sizeof sched, "%s/schedule", dir);
    size_t on_disk = count_files(sched);
    char label[128];
    snprintf(label, sizeof label, "every file of golden/schedule/ compared (%zu on disk, %zu compared)", on_disk,
             produced);
    check_true(label, on_disk == produced && produced > 0);
    formatter();
    definitions();
    empty_and_order();
    { /* WAVE1B (B1): a wall with a sleeve: cc_measure counts the exact cylinder pi r^2 t, not the 32-gon */
        cc_model m;
        cc_error e;
        size_t n;
        char *text = slurp("golden", "frame.json", &n);
        int ok = text && !cc_model_load(&m, text, n, &e);
        free(text);
        cc_gid wall;
        memset(&wall, 0, sizeof wall);
        for (int step = 0; ok && step < 2; step++) {
            cc_cmds l;
            cc_cmds_init(&l);
            Buf err, args;
            buf_init(&err), buf_init(&args);
            if (step == 0) buf_puts(&args, "{\"kind\":\"wall\",\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\",\"a\":[0,-3000],\"b\":[6000,-3000]}");
            else {
                buf_puts(&args, "{\"host\":\""), buf_append(&args, wall.c, 22);
                buf_puts(&args, "\",\"a\":[2000,-3000],\"shape\":\"round\",\"diameter\":300}");
            }
            ok = !cc_verb_run_ex(&m, step ? "opening_add" : "place", args.p, args.len, 0, -1, 0, &l, &err);
            for (size_t i = 0; ok && i < l.n; i++) {
                cc_command inv;
                ok = !cc_command_apply(&m, &l.v[i], &inv, &err);
                if (step == 0 && l.v[i].op == CC_CMD_ADD_ELEMENT) wall = l.v[i].element.id;
            }
            if (!ok) printf("  B1 setup: %s\n", buf_cstr(&err));
            cc_cmds_free(&l), buf_free(&err), buf_free(&args);
        }
        check_true("B1 setup: a wall and a sleeve of 300", ok);
        if (ok) {
            const cc_element *w = cc_model_element(&m, &wall);
            double gross = w->generator.u.wall.length * w->generator.u.wall.thickness * w->generator.u.wall.height / 1e9;
            double cyl = 3.141592653589793 * 150.0 * 150.0 * w->generator.u.wall.thickness / 1e9;
            cc_scene sc;
            cc_viewfile_meta mt;
            double v = -1;
            if (!cc_scene_build(&sc, &m, &e)) {
                if (!cc_viewfile_meta_from_model(&mt, &m, &sc)) {
                    for (size_t k = 0; k < sc.nelements; k++) {
                        if (cc_gid_cmp(&sc.elements[k].id, &wall)) continue;
                        Arena a;
                        arena_init(&a, 0);
                        cc_measures q;
                        if (cc_measure(&sc, &mt, k, &a, &q) == 1) v = q.volume_m3;
                        arena_free(&a);
                    }
                    cc_viewfile_meta_free(&mt);
                }
                cc_scene_free(&sc);
            }
            printf("  B1 wall %.0f x %.0f x %.0f with a sleeve of 300: %.9f m3 (hand: %.9f - %.9f = %.9f)\n", w->generator.u.wall.length,
                   w->generator.u.wall.thickness, w->generator.u.wall.height, v, gross, cyl, gross - cyl);
            check_true("B1 cc_measure: the wall's volume is the gross minus the exact cylinder pi r^2 t", v - (gross - cyl) < 1e-6 && (gross - cyl) - v < 1e-6); /* the mesh is f32: 1e-6 m3 */
            cc_model_free(&m);
        }
    }
    return report("quantity");
}
