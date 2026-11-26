/* Suite draw_view: any 2D view on paper, byte for byte against Rust (golden/drawing/view/, ticket
 * D4). golden/drawing/model.json holds a plan with its own view range, one dimension and one text,
 * a ceiling plan, sections north (1:50) and east (depth 5 m), a cropped plan and a plan hiding
 * IfcSlab, beside the default views (the 3D view, a plan and a ceiling plan per storey).
 * - view/<nn>.min.svg, <nn>.svg and their .info.txt: plan() of each effective view (number = its
 *   place), minimal template and default template, with the section marks of all views;
 * - view/index.txt: number, id and name of each view, and the error of a view with no drawing (the
 *   3D view, an empty ceiling plan), whole;
 * - the parts: Turn::apply/invert and ViewFrame::to_paper/to_world round trips (viewdraw.rs's
 *   turns_invert), what a view keeps (a_view_leaves_out_what_it_hides), a ceiling plan's
 *   reflected triangles, the range errors (view.rs resolve), blank names, a turned building's
 *   storey plan (a_turned_building_is_drawn_along_its_grid: element boxes against the unturned).
 * The goldens' "Bimble 0.18.1" (Rust's version) is matched by giving the plan Rust's version. */
#include <stdint.h>
#include <string.h>
#include "../session/session.h"
#include "../session/views.h"
#include "../src/draw_annot.h"
#include "../src/draw_cut.h"
#include "../src/draw_plan.h"
#include "../src/draw_style.h"
#include "../src/draw_view.h"
#include "../src/fmt.h"
#include "../src/georef.h"
#include "../src/ifc_math.h"
#include "../src/json_w.h"
#include "../src/model.h"
#include "../src/paper.h"
#include "../src/scene.h"
#include "../src/view_range.h"
#include "check.h"
#include "../host/host.h"

#define G "golden/drawing"
#define RUST_VERSION "0.18.1" /* Cargo.toml's, which made the goldens */

static void check_buf(const char *what, const char *got, size_t got_n, const char *name) {
    size_t n;
    char *want = slurp(G, name, &n);
    if (got_n != n || memcmp(got, want, n)) { /* first difference, for the log */
        size_t i = 0;
        while (i < got_n && i < n && got[i] == want[i]) i++;
        size_t from = i > 60 ? i - 60 : 0;
        printf("FAIL %s: first difference at byte %zu (got %zu bytes, want %zu)\n  got:  %.*s\n  want: %.*s\n", what, i,
               got_n, n, (int)((got_n - from) < 200 ? got_n - from : 200), got + from,
               (int)((n - from) < 200 ? n - from : 200), want + from);
        failed++;
    } else {
        passed++;
    }
    free(want);
}

static void dbg(Buf *b, double x) {
    if (fmt_f64_debug(b, x)) buf_puts(b, x != x ? "NaN" : x > 0 ? "inf" : "-inf");
}

static cc_str S(const char *s) {
    cc_str r = {s, strlen(s)};
    return r;
}

