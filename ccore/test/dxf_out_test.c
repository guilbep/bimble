/* Suite dxf_out (ticket DXF1, ADR 0082): the DXF export of views (src/dxf_out.h, `query view_dxf`,
 * `sheet_dxf`, session/sheet_svg.h). A C addition: Rust v0.19.0 never wrote DXF, so the goldens are
 * C-own (golden/dxf_c/), written with DXF1_WRITE=1.
 *   - golden/frame.json's plan of "Niveau 1" and a section facing north through y = 5000 (added
 *     by view_add): each file byte for byte its golden, the same bytes twice;
 *   - each read back by the core's own reader (src/dxf_in.h, the port of Rust's reference.rs over
 *     the dxf crate): units mm, the layers, the polylines per layer, the texts, the hatches
 *     (counted as not drawn), and the values hand-computed from the fixture (below);
 *   - a hatch's boundary area read from the file's text (the reader does not draw hatches);
 *   - French accents written as \U+XXXX and read back as such;
 *   - a drawing of 1,000,000 lines streams in chunks of at most CC_DXF_CHUNK bytes;
 *   - two linked models (LINKF): their elements on layers prefixed with each link's name;
 *   - a sheet in paper mm; the errors.
 * Fails on the code before DXF1: view_dxf and sheet_dxf were unknown queries, src/dxf_out.h did
 * not exist. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#define mkdir(path, mode) mkdir(path)
#endif
#include "../host/host.h" /* cc_platform_native */
#include "../session/calls.h"
#include "../session/session.h"
#include "../session/sheet_svg.h"
#include "../src/buf.h"
#include "../src/dxf_in.h"
#include "../src/dxf_out.h"
#include "check.h"

#define GOLD "golden/dxf_c"
#define DIR "out/test-dxf_out"

static int64_t new_year(void *ctx) { return (void)ctx, 1767225600; /* 2026-01-01 00:00 UTC */ }

static const char *find(const char *p, size_t n, const char *s) {
    size_t k = strlen(s);
    for (size_t i = 0; i + k <= n; i++)
        if (!memcmp(p + i, s, k)) return p + i;
    return NULL;
}

static size_t count(const char *p, size_t n, const char *s) {
    size_t c = 0, k = strlen(s);
    for (size_t i = 0; i + k <= n; i++) c += !memcmp(p + i, s, k);
    return c;
}

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_clear(out);
    out->oom = 0;
    return cc_call(s, op, name, body, strlen(body), out);
}

static cc_session *open_session(const char *path) {
    cc_platform *pf = malloc(sizeof *pf);
    *pf = *cc_platform_native();
    pf->unix_s = new_year;
    cc_session *s = cc_session_new(pf);
    cc_error e;
    int bad = !s || cc_session_open(s, path, &e);
    check_true(path, !bad);
    return bad ? NULL : s;
}

static void close_session(cc_session *s) {
    if (!s) return;
    const cc_platform *p = s->pf;
    cc_session_free(s);
    free((void *)p);
}

/* The id of the view named `name` in open_views (its "id" comes before its "name"). */
static int view_id(cc_session *s, const char *name, char id[23]) {
    Buf ov;
    buf_init(&ov);
    call(s, "query", "open_views", "", &ov);
    char key[96];
    snprintf(key, sizeof key, "\"name\":\"%s\"", name);
    const char *p = find(ov.p, ov.len, key);
    int got = 0;
    while (p && p > ov.p && memcmp(p, "\"id\":\"", 6)) p--;
    if (p && p > ov.p) memcpy(id, p + 6, 22), id[22] = 0, got = 1;
    buf_free(&ov);
    return got;
}

/* ---- the file read back ---- */

typedef struct {
    double lo[2], hi[2];
    size_t polylines;
} Ext;

/* The polylines of layer `name` (all when NULL): their count and extents. */
static Ext layer_ext(const cc_dxf *d, const char *name) {
    Ext e = {{1e300, 1e300}, {-1e300, -1e300}, 0};
    for (size_t k = 0; k < d->npolylines; k++) {
        if (name) {
            uint32_t L = d->layer[k];
            if (L == CC_DXF_NO_LAYER || strcmp(d->layers[L].name, name)) continue;
        }
        e.polylines++;
        for (size_t i = d->start[k]; i < d->start[k + 1]; i++)
            for (int j = 0; j < 2; j++) {
                double v = d->xy[2 * i + (size_t)j];
                if (v < e.lo[j]) e.lo[j] = v;
                if (v > e.hi[j]) e.hi[j] = v;
            }
    }
    return e;
}

