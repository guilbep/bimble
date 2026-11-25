/* Suite draw_annot: a plan's annotations, byte for byte against Rust (golden/drawing/, ticket D3).
 * - plan/<model>/full.svg, bare.svg, a1_50.svg and their .info.txt: plan() of the ground storey
 *   with the default template (and the model's project information): auto paper at 1:100, bare,
 *   A1 at 1:50. Axes through the columns, dimension chains, slab levels and openings, section
 *   labels, the scale bar, level note, legend, frame and title block.
 * - plan/<model>/errors.txt: scale 0, an unknown storey, A4 at 1:1 (the hint needs the axes'
 *   room in the paper's sums).
 * - the parts on their own: letter tags, clusters, cm and section texts, cut labels (rectangle,
 *   circle, timber), beam labels, the scale bar's round length, axes from the model's grids
 *   (golden/grids/model.json, as Rust's scene reads them from grid.ifc), section marks and a
 *   view's own dimension and text.
 * The golden's "Bimble 0.18.1" (Rust's version) is matched by giving the plan Rust's version:
 * o.version is an input, like the project and the date. */
#include <stdint.h>
#include <string.h>
#include "../src/draw_annot.h"
#include "../src/draw_cut.h"
#include "../src/draw_plan.h"
#include "../src/draw_style.h"
#include "../src/file.h"
#include "../src/fmt.h"
#include "../src/json_w.h"
#include "../src/model.h"
#include "../src/paper.h"
#include "../src/scene.h"
#include "check.h"

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

typedef struct {
    cc_model m;
    cc_scene s;
} Loaded;

static int load(const char *file, Loaded *L) {
    size_t n;
    char *text = slurp("golden", file, &n);
    cc_error err;
    int rc = cc_model_load(&L->m, text, n, &err);
    free(text);
    check_true(file, rc == 0);
    if (rc) return -1;
    rc = cc_scene_build(&L->s, &L->m, &err);
    check_true("scene builds", rc == 0);
    return rc ? -1 : 0;
}

static void unload(Loaded *L) {
    cc_scene_free(&L->s);
    cc_model_free(&L->m);
}

static cc_str S(const char *s) {
    cc_str r = {s, strlen(s)};
    return r;
}

/* {:#?} of a PlanInfo. */
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
    buf_puts(b, "    frame: ViewFrame {\n        turn: Plan(\n            ");
    dbg(b, in->frame.turn.a);
    buf_puts(b, ",\n        ),\n        origin: [\n");
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


static int str_is(cc_str s, const char *t) { return s.n == strlen(t) && !memcmp(s.p, t, s.n); }

static int default_template(cc_plan_template *t, const cc_model *m, Arena *a) {
    return cc_plan_template_default(t, a) || cc_plan_template_with_project(t, m->project, m->nproject, a) ? -1 : 0;
}

static void options(cc_plan_options *o, const char *storey, const cc_style *style, const cc_plan_template *tpl) {
    cc_plan_options_init(o);
    o->storey = S(storey);
    o->style = style;
    o->tpl = tpl;
    o->project = S("frame"); /* the dumper's DRAWING_PROJECT and DRAWING_DATE */
    o->date = S("2026-01-01");
    o->version = S(RUST_VERSION);
}

