/* Suite pdf_out: the PDF writer (pdf_out.h) against Rust's Sheet::to_pdf (golden/drawing/pdf/,
 * ticket D6). For each golden sheet:
 * - <name>-<font>.content: the page's content stream, byte for byte (both fonts);
 * - <name>-helvetica.pdf: the whole file, byte for byte (the producer's version masked by as
 *   many `x`, as the dumper does);
 * - sample-roboto.pdf: every object byte for byte and in the same order, but the two FontFile2
 *   streams: Rust packs them with flate2, the C core with its own deflate (DEVIATIONS 12), so
 *   there the dictionaries match but /Length, and both streams inflate to the shipped font file
 *   (same Adler-32); the cross-reference table has the same entries, each at its own offset.
 * Sheets: the dumper's sample sheet (drawing_sample_sheet, as test/draw_test.c builds it) and the
 * frame's full plan (as test/draw_annot_test.c) and sheet S-101 of model.json (D5's
 * cc_sheet_draw, as test/draw_sheet_test.c).
 * Then the writer's own cases: numbers as pdf-writer prints f32, strings, an unused bold face
 * (a free id in the cross-reference table), the font checks and a bad pen. */
#include <stdint.h>
#include <string.h>
#include "../host/host.h" /* cc_platform_native */
#include "../session/session.h"
#include "../session/sheet_svg.h"
#include "../src/arena.h"
#include "../src/draw_sheet.h"
#include "../src/draw_plan.h"
#include "../src/draw_style.h"
#include "../src/file.h"
#include "../src/inflate.h"
#include "../src/model.h"
#include "../src/paper.h"
#include "../src/pdf_out.h"
#include "../src/scene.h"
#include "check.h"

#define G "golden/drawing"
#define FONTS "../crates/bimble-drawing/fonts"
#define RUST_VERSION "0.18.1" /* Cargo.toml's, which made the goldens */

static double D(const char *s) { return strtod(s, NULL); } /* glibc: correctly rounded */

static unsigned char *ttf[2];
static size_t ttf_n[2];

static void check_bytes(const char *what, const char *got, size_t got_n, const char *want, size_t n) {
    if (got_n == n && !memcmp(got, want, n)) {
        passed++;
        return;
    }
    size_t i = 0;
    while (i < got_n && i < n && got[i] == want[i]) i++;
    size_t from = i > 60 ? i - 60 : 0;
    printf("FAIL %s: first difference at byte %zu (got %zu bytes, want %zu)\n  got:  %.*s\n  want: %.*s\n", what, i, got_n,
           n, (int)((got_n - from) < 200 ? got_n - from : 200), got + from, (int)((n - from) < 200 ? n - from : 200),
           want + from);
    failed++;
}

static cc_pdf_options options(void) {
    cc_pdf_options o;
    memset(&o, 0, sizeof o);
    o.version = RUST_VERSION;
    o.version_len = strlen(RUST_VERSION);
    for (int k = 0; k < 2; k++) o.roboto[k] = ttf[k], o.roboto_len[k] = ttf_n[k];
    return o;
}

/* mask_pdf_version of the dumper: "(Bimble 0.18.1)" -> "(Bimble xxxxxx)". */
static void mask(Buf *b) {
    const char *v = "(Bimble " RUST_VERSION ")";
    size_t n = strlen(v);
    for (size_t i = 0; i + n <= b->len; i++)
        if (!memcmp(b->p + i, v, n)) {
            memset(b->p + i + 8, 'x', n - 9);
            return;
        }
}

/* ---- the objects of a file ---- */

typedef struct {
    int id;
    size_t at, len;        /* the object, "N 0 obj" to "endobj\n\n" */
    size_t dict, dict_len; /* its dictionary */
    size_t data, data_len; /* its stream (0 0: none) */
} Obj;

typedef struct {
    Obj o[32];
    int n;
    size_t xref; /* offset of "xref" */
} Objs;

static const char *find(const char *p, size_t n, size_t from, const char *s) {
    size_t k = strlen(s);
    for (size_t i = from; i + k <= n; i++)
        if (!memcmp(p + i, s, k)) return p + i;
    return NULL;
}