static int ext_is(Ext e, double x0, double y0, double x1, double y1) {
    return e.lo[0] == x0 && e.lo[1] == y0 && e.hi[0] == x1 && e.hi[1] == y1;
}

static void print_layers(const char *what, const cc_dxf *d, const char *dxf, size_t n) {
    printf("  %s: %zu bytes, %zu polylines, %zu texts, %zu not drawn, %zu HATCH\n", what, n, d->npolylines, d->ntexts,
           d->skipped, count(dxf, n, "\n  0\nHATCH\n"));
    for (size_t i = 0; i < d->nlayers; i++) {
        size_t t = 0;
        for (size_t k = 0; k < d->ntexts; k++) t += d->texts[k].layer == i;
        Ext e = layer_ext(d, d->layers[i].name);
        if (e.polylines)
            printf("    %-28s %4zu polylines %3zu texts  x %g..%g  y %g..%g\n", d->layers[i].name, e.polylines, t, e.lo[0],
                   e.hi[0], e.lo[1], e.hi[1]);
        else printf("    %-28s %4zu polylines %3zu texts\n", d->layers[i].name, e.polylines, t);
    }
}

/* A header variable's two numbers (10, 20). */
static int header_xy(const char *p, size_t n, const char *var, double out[2]) {
    const char *v = find(p, n, var);
    if (!v) return 0;
    const char *a = find(v, n - (size_t)(v - p), "\n 10\n"), *b = find(v, n - (size_t)(v - p), "\n 20\n");
    if (!a || !b) return 0;
    out[0] = strtod(a + 5, NULL), out[1] = strtod(b + 5, NULL);
    return 1;
}

/* The area of the first HATCH on `layer`: its boundary paths (polylines), even-odd summed with
 * their signs as the shoelace gives them (one path: its area). */
static double hatch_area(const char *p, size_t n, const char *layer) {
    char key[128];
    snprintf(key, sizeof key, "\n  8\n%s\n", layer);
    for (const char *h = p; (h = find(h, n - (size_t)(h - p), "\n  0\nHATCH\n")) != NULL; h++) {
        const char *next = find(h + 1, n - (size_t)(h + 1 - p), "\n  0\n");
        size_t span = next ? (size_t)(next - h) : n - (size_t)(h - p);
        if (!find(h, span, key)) continue;
        double area = 0;
        const char *q = find(h, span, "\n 93\n");
        while (q && q < h + span) {
            int nv = atoi(q + 5);
            double xs[64], ys[64];
            const char *r = q + 5;
            int k = 0;
            for (; k < nv && k < 64; k++) {
                r = find(r, (size_t)(h + span - r), "\n 10\n");
                if (!r) break;
                xs[k] = strtod(r + 5, NULL);
                r = find(r, (size_t)(h + span - r), "\n 20\n");
                if (!r) break;
                ys[k] = strtod(r + 5, NULL);
            }
            double a = 0;
            for (int i = 0; i < k; i++) a += xs[i] * ys[(i + 1) % k] - xs[(i + 1) % k] * ys[i];
            area += (a < 0 ? -a : a) / 2;
            q = r ? find(r, (size_t)(h + span - r), "\n 93\n") : NULL;
        }
        return area;
    }
    return -1;
}

/* A closed polyline of `layer` with each corner of the rectangle `want` (lo, hi) and no point off
 * its sides (the cut keeps the mesh's points along a side: 8 points for a column). */
static int has_rect(const cc_dxf *d, const char *layer, double x0, double y0, double x1, double y1) {
    for (size_t k = 0; k < d->npolylines; k++) {
        uint32_t L = d->layer[k];
        if (L == CC_DXF_NO_LAYER || strcmp(d->layers[L].name, layer)) continue;
        int corner = 0, off = 0;
        for (size_t i = d->start[k]; i < d->start[k + 1]; i++) {
            double x = d->xy[2 * i], y = d->xy[2 * i + 1];
            int sx = x == x0 || x == x1, sy = y == y0 || y == y1;
            off |= !(sx && y >= y0 && y <= y1) && !(sy && x >= x0 && x <= x1);
            corner |= (x == x0 && y == y0) << 0 | (x == x1 && y == y0) << 1 | (x == x1 && y == y1) << 2 | (x == x0 && y == y1) << 3;
        }
        if (corner == 15 && !off) return 1;
    }
    return 0;
}

