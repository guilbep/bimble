/* Suite draw_sheet: sheets on paper, byte for byte against Rust (golden/drawing/sheet/, ticket D5).
 * golden/drawing/model.json holds sheet S-101 (A3: the plan with its own range at 1:100, the north
 * section at 1:50, a text, the field `revision`) and S-102 (A4 portrait placing the 3D view, which
 * has no paper drawing: a note).
 * - sheet/<number>.svg and .info.txt: sheetdraw::draw of each sheet (the dumper's sheet_options:
 *   default style in Roboto, the default template with the model's project information, project
 *   "frame", date 2026-01-01), the SheetInfo as {:#?};
 * - the parts: the effective views the library lists are the session's (view_effective.h, moved
 *   out of session/views.c), a sheet's errors (Sheet::validate's texts), the texts of a sheet
 *   (lines, text types), the merged legend once, the title, cc_sheet_scales.
 * The goldens' "Bimble 0.18.1" (Rust's version) is matched by giving the sheet Rust's version. */
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../session/calls.h"
#include "../session/sheet_svg.h"
#include "../session/session.h"
#include "../session/views.h"
#include "../src/draw_plan.h"
#include "../src/draw_sheet.h"
#include "../src/draw_style.h"
#include "../src/fmt.h"
#include "../src/ifc_out.h" /* CC_IFC_VERSION */
#include "../src/model.h"
#include "../src/paper.h"
#include "../src/view_effective.h"
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

static cc_str S(const char *s) {
    cc_str r = {s, strlen(s)};
    return r;
}

static void dbg(Buf *b, double x) {
    if (fmt_f64_debug(b, x)) buf_puts(b, x != x ? "NaN" : x > 0 ? "inf" : "-inf");
}

static void indent(Buf *b, int n) {
    for (int i = 0; i < n; i++) buf_putc(b, ' ');
}

/* {:?} of a String: quotes, backslashes and control characters escaped (the texts here hold no
 * other character Debug escapes). */
static void dbg_str(Buf *b, cc_str s) {
    buf_putc(b, '"');
    for (size_t i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c == '"' || c == '\\') buf_putc(b, '\\'), buf_putc(b, c);
        else if (c == '\n') buf_puts(b, "\\n");
        else if (c == '\t') buf_puts(b, "\\t");
        else if (c == '\r') buf_puts(b, "\\r");
        else buf_putc(b, c);
    }
    buf_putc(b, '"');
}

/* A list of f64 in {:#?}, at depth d (its items at d + 4). */
static void dbg_list(Buf *b, const double *v, size_t n, int d) {
    buf_puts(b, "[\n");
    for (size_t i = 0; i < n; i++) indent(b, d + 4), dbg(b, v[i]), buf_puts(b, ",\n");
    indent(b, d), buf_putc(b, ']');
}