/* Splits a file pdf-writer's way (pretty dictionaries: the outer one ends with "\n>>"). */
static int split(const char *p, size_t n, Objs *out) {
    memset(out, 0, sizeof *out);
    size_t at = 16;
    while (at < n && memcmp(p + at, "xref\n", 5)) {
        if (out->n == 32) return -1;
        Obj *o = &out->o[out->n++];
        o->at = at;
        o->id = atoi(p + at);
        const char *d = find(p, n, at, " 0 obj\n");
        const char *e = d ? find(p, n, (size_t)(d - p), "\n>>") : NULL;
        if (!e) return -1;
        o->dict = (size_t)(d - p) + 7;
        o->dict_len = (size_t)(e - p) + 3 - o->dict;
        size_t q = o->dict + o->dict_len;
        if (q + 8 <= n && !memcmp(p + q, "\nstream\n", 8)) {
            const char *l = find(p, q, o->dict, "/Length ");
            if (!l) return -1;
            o->data = q + 8;
            o->data_len = (size_t)atol(l + 8);
            q = o->data + o->data_len;
            if (q + 10 > n || memcmp(p + q, "\nendstream", 10)) return -1;
            q += 10;
        }
        if (q + 9 > n || memcmp(p + q, "\nendobj\n\n", 9)) return -1;
        o->len = q + 9 - at;
        at = q + 9;
    }
    out->xref = at;
    return at < n ? 0 : -1;
}

/* Inflates a zlib stream (its deflate data in a gzip member, with the CRC of `want`). */
static int unzlib(const char *z, size_t n, const unsigned char *want, size_t want_n, Buf *out) {
    if (n < 6) return -1;
    Buf gz;
    buf_init(&gz);
    static const unsigned char head[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};
    buf_append(&gz, head, 10);
    buf_append(&gz, z + 2, n - 6);
    uint32_t crc = cc_crc32(0, want, want_n), size = (uint32_t)want_n;
    for (int k = 0; k < 4; k++) buf_putc(&gz, (char)(crc >> (8 * k)));
    for (int k = 0; k < 4; k++) buf_putc(&gz, (char)(size >> (8 * k)));
    const char *why = NULL;
    int rc = cc_gunzip((const unsigned char *)gz.p, gz.len, 1u << 24, out, &why);
    if (rc) printf("  inflate: %s\n", why);
    buf_free(&gz);
    return rc;
}

/* A dictionary without its "/Length N" line. */
static void no_length(const char *p, size_t n, Buf *out) {
    buf_clear(out);
    for (size_t i = 0; i < n;) {
        size_t e = i;
        while (e < n && p[e] != '\n') e++;
        if (!(e - i > 9 && !memcmp(p + i, "  /Length ", 10))) buf_append(out, p + i, e - i);
        if (e < n) buf_putc(out, '\n');
        i = e + 1;
    }
}

