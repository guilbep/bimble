/* Suite draw: drawings on paper, byte for byte against Rust (golden/drawing/, ticket D1).
 * - fmt.txt: fmt_f64_fixed as Rust's {}, {:.1} and {:.3}; then a million values against glibc's
 *   printf("%.3f") / ("%.1f") (exact, ties to even, as Rust) and {} against the Ryu digits.
 * - roboto.txt: the generated Roboto table (src/roboto_widths.inc) has Rust's glyph ids and
 *   advances.
 * - cut/<model>.txt: cut::section and facing_up_region of every scene element at the ground
 *   plan's cut.
 * - svg/sample.svg, sample-roboto.svg: the dumper's sample sheet, written by cc_paper_svg.
 * - plan/<model>/cut.svg: the cut through the frame of plan/<model>/frame.txt (D2 computes it).
 * - crop.rs's own test cases. */
#include <stdint.h>
#include <string.h>
#include "../src/draw_cut.h"
#include "../src/file.h"
#include "../src/fmt.h"
#include "../src/json_w.h"
#include "../src/model.h"
#include "../src/paper.h"
#include "../src/scene.h"
#include "check.h"

#define G "golden/drawing"

static double fabs(double x) { return x < 0 ? -x : x; }
static double D(const char *s) { return strtod(s, NULL); } /* glibc: correctly rounded */

static void check_buf(const char *what, Buf *b, const char *name) {
    size_t n;
    char *want = slurp(G, name, &n);
    check_eq(what, b->p ? b->p : "", b->len, want, n);
    if (b->len != n || memcmp(b->p, want, n)) { /* first difference, for the log */
        size_t i = 0;
        while (i < b->len && i < n && b->p[i] == want[i]) i++;
        size_t from = i > 60 ? i - 60 : 0;
        printf("  %s: first difference at byte %zu\n  got:  %.*s\n  want: %.*s\n", name, i,
               (int)((b->len - from) < 160 ? b->len - from : 160), b->p + from, (int)((n - from) < 160 ? n - from : 160),
               want + from);
    }
    free(want);
}

/* ---- numbers ---- */

static void test_fmt_golden(void) {
    size_t n;
    char *text = slurp(G, "fmt.txt", &n);
    Buf b;
    buf_init(&b);
    int lines = 0;
    for (char *line = text; *line;) {
        char *eol = strchr(line, '\n');
        if (!eol) eol = line + strlen(line);
        char *f[4];
        size_t len[4];
        char *p = line;
        for (int i = 0; i < 4; i++) {
            char *sp = memchr(p, ' ', (size_t)(eol - p));
            if (!sp || i == 3) sp = eol;
            f[i] = p;
            len[i] = (size_t)(sp - p);
            p = sp + 1;
        }
        uint64_t bits = strtoull(f[0], NULL, 16);
        double x;
        memcpy(&x, &bits, 8);
        const int prec[3] = {-1, 1, 3};
        for (int k = 0; k < 3; k++) {
            char what[64];
            snprintf(what, sizeof what, "fmt %016llx prec %d", (unsigned long long)bits, prec[k]);
            buf_clear(&b);
            check_true(what, fmt_f64_fixed(&b, x, prec[k]) == 0);
            check_eq(what, b.p, b.len, f[k + 1], len[k + 1]);
        }
        lines++;
        line = *eol ? eol + 1 : eol;
    }
    check_true("fmt.txt: at least 1500 values", lines >= 1500);
    buf_clear(&b);
    double nan;
    uint64_t nb = 0x7ff8000000000000ull;
    memcpy(&nan, &nb, 8);
    check_true("fmt fixed: NaN is an error, nothing written", fmt_f64_fixed(&b, nan, 3) == CCORE_ERR_NONFINITE && b.len == 0);
    check_true("fmt fixed: prec 21 is an error", fmt_f64_fixed(&b, 1.0, 21) == CCORE_ERR_NONFINITE && b.len == 0);
    buf_free(&b);
    free(text);
    printf("draw: fmt.txt %d values\n", lines);
}

/* A million values: {:.3} and {:.1} against glibc's printf (exact decimal expansion, rounded half
 * to even, like Rust); {} against the shortest digits of fmt_f64_debug (Ryu). */
