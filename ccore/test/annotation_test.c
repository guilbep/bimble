/* Suite annotation (ticket C4, src/annot.h): the dimension and text verbs as Commands on
 * golden/frame.json through History (one step each, undo, redo), feature anchors that follow a
 * moved and a resized element, the drawing helpers against Rust's (DimUnit::format,
 * dimension_on_paper, dim_axis_and_offset), the anchor's serde form, the file round trip (v1
 * JSON and v2 binary), and the validation of a feature. */
#include <stdint.h>
#include <string.h>
#include "../src/annot.h"
#include "../src/command.h"
#include "../src/file.h"
#include "../src/history.h"
#include "../src/model.h"
#include "../src/verbs.h"
#include "../src/view_io.h"
#include "check.h"

static const char *PLAN_N1 = "3w8zGYnP$Ay1lyvMZjDnXq"; /* Plan Niveau 1 (Rust's derived id) */
static const char *C00 = "2P97ZHH3v3XutfaphHA_Tu", *C60 = "2KmXk9zn1FAPxjTPfLpwLP";

/* One verb through History. 0 ok. */
static int run(cc_history *h, cc_model *m, const char *verb, const char *args, Buf *err) {
    cc_cmds l;
    cc_cmds_init(&l);
    buf_clear(err);
    int rc = cc_verb_run(m, verb, args, strlen(args), &l, err);
    if (!rc) rc = cc_history_apply(h, m, l.v, l.n, err);
    cc_cmds_free(&l);
    return rc;
}

static const cc_view *plan(const cc_model *m) {
    cc_gid id;
    memcpy(id.c, PLAN_N1, 22);
    return cc_model_view(m, &id);
}

/* The view_annotations JSON of the plan. */
static void ann_json(const cc_model *m, Buf *out, int unit) {
    buf_clear(out);
    JsonW w;
    jw_init(&w, out, 0);
    const cc_view *v = plan(m);
    if (v) cc_annotations_json(&w, m, v, unit);
    else jw_null(&w);
    jw_finish(&w);
}

static int has(const Buf *b, const char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= b->len; i++)
        if (!memcmp(b->p + i, s, n)) return 1;
    return 0;
}

static void fmt_cases(void) {
    static const struct { double mm; int unit; const char *want; } C[] = {
        /* template.rs tests: Cm 3620 -> "362", 125 -> "12.5", 119.96 -> "12", Mm 3620.4 -> "3620",
         * M 3620 -> "3.62", Cm -0.2 -> "0" */
        {3620.0, CC_UNIT_CM, "362"}, {125.0, CC_UNIT_CM, "12.5"}, {119.96, CC_UNIT_CM, "12"},
        {3620.4, CC_UNIT_MM, "3620"}, {3620.0, CC_UNIT_M, "3.62"}, {-0.2, CC_UNIT_CM, "0"},
        {6000.0, CC_UNIT_MM, "6000"}, {-125.0, CC_UNIT_CM, "-12.5"}, {50.0, CC_UNIT_M, "0.05"},
        {-0.4, CC_UNIT_MM, "0"}, {12345.0, CC_UNIT_M, "12.35"}, {2.5, CC_UNIT_MM, "3"},
    };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        Buf b;
        buf_init(&b);
        cc_dim_format(&b, C[i].mm, C[i].unit);
        check_eq("DimUnit::format", b.p, b.len, C[i].want, strlen(C[i].want));
        buf_free(&b);
    }
}

static void geometry_cases(void) {
    double u[2], qa[2], qb[2], len;
    /* a -> b along +x, offset 100 to the left (up in plan) */
    double a[2] = {0, 0}, b[2] = {1000, 0};
    check_true("dim_line aligned", !cc_dim_line(a, b, 100, CC_AXIS_ALIGNED, u, qa, qb, &len) && len == 1000 &&
                                       qa[0] == 0 && qa[1] == 100 && qb[0] == 1000 && qb[1] == 100 && u[0] == 1);
    double c[2] = {300, 400};
    check_true("dim_line horizontal of a slanted pair: 300", !cc_dim_line(a, c, 0, CC_AXIS_HORIZONTAL, u, qa, qb, &len) && len == 300);
    check_true("dim_line vertical: 400", !cc_dim_line(a, c, 0, CC_AXIS_VERTICAL, u, qa, qb, &len) && len == 400);
    check_true("dim_line degenerate", cc_dim_line(a, a, 0, CC_AXIS_ALIGNED, u, qa, qb, &len) == -1);
    cc_dim_axis ax;
    double off;
    double above[2] = {150, 900}, right[2] = {800, 200}, inside[2] = {100, 300};
    cc_dim_axis_offset(a, c, above, &ax, &off);
    check_true("third point above the box: horizontal", ax == CC_AXIS_HORIZONTAL && off == 900);
    cc_dim_axis_offset(a, c, right, &ax, &off);
    check_true("third point right of the box: vertical", ax == CC_AXIS_VERTICAL && off == -800);
    cc_dim_axis_offset(a, c, inside, &ax, &off);
    check_true("third point inside the box: aligned", ax == CC_AXIS_ALIGNED);
    cc_dim_axis_offset(a, b, above, &ax, &off);
    check_true("a level pair: aligned, offset to the left", ax == CC_AXIS_ALIGNED && off == 900);
}