/* "<digit>e" followed by a sign or digit: a number with an exponent. */
static int has_exponent(const char *p, size_t n) {
    for (size_t i = 1; i + 1 < n; i++)
        if ((p[i] == 'e' || p[i] == 'E') && p[i - 1] >= '0' && p[i - 1] <= '9' &&
            (p[i + 1] == '+' || p[i + 1] == '-' || (p[i + 1] >= '0' && p[i + 1] <= '9')))
            return 1;
    return 0;
}

/* A copy under out/ for a CAD program or ezdxf. */
static void save_copy(const char *name, const Buf *b) {
    char path[160];
    snprintf(path, sizeof path, DIR "/%s", name);
    FILE *f = fopen(path, "wb");
    if (f) fwrite(b->p, 1, b->len, f), fclose(f);
}

static void golden(const char *name, const Buf *got) {
    char path[128];
    snprintf(path, sizeof path, GOLD "/%s", name);
    if (getenv("DXF1_WRITE")) {
        mkdir(GOLD, 0755);
        FILE *f = fopen(path, "wb");
        check_true("write golden", f && fwrite(got->p, 1, got->len, f) == got->len);
        if (f) fclose(f);
        return;
    }
    size_t n;
    char *want = slurp(GOLD, name, &n);
    char what[160];
    snprintf(what, sizeof what, "%s byte for byte (%zu bytes, golden %zu)", path, got->len, n);
    check_true(what, n == got->len && !memcmp(want, got->p, n));
    free(want);
    /* a copy for a CAD program or ezdxf */
    snprintf(path, sizeof path, DIR "/%s", name);
    FILE *f = fopen(path, "wb");
    if (f) fwrite(got->p, 1, got->len, f), fclose(f);
}

/* ---- the fixture's plan and section ---- */