static void test_fmt_million(void) {
    Buf b, d;
    buf_init(&b);
    buf_init(&d);
    uint64_t x = 0x2545f4914f6cdd1dull;
    int bad = 0, n = 0;
    char ref[2048];
    for (int i = 0; i < 1000000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        double v;
        switch (i % 4) {
        case 0: { /* any finite bits */
            uint64_t u = x;
            if (((u >> 52) & 0x7ff) == 0x7ff) u &= ~(1ull << 62);
            memcpy(&v, &u, 8);
            break;
        }
        case 1: v = (double)(x >> 11) / 9007199254740992.0 * 2000.0 - 1000.0; break; /* paper mm */
        case 2: v = (double)(int64_t)(x >> 40) / 16384.0; break;                       /* many ties */
        default: v = (double)(x >> 11) / 9007199254740992.0 * 1e-3; break;           /* tiny */
        }
        for (int p = 1; p <= 3; p += 2) {
            buf_clear(&b);
            fmt_f64_fixed(&b, v, p);
            int rn = snprintf(ref, sizeof ref, "%.*f", p, v);
            n++;
            if (rn < 0 || (size_t)rn != b.len || memcmp(ref, b.p, b.len)) {
                if (bad++ < 5) printf("FAIL fixed %.17g prec %d: got %.*s want %s\n", v, p, (int)b.len, b.p, ref);
            }
        }
        /* {} = the Debug digits written plainly, without ".0" (Debug is plain in [1e-4, 1e16)). */
        double a = v < 0 ? -v : v;
        if (a >= 1e-4 && a < 1e16) {
            buf_clear(&b);
            buf_clear(&d);
            fmt_f64_fixed(&b, v, -1);
            fmt_f64_debug(&d, v);
            size_t dl = d.len;
            if (dl > 2 && d.p[dl - 2] == '.' && d.p[dl - 1] == '0') dl -= 2;
            n++;
            if (dl != b.len || memcmp(d.p, b.p, dl)) {
                if (bad++ < 5) printf("FAIL display %.17g: got %.*s want %.*s\n", v, (int)b.len, b.p, (int)dl, d.p);
            }
        }
    }
    check_true("fmt fixed: a million values as printf and Ryu", bad == 0);
    printf("draw: fmt %d comparisons, %d differ\n", n, bad);
    buf_free(&b);
    buf_free(&d);
}

/* ---- Roboto ---- */

static void test_roboto(void) {
    size_t n;
    char *text = slurp(G, "roboto.txt", &n);
    int face = -1, rows = 0, bad = 0;
    for (char *line = text; *line;) {
        char *eol = strchr(line, '\n');
        if (!eol) eol = line + strlen(line);
        *eol = 0;
        unsigned cp, gid;
        char adv[64];
        if (!strncmp(line, "face ", 5)) {
            face++;
        } else if (!strncmp(line, "missing ", 8)) {
            uint16_t g, u;
            cc_roboto_glyph(0x4e00, face, &g, &u); /* a CJK character: not in Roboto */
            float want = (float)strtod(line + 8, NULL);
            if ((float)u * 0.48828125f != want || g != 0) bad++;
        } else if (sscanf(line, "%x %u %63s", &cp, &gid, adv) == 3) {
            uint16_t g, u;
            cc_roboto_glyph(cp, face, &g, &u);
            float want = (float)strtod(adv, NULL);
            if (g != gid || (float)u * 0.48828125f != want) {
                if (bad++ < 5) printf("FAIL roboto face %d U+%04x: gid %u units %u, want %u %s\n", face, cp, g, u, gid, adv);
            }
            rows++;
        }
        line = eol + 1;
        if (eol >= text + n) break;
    }
    check_true("roboto table: Rust's glyph ids and advances", bad == 0 && rows >= 1000 && face == 1);
    free(text);
    /* Widths: "0123456789" is 5.62 em in Roboto (tabular digits), Helvetica's AFM 5.56. */
    double w = cc_text_width_mm("0123456789", 10, 1.0, 0, CC_FONT_ROBOTO);
    check_true("roboto digits 0.562 em", w > 5.61 && w < 5.63);
    check_true("helvetica digits", cc_text_width_mm("0123456789", 10, 1.0, 0, CC_FONT_HELVETICA) == D("5.56"));
}