/* {:#?} of a PlanInfo, any turn. */
static void info_debug(Buf *b, const cc_plan_info *in) {
    static const char *const PAPER[5] = {"A4", "A3", "A2", "A1", "A0"};
    buf_puts(b, "PlanInfo {\n    paper: ");
    buf_puts(b, PAPER[in->paper]);
    buf_puts(b, ",\n    portrait: ");
    buf_puts(b, in->portrait ? "true" : "false");
    const struct {
        const char *k;
        size_t v;
    } counts[] = {{"cut_elements", in->cut_elements}, {"seen_elements", in->seen_elements},
                  {"projection_lines", in->projection_lines}, {"above_lines", in->above_lines}, {"axes", in->axes}};
    for (int i = 0; i < 5; i++) {
        buf_puts(b, ",\n    ");
        buf_puts(b, counts[i].k);
        buf_puts(b, ": ");
        buf_u64(b, counts[i].v);
    }
    buf_puts(b, ",\n    model_axes: ");
    buf_puts(b, in->model_axes ? "true" : "false");
    buf_puts(b, ",\n    hidden_elements: ");
    buf_u64(b, in->hidden_elements);
    buf_puts(b, ",\n    legend: [");
    for (size_t i = 0; i < in->nlegend; i++) {
        const cc_legend_entry *e = &in->legend[i];
        buf_puts(b, "\n        (\n            ");
        json_escape(b, e->label.p, e->label.n); /* the labels hold nothing Debug escapes otherwise */
        buf_puts(b, ",\n            ");
        if (e->kind == CC_SWATCH_ABOVE) {
            buf_puts(b, "Above");
        } else {
            buf_puts(b, e->kind == CC_SWATCH_CUT ? "Cut(\n                " : "Seen(\n                ");
            buf_puts(b, cc_category_label(e->cat));
            buf_puts(b, ",\n            )");
        }
        buf_puts(b, ",\n        ),");
    }
    buf_puts(b, in->nlegend ? "\n    ],\n" : "],\n");
    buf_puts(b, "    frame: ViewFrame {\n        turn: ");
    const cc_turn *t = &in->frame.turn;
    if (t->kind == CC_TURN_SECTION) {
        buf_puts(b, "Section(\n            [\n                ");
        dbg(b, t->d[0]);
        buf_puts(b, ",\n                ");
        dbg(b, t->d[1]);
        buf_puts(b, ",\n            ],\n        ),\n");
    } else {
        buf_puts(b, t->kind == CC_TURN_PLAN ? "Plan(\n            " : "Reflect(\n            ");
        dbg(b, t->a);
        buf_puts(b, ",\n        ),\n");
    }
    buf_puts(b, "        origin: [\n");
    for (int i = 0; i < 3; i++) {
        buf_puts(b, "            ");
        dbg(b, in->frame.origin[i]);
        buf_puts(b, ",\n");
    }
    buf_puts(b, "        ],\n");
    const struct {
        const char *k;
        double v;
    } f[] = {{"x0", in->frame.x0}, {"y1", in->frame.y1}, {"k", in->frame.k},
             {"ox", in->frame.ox}, {"oy", in->frame.oy}, {"cut", in->frame.cut}};
    for (int i = 0; i < 6; i++) {
        buf_puts(b, "        ");
        buf_puts(b, f[i].k);
        buf_puts(b, ": ");
        dbg(b, f[i].v);
        buf_puts(b, ",\n");
    }
    buf_puts(b, "    },\n}\n");
}

/* The dumper's min_template: no axes, no annotation, no legend; with the project's values. */
static int min_template(cc_plan_template *t, const cc_model *m, Arena *a) {
    if (cc_plan_template_default(t, a)) return -1;
    t->axes = CC_AXES_OFF;
    t->dimensions = t->slab_levels = t->opening_crosses = t->opening_dimensions = t->element_labels =
        t->section_markers = 0;
    t->legend.show = 0;
    return cc_plan_template_with_project(t, m->project, m->nproject, a);
}

static int default_template(cc_plan_template *t, const cc_model *m, Arena *a) {
    return cc_plan_template_default(t, a) || cc_plan_template_with_project(t, m->project, m->nproject, a) ? -1 : 0;
}

static cc_session *open_model(const char *path) {
    cc_session *s = cc_session_new(cc_platform_native());
    cc_error e;
    if (!s || cc_session_open(s, path, &e)) {
        printf("open %s: %s\n", path, s ? e.msg : "out of memory");
        exit(2);
    }
    return s;
}