static void test_views(void) {
    cc_session *s = open_session(DIR "/frame.json");
    if (!s) return;
    Buf a, b, body;
    buf_init(&a), buf_init(&b), buf_init(&body);
    char plan[23], sec[23];
    check_true("the plan of Niveau 1", view_id(s, "Plan Niveau 1", plan));
    int rc = call(s, "verb", "view_add", "{\"kind\":\"section\",\"facing\":\"north\",\"origin\":[9000,5000],\"name\":\"Coupe A\"}", &a);
    check_true("view_add a section facing north through y = 5000", rc == CC_OK);
    check_true("its id", view_id(s, "Coupe A", sec));

    /* ---- the plan */
    char q[128];
    snprintf(q, sizeof q, "{\"view\":\"%s\"}", plan);
    rc = call(s, "query", "view_dxf", q, &a);
    int rc2 = call(s, "query", "view_dxf", q, &b);
    check_true("view_dxf of the plan", rc == CC_OK && a.len > 1000);
    check_true("the same bytes twice", rc2 == CC_OK && a.len == b.len && !memcmp(a.p, b.p, a.len));
    check_true("AC1015, mm, metric", find(a.p, a.len, "$ACADVER\n  1\nAC1015\n") && find(a.p, a.len, "$INSUNITS\n 70\n4\n") &&
                                         find(a.p, a.len, "$MEASUREMENT\n 70\n1\n"));
    check_true("starts with its comment, ends with EOF", !strncmp(a.p, "999\n", 4) && a.len > 8 && !memcmp(a.p + a.len - 8, "  0\nEOF\n", 8));
    check_true("no -0 and no exponent", !find(a.p, a.len, "\n-0\n") && !has_exponent(a.p, a.len));
    golden("frame-plan-niveau-1.dxf", &a);
    cc_dxf d;
    Buf err;
    buf_init(&err);
    rc = cc_dxf_read((const uint8_t *)a.p, a.len, &d, &err);
    check_true("the core's reader reads it back", rc == 0);
    if (!rc) {
        if (getenv("DXF1_DUMP")) print_layers("plan", &d, a.p, a.len);
        check_true("units mm ($INSUNITS 4)", d.unit_mm == 1.0 && !strcmp(d.unit, "mm"));
        /* 12 columns of Niveau 1 (P1.00..P1.32, 300 x 300, centred on the grid x 0, 6000, 12000,
         * 18000 and y 0, 5000, 10000) cut at 1200 mm: 12 closed outlines from -150..18150 by
         * -150..10150 (18000 + 150, 10000 + 150). */
        Ext c = layer_ext(&d, "IfcColumn-cut");
        check_true("IfcColumn-cut: 12 outlines over x -150..18150, y -150..10150", c.polylines == 12 && ext_is(c, -150, -150, 18150, 10150));
        check_true("P1.00's cut outline: (+-150, +-150)", has_rect(&d, "IfcColumn-cut", -150, -150, 150, 150));
        check_true("P1.32's cut outline: 17850..18150, 9850..10150", has_rect(&d, "IfcColumn-cut", 17850, 9850, 18150, 10150));
        /* The fill of each cut column: a solid HATCH on Hatch-Concrete (C30/37 is concrete), its
         * boundary the column's 300 x 300 = 90000 mm2. */
        double ha = hatch_area(a.p, a.len, "Hatch-Concrete");
        check_true("the first Hatch-Concrete: 300 x 300 = 90000 mm2", ha == 90000.0);
        check_true("12 solid hatches of the cut columns", count(a.p, a.len, "\n  8\nHatch-Concrete\n") >= 12);
        /* The reader (Rust's dxf crate 0.6.2 ported) swallows HATCH (an entity it does not know)
         * and counts TEXT as not drawn; every LINE, LWPOLYLINE and CIRCLE is one of its polylines. */
        size_t lines = count(a.p, a.len, "\n  0\nLINE\n") + count(a.p, a.len, "\n  0\nLWPOLYLINE\n") + count(a.p, a.len, "\n  0\nCIRCLE\n");
        check_true("every LINE, LWPOLYLINE, CIRCLE read back; TEXTs counted as not drawn",
                   d.npolylines == lines && d.skipped == d.ntexts && d.ntexts == count(a.p, a.len, "\n  0\nTEXT\n"));
        /* The 17 beams of Niveau 1 (z 2530) are above the cut: dashed on IfcBeam-above (the
         * axis lines of the beams lie on the grid: x 0..18000, y 0..10000, their 250 width
         * +-125). */
        Ext ab = layer_ext(&d, "IfcBeam-above");
        check_true("IfcBeam-above: within x -125..18125, y -125..10125", ab.polylines > 0 && ab.lo[0] >= -125 && ab.hi[0] <= 18125 &&
                                                                         ab.lo[1] >= -125 && ab.hi[1] <= 10125);
        Ext g = layer_ext(&d, "IfcGrid");
        check_true("IfcGrid: axes and bubbles", g.polylines >= 7 * 2);
        /* The header's extents hold every polyline read. */
        Ext all = layer_ext(&d, NULL);
        double lo[2], hi[2];
        check_true("$EXTMIN/$EXTMAX hold every polyline", header_xy(a.p, a.len, "$EXTMIN", lo) && header_xy(a.p, a.len, "$EXTMAX", hi) &&
                                                               lo[0] <= all.lo[0] && lo[1] <= all.lo[1] && hi[0] >= all.hi[0] && hi[1] >= all.hi[1]);
        int dim = 0;
        for (size_t k = 0; k < d.ntexts; k++)
            dim |= d.texts[k].layer != CC_DXF_NO_LAYER && !strcmp(d.layers[d.texts[k].layer].name, "Annotation-dimension") &&
                   !strcmp(d.texts[k].text, "600");
        check_true("a dimension text: 600 (6000 mm in cm) on Annotation-dimension", dim);
        size_t tall = 0;
        for (size_t k = 0; k < d.ntexts; k++) tall += d.texts[k].height >= 100.0;
        check_true("texts at 1:100: a 2.5 mm text is 250 mm tall", tall > 0);
        cc_dxf_free(&d);
    }

    /* ---- the section: x along it (world x), y the height (world z) */
    snprintf(q, sizeof q, "{\"view\":\"%s\"}", sec);
    rc = call(s, "query", "view_dxf", q, &a);
    check_true("view_dxf of the section", rc == CC_OK);
    golden("frame-section-coupe-a.dxf", &a);
    buf_clear(&err);
    rc = cc_dxf_read((const uint8_t *)a.p, a.len, &d, &err);
    check_true("the section read back", rc == 0);
    if (!rc) {
        if (getenv("DXF1_DUMP")) print_layers("section", &d, a.p, a.len);
        /* The columns at y = 5000 (P?.?1) are cut through their middle: Niveau 1's from z 0 to
         * 2780, Niveau 2's from 3000 to 5780 (its level + 2780); x -150..18150. */
        Ext c = layer_ext(&d, "IfcColumn-cut");
        check_true("IfcColumn-cut: x -150..18150, z 0..5780", c.polylines >= 8 && ext_is(c, -150, 0, 18150, 5780));
        check_true("P1.01's cut: x +-150, z 0..2780", has_rect(&d, "IfcColumn-cut", -150, 0, 150, 2780));
        check_true("P2.01's cut: x +-150, z 3000..5780", has_rect(&d, "IfcColumn-cut", -150, 3000, 150, 5780));
        /* The slabs D1 (2780 + 220) and D2 (3000 + 2780 + 220 = 6000), the footings (800 deep
         * under 0, 1200 wide: +-600) cut along y = 5000. */
        check_true("IfcSlab-cut: x -150..18150, z 2780..6000", ext_is(layer_ext(&d, "IfcSlab-cut"), -150, 2780, 18150, 6000));
        check_true("IfcFooting-cut: x -600..18600, z -800..0", ext_is(layer_ext(&d, "IfcFooting-cut"), -600, -800, 18600, 0));
        cc_dxf_free(&d);
    }
    check_true("errors: no view", call(s, "query", "view_dxf", "{\"view\":\"nope\"}", &a) == CC_ERR && find(a.p, a.len, "no view with id \"nope\""));
    check_true("errors: no sheet", call(s, "query", "sheet_dxf", "{\"sheet\":\"nope\"}", &a) == CC_ERR && find(a.p, a.len, "no sheet with id"));
    check_true("errors: missing field", call(s, "query", "view_dxf", "{}", &a) == CC_ERR && find(a.p, a.len, "missing field `view`"));
    check_true("errors: path", call(s, "query", "view_dxf", "{\"view\":\"x\",\"path\":3}", &a) == CC_ERR && find(a.p, a.len, "path: a string"));
    /* written at a path: {bytes, path} */
    snprintf(q, sizeof q, "{\"view\":\"%s\",\"path\":\"" DIR "/written.dxf\"}", plan);
    rc = call(s, "query", "view_dxf", q, &a);
    size_t wn = 0;
    char *wf = rc == CC_OK ? slurp(DIR, "written.dxf", &wn) : NULL;
    check_true("with a path: written, {bytes, path}", rc == CC_OK && find(a.p, a.len, "\"bytes\":") && wf && wn > 1000);
    free(wf);
    remove(DIR "/written.dxf");
    buf_free(&err);
    buf_free(&a), buf_free(&b), buf_free(&body);
    close_session(s);
}