/* ---- the cut ---- */

static void dbg(Buf *b, double x) {
    if (fmt_f64_debug(b, x)) buf_puts(b, x != x ? "NaN" : x > 0 ? "inf" : "-inf");
}

static void ring_line(Buf *b, const char *tag, const cc_ring *r) {
    buf_puts(b, tag);
    buf_putc(b, ' ');
    buf_u64(b, r->n);
    for (size_t i = 0; i < r->n; i++) {
        buf_putc(b, ' ');
        dbg(b, r->p[i][0]);
        buf_putc(b, ' ');
        dbg(b, r->p[i][1]);
    }
    buf_putc(b, '\n');
}

typedef struct {
    cc_model m;
    cc_scene s;
    double level, cut;
    const cc_storey *st;
} Loaded;

static int load(const char *file, const char *storey, Loaded *L) {
    size_t n;
    char *text = slurp("golden", file, &n);
    cc_error err;
    int rc = cc_model_load(&L->m, text, n, &err);
    free(text);
    check_true(file, rc == 0);
    if (rc) return -1;
    rc = cc_scene_build(&L->s, &L->m, &err);
    check_true("scene builds", rc == 0);
    if (rc) return -1;
    L->st = NULL;
    for (size_t i = 0; i < L->m.nstoreys; i++)
        if (L->m.storeys[i].name.n == strlen(storey) && !memcmp(L->m.storeys[i].name.p, storey, L->m.storeys[i].name.n))
            L->st = &L->m.storeys[i];
    check_true("ground storey", L->st != NULL);
    if (!L->st) return -1;
    L->level = L->st->elevation - L->s.origin[2];
    L->cut = L->level + 1200.0;
    return 0;
}

static void unload(Loaded *L) {
    cc_scene_free(&L->s);
    cc_model_free(&L->m);
}

static void test_cut(const char *name, const char *file, const char *storey) {
    Loaded L;
    if (load(file, storey, &L)) return;
    Buf b;
    buf_init(&b);
    buf_puts(&b, "storey ");
    json_escape(&b, storey, strlen(storey));
    buf_puts(&b, "\norigin ");
    for (int k = 0; k < 3; k++) {
        if (k) buf_putc(&b, ' ');
        dbg(&b, L.s.origin[k]);
    }
    buf_puts(&b, "\nlevel ");
    dbg(&b, L.level);
    buf_puts(&b, "\ncut ");
    dbg(&b, L.cut);
    buf_putc(&b, '\n');
    int ok = 1;
    for (size_t k = 0; k < L.s.nelements; k++) {
        Arena a;
        arena_init(&a, 0);
        cc_mesh m;
        cc_section sec;
        cc_ring *up;
        size_t nup;
        if (cc_scene_world_mesh(&L.s, k, &a, &m) || cc_cut_section(&m, L.cut, &a, &sec) ||
            cc_cut_facing_up(&m, &a, &up, &nup)) {
            ok = 0;
            arena_free(&a);
            break;
        }
        const cc_scene_element *e = &L.s.elements[k];
        buf_puts(&b, "element ");
        buf_append(&b, e->id.c, 22);
        buf_putc(&b, ' ');
        buf_append(&b, e->class_name.p, e->class_name.n);
        buf_puts(&b, " loops ");
        buf_u64(&b, sec.nloops);
        buf_puts(&b, " open ");
        buf_u64(&b, sec.nopen);
        buf_puts(&b, " up ");
        buf_u64(&b, nup);
        buf_putc(&b, '\n');
        for (size_t i = 0; i < sec.nloops; i++) ring_line(&b, "loop", &sec.loops[i]);
        for (size_t i = 0; i < sec.nopen; i++) ring_line(&b, "open", &sec.open[i]);
        for (size_t i = 0; i < nup; i++) ring_line(&b, "up", &up[i]);
        arena_free(&a);
    }
    check_true("cut: every element", ok);
    char what[64], file_name[64];
    snprintf(what, sizeof what, "cut/%s.txt", name);
    snprintf(file_name, sizeof file_name, "cut/%s.txt", name);
    check_buf(what, &b, file_name);
    buf_free(&b);
    unload(&L);
}