/* The dumper's drawing_views: every effective view of model.json, both templates. */
static void test_views(void) {
    cc_session *ses = open_model(G "/model.json");
    const cc_model *m = cc_session_model(ses);
    const cc_scene *s = cc_session_scene(ses);
    check_true("model.json opens with a scene", m && s);
    if (!m || !s) return;
    Arena a;
    arena_init(&a, 0);
    cc_view *views;
    size_t nv;
    cc_section_mark *marks;
    size_t nm;
    cc_style style;
    cc_plan_template tpls[2];
    int ok = cc_sv_list(ses, &a, &views, &nv) == 0 && cc_section_marks(views, nv, &a, &marks, &nm) == 0 &&
             cc_style_default(&style, &a) == 0 && min_template(&tpls[0], m, &a) == 0 &&
             default_template(&tpls[1], m, &a) == 0;
    check_true("views, marks, style and templates", ok);
    if (!ok) return;
    check_true("eleven effective views", nv == 11);
    Buf index, b, err;
    buf_init(&index), buf_init(&b), buf_init(&err);
    static const char *const SUFFIX[2] = {"min.", ""};
    for (size_t i = 0; i < nv; i++) {
        const cc_view *v = &views[i];
        char n[8];
        snprintf(n, sizeof n, "%02zu", i);
        buf_puts(&index, n);
        buf_putc(&index, ' ');
        buf_append(&index, v->id.c, 22);
        buf_putc(&index, ' ');
        json_escape(&index, v->name.p, v->name.n);
        buf_putc(&index, '\n');
        for (int t = 0; t < 2; t++) {
            cc_plan_options o;
            cc_plan_options_init(&o);
            o.scale = v->scale;
            o.project = S("frame"); /* the dumper's DRAWING_PROJECT and DRAWING_DATE */
            o.date = S("2026-01-01");
            o.version = S(RUST_VERSION);
            o.style = &style;
            o.tpl = &tpls[t];
            o.view = v;
            o.section_marks = marks;
            o.nsection_marks = nm;
            cc_paper sheet;
            cc_plan_info info;
            buf_clear(&err);
            int rc = cc_plan(s, m, &o, &a, &sheet, &info, &err);
            char path[64];
            if (rc == 1) {
                buf_puts(&index, n);
                buf_putc(&index, ' ');
                buf_puts(&index, SUFFIX[t]);
                buf_puts(&index, "error: ");
                buf_append(&index, err.p, err.len);
                buf_putc(&index, '\n');
                continue;
            }
            snprintf(path, sizeof path, "view %s %sdraws", n, SUFFIX[t]);
            check_true(path, rc == 0);
            if (rc) continue;
            buf_clear(&b);
            check_true("svg writes", cc_paper_svg(&sheet, &b) == 0);
            snprintf(path, sizeof path, "view/%s.%ssvg", n, SUFFIX[t]);
            check_buf(path, b.p, b.len, path);
            buf_clear(&b);
            info_debug(&b, &info);
            snprintf(path, sizeof path, "view/%s.%sinfo.txt", n, SUFFIX[t]);
            check_buf(path, b.p, b.len, path);
            /* The frame maps back: the paper point of a world point on the cut plane. */
            double paper[2] = {info.frame.ox + 10.0, info.frame.oy + 20.0}, w[3], back[2];
            cc_view_frame_to_world(&info.frame, paper, w);
            cc_view_frame_to_paper(&info.frame, w, back);
            snprintf(path, sizeof path, "view %s %sframe round trip", n, SUFFIX[t]);
            check_true(path, cc_hypot(back[0] - paper[0], back[1] - paper[1]) < 1e-9);
        }
    }
    check_buf("view/index.txt", index.p, index.len, "view/index.txt");
    buf_free(&index), buf_free(&b), buf_free(&err);
    arena_free(&a);
    cc_session_free(ses);
}