/* ---- synthetic drawings ---- */

static cc_dxf_options view_opts(void) {
    cc_dxf_options o;
    memset(&o, 0, sizeof o);
    o.frame.turn.kind = CC_TURN_PLAN;
    o.frame.k = 0.01; /* 1:100 */
    o.scale = 100;
    return o;
}

static void test_accents(void) {
    static const char T[] = "Fa\xc3\xa7" "ade c\xc3\xb4t\xc3\xa9 rue \xe2\x80\x93 \xc3\x98 30";
    cc_item it;
    memset(&it, 0, sizeof it);
    it.kind = CC_ITEM_TEXT;
    it.pos[0] = 10, it.pos[1] = 20;
    it.size_mm = 2.5;
    it.text = T, it.text_len = sizeof T - 1;
    it.tag.role = CC_TAG_TEXT;
    cc_paper p;
    memset(&p, 0, sizeof p);
    p.items = &it, p.nitems = 1, p.width_mm = 100, p.height_mm = 100;
    cc_dxf_options o = view_opts();
    Buf b, err;
    buf_init(&b), buf_init(&err);
    check_true("a text written", cc_dxf_write_buf(&p, &o, &b) == CC_DXF_OK);
    static const char W[] = "\n  1\nFa\\U+00E7ade c\\U+00F4t\\U+00E9 rue \\U+2013 \\U+00D8 30\n";
    check_true("accents as \\U+XXXX (AC1015's code page text)", find(b.p, b.len, W) != NULL);
    cc_dxf d;
    int rc = cc_dxf_read((const uint8_t *)b.p, b.len, &d, &err);
    check_true("read back: one text on Annotation-text, 250 mm at (1000, -2000)",
               !rc && d.ntexts == 1 && d.texts[0].height == 250.0 && d.texts[0].at[0] == 1000.0 && d.texts[0].at[1] == -2000.0 &&
                   d.texts[0].layer != CC_DXF_NO_LAYER && !strcmp(d.layers[d.texts[0].layer].name, "Annotation-text"));
    if (!rc) {
        check_eq("its content", d.texts[0].text, d.texts[0].n, W + 5, sizeof W - 7);
        cc_dxf_free(&d);
    }
    /* the sheet's own items are not in a view's model space; a sheet keeps them, in paper mm */
    it.tag.role = CC_TAG_PAPER;
    buf_clear(&b);
    check_true("a view leaves the sheet's own items out", cc_dxf_write_buf(&p, &o, &b) == CC_DXF_OK && !find(b.p, b.len, "\nTEXT\n"));
    o.sheet = 1;
    buf_clear(&b);
    check_true("a sheet keeps them, y up in paper mm (10, 80) on Sheet",
               cc_dxf_write_buf(&p, &o, &b) == CC_DXF_OK && find(b.p, b.len, "\nTEXT\n  5\n") && find(b.p, b.len, "\n  8\nSheet\n") &&
                   find(b.p, b.len, "\n 10\n10\n 20\n80\n"));
    buf_free(&b), buf_free(&err);
}