/* The C file against Rust's, object by object; a FontFile2 by its dictionary and its font. */
static void check_objects(const char *what, const Buf *c, const char *r, size_t rn) {
    Objs oc, orr;
    char m[160];
    snprintf(m, sizeof m, "%s: both files split into objects", what);
    check_true(m, split(c->p, c->len, &oc) == 0 && split(r, rn, &orr) == 0);
    snprintf(m, sizeof m, "%s: the same objects in the same order", what);
    int same = oc.n == orr.n;
    for (int i = 0; same && i < oc.n; i++) same = oc.o[i].id == orr.o[i].id;
    check_true(m, same);
    if (!same) return;
    check_bytes("header", c->p, 16, r, 16);
    Buf a, b, inf;
    buf_init(&a), buf_init(&b), buf_init(&inf);
    for (int i = 0; i < oc.n; i++) {
        const Obj *x = &oc.o[i], *y = &orr.o[i];
        int font = find(r, y->dict + y->dict_len, y->dict, "/Filter /FlateDecode") != NULL;
        snprintf(m, sizeof m, "%s: object %d", what, x->id);
        if (!font) {
            check_bytes(m, c->p + x->at, x->len, r + y->at, y->len);
            continue;
        }
        no_length(c->p + x->dict, x->dict_len, &a);
        no_length(r + y->dict, y->dict_len, &b);
        snprintf(m, sizeof m, "%s: object %d (FontFile2) dictionary but /Length", what, x->id);
        check_bytes(m, a.p, a.len, b.p, b.len);
        int bold = x->id == 15;
        for (int side = 0; side < 2; side++) {
            const char *z = side ? r + y->data : c->p + x->data;
            size_t zn = side ? y->data_len : x->data_len;
            buf_clear(&inf);
            int rc = unzlib(z, zn, ttf[bold], ttf_n[bold], &inf);
            snprintf(m, sizeof m, "%s: object %d inflates (%s) to Roboto-%s.ttf", what, x->id, side ? "Rust" : "C",
                     bold ? "Bold" : "Regular");
            check_true(m, rc == 0 && inf.len == ttf_n[bold] && !memcmp(inf.p, ttf[bold], inf.len));
        }
        snprintf(m, sizeof m, "%s: object %d: the same Adler-32", what, x->id);
        check_true(m, !memcmp(c->p + x->data + x->data_len - 4, r + y->data + y->data_len - 4, 4));
        printf("  %s: Roboto-%s packed: C %zu bytes, Rust %zu bytes (file %zu)\n", what, bold ? "Bold" : "Regular",
               x->data_len, y->data_len, ttf_n[bold]);
    }
    /* The cross-reference table: the same entries (offsets aside), each pointing at its object. */
    const char *tc = find(c->p, c->len, oc.xref, "trailer\n"), *tr = find(r, rn, orr.xref, "trailer\n");
    snprintf(m, sizeof m, "%s: xref heads and trailers", what);
    int ok = tc && tr && (size_t)(tc - c->p) - oc.xref == (size_t)(tr - r) - orr.xref;
    const char *sc = ok ? find(c->p, c->len, (size_t)(tc - c->p), "startxref\n") : NULL;
    const char *sr = ok ? find(r, rn, (size_t)(tr - r), "startxref\n") : NULL;
    ok = ok && sc && sr && sc - tc == sr - tr && !memcmp(tc, tr, (size_t)(sc - tc));
    ok = ok && (size_t)atol(sc + 10) == oc.xref && (size_t)atol(sr + 10) == orr.xref;
    ok = ok && c->len > 6 && !memcmp(c->p + c->len - 6, "\n%%EOF", 6) && !memcmp(r + rn - 6, "\n%%EOF", 6);
    check_true(m, ok);
    if (ok) {
        const char *ec = find(c->p, c->len, oc.xref + 5, "\n") + 1, *er = find(r, rn, orr.xref + 5, "\n") + 1;
        check_bytes("xref head", c->p + oc.xref, (size_t)(ec - c->p) - oc.xref, r + orr.xref, (size_t)(er - r) - orr.xref);
        int entries = tc - ec == tr - er && (tc - ec) % 20 == 0;
        for (int id = 0; entries && ec < tc; id++, ec += 20, er += 20) {
            entries &= !memcmp(ec + 10, er + 10, 10);
            if (ec[17] == 'f') {
                entries &= !memcmp(ec, er, 10); /* the free list: same next ids */
                continue;
            }
            int found = 0; /* the offset is where object `id` starts */
            for (int i = 0; i < oc.n; i++) found |= oc.o[i].id == id && oc.o[i].at == (size_t)atol(ec);
            entries &= found;
        }
        snprintf(m, sizeof m, "%s: xref entries (offsets at the objects, free list as Rust's)", what);
        check_true(m, entries);
    }
    buf_free(&a), buf_free(&b), buf_free(&inf);
}

/* Content, and the whole file (Helvetica) or object by object (Roboto, when kept whole). */
static void check_sheet(const char *name, const cc_paper *p) {
    const char *tag = p->font == CC_FONT_ROBOTO ? "roboto" : "helvetica";
    char file[96];
    Buf b;
    buf_init(&b);
    snprintf(file, sizeof file, "pdf/%s-%s.content", name, tag);
    check_true("content writes", cc_paper_pdf_content(p, &b) == 0);
    size_t n;
    char *want = slurp(G, file, &n);
    check_bytes(file, b.p, b.len, want, n);
    free(want);
    buf_clear(&b);
    cc_pdf_options o = options();
    check_true("pdf writes", cc_paper_pdf(p, &o, &b) == 0);
    mask(&b);
    snprintf(file, sizeof file, "pdf/%s-%s.pdf", name, tag);
    if (p->font == CC_FONT_HELVETICA) {
        want = slurp(G, file, &n);
        check_bytes(file, b.p, b.len, want, n);
        free(want);
    } else if (!strcmp(name, "sample")) {
        want = slurp(G, file, &n);
        check_objects(file, &b, want, n);
        free(want);
    }
    buf_free(&b);
}

/* ---- the sample sheet (ccore_golden.rs drawing_sample_sheet; test/draw_test.c's copy) ---- */

static cc_pen pen(double w, cc_rgb c, const double *dash, size_t nd) {
    cc_pen p = {w, c, dash, nd};
    return p;
}

