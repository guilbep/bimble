/* Suite draw_plan: the style of drawings and the minimal plan, byte for byte against Rust
 * (golden/drawing/, ticket D2).
 * - style/default.json: GraphicStyle::default().to_json() (draw_style.c's tables).
 * - style/pens.txt: role_pen of each role at each standard scale.
 * - style/template.json: PlanTemplate::default().to_json().
 * - plan/<model>/min.svg, min.info.txt: plan() of the ground storey, bare, minimal template.
 * - plan/<model>/cut.svg and frame.txt: the cut through the frame plan() computed.
 * - the first two lines of plan/<model>/errors.txt (scale 0, unknown storey). */
#include <stdint.h>
#include <string.h>
#include "../src/draw_cut.h"
#include "../src/draw_plan.h"
#include "../src/draw_style.h"
#include "../src/file.h"
#include "../src/fmt.h"
#include "../src/json_w.h"
#include "../src/model.h"
#include "../src/paper.h"
#include "../src/scene.h"
#include "../src/command.h" /* WAVE1B (B1) */
#include "../src/verbs.h"   /* WAVE1B (B1) */
#include "check.h"

#define G "golden/drawing"

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

/* ---- the style ---- */

static void test_style(void) {
    Arena a;
    arena_init(&a, 0);
    cc_style s;
    check_true("style default", cc_style_default(&s, &a) == 0);
    Buf b;
    buf_init(&b);
    check_true("style json", cc_style_json(&s, &b) == 0);
    check_buf("style/default.json", b.p, b.len, "style/default.json");
    /* pens.txt */
    static const uint32_t SCALES[18] = {1, 2, 5, 10, 20, 25, 50, 75, 100, 125, 200, 250, 500, 1000, 1250, 2000, 2500, 5000};
    buf_clear(&b);
    int ok = 1;
    for (int r = 0; r < CC_ROLE_COUNT; r++)
        for (int i = 0; i < 18; i++) {
            cc_pen p;
            if (cc_role_pen(&s, (cc_role)r, SCALES[i], &a, &p)) ok = 0;
            buf_puts(&b, cc_role_name((cc_role)r));
            buf_puts(&b, " 1:");
            buf_u64(&b, SCALES[i]);
            buf_puts(&b, " width ");
            dbg(&b, p.width_mm);
            buf_puts(&b, " color ");
            cc_rgb_hex(&b, p.color);
            buf_puts(&b, " dash [");
            for (size_t k = 0; k < p.ndash; k++) {
                if (k) buf_puts(&b, ", ");
                dbg(&b, p.dash[k]);
            }
            buf_puts(&b, "]\n");
        }
    check_true("role pens", ok);
    check_buf("style/pens.txt", b.p, b.len, "style/pens.txt");
    /* pens.rs's own test: the nearest scale in ratio. */
    float w;
    check_true("model_mm(5, 100) = 0.5", cc_pen_model_mm(&s.paper.pens, 5, 100, &w) && w == 0.5f);
    check_true("model_mm(5, 20) = 0.7", cc_pen_model_mm(&s.paper.pens, 5, 20, &w) && w == 0.7f);
    check_true("model_mm(7, 75) = 1.0 (1:75 nearer 1:100)", cc_pen_model_mm(&s.paper.pens, 7, 75, &w) && w == 1.0f);
    check_true("model_mm(7, 1000) = 0.5", cc_pen_model_mm(&s.paper.pens, 7, 1000, &w) && w == 0.5f);
    check_true("model_mm(17), (0): none", !cc_pen_model_mm(&s.paper.pens, 17, 100, &w) && !cc_pen_model_mm(&s.paper.pens, 0, 100, &w));
    /* f32 shortest digits: a few values Ryu prints. */
    double d;
    check_true("f32 0.1", cc_f32_shortest(0.1f, &d) == 0 && d == 0.1);
    check_true("f32 3.535534", cc_f32_shortest(5.0f / 1.41421356237309504880f, &d) == 0 && d == 3.535534);
    check_true("f32 1e-7", cc_f32_shortest(1e-7f, &d) == 0 && d == 1e-7);
    check_true("f32 16777216", cc_f32_shortest(16777216.0f, &d) == 0 && d == 16777216.0);
    check_true("f32 -2.5", cc_f32_shortest(-2.5f, &d) == 0 && d == -2.5);
    check_true("f32 NaN refused", cc_f32_shortest(0.0f / 0.0f, &d) == -1);
    /* darken, lighten as plan.rs (f32, half away from zero). */
    cc_rgb c = {{214, 214, 210}};
    cc_rgb l = cc_rgb_lighten(c, 0.6f), k = cc_rgb_darken(c, 0.45f);
    check_true("lighten concrete: #efefed", l.c[0] == 239 && l.c[1] == 239 && l.c[2] == 237);
    check_true("darken concrete", k.c[0] == 118 && k.c[2] == 116);
    buf_free(&b);
    arena_free(&a);
}