static void test_plan(const char *name, const char *file, const char *storey) {
    Loaded L;
    if (load(file, &L)) return;
    Arena a;
    arena_init(&a, 0);
    cc_style style;
    cc_plan_template tpl;
    check_true("style and template", cc_style_default(&style, &a) == 0 && default_template(&tpl, &L.m, &a) == 0);
    Buf b, err;
    buf_init(&b);
    buf_init(&err);
    static const char *const FILES[3] = {"full", "bare", "a1_50"};
    for (int f = 0; f < 3; f++) {
        cc_plan_options o;
        options(&o, storey, &style, &tpl);
        if (f == 1) o.bare = 1;
        if (f == 2) {
            o.scale = 50;
            o.has_paper = 1;
            o.paper = CC_PAPER_A1;
        }
        cc_paper sheet;
        cc_plan_info info;
        buf_clear(&err);
        int rc = cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
        char path[64];
        snprintf(path, sizeof path, "plan %s/%s draws", name, FILES[f]);
        check_true(path, rc == 0);
        if (rc) {
            printf("  %.*s\n", (int)err.len, err.p);
            continue;
        }
        buf_clear(&b);
        check_true("svg writes", cc_paper_svg(&sheet, &b) == 0);
        snprintf(path, sizeof path, "plan/%s/%s.svg", name, FILES[f]);
        check_buf(path, b.p, b.len, path);
        buf_clear(&b);
        info_debug(&b, &info);
        snprintf(path, sizeof path, "plan/%s/%s.info.txt", name, FILES[f]);
        check_buf(path, b.p, b.len, path);
    }
    /* errors.txt, whole. */
    static const char *const WHAT[3] = {"scale 0", "unknown storey", "A4 at 1:1"};
    buf_clear(&b);
    int all = 1;
    for (int i = 0; i < 3; i++) {
        cc_plan_options o;
        options(&o, storey, &style, &tpl);
        if (i == 0) o.scale = 0;
        if (i == 1) o.storey = S("No such storey");
        if (i == 2) {
            o.scale = 1;
            o.has_paper = 1;
            o.paper = CC_PAPER_A4;
        }
        cc_paper sheet;
        cc_plan_info info;
        buf_clear(&err);
        all &= cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err) == 1;
        buf_puts(&b, WHAT[i]);
        buf_puts(&b, ": ");
        buf_append(&b, err.p, err.len);
        buf_putc(&b, '\n');
    }
    check_true("errors: three", all);
    char path[64];
    snprintf(path, sizeof path, "plan/%s/errors.txt", name);
    check_buf(path, b.p, b.len, path);
    buf_free(&b);
    buf_free(&err);
    arena_free(&a);
    unload(&L);
}

/* ---- the parts ---- */

static int texts_eq(const cc_paper *p, const char *want) {
    int n = 0;
    for (size_t i = 0; i < p->nitems; i++)
        n += p->items[i].kind == CC_ITEM_TEXT && p->items[i].text_len == strlen(want) &&
             !memcmp(p->items[i].text, want, p->items[i].text_len);
    return n;
}

static void buf_is(const char *what, Buf *b, const char *want) {
    check_eq(what, b->p, b->len, want, strlen(want));
    buf_clear(b);
}