/* viewdraw.rs turns_invert. */
static void test_turns(void) {
    cc_turn T[9];
    memset(T, 0, sizeof T);
    T[0].kind = CC_TURN_PLAN, T[0].a = 0.0;
    T[1].kind = CC_TURN_REFLECT, T[1].a = 0.0;
    T[2].kind = CC_TURN_PLAN, T[2].a = 0.6;
    T[3].kind = CC_TURN_REFLECT, T[3].a = -1.2;
    const double D[5][2] = {{0.0, 1.0}, {0.866025403784, 0.5}, {1.0, 0.0}, {0.0, -1.0}, {-1.0, 0.0}};
    for (int i = 0; i < 5; i++) T[4 + i].kind = CC_TURN_SECTION, T[4 + i].d[0] = D[i][0], T[4 + i].d[1] = D[i][1];
    int ok = 1;
    for (int i = 0; i < 9; i++) {
        double p[3] = {1234.0, -567.0, 89.0}, q[3], r[3];
        cc_turn_apply_point(&T[i], p, q);
        cc_turn_invert(&T[i], q, r);
        for (int k = 0; k < 3; k++) ok &= (r[k] - p[k] < 1e-9 && p[k] - r[k] < 1e-9);
    }
    check_true("turns invert", ok);
    /* Plan(0) and Reflect(0) leave x, y exactly; a section looking north lays y down. */
    double p[3] = {1234.5, -567.25, 89.0}, q[3];
    cc_turn_apply_point(&T[1], p, q);
    check_true("Reflect(0): x, y kept, z reflected", q[0] == p[0] && q[1] == p[1] && q[2] == -p[2]);
    cc_turn_apply_point(&T[4], p, q);
    check_true("Section north: x along, z up, -y towards the viewer", q[0] == p[0] && q[1] == p[2] && q[2] == -p[1]);
    cc_view_frame f;
    memset(&f, 0, sizeof f);
    f.turn = T[6]; /* Section([1, 0]) */
    f.origin[0] = 100.0, f.origin[1] = 200.0, f.origin[2] = 300.0;
    f.x0 = -5000.0, f.y1 = 4000.0, f.k = 0.01, f.ox = 20.0, f.oy = 30.0, f.cut = -50.0;
    double paper[2] = {55.0, 66.0}, w[3], back[2];
    cc_view_frame_to_world(&f, paper, w);
    cc_view_frame_to_paper(&f, w, back);
    check_true("frame to_world then to_paper", cc_hypot(back[0] - 55.0, back[1] - 66.0) < 1e-9);
    double d[2] = {3.0, 4.0};
    cc_view_frame g = cc_view_frame_shifted(f, d);
    check_true("frame shifted", g.ox == 23.0 && g.oy == 34.0 && g.x0 == f.x0);
}

static void test_names(void) {
    check_true("blank: empty", cc_view_name_blank(S("")));
    check_true("blank: spaces and a tab", cc_view_name_blank(S(" \t ")));
    check_true("blank: no-break space", cc_view_name_blank(S("\xc2\xa0")));
    check_true("blank: ideographic space", cc_view_name_blank(S("\xe3\x80\x80 ")));
    check_true("not blank", !cc_view_name_blank(S(" A ")));
    check_true("not blank: e acute", !cc_view_name_blank(S("\xc3\xa9")));
}

static cc_view *view_named(cc_view *v, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (v[i].name.n == strlen(name) && !memcmp(v[i].name.p, name, v[i].name.n)) return &v[i];
    return NULL;
}