/* cut/<model>.more.txt: sections at each storey's cut and at the element's mid height, then each
 * edge clipped to the ground plan's heights with edge_seen_from_above. */
static void test_cut_more(const char *name, const char *file, const char *storey) {
    Loaded L;
    if (load(file, storey, &L)) return;
    Buf b;
    buf_init(&b);
    int ok = 1;
    for (size_t k = 0; k < L.s.nelements && ok; k++) {
        Arena a;
        arena_init(&a, 0);
        cc_mesh m;
        if (cc_scene_world_mesh(&L.s, k, &a, &m)) {
            ok = 0;
            arena_free(&a);
            break;
        }
        double inf = 1.0 / 0.0, zmin = inf, zmax = -inf;
        for (uint32_t i = 0; i < m.nv; i++) {
            double z = (double)m.pos[i][2];
            zmin = z < zmin ? z : zmin; /* no NaN in a scene mesh */
            zmax = z > zmax ? z : zmax;
        }
        const cc_scene_element *e = &L.s.elements[k];
        buf_puts(&b, "element ");
        buf_append(&b, e->id.c, 22);
        buf_putc(&b, ' ');
        buf_append(&b, e->class_name.p, e->class_name.n);
        buf_putc(&b, '\n');
        for (size_t c = 0; c <= L.m.nstoreys && ok; c++) {
            /* Scene storeys, lowest first: the model's storeys sorted by elevation. */
            double h;
            if (c < L.m.nstoreys) {
                size_t rank = 0;
                const cc_storey *st = NULL;
                for (size_t i = 0; i < L.m.nstoreys && !st; i++) {
                    rank = 0;
                    for (size_t j = 0; j < L.m.nstoreys; j++)
                        if (L.m.storeys[j].elevation < L.m.storeys[i].elevation ||
                            (L.m.storeys[j].elevation == L.m.storeys[i].elevation && j < i))
                            rank++;
                    if (rank == c) st = &L.m.storeys[i];
                }
                h = st->elevation - L.s.origin[2] + 1200.0;
            } else {
                h = (zmin + zmax) / 2.0;
            }
            cc_section sec;
            if (cc_cut_section(&m, h, &a, &sec)) {
                ok = 0;
                break;
            }
            buf_puts(&b, "at ");
            dbg(&b, h);
            buf_puts(&b, " loops ");
            buf_u64(&b, sec.nloops);
            buf_puts(&b, " open ");
            buf_u64(&b, sec.nopen);
            buf_putc(&b, '\n');
            for (size_t i = 0; i < sec.nloops; i++) ring_line(&b, "loop", &sec.loops[i]);
            for (size_t i = 0; i < sec.nopen; i++) ring_line(&b, "open", &sec.open[i]);
        }
        for (uint32_t i = 0; i < m.ne; i++) {
            int seen = i >= m.nn || cc_cut_edge_seen_from_above(m.normals[i]);
            double pa[3] = {m.edges[i][0], m.edges[i][1], m.edges[i][2]}, pb[3] = {m.edges[i][3], m.edges[i][4], m.edges[i][5]};
            double c[2][2];
            buf_puts(&b, "clip ");
            buf_u64(&b, i);
            buf_puts(&b, seen ? " 1 " : " 0 ");
            if (cc_cut_clip_z(pa, pb, L.level - 1.0, L.cut, c)) {
                cc_ring r = {c, 2};
                Buf t;
                buf_init(&t);
                ring_line(&t, "", &r); /* " 2 x y x y\n": drop the tag and the count */
                buf_append(&b, t.p + 3, t.len - 3);
                buf_free(&t);
            } else {
                buf_puts(&b, "none\n");
            }
        }
        arena_free(&a);
    }
    check_true("cut more: every element", ok);
    char path[64];
    snprintf(path, sizeof path, "cut/%s.more.txt", name);
    check_buf(path, &b, path);
    buf_free(&b);
    unload(&L);
}

/* cut.rs's own tests: a wall cut halfway, a hollow tube (two loops), an open sheet (an open
 * chain walked from a free end), a cut at a face. */