/* plan.rs's own tests (section_labels_are_written_in_cm..., letter tags, clusters). */
static void test_texts(void) {
    Buf b;
    buf_init(&b);
    cc_cm(&b, 300.0);
    buf_is("cm(300) = 30", &b, "30");
    cc_cm(&b, 245.0);
    buf_is("cm(245) = 24.5", &b, "24.5");
    cc_cm(&b, -0.2);
    buf_is("cm(-0.2) = 0", &b, "0");
    cc_section_text(&b, CC_CAT_TIMBER, S(" GL24h "), 240.0, 460.0);
    buf_is("timber: GL24h 24x46", &b, "GL24h 24x46");
    cc_section_text(&b, CC_CAT_CONCRETE, S("C30/37"), 300.0, 300.0);
    buf_is("concrete: 30/30", &b, "30/30");
    cc_section_text(&b, CC_CAT_TIMBER, S("   "), 240.0, 460.0);
    buf_is("timber without a name: 24/46", &b, "24/46");
    static const struct {
        size_t i;
        const char *t;
    } TAGS[] = {{0, "A"}, {25, "Z"}, {26, "AA"}, {27, "AB"}, {701, "ZZ"}, {702, "AAA"}};
    for (size_t i = 0; i < sizeof TAGS / sizeof TAGS[0]; i++) {
        cc_letter_tag(&b, TAGS[i].i);
        buf_is(TAGS[i].t, &b, TAGS[i].t);
    }
    double v[7] = {6000.0, 10.0, 0.0, 6030.0, 12000.0, 40.0, 90.0}, out[4];
    size_t n = cc_clusters(v, 7, 50.0, out);
    /* 0, 10, 40, 90 chain within 50 of each other: one cluster; 6000, 6030: one; 12000 alone. */
    check_true("clusters: two", n == 2 && out[0] == (0.0 + 10.0 + 40.0 + 90.0) / 4.0 && out[1] == 6015.0);
    /* Cut labels: a rectangle, a circle of 36 sides, an L (none), a wall (not labelled). */
    double sq[4][2] = {{0, 0}, {300, 0}, {300, 500}, {0, 500}};
    cc_ring r = {sq, 4};
    check_true("column 30/50", cc_cut_label(S("ifccolumn"), CC_CAT_CONCRETE, S(""), &r, 1, &b) == 1);
    buf_is("column 30/50 text", &b, "30/50");
    check_true("wall: no label", cc_cut_label(S("IfcWall"), CC_CAT_CONCRETE, S(""), &r, 1, &b) == 0 && !b.len);
    double ci[36][2];
    for (int i = 0; i < 36; i++) {
        /* cos and sin of multiples of 10°, to 12 digits: the label needs no more. */
        static const double C[10] = {1.0, 0.984807753012, 0.939692620786, 0.866025403784, 0.766044443119,
                                     0.642787609687, 0.5, 0.342020143326, 0.173648177667, 0.0};
        int q = i % 9, quad = i / 9;
        double c = C[q], s_ = C[9 - q];
        double x = quad == 0 ? c : quad == 1 ? -s_ : quad == 2 ? -c : s_;
        double y = quad == 0 ? s_ : quad == 1 ? c : quad == 2 ? -s_ : -c;
        ci[i][0] = 200.0 * x;
        ci[i][1] = 200.0 * y;
    }
    cc_ring rc = {ci, 36};
    check_true("round pile: a label", cc_cut_label(S("IfcPile"), CC_CAT_CONCRETE, S(""), &rc, 1, &b) == 1);
    buf_is("round pile: \xc3\x98" "40", &b, "\xc3\x98" "40");
    double el[6][2] = {{0, 0}, {600, 0}, {600, 200}, {200, 200}, {200, 600}, {0, 600}};
    cc_ring rl = {el, 6};
    check_true("L column: no label", cc_cut_label(S("IfcColumn"), CC_CAT_CONCRETE, S(""), &rl, 1, &b) == 0);
    buf_free(&b);
}

/* columns_and_beams_get_section_labels: the frame at 1:50 on A1. */
static void test_labels(void) {
    Loaded L;
    if (load("frame.json", &L)) return;
    Arena a;
    arena_init(&a, 0);
    cc_style style;
    cc_plan_template tpl;
    check_true("labels: style and template", cc_style_default(&style, &a) == 0 && default_template(&tpl, &L.m, &a) == 0);
    cc_plan_options o;
    options(&o, "Niveau 1", &style, &tpl);
    o.scale = 50;
    o.has_paper = 1;
    o.paper = CC_PAPER_A1;
    cc_paper sheet;
    cc_plan_info info;
    Buf err;
    buf_init(&err);
    int rc = cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
    check_true("labels: plan", rc == 0);
    if (!rc) {
        check_true("12 columns 30/30", texts_eq(&sheet, "30/30") == 12);
        check_true("17 beams 25/50", texts_eq(&sheet, "25/50") == 17);
        int along_y = 0;
        for (size_t i = 0; i < sheet.nitems; i++) {
            const cc_item *it = &sheet.items[i];
            double d = it->angle_deg < 0 ? -it->angle_deg : it->angle_deg;
            along_y += it->kind == CC_ITEM_TEXT && it->text_len == 5 && !memcmp(it->text, "25/50", 5) && d - 90.0 < 1e-6 &&
                       90.0 - d < 1e-6;
        }
        check_true("8 beams along y", along_y == 8);
    }
    tpl.element_labels = 0;
    rc = cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
    check_true("labels off: none", rc == 0 && texts_eq(&sheet, "30/30") + texts_eq(&sheet, "25/50") == 0);
    buf_free(&err);
    arena_free(&a);
    unload(&L);
}

/* Triangles without outline (arrow heads, slab level marks) of a sheet. */
static size_t triangles(const cc_paper *p, const cc_item **out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < p->nitems; i++) {
        const cc_item *it = &p->items[i];
        if (it->kind == CC_ITEM_AREA && !it->has_pen && it->nrings == 1 && it->rings[0].n == 3) {
            if (n < cap) out[n] = it;
            n++;
        }
    }
    return n;
}