static void test_template(void) {
    Arena a;
    arena_init(&a, 0);
    cc_plan_template t;
    check_true("template default", cc_plan_template_default(&t, &a) == 0);
    Buf b;
    buf_init(&b);
    check_true("template json", cc_plan_template_json(&t, &b) == 0);
    buf_putc(&b, '\n');
    check_buf("style/template.json", b.p, b.len, "style/template.json");
    check_true("hides IfcSpace, ifcspace", cc_plan_template_hides(&t, "IfcSpace", 8) && cc_plan_template_hides(&t, "ifcspace", 8));
    check_true("does not hide IfcWall", !cc_plan_template_hides(&t, "IfcWall", 7));
    buf_free(&b);
    arena_free(&a);
}


/* ---- plans ---- */

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

/* The minimal template (the dumper's min_template): no axes, no annotation, no legend. */
static int min_template(cc_plan_template *t, const cc_model *m, Arena *a) {
    if (cc_plan_template_default(t, a)) return -1;
    t->axes = CC_AXES_OFF;
    t->dimensions = t->slab_levels = t->opening_crosses = t->opening_dimensions = t->element_labels =
        t->section_markers = 0;
    t->legend.show = 0;
    return cc_plan_template_with_project(t, m->project, m->nproject, a);
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

static cc_pen pen(double w, cc_rgb c, const double *dash, size_t nd) {
    cc_pen p = {w, c, dash, nd};
    return p;
}

/* plan/<model>/cut.svg: one area per element through the frame plan() computed (D1's test read
 * it from frame.txt), and frame.txt itself. */
static void test_cut_svg(const char *name, const Loaded *L, const char *storey, const cc_paper *min,
                         const cc_plan_info *info) {
    const cc_view_frame *f = &info->frame;
    Arena a;
    arena_init(&a, 0);
    size_t ni = 0;
    cc_item *items = calloc(2 * L->s.nelements + 1, sizeof *items);
    static const double thin_dash[2] = {1.0, 0.5};
    int ok = 1;
    for (size_t k = 0; k < L->s.nelements && ok; k++) {
        cc_mesh m;
        cc_section sec;
        if (cc_scene_world_mesh(&L->s, k, &a, &m) || cc_cut_section(&m, f->cut, &a, &sec)) {
            ok = 0;
            break;
        }
        for (int pass = 0; pass < 2; pass++) {
            cc_ring *src = pass ? sec.open : sec.loops;
            size_t nr = pass ? sec.nopen : sec.nloops;
            if (!nr) continue;
            cc_ring *r = arena_alloc(&a, nr * sizeof *r, _Alignof(cc_ring));
            for (size_t i = 0; i < nr; i++) {
                r[i].n = src[i].n;
                r[i].p = arena_alloc(&a, src[i].n * sizeof *r[i].p, _Alignof(double));
                for (size_t j = 0; j < src[i].n; j++) {
                    double w[3] = {src[i].p[j][0] + L->s.origin[0], src[i].p[j][1] + L->s.origin[1], f->cut + L->s.origin[2]};
                    cc_view_frame_to_paper(f, w, r[i].p[j]);
                }
            }
            cc_item *it = &items[ni++];
            it->kind = pass ? CC_ITEM_LINES : CC_ITEM_AREA;
            it->rings = r;
            it->nrings = nr;
            it->has_pen = 1;
            it->pen = pass ? pen(strtod("0.18", NULL), (cc_rgb){{200, 30, 30}}, thin_dash, 2)
                           : pen(strtod("0.35", NULL), (cc_rgb){{0, 0, 0}}, NULL, 0);
        }
    }
    check_true("cut.svg: every element", ok);
    char title[128];
    snprintf(title, sizeof title, "Cut %s", storey);
    cc_paper p = {min->width_mm, min->height_mm, title, strlen(title), items, ni, min->font};
    Buf b;
    buf_init(&b);
    check_true("cut.svg writes", cc_paper_svg(&p, &b) == 0);
    char path[64];
    snprintf(path, sizeof path, "plan/%s/cut.svg", name);
    check_buf(path, b.p, b.len, path);
    /* frame.txt */
    buf_clear(&b);
    buf_puts(&b, "sheet ");
    dbg(&b, min->width_mm);
    buf_putc(&b, ' ');
    dbg(&b, min->height_mm);
    buf_puts(&b, "\nturn Plan(");
    dbg(&b, f->turn.a);
    buf_puts(&b, ")\norigin ");
    for (int i = 0; i < 3; i++) {
        if (i) buf_putc(&b, ' ');
        dbg(&b, f->origin[i]);
    }
    const struct {
        const char *k;
        double v;
    } kv[] = {{"x0", f->x0}, {"y1", f->y1}, {"k", f->k}, {"ox", f->ox}, {"oy", f->oy}, {"cut", f->cut}};
    for (int i = 0; i < 6; i++) {
        buf_putc(&b, '\n');
        buf_puts(&b, kv[i].k);
        buf_putc(&b, ' ');
        dbg(&b, kv[i].v);
    }
    buf_putc(&b, '\n');
    snprintf(path, sizeof path, "plan/%s/frame.txt", name);
    check_buf(path, b.p, b.len, path);
    buf_free(&b);
    free(items);
    arena_free(&a);
}

static void test_plan(const char *name, const char *file, const char *storey) {
    Loaded L;
    if (load(file, &L)) return;
    Arena a;
    arena_init(&a, 0);
    cc_style style;
    cc_plan_template tpl;
    check_true("style and template", cc_style_default(&style, &a) == 0 && min_template(&tpl, &L.m, &a) == 0);
    cc_plan_options o;
    cc_plan_options_init(&o);
    o.storey = S(storey);
    o.project = S("frame");
    o.date = S("2026-01-01");
    o.style = &style;
    o.tpl = &tpl;
    o.bare = 1;
    cc_paper sheet;
    cc_plan_info info;
    Buf err, b;
    buf_init(&err);
    buf_init(&b);
    int rc = cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
    char path[64];
    snprintf(path, sizeof path, "plan %s: the minimal plan draws", name);
    check_true(path, rc == 0);
    if (rc) printf("  %.*s\n", (int)err.len, err.p);
    if (!rc) {
        check_true("min.svg writes", cc_paper_svg(&sheet, &b) == 0);
        snprintf(path, sizeof path, "plan/%s/min.svg", name);
        check_buf(path, b.p, b.len, path);
        buf_clear(&b);
        info_debug(&b, &info);
        snprintf(path, sizeof path, "plan/%s/min.info.txt", name);
        check_buf(path, b.p, b.len, path);
        test_cut_svg(name, &L, storey, &sheet, &info);
    }
    /* errors.txt's first two lines (the third, a paper too small, needs D3's axes). */
    cc_plan_template full;
    check_true("default template", cc_plan_template_default(&full, &a) == 0 &&
                                       cc_plan_template_with_project(&full, L.m.project, L.m.nproject, &a) == 0);
    snprintf(path, sizeof path, "plan/%s/errors.txt", name);
    size_t n;
    char *want = slurp(G, path, &n);
    o.tpl = &full;
    o.bare = 0;
    o.scale = 0;
    buf_clear(&b);
    buf_clear(&err);
    rc = cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
    buf_puts(&b, "scale 0: ");
    buf_append(&b, err.p, err.len);
    buf_putc(&b, '\n');
    o.scale = 100;
    o.storey = S("No such storey");
    buf_clear(&err);
    int rc2 = cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
    buf_puts(&b, "unknown storey: ");
    buf_append(&b, err.p, err.len);
    buf_putc(&b, '\n');
    check_true("errors: scale 0, unknown storey", rc == 1 && rc2 == 1);
    check_eq(path, b.p, b.len, want, b.len <= n ? b.len : n);
    free(want);
    buf_free(&err);
    buf_free(&b);
    arena_free(&a);
    unload(&L);
}

/* A synthetic model: two pad footings below the cut, the first by id 0.5 mm higher than the second,
 * whose top is 0.5 mm under the storey level. Both are seen (plan.rs takes what reaches level - 1);
 * the lower is painted first (painter's order, stable by top height); the drawing is narrower
 * than its title, so the bare page ends PAD after the title. */
static void test_plan_synthetic(void) {
    static const char *const JSON =
        "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"0000000000000000000000\","
        "\"project_name\":\"p\",\"storeys\":{\"0000000000000000000001\":{\"id\":\"0000000000000000000001\","
        "\"name\":\"L0\",\"elevation\":0.0},\"0000000000000000000005\":{\"id\":\"0000000000000000000005\","
        "\"name\":\"L1\",\"elevation\":5000.0}},\"materials\":{\"0000000000000000000002\":{\"id\":"
        "\"0000000000000000000002\",\"name\":\"C30/37\",\"category\":\"concrete\"}},\"elements\":{"
        "\"0000000000000000000003\":{\"id\":\"0000000000000000000003\",\"class\":\"IfcFooting\",\"name\":\"A\","
        "\"storey\":\"0000000000000000000001\",\"material\":\"0000000000000000000002\",\"placement\":{\"origin\":"
        "[0.0,0.0,-300.0],\"z_axis\":[0.0,0.0,1.0],\"x_axis\":[1.0,0.0,0.0]},\"generator\":{\"kind\":"
        "\"pad_footing\",\"width\":500.0,\"depth\":500.0,\"height\":300.0}},"
        "\"0000000000000000000004\":{\"id\":\"0000000000000000000004\",\"class\":\"IfcFooting\",\"name\":\"B\","
        "\"storey\":\"0000000000000000000001\",\"material\":\"0000000000000000000002\",\"placement\":{\"origin\":"
        "[600.0,0.0,-300.5],\"z_axis\":[0.0,0.0,1.0],\"x_axis\":[1.0,0.0,0.0]},\"generator\":{\"kind\":"
        "\"pad_footing\",\"width\":500.0,\"depth\":500.0,\"height\":300.0}}}}}";
    Loaded L;
    cc_error e;
    int rc = cc_model_load(&L.m, JSON, strlen(JSON), &e);
    check_true("synthetic model loads", rc == 0);
    if (rc) return;
    rc = cc_scene_build(&L.s, &L.m, &e);
    check_true("synthetic scene", rc == 0 && L.s.nelements == 2);
    Arena a;
    arena_init(&a, 0);
    cc_style style;
    cc_plan_template tpl;
    check_true("style and template", cc_style_default(&style, &a) == 0 && min_template(&tpl, &L.m, &a) == 0);
    cc_plan_options o;
    cc_plan_options_init(&o);
    o.storey = S("l0"); /* found ignoring ASCII case */
    o.style = &style;
    o.tpl = &tpl;
    o.bare = 1;
    cc_paper sheet;
    cc_plan_info info;
    Buf err;
    buf_init(&err);
    rc = cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
    check_true("synthetic plan draws", rc == 0);
    if (rc) printf("  %.*s\n", (int)err.len, err.p);
    if (!rc) {
        check_true("both footings seen (the one 0.5 mm under the level too)", info.seen_elements == 2 && info.cut_elements == 0);
        /* The seen tops are the filled areas; the first painted is the lower one (x from 600). */
        const cc_item *first = NULL;
        for (size_t i = 0; i < sheet.nitems && !first; i++)
            if (sheet.items[i].kind == CC_ITEM_AREA && sheet.items[i].has_fill) first = &sheet.items[i];
        double x0 = info.frame.ox + (600.0 - 250.0 - L.s.origin[0] - info.frame.x0) * info.frame.k;
        int lower_first = 0;
        if (first)
            for (size_t j = 0; j < first->rings[0].n; j++) lower_first |= first->rings[0].p[j][0] >= x0 - 1e-9;
        check_true("painter's order: the lower top first", lower_first);
        double tw = cc_text_width_mm("Plan L0, 1:100", 14, 5.0, 1, CC_FONT_ROBOTO);
        check_true("a bare page reaches PAD past its title", sheet.width_mm == info.frame.ox + tw + 5.0);
        check_true("the sheet title without a project", sheet.title_len == 14 && !memcmp(sheet.title, "Plan L0, 1:100", 14));
    }
    /* Nothing to draw: the storey above, empty. */
    buf_clear(&err);
    o.storey = S("L1");
    rc = cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err);
    const char *want = "nothing to draw at storey L1 (cut at +6.200 m)";
    check_true("nothing to draw", rc == 1);
    check_eq("nothing to draw: text", err.p, err.len, want, strlen(want));
    buf_clear(&err);
    o.cut_above_mm = 1.0 / 0.0;
    check_true("cut height not finite", cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err) == 1);
    buf_free(&err);
    arena_free(&a);
    unload(&L);
}