static void test_sample(cc_sheet_font font) {
    cc_rgb black = {{0, 0, 0}}, red = {{200, 30, 30}};
    cc_item items[64];
    size_t ni = 0;
    memset(items, 0, sizeof items);
    double sq[4][2] = {{10, 10}, {60, 10}, {60, 60}, {10, 60}}, hole[4][2] = {{20, 20}, {50, 20}, {50, 50}, {20, 50}};
    cc_ring r1[2] = {{sq, 4}, {hole, 4}};
    static double dash1[2];
    dash1[0] = D("0.8");
    dash1[1] = D("0.4");
    cc_hatch h1[2] = {{45.0, 2.0, 0.0, pen(D("0.13"), black, NULL, 0)}, {-30.0, D("3.3"), D("0.7"), pen(D("0.1"), black, dash1, 2)}};
    items[ni].kind = CC_ITEM_AREA;
    items[ni].rings = r1;
    items[ni].nrings = 2;
    items[ni].has_fill = 1;
    items[ni].fill = (cc_rgb){{214, 214, 210}};
    items[ni].hatches = h1;
    items[ni].nhatches = 2;
    items[ni].has_pen = 1;
    items[ni].pen = pen(0.5, black, NULL, 0);
    ni++;
    double tri[3][2] = {{70, 60}, {D("110.33333333333333"), 12.5}, {95, 60}};
    cc_ring r2 = {tri, 3};
    uint64_t nanb = 0x7ff8000000000000ull;
    double nan;
    memcpy(&nan, &nanb, 8);
    cc_hatch h2[2] = {{0.0, 1.5, 0.25, pen(D("0.1"), black, NULL, 0)}, {90.0, 1.5, nan, pen(D("0.1"), black, NULL, 0)}};
    items[ni].kind = CC_ITEM_AREA;
    items[ni].rings = &r2;
    items[ni].nrings = 1;
    items[ni].hatches = h2;
    items[ni].nhatches = 2;
    ni++;
    double tri2[3][2] = {{120, 10}, {140, 10}, {130, 30}};
    cc_ring r3 = {tri2, 3};
    static const double dash3[4] = {2.0, 1.0, 0.25, 1.0};
    cc_hatch h3 = {15.0, 0.0, 0.0, pen(D("0.1"), black, NULL, 0)};
    items[ni].kind = CC_ITEM_AREA;
    items[ni].rings = &r3;
    items[ni].nrings = 1;
    items[ni].has_fill = 1;
    items[ni].fill = (cc_rgb){{255, 255, 255}};
    items[ni].hatches = &h3;
    items[ni].nhatches = 1;
    items[ni].has_pen = 1;
    items[ni].pen = pen(0.25, black, dash3, 4);
    ni++;
    cc_hatch h4 = {45.0, 1.0, 0.0, pen(D("0.1"), black, NULL, 0)};
    items[ni].kind = CC_ITEM_AREA;
    items[ni].hatches = &h4;
    items[ni].nhatches = 1;
    items[ni].has_pen = 1;
    items[ni].pen = pen(0.25, black, NULL, 0);
    ni++;
    double l1[3][2] = {{0.0625, 0.1875}, {D("1.0005"), D("2.0004999")}, {D("-0.0004"), -0.0}};
    double l2[2][2] = {{D("150.123456"), D("70.98765")}, {D("290.9999"), D("200.00049999999")}};
    cc_ring r5[2] = {{l1, 3}, {l2, 2}};
    static double dash5[4];
    dash5[0] = 5.0;
    dash5[1] = 1.25;
    dash5[2] = D("0.3333");
    dash5[3] = 1.25;
    items[ni].kind = CC_ITEM_LINES;
    items[ni].rings = r5;
    items[ni].nrings = 2;
    items[ni].has_pen = 1;
    items[ni].pen = pen(0.0625, (cc_rgb){{1, 2, 254}}, dash5, 4);
    ni++;
    double l3[2][2] = {{D("1e-7"), 123456789.0}, {D("-1e21"), 3.0}};
    cc_ring r6 = {l3, 2};
    items[ni].kind = CC_ITEM_LINES;
    items[ni].rings = &r6;
    items[ni].nrings = 1;
    items[ni].has_pen = 1;
    items[ni].pen = pen(D("0.18"), black, NULL, 0);
    ni++;
    const char *plan_title = "Plan Niveau 1, 1:100";
    double y = 80.0;
    const double angles[3] = {0.0, 90.0, D("-33.5")};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            cc_item *t = &items[ni++];
            t->kind = CC_ITEM_TEXT;
            t->pos[0] = 150.0;
            t->pos[1] = y;
            t->size_mm = 2.5 + (double)i * 0.5;
            t->text = plan_title;
            t->text_len = strlen(plan_title);
            t->anchor = (cc_text_anchor)i;
            t->angle_deg = angles[j];
            t->bold = j == 1;
            t->color = t->bold ? red : black;
            y += 9.0;
        }
    const struct {
        double x, y, size;
        const char *text;
        cc_text_anchor anchor;
        double angle;
        int bold;
    } texts[] = {
        {20, 100, 3.5, "<b> & \"q\" 'a' \xe2\x80\x94 \xc3\x89tage \"1\" Rez-de-chauss\xc3\xa9" "e \xc2\xb1" "0.00 \xc3\x97 2 \xc2\xb5",
         CC_ANCHOR_START, 0.0, 0},
        {20, 110, 3.5, "Bold: \xc5\xb8 \xc3\xbf \xc5\x92 \xc5\x93 \xc4\xb3 \xc5\x82 \xc5\x91 \xc8\x99 \xc8\x9b 50 \xc3\x97 30 cm",
         CC_ANCHOR_MIDDLE, 0.0, 1},
        {20, 120, 2.0, "", CC_ANCHOR_END, 12.0, 0},
        {280, 200, 5.0, "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz {|}~", CC_ANCHOR_END, 0.0, 0},
        {20, 130, 3.0, "\xe6\x97\xa5\xe6\x9c\xac \xc4\x80\xc5\xbb \xc7\x85 \xc9\x8f \xe2\x80\x89\xe2\x80\x93\xe2\x80\x94\xe2\x80\x99",
         CC_ANCHOR_MIDDLE, 180.0, 0},
    };
    for (size_t k = 0; k < sizeof texts / sizeof texts[0]; k++) {
        cc_item *t = &items[ni++];
        t->kind = CC_ITEM_TEXT;
        t->pos[0] = texts[k].x;
        t->pos[1] = texts[k].y;
        t->size_mm = texts[k].size;
        t->text = texts[k].text;
        t->text_len = strlen(texts[k].text);
        t->anchor = texts[k].anchor;
        t->angle_deg = texts[k].angle;
        t->bold = texts[k].bold;
        t->color = t->bold ? red : black;
    }
    const char *title = "Sample <sheet> & \"\xc3\xa9\"";
    cc_paper p = {297.0, 210.0, title, strlen(title), items, ni, font};
    check_sheet("sample", &p);
}