/* sections_are_marked_on_plans. */
static void test_section_marks(void) {
    Loaded L;
    if (load("frame.json", &L)) return;
    Arena a;
    arena_init(&a, 0);
    cc_view views[4];
    memset(views, 0, sizeof views);
    const char *names[4] = {"Coupe S1", "Far away", "Plan", "Section 2-b"};
    double ys[4] = {2500.0, 90000.0, 0.0, 1000.0};
    for (int i = 0; i < 4; i++) {
        views[i].name = S(names[i]);
        views[i].kind = i == 2 ? CC_VIEW_PLAN : CC_VIEW_SECTION;
        views[i].origin[0] = 3000.0;
        views[i].origin[1] = ys[i];
        views[i].facing = CC_FACING_NORTH;
    }
    cc_section_mark *marks;
    size_t nm;
    check_true("section marks", cc_section_marks(views, 4, &a, &marks, &nm) == 0 && nm == 3);
    check_true("tags S1, B, 2 (the plan not counted)", str_is(marks[0].tag, "S1") && str_is(marks[1].tag, "B") &&
                                                         str_is(marks[2].tag, "2"));
    cc_style style;
    cc_plan_template tpl;
    check_true("marks: style and template", cc_style_default(&style, &a) == 0 && default_template(&tpl, &L.m, &a) == 0);
    cc_plan_options o;
    options(&o, "Niveau 1", &style, &tpl);
    o.scale = 50;
    o.has_paper = 1;
    o.paper = CC_PAPER_A1;
    cc_paper bare, sheet;
    cc_plan_info info;
    Buf err;
    buf_init(&err);
    int rc = cc_plan(&L.s, &L.m, &o, &a, &bare, &info, &err);
    o.section_marks = marks;
    o.nsection_marks = 2;
    rc |= cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
    check_true("marks: plans", rc == 0);
    if (!rc) {
        check_true("S1 at both ends", texts_eq(&sheet, "S1") == 2);
        check_true("the far section adds no B", texts_eq(&sheet, "B") == texts_eq(&bare, "B"));
        const cc_item *tb[64], *ts[64];
        size_t nb = triangles(&bare, tb, 64), ns = triangles(&sheet, ts, 64);
        check_true("two arrows more", ns == nb + 2 && ns <= 64);
        int up = 1;
        for (size_t i = 0; i < ns && ns <= 64; i++) {
            int old = 0;
            for (size_t j = 0; j < nb; j++) old |= !memcmp(tb[j]->rings[0].p, ts[i]->rings[0].p, 6 * sizeof(double));
            const double(*q)[2] = (const double(*)[2])ts[i]->rings[0].p;
            if (!old) up &= q[0][1] < q[1][1] && q[0][1] < q[2][1];
        }
        check_true("the arrows point north (up the paper)", up);
    }
    tpl.section_markers = 0;
    rc = cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
    check_true("marks off: none", rc == 0 && texts_eq(&sheet, "S1") == 0);
    buf_free(&err);
    arena_free(&a);
    unload(&L);
}