static void box(float (*pos)[3], uint32_t *idx, uint32_t base, const double lo[3], const double hi[3], int inward) {
    for (int i = 0; i < 8; i++) {
        pos[i][0] = (float)(i & 1 ? hi[0] : lo[0]);
        pos[i][1] = (float)(i & 2 ? hi[1] : lo[1]);
        pos[i][2] = (float)(i & 4 ? hi[2] : lo[2]);
    }
    static const int q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (int f = 0; f < 6; f++) {
        uint32_t t[6] = {(uint32_t)q[f][0], (uint32_t)q[f][1], (uint32_t)q[f][2], (uint32_t)q[f][0], (uint32_t)q[f][2], (uint32_t)q[f][3]};
        for (int k = 0; k < 6; k += 3) {
            idx[f * 6 + k] = base + t[k];
            idx[f * 6 + k + 1] = base + (inward ? t[k + 2] : t[k + 1]);
            idx[f * 6 + k + 2] = base + (inward ? t[k + 1] : t[k + 2]);
        }
    }
}

static void test_cut_cases(void) {
    Arena a;
    arena_init(&a, 0);
    float pos[16][3];
    uint32_t idx[72];
    cc_mesh m = {pos, 8, idx, 36, NULL, 0, NULL, 0};
    cc_section s;
    double lo[3] = {0, 0, 0}, hi[3] = {5000, 200, 2800};
    box(pos, idx, 0, lo, hi, 0);
    check_true("a wall cut halfway: one loop", cc_cut_section(&m, 1200, &a, &s) == 0 && s.nloops == 1 && s.nopen == 0 &&
                                                   fabs(fabs(cc_cut_area(&s.loops[0])) - 1e6) < 1e-6);
    check_true("cut above: nothing", cc_cut_section(&m, 3000, &a, &s) == 0 && s.nloops == 0 && s.nopen == 0);
    double lo2[3] = {0, 0, 0}, hi2[3] = {1000, 1000, 3000}, lo3[3] = {200, 200, -10}, hi3[3] = {800, 800, 3010};
    box(pos, idx, 0, lo2, hi2, 0);
    box(pos + 8, idx + 36, 8, lo3, hi3, 1);
    m.nv = 16;
    m.ni = 72;
    int ok = cc_cut_section(&m, 1500, &a, &s) == 0 && s.nloops == 2;
    double a0 = ok ? fabs(cc_cut_area(&s.loops[0])) : 0, a1 = ok ? fabs(cc_cut_area(&s.loops[1])) : 0;
    check_true("a hollow section has two loops", ok && fabs((a0 < a1 ? a0 : a1) - 360000.0) < 1e-6 && fabs((a0 < a1 ? a1 : a0) - 1e6) < 1e-6);
    /* An L-shaped open sheet: two vertical quads meeting at (1000, 0). */
    float sheet[6][3] = {{0, 0, 0}, {1000, 0, 0}, {1000, 0, 1000}, {0, 0, 1000}, {1000, 800, 0}, {1000, 800, 1000}};
    uint32_t si[12] = {0, 1, 2, 0, 2, 3, 1, 4, 5, 1, 5, 2};
    cc_mesh sm = {sheet, 6, si, 12, NULL, 0, NULL, 0};
    /* Each quad's diagonal adds a point: (0,0) (500,0) (1000,0) (1000,400) (1000,800), either way. */
    ok = cc_cut_section(&sm, 500, &a, &s) == 0 && s.nloops == 0 && s.nopen == 1 && s.open[0].n == 5;
    check_true("an open L sheet: one chain of 5 points, from a free end, through the corner",
               ok && s.open[0].p[2][0] == 1000 && s.open[0].p[2][1] == 0 &&
                   ((s.open[0].p[0][0] == 0 && s.open[0].p[4][1] == 800) || (s.open[0].p[0][1] == 800 && s.open[0].p[4][0] == 0)));
    /* The other winding: now a segment's first point is the free end (the other branch). */
    uint32_t si2[12] = {0, 2, 1, 0, 3, 2, 1, 5, 4, 1, 2, 5};
    sm.idx = si2;
    ok = cc_cut_section(&sm, 500, &a, &s) == 0 && s.nloops == 0 && s.nopen == 1 && s.open[0].n == 5;
    check_true("an open L sheet, wound the other way: one chain of 5 points",
               ok && s.open[0].p[2][0] == 1000 && s.open[0].p[2][1] == 0 &&
                   ((s.open[0].p[0][0] == 0 && s.open[0].p[4][1] == 800) || (s.open[0].p[0][1] == 800 && s.open[0].p[4][0] == 0)));
    arena_free(&a);
}