/* {:#?} of a SheetInfo. */
static void info_debug(Buf *b, const cc_sheet_info *in) {
    buf_puts(b, "SheetInfo {\n    views_drawn: ");
    buf_u64(b, in->views_drawn);
    buf_puts(b, ",\n    problems: [");
    for (size_t i = 0; i < in->nproblems; i++) {
        buf_puts(b, "\n        ");
        dbg_str(b, in->problems[i]);
        buf_putc(b, ',');
    }
    buf_puts(b, in->nproblems ? "\n    ],\n" : "],\n");
    buf_puts(b, "    placed: [");
    for (size_t i = 0; i < in->nplaced; i++) {
        const cc_placed_info *p = &in->placed[i];
        buf_puts(b, "\n        PlacedInfo {\n            id: GlobalId(");
        buf_append(b, p->id.c, 22);
        buf_puts(b, "),\n            view: GlobalId(");
        buf_append(b, p->view.c, 22);
        buf_puts(b, "),\n            rect: ");
        dbg_list(b, p->rect, 4, 12);
        buf_puts(b, ",\n            frame: ViewFrame {\n                turn: ");
        const cc_turn *t = &p->frame.turn;
        if (t->kind == CC_TURN_SECTION) {
            buf_puts(b, "Section(\n                    ");
            dbg_list(b, t->d, 2, 20);
            buf_puts(b, ",\n                ),\n");
        } else {
            buf_puts(b, t->kind == CC_TURN_PLAN ? "Plan(\n                    " : "Reflect(\n                    ");
            dbg(b, t->a);
            buf_puts(b, ",\n                ),\n");
        }
        buf_puts(b, "                origin: ");
        dbg_list(b, p->frame.origin, 3, 16);
        buf_puts(b, ",\n");
        const struct {
            const char *k;
            double v;
        } f[] = {{"x0", p->frame.x0}, {"y1", p->frame.y1}, {"k", p->frame.k},
                 {"ox", p->frame.ox}, {"oy", p->frame.oy}, {"cut", p->frame.cut}};
        for (int k = 0; k < 6; k++) {
            indent(b, 16);
            buf_puts(b, f[k].k);
            buf_puts(b, ": ");
            dbg(b, f[k].v);
            buf_puts(b, ",\n");
        }
        buf_puts(b, "            },\n            segments: [");
        for (size_t k = 0; k < p->nsegments; k++) {
            buf_puts(b, "\n                [\n                    ");
            dbg_list(b, p->segments[k][0], 2, 20);
            buf_puts(b, ",\n                    ");
            dbg_list(b, p->segments[k][1], 2, 20);
            buf_puts(b, ",\n                ],");
        }
        buf_puts(b, p->nsegments ? "\n            ],\n        }," : "],\n        },");
    }
    buf_puts(b, in->nplaced ? "\n    ],\n}\n" : "],\n}\n");
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

/* The dumper's sheet_options (font Roboto, the default's). */
static int options(cc_sheet_options *o, cc_style *style, cc_plan_template *tpl, const cc_model *m, Arena *a) {
    if (cc_style_default(style, a) || cc_plan_template_default(tpl, a) ||
        cc_plan_template_with_project(tpl, m->project, m->nproject, a))
        return -1;
    style->paper.font = CC_FONT_ROBOTO;
    memset(o, 0, sizeof *o);
    o->style = style;
    o->tpl = tpl;
    o->project = S("frame"); /* the dumper's DRAWING_PROJECT and DRAWING_DATE */
    o->date = S("2026-01-01");
    o->version = S(RUST_VERSION);
    return 0;
}

static size_t count_text(const cc_paper *p, const char *t) {
    size_t n = 0, tn = strlen(t);
    for (size_t i = 0; i < p->nitems; i++)
        n += p->items[i].kind == CC_ITEM_TEXT && p->items[i].text_len == tn && !memcmp(p->items[i].text, t, tn);
    return n;
}

static const cc_item *find_text(const cc_paper *p, const char *t) {
    size_t tn = strlen(t);
    for (size_t i = 0; i < p->nitems; i++)
        if (p->items[i].kind == CC_ITEM_TEXT && p->items[i].text_len == tn && !memcmp(p->items[i].text, t, tn))
            return &p->items[i];
    return NULL;
}

/* The dumper's drawing_sheets: every sheet of model.json. */
static void test_sheets(void) {
    cc_session *ses = open_model(G "/model.json");
    const cc_model *m = cc_session_model(ses);
    const cc_scene *s = cc_session_scene(ses);
    check_true("model.json opens with a scene", m && s);
    if (!m || !s) return;
    Arena a;
    arena_init(&a, 0);
    cc_style style;
    cc_plan_template tpl;
    cc_sheet_options o;
    check_true("options", options(&o, &style, &tpl, m, &a) == 0);
    check_true("two sheets", m->nsheets == 2);
    Buf b, err;
    buf_init(&b), buf_init(&err);
    for (size_t i = 0; i < m->nsheets; i++) {
        const cc_sheet *def = &m->sheets[i];
        cc_paper paper;
        cc_sheet_info info;
        buf_clear(&err);
        int rc = cc_sheet_draw(s, m, def, &o, &a, &paper, &info, &err);
        char path[96];
        snprintf(path, sizeof path, "sheet %.*s draws", (int)def->number.n, def->number.p);
        check_true(path, rc == 0);
        if (rc) {
            printf("  %d %.*s\n", rc, (int)err.len, err.p);
            continue;
        }
        buf_clear(&b);
        check_true("svg writes", cc_paper_svg(&paper, &b) == 0);
        snprintf(path, sizeof path, "sheet/%.*s.svg", (int)def->number.n, def->number.p);
        check_buf(path, b.p, b.len, path);
        buf_clear(&b);
        info_debug(&b, &info);
        snprintf(path, sizeof path, "sheet/%.*s.info.txt", (int)def->number.n, def->number.p);
        check_buf(path, b.p, b.len, path);
        /* One title block and at most one legend on every sheet. */
        check_true("one title block", count_text(&paper, "PROJECT") == 1);
        check_true("at most one legend", count_text(&paper, "Legend") <= 1);
        if (def->number.n == 5 && !memcmp(def->number.p, "S-101", 5)) {
            check_true("S-101: two views drawn, one legend", info.views_drawn == 2 && count_text(&paper, "Legend") == 1);
            check_true("S-101: the title", paper.title_len == strlen("frame \xe2\x80\x93 S-101 ") + def->name.n &&
                                                !memcmp(paper.title, "frame \xe2\x80\x93 S-101 ", 13));
            /* Each placed view's segments lie in its box. */
            int in = 1;
            for (size_t k = 0; k < info.nplaced; k++) {
                const cc_placed_info *pl = &info.placed[k];
                in &= pl->nsegments > 0;
                for (size_t q = 0; q < pl->nsegments; q++)
                    for (int e = 0; e < 2; e++)
                        in &= pl->segments[q][e][0] >= pl->rect[0] - 1e-9 && pl->segments[q][e][0] <= pl->rect[2] + 1e-9 &&
                              pl->segments[q][e][1] >= pl->rect[1] - 1e-9 && pl->segments[q][e][1] <= pl->rect[3] + 1e-9;
            }
            check_true("S-101: segments inside their boxes", in);
        } else {
            check_true("S-102: the 3D view is a note", info.views_drawn == 0 && info.nproblems == 1 && info.nplaced == 0 &&
                                                         find_text(&paper, "3D: view 3D: a 3D view has no paper drawing"));
        }
    }
    buf_free(&b), buf_free(&err);
    arena_free(&a);
    cc_session_free(ses);
}

/* The library's effective views are the session's (the list moved out of session/views.c). */
static void test_effective(void) {
    cc_session *ses = open_model(G "/model.json");
    const cc_model *m = cc_session_model(ses);
    Arena a;
    arena_init(&a, 0);
    cc_view *sv, *lv;
    size_t ns, nl;
    int ok = cc_sv_list(ses, &a, &sv, &ns) == 0 && cc_views_effective(m, &a, &lv, &nl) == 0 && ns == nl && ns == 11;
    for (size_t i = 0; ok && i < ns; i++) ok = !cc_gid_cmp(&sv[i].id, &lv[i].id);
    check_true("effective views: the session's", ok);
    ok = cc_views_effective(NULL, &a, &lv, &nl) == 0 && nl == 1 && lv[0].kind == CC_VIEW_THREE_D;
    check_true("no model: the 3D view only", ok);
    arena_free(&a);
    cc_session_free(ses);
}

/* Errors, texts, the legend once, scales: a sheet built here from S-101. */
static void test_parts(void) {
    cc_session *ses = open_model(G "/model.json");
    const cc_model *m = cc_session_model(ses);
    const cc_scene *s = cc_session_scene(ses);
    Arena a;
    arena_init(&a, 0);
    cc_style style;
    cc_plan_template tpl;
    cc_sheet_options o;
    check_true("options", options(&o, &style, &tpl, m, &a) == 0);
    const cc_sheet *s101 = NULL;
    for (size_t i = 0; i < m->nsheets; i++)
        if (m->sheets[i].number.n == 5 && !memcmp(m->sheets[i].number.p, "S-101", 5)) s101 = &m->sheets[i];
    check_true("S-101 found", s101 && s101->nplaced == 2);
    if (!s101) return;
    Buf err;
    buf_init(&err);
    cc_paper paper;
    cc_sheet_info info;

    /* A paper not known: Sheet::validate's text. */
    cc_sheet d = *s101;
    d.paper = S("B5");
    int rc = cc_sheet_draw(s, m, &d, &o, &a, &paper, &info, &err);
    check_true("unknown paper: an error", rc == 1);
    const char *want = "paper \"B5\": one of A4, A3, A2, A1, A0";
    check_true("unknown paper: its text",
               err.len > strlen(want) && !memcmp(err.p + err.len - strlen(want), want, strlen(want)) &&
                   !memcmp(err.p, "sheet S-101 ", 12));
    /* A placed view not in the model. */
    cc_placed pl[1] = {s101->placed[0]};
    memcpy(pl[0].view.c, "0000000000000000000000", 22);
    d = *s101;
    d.placed = pl, d.nplaced = 1;
    buf_clear(&err);
    rc = cc_sheet_draw(s, m, &d, &o, &a, &paper, &info, &err);
    check_true("unknown view: an error", rc == 1 && err.len > 0);

    /* The same view twice: drawn twice, one legend; texts on several lines and with a type. */
    cc_placed two[2] = {s101->placed[0], s101->placed[0]};
    two[1].at_mm[0] += 150.0;
    cc_sheet_text texts[2];
    memset(texts, 0, sizeof texts);
    texts[0].text = S("Line one\nLine two");
    texts[0].at_mm[0] = 20.0, texts[0].at_mm[1] = 30.0;
    texts[0].size_mm = 4.0;
    texts[1].text = S("Typed");
    texts[1].at_mm[0] = 20.0, texts[1].at_mm[1] = 60.0;
    texts[1].size_mm = 9.0;
    texts[1].has_kind = 1;
    texts[1].kind = style.paper.ntext_types ? style.paper.text_types[0].name : S("none");
    d = *s101;
    d.placed = two, d.nplaced = 2;
    d.texts = texts, d.ntexts = 2;
    buf_clear(&err);
    rc = cc_sheet_draw(s, m, &d, &o, &a, &paper, &info, &err);
    check_true("twice: draws", rc == 0 && info.views_drawn == 2 && info.nplaced == 2);
    if (rc == 0) {
        check_true("twice: one legend", count_text(&paper, "Legend") == 1);
        const cc_item *l1 = find_text(&paper, "Line one"), *l2 = find_text(&paper, "Line two");
        check_true("text lines: 1.25 x size apart",
                   l1 && l2 && l1->pos[1] == 30.0 && l2->pos[1] == 30.0 + 4.0 * 1.25 && l2->size_mm == 4.0);
        const cc_item *ty = find_text(&paper, "Typed");
        check_true("text type: its size", ty && style.paper.ntext_types &&
                                              ty->size_mm == (double)style.paper.text_types[0].size_mm);
        check_true("twice: the second box 150 mm right",
                   info.placed[1].rect[0] - info.placed[0].rect[0] == 150.0 &&
                       info.placed[1].frame.ox - info.placed[0].frame.ox == 150.0);
        /* The legend lists each entry once: as many swatch labels as the single view's. */
        size_t nseen = count_text(&paper, "Legend");
        check_true("legend once", nseen == 1);
    }
    /* A view seen only (the upper plan) before a section that cuts: the merged legend still lists
     * what is cut first, then what is seen, then what is above (a stable sort by rank). */
    cc_placed order[2] = {s101->placed[0], s101->placed[0]};
    memcpy(order[0].view.c, "2DH5f8_IgQkBwS6o2uUx$_", 22); /* Plan Étage "1": seen only */
    memcpy(order[1].view.c, "1vK4FtqH5CsYhxI0mc40tX", 22); /* Section B-B: timber cut */
    order[1].at_mm[0] += 150.0;
    d = *s101;
    d.placed = order, d.nplaced = 2;
    d.texts = NULL, d.ntexts = 0;
    buf_clear(&err);
    rc = cc_sheet_draw(s, m, &d, &o, &a, &paper, &info, &err);
    check_true("seen then cut: draws", rc == 0 && info.views_drawn == 2);
    if (rc == 0) {
        int last = -1, sorted = 1, ranks[3] = {0, 0, 0}, on = 0;
        for (size_t i = 0; i < paper.nitems; i++) {
            const cc_item *it = &paper.items[i];
            if (it->kind != CC_ITEM_TEXT) continue;
            cc_str t = {it->text, it->text_len};
            if (t.n == 6 && !memcmp(t.p, "Legend", 6)) {
                on = 1;
                continue;
            }
            if (!on) continue;
            if (t.n == 7 && !memcmp(t.p, "PROJECT", 7)) break;
            int r = t.n > 5 && !memcmp(t.p + t.n - 5, ", cut", 5)    ? 0
                    : t.n > 6 && !memcmp(t.p + t.n - 6, ", seen", 6) ? 1
                                                                     : 2;
            ranks[r]++;
            sorted &= r >= last;
            last = r;
        }
        check_true("merged legend: cut, then seen, then above", sorted && ranks[0] && ranks[1]);
    }
    /* cc_sheet_scales: each once, ascending. */
    cc_view vs[3];
    memset(vs, 0, sizeof vs);
    memcpy(vs[0].id.c, "aaaaaaaaaaaaaaaaaaaaaa", 22);
    memcpy(vs[1].id.c, "bbbbbbbbbbbbbbbbbbbbbb", 22);
    memcpy(vs[2].id.c, "cccccccccccccccccccccc", 22);
    vs[0].scale = 100, vs[1].scale = 50, vs[2].scale = 100;
    cc_placed ps[4];
    memset(ps, 0, sizeof ps);
    ps[0].view = vs[0].id, ps[1].view = vs[1].id, ps[2].view = vs[2].id;
    memcpy(ps[3].view.c, "dddddddddddddddddddddd", 22);
    cc_sheet sc;
    memset(&sc, 0, sizeof sc);
    sc.placed = ps, sc.nplaced = 4;
    uint32_t out[4];
    size_t n = cc_sheet_scales(&sc, vs, 3, out);
    check_true("scales: 1:50, 1:100", n == 2 && out[0] == 50 && out[1] == 100);
    buf_free(&err);
    arena_free(&a);
    cc_session_free(ses);
}

/* The session's sheet_svg (sheet_svg.h): the golden once the project is the file's stem ("frame": a
 * copy of model.json named frame.json), the date the platform's clock (2026-01-01) and the note the
 * C core's version; the same answer again (the cache); an unknown sheet's text. */
static int64_t new_year(void *ctx) { return (void)ctx, 1767225600; /* 2026-01-01 00:00 UTC */ }

static const char *find_in(const Buf *b, const char *t) {
    size_t n = strlen(t);
    for (size_t i = 0; i + n <= b->len; i++)
        if (!memcmp(b->p + i, t, n)) return b->p + i;
    return NULL;
}

/* Cuts the "<text …>Bimble …</text>" element out of b. */
static void cut_note(Buf *b) {
    const char *at = find_in(b, ">Bimble ");
    if (!at) return;
    size_t i = (size_t)(at - b->p), j = i;
    while (i > 0 && memcmp(b->p + i, "<text", 5)) i--;
    while (j + 7 <= b->len && memcmp(b->p + j, "</text>", 7)) j++;
    j += 7;
    memmove(b->p + i, b->p + j, b->len - j);
    b->len -= j - i;
}

static void test_session(void) {
    size_t n;
    char *json = slurp(G, "model.json", &n);
    const char *copy = "out/test-draw_sheet/frame.json";
    mkdir("out/test-draw_sheet", 0755);
    FILE *f = fopen(copy, "wb");
    check_true("copy model.json", f && fwrite(json, 1, n, f) == n);
    if (f) fclose(f);
    free(json);
    cc_platform pf = *cc_platform_native();
    pf.unix_s = new_year;
    cc_session *ses = cc_session_new(&pf);
    cc_error e;
    if (!ses || cc_session_open(ses, copy, &e)) {
        check_true("frame.json opens", 0);
        cc_session_free(ses);
        return;
    }
    const cc_model *m = cc_session_model(ses);
    const cc_sheet *s101 = NULL;
    for (size_t i = 0; i < m->nsheets; i++)
        if (m->sheets[i].number.n == 5 && !memcmp(m->sheets[i].number.p, "S-101", 5)) s101 = &m->sheets[i];
    char body[64];
    snprintf(body, sizeof body, "{\"id\":\"%.22s\"}", s101->id.c);
    /* The golden as JSON. The frame's note says the C core's version, and its place follows the
     * text's width: both notes are cut out before comparing, the C one checked on its own. */
    size_t gn;
    char *g = slurp(G, "sheet/S-101.svg", &gn);
    Buf want, got, again;
    buf_init(&want), buf_init(&got), buf_init(&again);
    JsonW w;
    jw_init(&w, &want, 0);
    jw_obj_begin(&w);
    jw_key(&w, "svg"), jw_str_n(&w, g, gn);
    jw_obj_end(&w);
    jw_finish(&w);
    free(g);
    int rc = cc_call(ses, "query", "sheet_svg", body, strlen(body), &got);
    check_true("sheet_svg answers", rc == CC_OK);
    check_true("sheet_svg: the C core's version in the note", find_in(&got, ">Bimble " CC_IFC_VERSION "<") != NULL);
    size_t gl = got.len;
    cut_note(&got), cut_note(&want);
    check_true("sheet_svg: a note cut out", got.len < gl);
    {
        size_t i = 0;
        while (i < got.len && i < want.len && got.p[i] == want.p[i]) i++;
        size_t from = i > 80 ? i - 80 : 0;
        check_true("sheet_svg: the golden (project from the file, date from the clock)", got.len == want.len && i == got.len);
        if (got.len != want.len || i != got.len)
            printf("  at byte %zu:\n  got:  %.*s\n  want: %.*s\n", i, (int)(got.len - from < 160 ? got.len - from : 160),
                   got.p + from, (int)(want.len - from < 160 ? want.len - from : 160), want.p + from);
    }
    buf_clear(&got);
    rc = cc_call(ses, "query", "sheet_svg", body, strlen(body), &got);
    rc = cc_call(ses, "query", "sheet_svg", body, strlen(body), &again);
    check_true("sheet_svg again: the same", rc == CC_OK && again.len == got.len && !memcmp(again.p, got.p, got.len));
    /* The drawn boxes: the plan's as SheetInfo has it (S-101.info.txt's first rect). */
    double box[4];
    const cc_scene *sc = cc_session_scene(ses);
    check_true("placed box: the plan's drawing",
               cc_paper_placed_box(ses, sc, s101, &s101->placed[0].id, box) == 1 && box[0] == 62.86955566406249 &&
                   box[1] == 116.3602880859375 && box[2] == 197.1304443359375 && box[3] == 163.6397119140625);
    cc_gid none;
    memcpy(none.c, "2zzzzzzzzzzzzzzzzzzzzz", 22);
    check_true("placed box: not placed", cc_paper_placed_box(ses, sc, s101, &none, box) == 0);
    buf_clear(&got);
    rc = cc_call(ses, "query", "sheet_svg", "{\"id\":\"nope\"}", 13, &got);
    check_eq("sheet_svg: an unknown sheet", got.p, got.len, "no sheet with id \"nope\"", 23);
    check_true("sheet_svg: an unknown sheet is an error", rc == CC_ERR);
    buf_free(&want), buf_free(&got), buf_free(&again);
    cc_session_free(ses);
    remove(copy);
    rmdir("out/test-draw_sheet");
}

int main(void) {
    test_sheets();
    test_effective();
    test_parts();
    test_session();
    return report("draw_sheet");
}