/* plan.rs's opening test: a slab with a 1 x 2 m hole seen below the cut, four columns. */
static void test_openings(void) {
    static const char *const JSON =
        "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"0000000000000000000000\","
        "\"project_name\":\"p\",\"storeys\":{\"0000000000000000000001\":{\"id\":\"0000000000000000000001\","
        "\"name\":\"L0\",\"elevation\":0.0}},\"materials\":{\"0000000000000000000002\":{\"id\":"
        "\"0000000000000000000002\",\"name\":\"C30/37\",\"category\":\"concrete\"}},\"elements\":{"
        "\"0000000000000000000003\":{\"id\":\"0000000000000000000003\",\"class\":\"IfcSlab\",\"name\":\"S\","
        "\"storey\":\"0000000000000000000001\",\"material\":\"0000000000000000000002\",\"placement\":{\"origin\":"
        "[0.0,0.0,-200.0],\"z_axis\":[0.0,0.0,1.0],\"x_axis\":[1.0,0.0,0.0]},\"generator\":{\"kind\":"
        "\"extruded_polygon\",\"outline\":[[0.0,0.0],[6000.0,0.0],[6000.0,4000.0],[0.0,4000.0]],\"voids\":"
        "[[[1000.0,1000.0],[2000.0,1000.0],[2000.0,3000.0],[1000.0,3000.0]]],\"height\":200.0}},"
        "\"0000000000000000000004\":{\"id\":\"0000000000000000000004\",\"class\":\"IfcColumn\",\"name\":\"C1\","
        "\"storey\":\"0000000000000000000001\",\"material\":\"0000000000000000000002\",\"placement\":{\"origin\":"
        "[0.0,0.0,0.0],\"z_axis\":[0.0,0.0,1.0],\"x_axis\":[1.0,0.0,0.0]},\"generator\":{\"kind\":"
        "\"rect_column\",\"width\":300.0,\"depth\":300.0,\"height\":3000.0}},"
        "\"0000000000000000000005\":{\"id\":\"0000000000000000000005\",\"class\":\"IfcColumn\",\"name\":\"C2\","
        "\"storey\":\"0000000000000000000001\",\"material\":\"0000000000000000000002\",\"placement\":{\"origin\":"
        "[6000.0,0.0,0.0],\"z_axis\":[0.0,0.0,1.0],\"x_axis\":[1.0,0.0,0.0]},\"generator\":{\"kind\":"
        "\"rect_column\",\"width\":300.0,\"depth\":300.0,\"height\":3000.0}},"
        "\"0000000000000000000006\":{\"id\":\"0000000000000000000006\",\"class\":\"IfcColumn\",\"name\":\"C3\","
        "\"storey\":\"0000000000000000000001\",\"material\":\"0000000000000000000002\",\"placement\":{\"origin\":"
        "[0.0,4000.0,0.0],\"z_axis\":[0.0,0.0,1.0],\"x_axis\":[1.0,0.0,0.0]},\"generator\":{\"kind\":"
        "\"rect_column\",\"width\":300.0,\"depth\":300.0,\"height\":3000.0}},"
        "\"0000000000000000000007\":{\"id\":\"0000000000000000000007\",\"class\":\"IfcColumn\",\"name\":\"C4\","
        "\"storey\":\"0000000000000000000001\",\"material\":\"0000000000000000000002\",\"placement\":{\"origin\":"
        "[6000.0,4000.0,0.0],\"z_axis\":[0.0,0.0,1.0],\"x_axis\":[1.0,0.0,0.0]},\"generator\":{\"kind\":"
        "\"rect_column\",\"width\":300.0,\"depth\":300.0,\"height\":3000.0}}}}}";
    Loaded L;
    cc_error e;
    int rc = cc_model_load(&L.m, JSON, strlen(JSON), &e);
    check_true("openings: model loads", rc == 0);
    if (rc) return;
    rc = cc_scene_build(&L.s, &L.m, &e);
    check_true("openings: scene", rc == 0);
    Arena a;
    arena_init(&a, 0);
    cc_style style;
    cc_plan_template tpl;
    check_true("openings: style and template", cc_style_default(&style, &a) == 0 && default_template(&tpl, &L.m, &a) == 0);
    cc_plan_options o;
    options(&o, "L0", &style, &tpl);
    o.scale = 50;
    cc_paper sheet;
    cc_plan_info info;
    Buf err;
    buf_init(&err);
    rc = cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
    check_true("openings: plan", rc == 0);
    if (rc) printf("  %.*s\n", (int)err.len, err.p);
    if (!rc) {
        check_true("its size 100x200", texts_eq(&sheet, "100x200") == 1);
        /* 1 m from axis x = 0 to its left side, 1 m from axis y = 0 to its bottom. */
        check_true("its position: 100 twice", texts_eq(&sheet, "100") == 2);
        check_true("four columns 30/30", texts_eq(&sheet, "30/30") == 4);
        check_true("4 axes from the columns", info.axes == 4 && !info.model_axes);
    }
    tpl.opening_dimensions = 0;
    rc = cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
    check_true("opening dimensions off", rc == 0 && texts_eq(&sheet, "100x200") == 0 && texts_eq(&sheet, "100") == 0);
    buf_free(&err);
    arena_free(&a);
    unload(&L);
}