/* ---- the frame's full plan (as test/draw_annot_test.c's "full") ---- */

static cc_str S(const char *s) {
    cc_str r = {s, strlen(s)};
    return r;
}

static void test_frame_full(cc_sheet_font font) {
    size_t n;
    char *text = slurp("golden", "frame.json", &n);
    cc_model m;
    cc_scene s;
    cc_error e;
    int rc = cc_model_load(&m, text, n, &e);
    free(text);
    check_true("frame.json loads", rc == 0);
    if (rc) return;
    rc = cc_scene_build(&s, &m, &e);
    check_true("frame scene builds", rc == 0);
    if (rc) {
        cc_model_free(&m);
        return;
    }
    Arena a;
    arena_init(&a, 0);
    cc_style style;
    cc_plan_template tpl;
    rc = cc_style_default(&style, &a) || cc_plan_template_default(&tpl, &a) ||
         cc_plan_template_with_project(&tpl, m.project, m.nproject, &a);
    check_true("style and template", rc == 0);
    style.paper.font = font; /* the dumper: o.style.paper.font = font */
    cc_plan_options o;
    cc_plan_options_init(&o);
    o.storey = S("Niveau 1");
    o.style = &style;
    o.tpl = &tpl;
    o.project = S("frame");
    o.date = S("2026-01-01");
    o.version = S(RUST_VERSION);
    cc_paper sheet;
    cc_plan_info info;
    Buf err;
    buf_init(&err);
    rc = rc ? rc : cc_plan(&s, &m, &o, &a, &sheet, &info, &err);
    check_true("frame full plan draws", rc == 0);
    if (rc == 0) {
        check_true("the plan's font is the style's", sheet.font == font);
        check_sheet("frame-full", &sheet);
    }
    buf_free(&err);
    arena_free(&a);
    cc_scene_free(&s);
    cc_model_free(&m);
}

/* ---- sheet S-101 (D5's sheet composition) ---- */