/* ---- the sample sheet (ccore_golden.rs drawing_sample_sheet) ---- */


static cc_pen pen(double w, cc_rgb c, const double *dash, size_t nd) {
    cc_pen p = {w, c, dash, nd};
    return p;
}

static void test_sample(cc_sheet_font font, const char *file) {
    cc_rgb black = {{0, 0, 0}}, red = {{200, 30, 30}};
    cc_item items[64];
    size_t ni = 0;
    memset(items, 0, sizeof items);
    /* A square with a hole: fill, two hatches, outline. */
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
    /* A triangle, hatched at 0 and 90 degrees (a NaN offset), not filled, not outlined. */
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
    /* A filled area with a hatch of no spacing, and an empty area. */
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
    /* Lines. */
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
    /* Texts. */
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
    Buf b;
    buf_init(&b);
    check_true("sample svg writes", cc_paper_svg(&p, &b) == 0);
    char path[64];
    snprintf(path, sizeof path, "svg/%s", file);
    check_buf(path, &b, path);
    buf_free(&b);
}

/* ---- plan/<model>/cut.svg ---- */

typedef struct {
    double w, h, origin[3], x0, y1, k, ox, oy, cut;
} Frame;

static int read_frame(const char *name, Frame *f) {
    char path[64];
    snprintf(path, sizeof path, "plan/%s/frame.txt", name);
    size_t n;
    char *t = slurp(G, path, &n);
    int ok = sscanf(t,
                    "sheet %lf %lf\nturn Plan(0.0)\norigin %lf %lf %lf\nx0 %lf\ny1 %lf\nk %lf\nox %lf\noy %lf\ncut %lf\n",
                    &f->w, &f->h, &f->origin[0], &f->origin[1], &f->origin[2], &f->x0, &f->y1, &f->k, &f->ox, &f->oy,
                    &f->cut) == 11;
    free(t);
    check_true("frame.txt reads (a plan turned by 0)", ok);
    return ok ? 0 : -1;
}

static void to_paper(const Frame *f, const double o[3], const double p[2], double out[2]) {
    /* ViewFrame::to_paper(scene point + origin), Turn::Plan(0.0): the turn leaves it as it is. */
    double w[3] = {p[0] + o[0], p[1] + o[1], f->cut + o[2]};
    double q[3] = {w[0] - f->origin[0], w[1] - f->origin[1], w[2] - f->origin[2]};
    out[0] = f->ox + (q[0] - f->x0) * f->k;
    out[1] = f->oy + (f->y1 - q[1]) * f->k;
}

static void test_cut_svg(const char *name, const char *file, const char *storey) {
    Frame f;
    if (read_frame(name, &f)) return;
    Loaded L;
    if (load(file, storey, &L)) return;
    check_true("frame.txt: the same cut height", f.cut == L.cut);
    Arena a;
    arena_init(&a, 0);
    size_t cap = 2 * L.s.nelements + 1, ni = 0;
    cc_item *items = calloc(cap, sizeof *items);
    static const double thin_dash[2] = {1.0, 0.5};
    int ok = 1;
    for (size_t k = 0; k < L.s.nelements && ok; k++) {
        cc_mesh m;
        cc_section sec;
        if (cc_scene_world_mesh(&L.s, k, &a, &m) || cc_cut_section(&m, f.cut, &a, &sec)) {
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
                for (size_t j = 0; j < src[i].n; j++) to_paper(&f, L.s.origin, src[i].p[j], r[i].p[j]);
            }
            cc_item *it = &items[ni++];
            it->kind = pass ? CC_ITEM_LINES : CC_ITEM_AREA;
            it->rings = r;
            it->nrings = nr;
            it->has_pen = 1;
            it->pen = pass ? pen(D("0.18"), (cc_rgb){{200, 30, 30}}, thin_dash, 2) : pen(D("0.35"), (cc_rgb){{0, 0, 0}}, NULL, 0);
        }
    }
    check_true("cut.svg: every element", ok);
    char title[128];
    snprintf(title, sizeof title, "Cut %s", storey);
    cc_paper p = {f.w, f.h, title, strlen(title), items, ni, CC_FONT_ROBOTO};
    Buf b;
    buf_init(&b);
    check_true("cut.svg writes", cc_paper_svg(&p, &b) == 0);
    char path[64];
    snprintf(path, sizeof path, "plan/%s/cut.svg", name);
    check_buf(path, &b, path);
    buf_free(&b);
    free(items);
    arena_free(&a);
    unload(&L);
}