/* Axes from the model's grids (G1): golden/grids/model.json's three grids as Rust's scene reads
 * them from golden/grids/grid.ifc (read.rs grid_axes: grids in file order = by id, each grid's U
 * axes then its V axes, the ends of each IfcPolyline; the grids' placement leaves x, y as they
 * are). Checked by hand against grid.ifc: the same 13 tags in this order, the same ends. */
static void test_grid_axes(void) {
    Loaded L;
    if (load("grids/model.json", &L)) return;
    Arena a;
    arena_init(&a, 0);
    cc_plan_axis *ax;
    size_t n;
    int model;
    double lo[2] = {0, 0}, hi[2] = {1, 1};
    int rc = cc_plan_axes(&L.m, L.s.origin, CC_AXES_MODEL_OR_COLUMNS, NULL, 0, lo, hi, &a, &ax, &n, &model);
    static const char *const TAGS[13] = {"1", "2a", "3", "4", "A", "B", "C", "A'", "1'", "Diag", "S1", "S2", "SA"};
    int ok = rc == 0 && n == 13 && model;
    for (size_t i = 0; ok && i < 13; i++) ok = str_is(ax[i].tag, TAGS[i]);
    check_true("grid axes: 13 in Rust's order", ok);
    check_true("axis 2a: (6000, -1500) to (6000, 11000), scene mm",
               ok && ax[1].a[0] == 6000.0 - L.s.origin[0] && ax[1].a[1] == -1500.0 - L.s.origin[1] &&
                   ax[1].b[0] == 6000.0 - L.s.origin[0] && ax[1].b[1] == 11000.0 - L.s.origin[1]);
    rc = cc_plan_axes(&L.m, L.s.origin, CC_AXES_OFF, NULL, 0, lo, hi, &a, &ax, &n, &model);
    check_true("axes off: none", rc == 0 && n == 0 && !model);
    cc_style style;
    cc_plan_template tpl;
    check_true("grids: style and template", cc_style_default(&style, &a) == 0 && default_template(&tpl, &L.m, &a) == 0);
    cc_plan_options o;
    options(&o, "Niveau 1", &style, &tpl);
    cc_paper sheet;
    cc_plan_info info;
    Buf err;
    buf_init(&err);
    rc = cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
    check_true("grids: the plan draws", rc == 0);
    if (rc) printf("  %.*s\n", (int)err.len, err.p);
    if (!rc) {
        check_true("13 model axes drawn", info.axes == 13 && info.model_axes);
        check_true("bubbles 2a, Diag", texts_eq(&sheet, "2a") == 1 && texts_eq(&sheet, "Diag") == 1);
        int small = 0;
        for (size_t i = 0; i < sheet.nitems; i++)
            small |= sheet.items[i].kind == CC_ITEM_TEXT && sheet.items[i].text_len == 4 &&
                     !memcmp(sheet.items[i].text, "Diag", 4) && sheet.items[i].size_mm == 3.0;
        check_true("a tag over 2 characters is smaller", small);
    }
    tpl.axes = CC_AXES_MODEL;
    rc = cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
    check_true("axes model: the same", rc == 0 && info.axes == 13);
    buf_free(&err);
    arena_free(&a);
    unload(&L);
}