/* S-101 of golden/drawing/model.json, sheetdraw::draw with the dumper's sheet_options(dm, font)
 * (as test/draw_sheet_test.c's options, the font set). */
static void test_s101(cc_sheet_font font) {
    cc_session *ses = cc_session_new(cc_platform_native());
    cc_error e;
    if (!ses || cc_session_open(ses, G "/model.json", &e)) {
        check_true("model.json opens", 0);
        if (ses) cc_session_free(ses);
        return;
    }
    const cc_model *m = cc_session_model(ses);
    const cc_scene *sc = cc_session_scene(ses);
    const cc_sheet *def = NULL;
    for (size_t i = 0; m && i < m->nsheets; i++)
        if (m->sheets[i].number.n == 5 && !memcmp(m->sheets[i].number.p, "S-101", 5)) def = &m->sheets[i];
    check_true("S-101 found", sc && def);
    Arena a;
    arena_init(&a, 0);
    cc_style style;
    cc_plan_template tpl;
    int rc = def && sc ? cc_style_default(&style, &a) || cc_plan_template_default(&tpl, &a) ||
                             cc_plan_template_with_project(&tpl, m->project, m->nproject, &a)
                       : -1;
    if (rc == 0) {
        style.paper.font = font;
        cc_sheet_options o;
        memset(&o, 0, sizeof o);
        o.style = &style;
        o.tpl = &tpl;
        o.project = S("frame");
        o.date = S("2026-01-01");
        o.version = S(RUST_VERSION);
        cc_paper paper;
        cc_sheet_info info;
        Buf err;
        buf_init(&err);
        rc = cc_sheet_draw(sc, m, def, &o, &a, &paper, &info, &err);
        check_true("S-101 draws", rc == 0);
        if (rc == 0) check_sheet("S-101", &paper);
        buf_free(&err);
    }
    if (def && sc && font == CC_FONT_ROBOTO) { /* the session's export (today, its own version) */
        const unsigned char *fonts[2] = {ttf[0], ttf[1]};
        size_t lens[2] = {ttf_n[0], ttf_n[1]};
        Buf pdf, err;
        buf_init(&pdf), buf_init(&err);
        rc = cc_paper_sheet_pdf(ses, sc, def, fonts, lens, &pdf, &err);
        Objs objs;
        check_true("session: S-101 as PDF", rc == 0 && pdf.len > 16 && !memcmp(pdf.p, "%PDF-1.7\n", 9) &&
                                                split(pdf.p, pdf.len, &objs) == 0 && objs.n == 15);
        if (rc) printf("  %.*s\n", (int)err.len, err.p);
        buf_clear(&pdf), buf_clear(&err);
        rc = cc_paper_sheet_pdf(ses, sc, def, NULL, NULL, &pdf, &err);
        check_true("session: no fonts, no PDF", rc == -1 && err.len && !memcmp(err.p, "the Roboto font files", 21));
        buf_free(&pdf), buf_free(&err);
    }
    arena_free(&a);
    cc_session_free(ses);
}

/* ---- the writer's own cases ---- */

static void num_case(float f, const char *want) {
    double sq[1][2] = {{(double)f, 0.0}};
    cc_ring r = {sq, 1};
    cc_item it;
    memset(&it, 0, sizeof it);
    it.kind = CC_ITEM_LINES;
    it.rings = &r;
    it.nrings = 1;
    it.pen.width_mm = 1.0;
    cc_paper p = {10.0, 10.0, "", 0, &it, 1, CC_FONT_HELVETICA};
    Buf b;
    buf_init(&b);
    cc_paper_pdf_content(&p, &b);
    /* the line "<x> 0 m" */
    const char *mline = find(b.p, b.len, 0, "\n[] 0 d\n");
    char m[80];
    snprintf(m, sizeof m, "number %.9g prints %s", (double)f, want);
    size_t wn = strlen(want);
    check_true(m, mline && !memcmp(mline + 8, want, wn) && !memcmp(mline + 8 + wn, " 0 m", 4));
    buf_free(&b);
}