/* What a view keeps and leaves out; the range's errors; a ceiling plan's triangles. */
static void test_setup(void) {
    cc_session *ses = open_model(G "/model.json");
    const cc_model *m = cc_session_model(ses);
    const cc_scene *s = cc_session_scene(ses);
    if (!m || !s) return;
    Arena a;
    arena_init(&a, 0);
    cc_view *views;
    size_t nv;
    if (cc_sv_list(ses, &a, &views, &nv)) return;
    cc_view *plan = view_named(views, nv, "Plan Rez-de-chauss\xc3\xa9" "e");
    cc_view *ceil = view_named(views, nv, "Ceiling Rez-de-chauss\xc3\xa9" "e");
    cc_view *noslab = view_named(views, nv, "Plan RDC no slabs");
    check_true("the views are there", plan && ceil && noslab);
    if (!plan || !ceil || !noslab) return;
    Buf err;
    buf_init(&err);
    cc_view_setup su;
    /* A plan with nothing hidden is the scene as it is. */
    check_true("plan: set up", cc_view_prepare(s, m, plan, &su, &err) == 0 && !su.mapped && su.scene == s && su.plan_down);
    size_t all = s->nelements;
    cc_view_setup_free(&su);
    /* Hiding IfcSlab leaves the slabs out of the copy. */
    check_true("no slabs: set up", cc_view_prepare(s, m, noslab, &su, &err) == 0 && su.mapped);
    size_t slabs = 0, left = 0;
    for (size_t k = 0; k < s->nelements; k++) slabs += s->elements[k].class_name.n == 7 && !memcmp(s->elements[k].class_name.p, "IfcSlab", 7);
    for (size_t k = 0; k < su.scene->nelements; k++)
        left += su.scene->elements[k].class_name.n == 7 && !memcmp(su.scene->elements[k].class_name.p, "IfcSlab", 7);
    check_true("no slabs: none left, some were there", slabs > 0 && left == 0 && su.scene->nelements < all);
    /* What lies wholly above its range (the upper storey) is not copied (ADR 0046;
     * viewdraw.rs a_plan_leaves_out_what_is_above_its_range). */
    double r[4];
    check_true("no slabs: range", cc_view_resolve(m, noslab, r, NULL) == 0);
    size_t above = 0, above_left = 0;
    for (int pass = 0; pass < 2; pass++) {
        const cc_scene *sc = pass ? su.scene : s;
        for (size_t k = 0; k < sc->nelements; k++) {
            cc_mesh w;
            if (cc_scene_world_mesh(sc, k, &a, &w)) continue;
            double zmin = 1.0 / 0.0;
            for (uint32_t i = 0; i < w.nv; i++) zmin = w.pos[i][2] < zmin ? w.pos[i][2] : zmin;
            if (zmin + s->origin[2] > r[CC_RANGE_TOP] + 1.0) *(pass ? &above_left : &above) += 1;
        }
    }
    check_true("no slabs: what is above its range left out", above > 0 && above_left == 0);
    cc_view_setup_free(&su);
    /* A section with a depth copies nothing wholly beyond it, nor behind its cut. */
    cc_view *east = view_named(views, nv, "Section B-B");
    check_true("east: set up", east && cc_view_prepare(s, m, east, &su, &err) == 0 && su.mapped);
    size_t out = 0, out_left = 0;
    for (int pass = 0; east && pass < 2; pass++) {
        const cc_scene *sc = pass ? su.scene : s;
        for (size_t k = 0; k < sc->nelements; k++) {
            /* the original's points, laid down by the section's turn: z towards the viewer */
            size_t j = 0;
            while (j < s->nelements && memcmp(s->elements[j].id.c, sc->elements[k].id.c, 22)) j++;
            cc_mesh w;
            if (j == s->nelements || cc_scene_world_mesh(s, j, &a, &w)) continue;
            double lo = 1.0 / 0.0, hi = -1.0 / 0.0;
            for (uint32_t i = 0; i < w.nv; i++) {
                double z = -(su.turn.d[0] * w.pos[i][0] + su.turn.d[1] * w.pos[i][1]);
                lo = z < lo ? z : lo, hi = z > hi ? z : hi;
            }
            if (hi < su.level - 1.0 || lo > su.cut + 1.0) *(pass ? &out_left : &out) += 1;
        }
    }
    check_true("east: what is beyond its depth or behind its cut left out", out > 0 && out_left == 0);
    cc_view_setup_free(&su);
    /* Each kind of hide (viewdraw.rs a_view_leaves_out_what_it_hides). */
    const cc_scene_element *e0 = &s->elements[0];
    cc_view v = *plan;
    cc_str id = {e0->id.c, 22};
    v.visibility.hidden_elements.v = &id, v.visibility.hidden_elements.n = 1;
    int hides = cc_view_prepare(s, m, &v, &su, &err) == 0 && su.scene->nelements < all;
    for (size_t k = 0; k < su.scene->nelements; k++) hides &= memcmp(su.scene->elements[k].id.c, e0->id.c, 22) != 0;
    check_true("hidden element left out", hides);
    cc_view_setup_free(&su);
    v = *plan;
    v.visibility.isolated_elements.v = &id, v.visibility.isolated_elements.n = 1;
    check_true("isolated element alone",
               cc_view_prepare(s, m, &v, &su, &err) == 0 && su.scene->nelements <= 1 &&
                   (su.scene->nelements == 0 || !memcmp(su.scene->elements[0].id.c, e0->id.c, 22)));
    cc_view_setup_free(&su);
    v = *plan;
    cc_str fam = S(cc_category_label(e0->category));
    v.visibility.hidden_families.v = &fam, v.visibility.hidden_families.n = 1;
    int nofam = cc_view_prepare(s, m, &v, &su, &err) == 0;
    for (size_t k = 0; k < su.scene->nelements; k++) nofam &= su.scene->elements[k].category != e0->category;
    check_true("hidden family left out", nofam);
    cc_view_setup_free(&su);
    /* A ceiling plan: reflected, its triangles flipped so they keep facing outwards. */
    check_true("ceiling: set up", cc_view_prepare(s, m, ceil, &su, &err) == 0 && su.mapped && su.turn.kind == CC_TURN_REFLECT);
    int flipped = su.scene->nelements > 0;
    for (size_t k = 0; k < su.scene->nelements && flipped; k++) {
        const cc_scene_element *te = &su.scene->elements[k];
        size_t j = 0;
        while (j < s->nelements && memcmp(s->elements[j].id.c, te->id.c, 22)) j++;
        cc_mesh w;
        if (j == s->nelements || cc_scene_world_mesh(s, j, &a, &w)) {
            flipped = 0;
            break;
        }
        const cc_mesh *tm = &te->mesh;
        flipped &= tm->ni == w.ni && tm->nv == w.nv;
        for (uint32_t t = 0; flipped && t + 2 < w.ni; t += 3)
            flipped &= tm->idx[t] == w.idx[t] && tm->idx[t + 1] == w.idx[t + 2] && tm->idx[t + 2] == w.idx[t + 1];
        for (uint32_t i = 0; flipped && i < w.nv; i++)
            flipped &= tm->pos[i][0] == w.pos[i][0] && tm->pos[i][1] == w.pos[i][1] && tm->pos[i][2] == -w.pos[i][2];
    }
    check_true("ceiling: reflected, triangles flipped", flipped);
    cc_view_setup_free(&su);
    /* view.rs resolve's errors, through the drawing. */
    struct {
        int plane;
        cc_level_ref level;
        double offset;
        const char *want;
    } bad[] = {
        {CC_RANGE_CUT, CC_LEVEL_ASSOCIATED, 1.0 / 0.0, "offsets must be numbers within \xc2\xb1" "1000 m"},
        {CC_RANGE_CUT, CC_LEVEL_ASSOCIATED, 2.0e6, "offsets must be numbers within \xc2\xb1" "1000 m"},
        {CC_RANGE_CUT, CC_LEVEL_UNLIMITED, 0.0, "the cut plane needs a level"},
        {CC_RANGE_CUT, CC_LEVEL_ASSOCIATED, -500.0,
         "looking down, the planes must be in order: view depth \xe2\x89\xa4 bottom \xe2\x89\xa4 cut \xe2\x89\xa4 top"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        v = *plan;
        v.range[bad[i].plane].level = bad[i].level;
        v.range[bad[i].plane].offset_mm = bad[i].offset;
        buf_clear(&err);
        int rc = cc_view_prepare(s, m, &v, &su, &err);
        cc_view_setup_free(&su);
        check_true("range error: refused", rc == 1);
        check_eq("range error text", err.p, err.len, bad[i].want, strlen(bad[i].want));
    }
    v = *ceil;
    v.range[CC_RANGE_DEPTH].level = CC_LEVEL_ASSOCIATED;
    buf_clear(&err);
    check_true("ceiling: depth under the top refused", cc_view_prepare(s, m, &v, &su, &err) == 1);
    cc_view_setup_free(&su);
    const char *up = "looking up, the planes must be in order: bottom \xe2\x89\xa4 cut \xe2\x89\xa4 top \xe2\x89\xa4 view depth";
    check_eq("ceiling range error text", err.p, err.len, up, strlen(up));
    v = *plan;
    v.storey = S("0000000000000000000000");
    buf_clear(&err);
    check_true("unknown storey refused", cc_view_prepare(s, m, &v, &su, &err) == 1);
    cc_view_setup_free(&su);
    const char *nost = "view Plan Rez-de-chauss\xc3\xa9" "e: no storey 0000000000000000000000 in this model";
    check_eq("unknown storey text", err.p, err.len, nost, strlen(nost));
    /* The screen's resolve is the same function (session/views.c). */
    double r1[4], r2[4];
    check_true("cc_sv_resolve = cc_view_resolve",
               cc_sv_resolve(m, plan, r1) == 0 && cc_view_resolve(m, plan, r2, NULL) == 0 && !memcmp(r1, r2, sizeof r1));
    buf_free(&err);
    arena_free(&a);
    cc_session_free(ses);
}

static int keep_all(const void *ctx, const cc_scene *s, size_t k) {
    (void)ctx, (void)s, (void)k;
    return 1;
}

/* Each element's plan box (x, y), by its place: the scenes compared keep the same elements. */
static int boxes_near(const cc_scene *a, const cc_scene *b, Arena *ar) {
    if (a->nelements != b->nelements || !a->nelements) return 0;
    for (size_t k = 0; k < a->nelements; k++) {
        if (memcmp(a->elements[k].id.c, b->elements[k].id.c, 22)) return 0;
        float bx[2][4];
        for (int p = 0; p < 2; p++) {
            cc_mesh w;
            if (cc_scene_world_mesh(p ? b : a, k, ar, &w)) return 0;
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
            for (uint32_t i = 0; i < w.nv; i++)
                for (int j = 0; j < 2; j++) {
                    lo[j] = w.pos[i][j] < lo[j] ? w.pos[i][j] : lo[j];
                    hi[j] = w.pos[i][j] > hi[j] ? w.pos[i][j] : hi[j];
                }
            bx[p][0] = lo[0], bx[p][1] = lo[1], bx[p][2] = hi[0], bx[p][3] = hi[1];
        }
        for (int j = 0; j < 4; j++)
            if (bx[0][j] - bx[1][j] > 0.05f || bx[1][j] - bx[0][j] > 0.05f) return 0;
    }
    return 1;
}

/* viewdraw.rs a_turned_building_is_drawn_along_its_grid: a building turned 30 degrees in its
 * site (ADR 0045) is drawn in project north as the unturned one, element by element; in true
 * north it stays turned. A storey plan of it draws (D2 refused it) what the unturned one draws. */
static void test_turned_building(void) {
    cc_session *ses = open_model(G "/model.json");
    const cc_model *m = cc_session_model(ses);
    const cc_scene *s = cc_session_scene(ses);
    if (!m || !s) return;
    Arena a;
    arena_init(&a, 0);
    cc_view *views;
    size_t nv;
    if (cc_sv_list(ses, &a, &views, &nv)) return;
    cc_view *plan = view_named(views, nv, "Plan Rez-de-chauss\xc3\xa9" "e");
    if (!plan) return;
    double ang = 30.0 * 3.14159265358979323846 / 180.0;
    cc_turn back = {CC_TURN_PLAN, -ang, {0, 0}}; /* Turn::Plan turns by the opposite of its angle */
    cc_scene turned;
    check_true("turn the building", cc_view_map_scene(s, keep_all, NULL, &back, 0, &turned) == 0);
    cc_model tm = *m; /* a shallow copy: only the georeference differs */
    tm.georef.rotation_deg = 30.0;
    Buf err;
    buf_init(&err);
    cc_view_setup su;
    /* The whole plain scene against the plan's copy of the turned one: the plan keeps all of
     * the ground storey's range; compare what it kept. */
    int ok = cc_view_prepare(&turned, &tm, plan, &su, &err) == 0 && su.mapped && su.turn.kind == CC_TURN_PLAN &&
             su.turn.a == cc_georef_plan_turn(&tm.georef, 0);
    cc_view_setup plain;
    cc_view pv = *plan;
    cc_str none = {"IfcNothing", 10}; /* any hide maps the plain scene too, with the same range */
    pv.visibility.hidden_classes.v = &none, pv.visibility.hidden_classes.n = 1;
    ok &= cc_view_prepare(s, m, &pv, &plain, &err) == 0;
    check_true("turned: set up", ok);
    check_true("project north: along the grid", ok && boxes_near(su.scene, plain.scene, &a));
    cc_view_setup_free(&su);
    cc_view tn = *plan;
    tn.true_north = 1;
    check_true("true north: set up", cc_view_prepare(&turned, &tm, &tn, &su, &err) == 0);
    check_true("true north: still turned", !boxes_near(su.scene, plain.scene, &a));
    cc_view_setup_free(&su);
    cc_view_setup_free(&plain);
    /* A section looking "north" looks along the building's north. */
    cc_view sec = *plan;
    sec.kind = CC_VIEW_SECTION;
    sec.facing = CC_FACING_NORTH;
    sec.origin[0] = s->origin[0], sec.origin[1] = s->origin[1];
    check_true("section: set up", cc_view_prepare(&turned, &tm, &sec, &su, &err) == 0);
    double sn = cc_sin(ang), cs = cc_cos(ang);
    check_true("section: looks along the building's north", su.turn.kind == CC_TURN_SECTION &&
                                                                 su.turn.d[0] + sn < 1e-12 && -(su.turn.d[0] + sn) < 1e-12 &&
                                                                 su.turn.d[1] - cs < 1e-12 && cs - su.turn.d[1] < 1e-12);
    cc_view_setup_free(&su);
    /* The storey plan of the turned building (no view): drawn, as the plain one. */
    cc_style style;
    cc_plan_template tpl;
    check_true("style, template", cc_style_default(&style, &a) == 0 && min_template(&tpl, m, &a) == 0);
    cc_plan_info info[2];
    for (int p = 0; p < 2; p++) {
        cc_plan_options o;
        cc_plan_options_init(&o);
        o.storey = S("Rez-de-chauss\xc3\xa9" "e");
        o.style = &style;
        o.tpl = &tpl;
        cc_paper sheet;
        buf_clear(&err);
        int rc = cc_plan(p ? &turned : s, p ? &tm : m, &o, &a, &sheet, &info[p], &err);
        check_true(p ? "turned storey plan draws" : "plain storey plan draws", rc == 0);
        if (rc) printf("  %.*s\n", (int)err.len, err.p);
    }
    check_true("turned storey plan: as the plain one",
               info[1].frame.turn.kind == CC_TURN_PLAN && info[1].frame.turn.a == cc_georef_plan_turn(&tm.georef, 0) &&
                   info[0].cut_elements == info[1].cut_elements && info[0].seen_elements == info[1].seen_elements &&
                   info[0].above_lines == info[1].above_lines && info[0].paper == info[1].paper);
    buf_free(&err);
    cc_scene_free(&turned);
    arena_free(&a);
    cc_session_free(ses);
}

int main(void) {
    test_views();
    test_turned_building();
    test_turns();
    test_names();
    test_setup();
    return report("draw_view");
}