static void model_cases(const char *golden) {
    size_t n;
    char *text = slurp(golden, "../frame.json", &n);
    cc_model m;
    cc_error e;
    if (cc_model_load(&m, text, n, &e)) {
        printf("frame.json: %s\n", e.msg);
        failed++;
        free(text);
        return;
    }
    free(text);
    cc_history h;
    cc_history_init(&h);
    Buf err, j;
    buf_init(&err), buf_init(&j);
    char args[1024];

    /* features */
    cc_gid c60;
    memcpy(c60.c, C60, 22);
    const cc_element *col = cc_model_element(&m, &c60);
    double p[3];
    cc_str f = {"corner:2", 8};
    check_true("feature corner:2 of the column at (6000,0): (6150,150,0)",
               col && !cc_feature_point(&m, col, f, p) && p[0] == 6150 && p[1] == 150 && p[2] == 0);
    cc_str bad[] = {{"corner:4", 8}, {"end", 3}, {"corner:01", 9}, {"corner:", 7}, {"top", 3}};
    for (int i = 0; i < 5; i++)
        check_true("a column has no such feature", col && cc_feature_point(&m, col, bad[i], p));
    check_true("feature names", cc_feature_name_ok(f) && !cc_feature_name_ok(bad[2]) && !cc_feature_name_ok(bad[3]));
    cc_gid el;
    char feat[24];
    double near[2] = {6149.6, -149.7};
    check_true("feature near (6150,-150): the column's corner:1",
               cc_feature_near(&m, near, 1.0, &col->storey, &el, feat, p) && !cc_gid_cmp(&el, &c60) && !strcmp(feat, "corner:1"));

    /* add_dimension: a default plan stored, one step */
    snprintf(args, sizeof args,
             "{\"view\":\"%s\",\"a\":{\"element\":\"%s\",\"feature\":\"corner:0\"},\"b\":{\"element\":\"%s\",\"feature\":\"corner:0\"},\"offset\":-800}",
             PLAN_N1, C00, C60);
    check_true("add_dimension", !run(&h, &m, "add_dimension", args, &err));
    check_true("the plan stored with one dimension", plan(&m) && plan(&m)->nannotations == 1);
    ann_json(&m, &j, CC_UNIT_MM);
    check_true("value 6000", has(&j, "\"text\":\"6000\"") && has(&j, "\"value_mm\":6000"));
    snprintf(args, sizeof args, "{\"view\":\"%s\",\"at\":[100,200],\"text\":\"Note\",\"size_mm\":5}", PLAN_N1);
    check_true("add_text", !run(&h, &m, "add_text", args, &err));
    check_true("two annotations", plan(&m)->nannotations == 2 && plan(&m)->annotations[1].size_mm == 5.0);

    /* anchors follow: move (set_placement), resize (set_generator), undo */
    snprintf(args, sizeof args, "{\"ids\":[\"%s\"],\"d\":[-1000,0,0]}", C60);
    check_true("move the column", !run(&h, &m, "move", args, &err));
    ann_json(&m, &j, CC_UNIT_MM);
    check_true("moved: 5000", has(&j, "\"value_mm\":5000"));
    snprintf(args, sizeof args, "{\"id\":\"%s\",\"params\":{\"width\":700}}", C60);
    check_true("resize the column", !run(&h, &m, "set_generator", args, &err));
    ann_json(&m, &j, CC_UNIT_CM);
    check_true("resized: corner 0 at 5000-350: 4800 mm, \"480\" cm", has(&j, "\"value_mm\":4800") && has(&j, "\"text\":\"480\""));
    cc_history_undo(&h, &m);
    cc_history_undo(&h, &m);
    ann_json(&m, &j, CC_UNIT_MM);
    check_true("undo both: 6000", has(&j, "\"value_mm\":6000"));
    /* the element removed: the stored point (at) stays */
    snprintf(args, sizeof args, "{\"ids\":[\"%s\"]}", C60);
    check_true("delete the column", !run(&h, &m, "delete", args, &err));
    ann_json(&m, &j, CC_UNIT_MM);
    check_true("its anchor falls back on at: 6000, follows false", has(&j, "\"value_mm\":6000") && has(&j, "\"follows\":false"));
    cc_history_undo(&h, &m);

    /* move and delete, undo, redo */
    cc_gid tid = plan(&m)->annotations[1].id;
    snprintf(args, sizeof args, "{\"view\":\"%s\",\"id\":\"%.22s\",\"by\":[10,20]}", PLAN_N1, tid.c);
    check_true("move_annotation text", !run(&h, &m, "move_annotation", args, &err) && plan(&m)->annotations[1].at[0] == 110 &&
                                           plan(&m)->annotations[1].at[1] == 220);
    snprintf(args, sizeof args, "{\"view\":\"%s\",\"id\":\"%.22s\"}", PLAN_N1, tid.c);
    check_true("delete_annotation", !run(&h, &m, "delete_annotation", args, &err) && plan(&m)->nannotations == 1);
    check_true("undo the delete", cc_history_undo(&h, &m) == 1 && plan(&m)->nannotations == 2);
    check_true("redo the delete", cc_history_redo(&h, &m) == 1 && plan(&m)->nannotations == 1);
    check_true("undo all: the plan not stored", cc_history_undo(&h, &m) && cc_history_undo(&h, &m) && cc_history_undo(&h, &m) &&
                                                     cc_history_undo(&h, &m) && !plan(&m));
    check_true("redo all", cc_history_redo(&h, &m) && cc_history_redo(&h, &m) && cc_history_redo(&h, &m) && cc_history_redo(&h, &m) &&
                               plan(&m) && plan(&m)->nannotations == 1);
    /* a dimension with an element anchor without a feature (Rust's shape), and free points */
    snprintf(args, sizeof args, "{\"view\":\"%s\",\"a\":{\"at\":[0,0],\"element\":\"%s\"},\"b\":[0,5000],\"offset\":300,\"axis\":\"vertical\"}",
             PLAN_N1, C00);
    check_true("add_dimension, Rust's anchor shape", !run(&h, &m, "add_dimension", args, &err));
    check_true("errors: a 3D view", run(&h, &m, "add_text", "{\"view\":\"1v7Vtm2FfafmYPKIQEXnlY\",\"at\":[0,0],\"text\":\"x\"}", &err) &&
                                       has(&err, "2D views"));

    /* the file round trip, v1 and v2 */
    ann_json(&m, &j, CC_UNIT_MM);
    Buf before;
    buf_init(&before);
    buf_append(&before, j.p, j.len);
    for (int v2 = 0; v2 < 2; v2++) {
        Buf file;
        buf_init(&file);
        check_true("save", !(v2 ? cc_model_save_v2(&m, &file) : cc_model_save(&m, &file)));
        if (!v2) check_true("v1: the feature key, Rust's keys otherwise", has(&file, "\"feature\": \"corner:0\"") && has(&file, "\"axis\": \"vertical\""));
        cc_model r;
        check_true(v2 ? "load v2" : "load v1", !cc_model_load(&r, file.p, file.len, &e));
        ann_json(&r, &j, CC_UNIT_MM);
        check_eq(v2 ? "v2 round trip" : "v1 round trip", j.p, j.len, before.p, before.len);
        Buf again;
        buf_init(&again);
        check_true("saved again: the same bytes", !(v2 ? cc_model_save_v2(&r, &again) : cc_model_save(&r, &again)) &&
                                                     again.len == file.len && !memcmp(again.p, file.p, file.len));
        buf_free(&again);
        cc_model_free(&r);
        buf_free(&file);
    }
    buf_free(&before);

    /* validation: a feature without its element, an unknown feature */
    const char *bad_files[] = {
        "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"0000000000000000000001\",\"project_name\":\"p\","
        "\"storeys\":{},\"materials\":{},\"elements\":{},\"views\":{\"0000000000000000000002\":{\"id\":\"0000000000000000000002\","
        "\"name\":\"S\",\"kind\":{\"type\":\"section\",\"origin\":[0.0,0.0],\"facing\":\"north\",\"depth_mm\":1000.0},"
        "\"annotations\":[{\"type\":\"dimension\",\"id\":\"0000000000000000000003\",\"a\":{\"at\":[0.0,0.0,0.0],\"feature\":\"origin\"},"
        "\"b\":{\"at\":[1.0,0.0,0.0]},\"offset_mm\":0.0}]}}}}",
        "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"0000000000000000000001\",\"project_name\":\"p\","
        "\"storeys\":{},\"materials\":{},\"elements\":{},\"views\":{\"0000000000000000000002\":{\"id\":\"0000000000000000000002\","
        "\"name\":\"S\",\"kind\":{\"type\":\"section\",\"origin\":[0.0,0.0],\"facing\":\"north\",\"depth_mm\":1000.0},"
        "\"annotations\":[{\"type\":\"dimension\",\"id\":\"0000000000000000000003\",\"a\":{\"at\":[0.0,0.0,0.0],"
        "\"element\":\"0000000000000000000004\",\"feature\":\"top\"},\"b\":{\"at\":[1.0,0.0,0.0]},\"offset_mm\":0.0}]}}}}",
    };
    for (int i = 0; i < 2; i++) {
        cc_model r;
        int rc = cc_model_load(&r, bad_files[i], strlen(bad_files[i]), &e);
        check_true("a bad feature is refused", rc && strstr(e.msg, "feature") != NULL);
        if (!rc || strstr(e.msg, "feature") == NULL) printf("  got: %s\n", rc ? e.msg : "loaded");
        cc_model_free(&r);
    }

    buf_free(&err), buf_free(&j);
    cc_history_free(&h);
    cc_model_free(&m);
}

int main(int argc, char **argv) {
    const char *golden = argc > 1 ? argv[1] : "golden/floats";
    fmt_cases();
    geometry_cases();
    model_cases(golden);
    return report("annotation");
}