typedef struct {
    size_t calls, max, bytes;
} Count;

static int counting(void *ctx, const char *p, size_t n) {
    (void)p;
    Count *c = ctx;
    c->calls++, c->bytes += n;
    if (n > c->max) c->max = n;
    return 0;
}

static void test_stream(void) {
    size_t n = 1000000;
    double(*pts)[2] = malloc(2 * n * sizeof *pts);
    cc_ring *r = malloc(n * sizeof *r);
    if (!pts || !r) {
        check_true("memory for 1e6 lines", 0);
        free(pts), free(r);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        pts[2 * i][0] = (double)(i % 1000), pts[2 * i][1] = (double)(i / 1000);
        pts[2 * i + 1][0] = pts[2 * i][0] + 0.5, pts[2 * i + 1][1] = pts[2 * i][1];
        r[i].p = pts + 2 * i, r[i].n = 2;
    }
    cc_item it;
    memset(&it, 0, sizeof it);
    it.kind = CC_ITEM_LINES;
    it.rings = r, it.nrings = n;
    it.has_pen = 1;
    it.pen.width_mm = 0.25;
    it.tag.role = CC_TAG_SYMBOL;
    cc_paper p;
    memset(&p, 0, sizeof p);
    p.items = &it, p.nitems = 1, p.width_mm = 1000, p.height_mm = 1000;
    cc_dxf_options o = view_opts();
    Count c = {0, 0, 0};
    o.sink = counting, o.sink_ctx = &c;
    int rc = cc_dxf_write(&p, &o);
    char what[200];
    snprintf(what, sizeof what, "1e6 lines streamed: %zu bytes in %zu chunks of at most %zu (CC_DXF_CHUNK %d)", c.bytes, c.calls, c.max,
             CC_DXF_CHUNK);
    check_true(what, rc == CC_DXF_OK && c.max <= CC_DXF_CHUNK && c.calls > 100 && c.bytes > 50 * n);
    if (getenv("DXF1_DUMP")) printf("  %s\n", what);
    free(pts), free(r);
}

/* ---- two linked models (LINKF): layers prefixed with each link's name ---- */

static int copy_file(const char *from, const char *to) {
    size_t n;
    char *p = slurp(".", from, &n);
    FILE *f = fopen(to, "wb");
    int ok = f && fwrite(p, 1, n, f) == n;
    if (f) fclose(f);
    free(p);
    return ok;
}