static float fbits(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void test_numbers(void) {
    num_case(0.0f, "0");
    num_case(-0.0f, "0");
    num_case(3.0f, "3");
    num_case(-17.0f, "-17");
    num_case(0.5f, "0.5");
    num_case(fbits(0x3dcccccd), "0.1");
    num_case(fbits(0x4f000000), "2147483647");  /* 2^31: `as i32` saturates, back to 2^31 */
    num_case(fbits(0xcf000000), "-2147483648"); /* -2^31 */
    num_case(fbits(0x4f800000), "4294967300.0"); /* 2^32: shortest digits */
    num_case(fbits(0x3586d6f0), "0.0000010046333"); /* just above 1e-6: ryu, plain */
    num_case(fbits(0x358637bd), "0.000001");     /* 1e-6 itself: Display */
    num_case(fbits(0x5368d4a5), "1000000000000"); /* 1e12 itself: Display, no ".0" */
    num_case(fbits(0x5368d4a4), "999999900000.0");
    num_case(fbits(0x7fc00000), "NaN");
    num_case(fbits(0x7f800000), "inf");
    num_case(fbits(0xff800000), "-inf");
    num_case(fbits(0x00000001), "0.000000000000000000000000000000000000000000001");
    num_case(fbits(0xc3742000), "-244.125");
    num_case(-244.140625f, "-244.14062"); /* a tie: ryu takes the even digit (Rust's {:?}: ...63) */
}

static void test_strings(void) {
    /* Helvetica: WinAnsi bytes; Roboto: 2-byte glyph ids, so control bytes and parentheses
     * ("#$%'(": glyphs 8, 9, 10, 12, 13; "C" 40 = '(', "D" 41 = ')', "w" 92 = '\\'). */
    static const struct {
        const char *text, *want;
        cc_sheet_font font;
    } C[] = {
        {"a(b)c", "(a(b)c) Tj", CC_FONT_HELVETICA},
        {"a)b(c", "(a\\)b\\(c) Tj", CC_FONT_HELVETICA},
        {"(a", "(\\(a) Tj", CC_FONT_HELVETICA},
        {"x\\y", "(x\\\\y) Tj", CC_FONT_HELVETICA},
        {"t\tn\x01", "(t?n?) Tj", CC_FONT_HELVETICA},
        {"\xc3\xa9\xe2\x82\xac", "<E93F> Tj", CC_FONT_HELVETICA}, /* win_ansi: é as is, € not there */
        {"#$%'(", "(\\000\\b\\000\\t\\000\\n\\000\\f\\000\\r) Tj", CC_FONT_ROBOTO},
        {"CD", "(\\000(\\000)) Tj", CC_FONT_ROBOTO},
        {"DC", "(\\000\\)\\000\\() Tj", CC_FONT_ROBOTO},
        {"w", "(\\000\\\\) Tj", CC_FONT_ROBOTO},
    };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        cc_item it;
        memset(&it, 0, sizeof it);
        it.kind = CC_ITEM_TEXT;
        it.size_mm = 2.0;
        it.text = C[i].text;
        it.text_len = strlen(C[i].text);
        cc_paper p = {10.0, 10.0, "", 0, &it, 1, C[i].font};
        Buf b;
        buf_init(&b);
        cc_paper_pdf_content(&p, &b);
        const char *tj = find(b.p, b.len, 0, " Tm\n");
        size_t wn = strlen(C[i].want);
        char m[80];
        snprintf(m, sizeof m, "string %zu: %s", i, C[i].want);
        int ok = tj && (size_t)(b.p + b.len - (tj + 4)) >= wn && !memcmp(tj + 4, C[i].want, wn);
        check_true(m, ok);
        if (!ok && tj) printf("  got: %.*s\n", (int)(b.p + b.len - (tj + 4)), tj + 4);
        buf_free(&b);
    }
    /* The title: ASCII as a literal string, anything else UTF-16BE (a surrogate pair). */
    cc_paper p = {10.0, 10.0, "Plan (1)", 8, NULL, 0, CC_FONT_HELVETICA};
    cc_pdf_options o = options();
    Buf b;
    buf_init(&b);
    check_true("empty sheet writes", cc_paper_pdf(&p, &o, &b) == 0);
    check_true("ASCII title", find(b.p, b.len, 0, "/Title (Plan (1))\n") != NULL);
    buf_clear(&b);
    p.title = "\xf0\x9f\x98\x80";
    p.title_len = 4;
    cc_paper_pdf(&p, &o, &b);
    check_true("title past the BMP", find(b.p, b.len, 0, "/Title <FEFFD83DDE00>\n") != NULL);
    buf_free(&b);
}

/* Roboto with no bold text: the bold face is declared without its file, so id 15 is never
 * written and the table ends at 14; with no text at all, 11 is free too (pdf-writer's list). */