/* A view's own dimension and text (#33), drawn on a plan's paper. */
static void test_view_annotations(void) {
    Loaded L;
    if (load("frame.json", &L)) return;
    Arena a;
    arena_init(&a, 0);
    cc_style style;
    cc_plan_template tpl;
    check_true("view: style and template", cc_style_default(&style, &a) == 0 && default_template(&tpl, &L.m, &a) == 0);
    cc_item_list items = {0};
    cc_annot_ctx c = {&a, &items, &style, &tpl, 100, 20.0, 30.0, 0.01, {-9600.0, -5600.0}, {9600.0, 5600.0}};
    cc_turn turn = {CC_TURN_PLAN, 0.0, {0, 0}};
    cc_annotation an[3];
    memset(an, 0, sizeof an);
    an[0].type = CC_ANN_DIMENSION;
    an[0].a.at[0] = 0.0, an[0].b.at[0] = 6000.0;
    an[0].offset_mm = 1000.0;
    an[0].axis = CC_AXIS_HORIZONTAL;
    an[1] = an[0];
    an[1].has_kind = 1;
    an[1].kind = S("red m");
    /* Its second end on a column's origin (18000, 0): the feature wins over `at`. */
    an[1].b.has_element = 1;
    memcpy(an[1].b.element.c, "08Bn0kkKjBp90qLHF1eSQc", 22); /* a column at (18000, 0) */
    an[1].b.has_feature = 1;
    an[1].b.feature = S("origin");
    an[2].type = CC_ANN_TEXT;
    an[2].text = S("first\nsecond");
    an[2].at[0] = 9000.0, an[2].at[1] = 5000.0;
    an[2].size_mm = 3.5;
    cc_view v;
    memset(&v, 0, sizeof v);
    v.annotations = an;
    v.nannotations = 3;
    int rc = cc_annot_view(&c, &L.m, &v, &turn, L.s.origin);
    check_true("view annotations draw", rc == 0 && items.n == 8);
    if (!rc && items.n == 8) {
        const cc_item *t0 = &items.p[2], *t1 = &items.p[5];
        check_true("dimension in the template's cm", t0->kind == CC_ITEM_TEXT && t0->text_len == 3 && !memcmp(t0->text, "600", 3));
        check_true("its type's text: 18.00 m, red, 2.5 mm",
                   t1->text_len == 7 && !memcmp(t1->text, "18.00 m", 7) && t1->color.c[0] == 230 && t1->size_mm == 2.5);
        check_true("its type's line colour", items.p[3].pen.color.c[0] == 230);
        const cc_item *a1 = &items.p[6], *a2 = &items.p[7];
        double x = 20.0 + (9000.0 - L.s.origin[0] + 9600.0) * 0.01;
        check_true("two lines of text, 1.25 apart", a1->text_len == 5 && a2->text_len == 6 && a1->pos[0] == x &&
                                                        a2->pos[1] == a1->pos[1] + 1.0 * 3.5 * 1.25);
        /* The value is on the line, 10 mm (1000 mm at 1:100) off the points, upright. */
        check_true("value above its line", t0->angle_deg == 0.0 && t0->pos[1] == 30.0 + (5600.0 - (0.0 - L.s.origin[1])) * 0.01 - 10.0 - 0.8);
    }
    free(items.p);
    arena_free(&a);
    unload(&L);
}

/* The scale bar's round length at each standard scale (what Rust writes: Python's log10, floor
 * and powers of ten, glibc's, give the same lengths). */
static void test_scale_bar(void) {
    Arena a;
    arena_init(&a, 0);
    cc_style style;
    cc_plan_template tpl;
    check_true("bar: style and template", cc_style_default(&style, &a) == 0 && cc_plan_template_default(&tpl, &a) == 0);
    static const char *const WANT[CC_NSTANDARD_SCALES] = {"50 mm", "100 mm", "200 mm", "500 mm", "1 m",  "1 m",
                                                          "2 m",   "2 m",    "5 m",    "5 m",    "10 m", "10 m",
                                                          "20 m",  "50 m",   "50 m",   "100 m",  "100 m", "200 m"};
    int ok = 1;
    for (int i = 0; i < CC_NSTANDARD_SCALES; i++) {
        cc_item_list items = {0};
        uint32_t sc = CC_STANDARD_SCALES[i];
        cc_annot_ctx c = {&a, &items, &style, &tpl, sc, 0, 0, 1.0 / (double)sc, {0, 0}, {0, 0}};
        int rc = cc_annot_scale_bar(&c, 420.0, 200.0);
        const cc_item *l = items.n ? &items.p[items.n - 1] : NULL;
        int good = rc == 0 && items.n == 6 && l->text_len == strlen(WANT[i]) && !memcmp(l->text, WANT[i], l->text_len);
        if (!good) printf("  1:%u: got %.*s\n", sc, l ? (int)l->text_len : 0, l ? l->text : "");
        ok &= good;
        free(items.p);
    }
    check_true("scale bar lengths", ok);
    arena_free(&a);
}

int main(void) {
    test_plan("frame", "frame.json", "Niveau 1");
    test_plan("frame2", "frame2.json", "Rez-de-chauss\xc3\xa9" "e");
    test_texts();
    test_labels();
    test_section_marks();
    test_openings();
    test_grid_axes();
    test_view_annotations();
    test_scale_bar();
    return report("draw_annot");
}