/* ---- crop.rs's tests ---- */

static double area_of(const cc_ring *r) { return cc_cut_area(r); }

static void test_crop(void) {
    cc_crop_rect r = {{0, 0}, {10, 10}};
    Arena a;
    arena_init(&a, 0);
    double sq[4][2] = {{5, 2}, {15, 2}, {15, 8}, {5, 8}};
    cc_ring in = {sq, 4}, out;
    check_true("crop polygon", cc_crop_polygon(&r, &in, &a, &out) == 0 && out.n >= 3 && fabs(area_of(&out) - 30.0) < 1e-9);
    double far[3][2] = {{20, 0}, {30, 0}, {30, 5}};
    cc_ring fin = {far, 3};
    check_true("crop polygon outside", cc_crop_polygon(&r, &fin, &a, &out) == 0 && out.n == 0);
    double s1[2][2] = {{-5, 5}, {15, 5}}, c[2][2];
    check_true("crop segment across", cc_crop_segment(&r, (const double(*)[2])s1, c) && c[0][0] == 0 && c[0][1] == 5 && c[1][0] == 10);
    double s2[2][2] = {{-5, -5}, {-1, 20}};
    check_true("crop segment outside", !cc_crop_segment(&r, (const double(*)[2])s2, c));
    double pl[4][2] = {{2, 5}, {12, 5}, {12, 7}, {2, 7}};
    cc_ring line = {pl, 4}, *pieces;
    size_t np;
    check_true("crop polyline: two pieces", cc_crop_polyline(&r, &line, &a, &pieces, &np) == 0 && np == 2);
    double za[3] = {0, 0, 0}, zb[3] = {1000, 0, 2000}, seg[2][2];
    check_true("clip_z", cc_cut_clip_z(za, zb, 500, 1000, seg) && seg[0][0] == 250 && seg[1][0] == 500);
    double zc[3] = {0, 0, 2000}, zd[3] = {5, 0, 2000};
    check_true("clip_z above", !cc_cut_clip_z(zc, zd, 0, 1200, seg));
    int8_t up[8] = {0, 0, 127, 0, 0, 0, -127, 0}, down[8] = {0, 0, -127, 0, 0, 127, 0, 0}, unk[8] = {0};
    check_true("edge seen from above", cc_cut_edge_seen_from_above(up) && !cc_cut_edge_seen_from_above(down) &&
                                           cc_cut_edge_seen_from_above(unk));
    arena_free(&a);
}

int main(void) {
    test_fmt_golden();
    test_fmt_million();
    test_roboto();
    test_crop();
    test_cut("frame", "frame.json", "Niveau 1");
    test_cut("frame2", "frame2.json", "Rez-de-chauss\xc3\xa9" "e");
    test_cut_more("frame", "frame.json", "Niveau 1");
    test_cut_more("frame2", "frame2.json", "Rez-de-chauss\xc3\xa9" "e");
    test_cut_cases();
    test_sample(CC_FONT_HELVETICA, "sample.svg");
    test_sample(CC_FONT_ROBOTO, "sample-roboto.svg");
    test_cut_svg("frame", "frame.json", "Niveau 1");
    test_cut_svg("frame2", "frame2.json", "Rez-de-chauss\xc3\xa9" "e");
    return report("draw");
}