/* WAVE1B (B1): a sleeve of 300 in the slab D1 (Niveau 1, seen from above in the plan of Niveau 2):
 * with the "Openings" toggle (opening_dimensions) its round hole gets a cross of two diameters at
 * 45 degrees (two more line rings, each 300 mm long on the paper's scale); without it, none. */
static size_t line_rings(const cc_paper *p, double *len_mm) {
    size_t n = 0;
    for (size_t i = 0; i < p->nitems; i++)
        if (p->items[i].kind == CC_ITEM_LINES)
            for (size_t r = 0; r < p->items[i].nrings; r++) {
                n++;
                const cc_ring *g = &p->items[i].rings[r];
                if (len_mm && g->n == 2) {
                    double dx = g->p[1][0] - g->p[0][0], dy = g->p[1][1] - g->p[0][1];
                    double l = dx * dx + dy * dy;
                    if (l > *len_mm) *len_mm = l;
                }
            }
    return n;
}
static void test_sleeve_symbol(void) {
    Loaded L;
    if (load("frame.json", &L)) return;
    cc_gid slab;
    int found = 0;
    for (size_t i = 0; i < L.m.nelements && !found; i++)
        if (L.m.elements[i].name.n == 2 && !memcmp(L.m.elements[i].name.p, "D1", 2)) slab = L.m.elements[i].id, found = 1;
    check_true("B1 the slab D1", found);
    if (!found) return unload(&L);
    char args[160];
    snprintf(args, sizeof args, "{\"host\":\"%.22s\",\"a\":[5000,5000],\"shape\":\"round\",\"diameter\":300}", slab.c);
    cc_cmds l;
    cc_cmds_init(&l);
    Buf err;
    buf_init(&err);
    int ok = !cc_verb_run_ex(&L.m, "opening_add", args, strlen(args), 0, -1, 0, &l, &err);
    for (size_t i = 0; ok && i < l.n; i++) {
        cc_command inv;
        ok = !cc_command_apply(&L.m, &l.v[i], &inv, &err);
    }
    cc_cmds_free(&l);
    check_true("B1 a sleeve of 300 in D1", ok);
    cc_error e;
    cc_scene_free(&L.s);
    ok = ok && !cc_scene_build(&L.s, &L.m, &e);
    Arena a;
    arena_init(&a, 0);
    cc_style style;
    cc_plan_template tpl;
    size_t n[2] = {0, 0};
    double len = 0;
    for (int on = 0; ok && on < 2; on++) {
        ok = !cc_style_default(&style, &a) && !min_template(&tpl, &L.m, &a);
        tpl.opening_dimensions = on;
        cc_plan_options o;
        cc_plan_options_init(&o);
        o.storey = S("Niveau 2"), o.project = S("frame"), o.date = S("2026-01-01"), o.style = &style, o.tpl = &tpl, o.bare = 1;
        cc_paper sheet;
        cc_plan_info info;
        buf_clear(&err);
        ok = ok && cc_plan(&L.s, &L.m, &o, &a, &sheet, &info, &err) == 0;
        if (ok) n[on] = line_rings(&sheet, on ? &len : NULL);
        if (ok && on) printf("  B1 plan of Niveau 2 at 1:%u: %zu line rings with the toggle, %zu without\n", (unsigned)o.scale, n[1], n[0]);
    }
    check_true("B1 the sleeve's cross: two more lines with the Openings toggle", ok && n[1] == n[0] + 2);
    buf_free(&err);
    arena_free(&a);
    unload(&L);
}

int main(void) {
    test_sleeve_symbol(); /* WAVE1B (B1) */
    test_style();
    test_template();
    test_plan("frame", "frame.json", "Niveau 1");
    test_plan("frame2", "frame2.json", "Rez-de-chauss\xc3\xa9" "e");
    test_plan_synthetic();
    return report("draw_plan");
}