static void test_roboto_unused(void) {
    cc_item it;
    memset(&it, 0, sizeof it);
    it.kind = CC_ITEM_TEXT;
    it.size_mm = 2.0;
    it.text = "A";
    it.text_len = 1;
    cc_paper p = {297.0, 210.0, "t", 1, &it, 1, CC_FONT_ROBOTO};
    cc_pdf_options o = options();
    Buf b;
    buf_init(&b);
    check_true("roboto, regular only, writes", cc_paper_pdf(&p, &o, &b) == 0);
    check_true("no bold file", !find(b.p, b.len, 0, "/FontFile2 15 0 R") && find(b.p, b.len, 0, "/FontFile2 11 0 R"));
    check_true("bold: empty widths", find(b.p, b.len, 0, "/CIDToGIDMap /Identity\n  /W []\n") != NULL);
    const char *x = find(b.p, b.len, 0, "\nxref\n0 15\n0000000000 65535 f\r\n");
    check_true("xref of 15, all used", x != NULL);
    buf_clear(&b);
    p.nitems = 0;
    check_true("roboto, no text, writes", cc_paper_pdf(&p, &o, &b) == 0);
    x = find(b.p, b.len, 0, "\nxref\n0 15\n0000000011 65535 f\r\n");
    check_true("xref: 0 points to the free 11", x != NULL);
    check_true("xref: 11 free, last", x && !memcmp(x + 11 + 20 * 11, "0000000000 00000 f\r\n", 20));
    check_true("no bfchar block", !find(b.p, b.len, 0, "beginbfchar"));
    buf_free(&b);
}

static void test_errors(void) {
    cc_item it;
    memset(&it, 0, sizeof it);
    it.kind = CC_ITEM_TEXT;
    it.size_mm = 2.0;
    it.text = "B";
    it.text_len = 1;
    it.bold = 1;
    cc_paper p = {297.0, 210.0, "t", 1, &it, 1, CC_FONT_ROBOTO};
    cc_pdf_options o = options();
    Buf b;
    buf_init(&b);
    o.roboto[1] = NULL;
    o.roboto_len[1] = 0;
    check_true("bold text without the bold file", cc_paper_pdf(&p, &o, &b) == CC_PDF_ERR_FONT);
    o = options();
    o.roboto[1] = ttf[0];
    o.roboto_len[1] = ttf_n[0];
    buf_clear(&b);
    check_true("the regular file as bold", cc_paper_pdf(&p, &o, &b) == CC_PDF_ERR_FONT);
    check_true("shipped files known", cc_pdf_roboto_is_shipped(ttf[0], ttf_n[0], 0) && cc_pdf_roboto_is_shipped(ttf[1], ttf_n[1], 1));
    check_true("truncated file refused", !cc_pdf_roboto_is_shipped(ttf[0], 100, 0));
    /* A Helvetica sheet needs no font file. */
    p.font = CC_FONT_HELVETICA;
    o.roboto[0] = o.roboto[1] = NULL;
    buf_clear(&b);
    check_true("helvetica without files", cc_paper_pdf(&p, &o, &b) == 0);
    /* A negative pen: Rust's set_line_width asserts. */
    double l[2][2] = {{0, 0}, {1, 1}};
    cc_ring r = {l, 2};
    cc_item ln;
    memset(&ln, 0, sizeof ln);
    ln.kind = CC_ITEM_LINES;
    ln.rings = &r;
    ln.nrings = 1;
    ln.pen.width_mm = -0.1;
    cc_paper q = {10.0, 10.0, "", 0, &ln, 1, CC_FONT_HELVETICA};
    buf_clear(&b);
    check_true("negative pen refused", cc_paper_pdf(&q, &o, &b) == CC_PDF_ERR_PEN && b.len == 0);
    buf_free(&b);
}

int main(void) {
    static const char *const NAMES[2] = {"Roboto-Regular.ttf", "Roboto-Bold.ttf"};
    for (int k = 0; k < 2; k++) ttf[k] = (unsigned char *)slurp(FONTS, NAMES[k], &ttf_n[k]);
    test_sample(CC_FONT_HELVETICA);
    test_sample(CC_FONT_ROBOTO);
    test_frame_full(CC_FONT_HELVETICA);
    test_frame_full(CC_FONT_ROBOTO);
    test_s101(CC_FONT_HELVETICA);
    test_s101(CC_FONT_ROBOTO);
    test_numbers();
    test_strings();
    test_roboto_unused();
    test_errors();
    for (int k = 0; k < 2; k++) free(ttf[k]);
    return report("pdf_out");
}