static void test_links(void) {
    check_true("copy a.ifc, b.ifc", copy_file("golden/ifc/frame.ifc", DIR "/Structure.ifc") && copy_file("golden/ifc/frame.ifc", DIR "/Archi.ifc"));
    cc_session *s = open_session(DIR "/frame.json");
    if (!s) return;
    Buf a;
    buf_init(&a);
    check_true("link Structure.ifc", call(s, "action", "link_model", "{\"path\":\"" DIR "/Structure.ifc\"}", &a) == CC_OK);
    check_true("link Archi.ifc", call(s, "action", "link_model", "{\"path\":\"" DIR "/Archi.ifc\"}", &a) == CC_OK);
    char plan[23], q[96];
    check_true("the plan", view_id(s, "Plan Niveau 1", plan));
    snprintf(q, sizeof q, "{\"view\":\"%s\"}", plan);
    int rc = call(s, "query", "view_dxf", q, &a);
    check_true("view_dxf with two links", rc == CC_OK);
    save_copy("frame-plan-two-links.dxf", &a);
    cc_dxf d;
    Buf err;
    buf_init(&err);
    rc = rc == CC_OK ? cc_dxf_read((const uint8_t *)a.p, a.len, &d, &err) : -1;
    if (!rc) {
        if (getenv("DXF1_DUMP")) print_layers("links", &d, a.p, a.len);
        Ext h = layer_ext(&d, "IfcColumn-cut"), s1 = layer_ext(&d, "Structure.ifc-IfcColumn-cut"), a1 = layer_ext(&d, "Archi.ifc-IfcColumn-cut");
        char what[200];
        snprintf(what, sizeof what, "the host's 12 columns, and each link's on its own layer (%zu, %zu, %zu)", h.polylines, s1.polylines, a1.polylines);
        check_true(what, h.polylines == 12 && s1.polylines > 0 && s1.polylines == a1.polylines);
        int hatch = find(a.p, a.len, "\n  8\nStructure.ifc-Hatch-Concrete\n") || find(a.p, a.len, "\n  8\nStructure.ifc-Hatch-");
        check_true("a link's hatches prefixed too", hatch);
        cc_dxf_free(&d);
    }
    buf_free(&err), buf_free(&a);
    close_session(s);
    remove(DIR "/Structure.ifc"), remove(DIR "/Archi.ifc");
}

/* ---- a sheet: paper mm, the frame and title block kept ---- */

static void test_sheet(void) {
    size_t n;
    char *json = slurp("golden/drawing", "model.json", &n);
    FILE *f = fopen(DIR "/sheets.json", "wb");
    check_true("copy drawing/model.json", f && fwrite(json, 1, n, f) == n);
    if (f) fclose(f);
    free(json);
    cc_session *s = open_session(DIR "/sheets.json");
    if (!s) return;
    Buf a;
    buf_init(&a);
    call(s, "query", "sheets", "", &a);
    const char *id = find(a.p, a.len, "\"id\":\"");
    char q[96];
    snprintf(q, sizeof q, "{\"sheet\":\"%.22s\"}", id ? id + 6 : "");
    int rc = call(s, "query", "sheet_dxf", q, &a);
    double lo[2], hi[2];
    if (rc == CC_OK) save_copy("drawing-sheet.dxf", &a);
    check_true("sheet_dxf: paper mm, its frame on Sheet, its views' cut layers",
               rc == CC_OK && find(a.p, a.len, "\n  8\nSheet\n") && find(a.p, a.len, "-cut\n") &&
                   header_xy(a.p, a.len, "$EXTMIN", lo) && header_xy(a.p, a.len, "$EXTMAX", hi) && lo[0] >= 0 && hi[0] <= 1189 && hi[1] <= 841);
    buf_free(&a);
    close_session(s);
    remove(DIR "/sheets.json");
}

int main(void) {
    mkdir("out", 0755);
    mkdir(DIR, 0755);
    size_t n;
    char *json = slurp("golden", "frame.json", &n);
    FILE *f = fopen(DIR "/frame.json", "wb");
    check_true("copy frame.json", f && fwrite(json, 1, n, f) == n);
    if (f) fclose(f);
    free(json);
    test_views();
    test_accents();
    test_stream();
    test_links();
    test_sheet();
    remove(DIR "/frame.json");
    return report("dxf_out");
}
