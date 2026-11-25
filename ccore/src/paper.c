/* sheet.rs of bimble-drawing: paper, items, text widths, hatch lines, SVG (paper.h). */
#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */
#include "paper.h"
#include <stdlib.h>
#include <string.h>
#include "fmt.h"
#include "ifc_math.h" /* cc_sin, cc_cos: correctly rounded */

#include "roboto_widths.inc"

static double f64_bits(uint64_t u) {
    double x;
    memcpy(&x, &u, 8);
    return x;
}

#define DEG_TO_RAD f64_bits(0x3f91df46a2529d39ull) /* PI / 180.0, as f64::to_radians */
#define BOLD_WIDER f64_bits(0x3ff0f5c28f5c28f6ull) /* 1.06 */

static double fmax_rs(double a, double b) { return a != a ? b : b != b ? a : a > b ? a : b; }
static double fmin_rs(double a, double b) { return a != a ? b : b != b ? a : a < b ? a : b; }
static int is_finite(double x) { return x == x && x - x == 0.0; }

/* x as i64 after floor/ceil (Rust `as`: saturating, NaN 0). */
static int64_t to_i64(double x) {
    if (x != x) return 0;
    if (x >= 9223372036854775808.0) return INT64_MAX;
    if (x <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)x;
}

static int64_t floor_i64(double x) {
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return to_i64(x);
    int64_t t = (int64_t)x;
    return (double)t > x ? t - 1 : t;
}

static int64_t ceil_i64(double x) {
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return to_i64(x);
    int64_t t = (int64_t)x;
    return (double)t < x ? t + 1 : t;
}

/* ---- paper sizes ---- */

static const struct {
    const char *name;
    double w, h;
} SIZES[5] = {{"A4", 297.0, 210.0}, {"A3", 420.0, 297.0}, {"A2", 594.0, 420.0}, {"A1", 841.0, 594.0}, {"A0", 1189.0, 841.0}};

void cc_paper_size_mm(cc_paper_size p, double *w, double *h) {
    *w = SIZES[p].w;
    *h = SIZES[p].h;
}

const char *cc_paper_size_name(cc_paper_size p) { return SIZES[p].name; }

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

int cc_paper_size_parse(const char *s, size_t n) {
    while (n && is_space(*s)) s++, n--;
    while (n && is_space(s[n - 1])) n--;
    if (n != 2 || (s[0] != 'a' && s[0] != 'A')) return -1;
    for (int i = 0; i < 5; i++)
        if (SIZES[i].name[1] == s[1]) return i;
    return -1;
}

/* ---- text ---- */

/* Next code point of UTF-8 text (invalid bytes: U+FFFD, one byte each). */
static uint32_t utf8_next(const char *s, size_t n, size_t *i) {
    const unsigned char *u = (const unsigned char *)s + *i;
    size_t left = n - *i;
    uint32_t c = u[0];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (len == 0 || (size_t)len > left) {
        (*i)++;
        return 0xfffd;
    }
    if (len == 1) {
        (*i)++;
        return c;
    }
    c &= 0x7fu >> len;
    for (int k = 1; k < len; k++) {
        if ((u[k] & 0xc0) != 0x80) {
            (*i)++;
            return 0xfffd;
        }
        c = (c << 6) | (u[k] & 0x3f);
    }
    *i += (size_t)len;
    return c;
}

int cc_roboto_glyph(uint32_t cp, int bold, uint16_t *gid, uint16_t *units) {
    size_t lo = 0, hi = ROBOTO_N;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ROBOTO_CP[mid] < cp) lo = mid + 1;
        else hi = mid;
    }
    if (lo < ROBOTO_N && ROBOTO_CP[lo] == cp) {
        *gid = bold ? ROBOTO_GID_1[lo] : ROBOTO_GID_0[lo];
        *units = bold ? ROBOTO_UNITS_1[lo] : ROBOTO_UNITS_0[lo];
        return 1;
    }
    *gid = 0;
    *units = ROBOTO_MISSING[bold ? 1 : 0];
    return 0;
}

/* Advance widths of Helvetica for ASCII 0x20..=0x7e, 1/1000 em (Adobe AFM). */
static const uint16_t HELVETICA[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, /* ' '../ */
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556,                               /* 0..9 */
    278, 278, 584, 584, 584, 556, 1015,                                             /* :..@ */
    667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833,                /* A..M */
    722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611,                /* N..Z */
    278, 278, 278, 469, 556, 333,                                                   /* [..` */
    556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833,                /* a..m */
    556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500,                /* n..z */
    334, 260, 334, 584,                                                             /* {..~ */
};

double cc_text_width_mm(const char *text, size_t n, double size_mm, int bold, cc_sheet_font font) {
    if (font == CC_FONT_ROBOTO) {
        /* Face::width_em: f32 advances (units * 1000 / 2048, exact) summed in order from -0.0
         * (Rust's f32 Sum), then as f64 / 1000. */
        float sum = -0.0f;
        for (size_t i = 0; i < n;) {
            uint16_t gid, units;
            cc_roboto_glyph(utf8_next(text, n, &i), bold, &gid, &units);
            sum += (float)units * 0.48828125f;
        }
        return (double)sum / 1000.0 * size_mm;
    }
    uint32_t units = 0;
    for (size_t i = 0; i < n;) {
        uint32_t c = utf8_next(text, n, &i);
        units += c >= 0x20 && c <= 0x7e ? HELVETICA[c - 0x20] : 556;
    }
    return (double)units / 1000.0 * size_mm * (bold ? BOLD_WIDER : 1.0);
}

/* text_start: the left end of the baseline and the baseline direction (y down). */
static void text_start(const cc_item *t, cc_sheet_font font, double p[2], double u[2]) {
    double a = t->angle_deg * DEG_TO_RAD;
    u[0] = cc_cos(a);
    u[1] = -cc_sin(a);
    double back = 0.0;
    if (t->anchor == CC_ANCHOR_MIDDLE) back = cc_text_width_mm(t->text, t->text_len, t->size_mm, t->bold, font) / 2.0;
    else if (t->anchor == CC_ANCHOR_END) back = cc_text_width_mm(t->text, t->text_len, t->size_mm, t->bold, font);
    p[0] = t->pos[0] - u[0] * back;
    p[1] = t->pos[1] - u[1] * back;
}

/* ---- hatches ---- */

int cc_hatch_lines(const cc_ring *loops, size_t nloops, const cc_hatch *h, double (**out)[2][2], size_t *n) {
    *out = NULL;
    *n = 0;
    double inf = f64_bits(0x7ff0000000000000ull);
    double lo[2] = {inf, inf}, hi[2] = {-inf, -inf};
    for (size_t l = 0; l < nloops; l++)
        for (size_t i = 0; i < loops[l].n; i++)
            for (int k = 0; k < 2; k++) {
                lo[k] = fmin_rs(lo[k], loops[l].p[i][k]);
                hi[k] = fmax_rs(hi[k], loops[l].p[i][k]);
            }
    /* No points (or NaN bounds), or no spacing: nothing to hatch. */
    if ((!(lo[0] < hi[0]) && lo[0] != hi[0]) || h->spacing_mm <= 0.0) return 0;
    double a = h->angle_deg * DEG_TO_RAD;
    double d[2] = {cc_cos(a), -cc_sin(a)};
    double nv[2] = {-d[1], d[0]};
    double corners[4][2] = {{lo[0], lo[1]}, {hi[0], lo[1]}, {hi[0], hi[1]}, {lo[0], hi[1]}};
    double n0 = inf, n1 = -inf, d0 = inf, d1 = -inf;
    for (int c = 0; c < 4; c++) {
        double pn = corners[c][0] * nv[0] + corners[c][1] * nv[1];
        double pd = corners[c][0] * d[0] + corners[c][1] * d[1];
        n0 = fmin_rs(n0, pn);
        n1 = fmax_rs(n1, pn);
        d0 = fmin_rs(d0, pd);
        d1 = fmax_rs(d1, pd);
    }
    double off = is_finite(h->offset_mm) ? h->offset_mm : 0.0;
    int64_t first = ceil_i64((n0 - off) / h->spacing_mm), last = floor_i64((n1 - off) / h->spacing_mm);
    if (last >= first && (uint64_t)last - (uint64_t)first > 100000) return 0;
    if (last < first) return 0;
    size_t cnt = (size_t)(last - first) + 1;
    double(*v)[2][2] = malloc(cnt * sizeof *v);
    if (!v) return -1;
    for (size_t i = 0; i < cnt; i++) {
        double s = (double)(first + (int64_t)i) * h->spacing_mm + off;
        v[i][0][0] = nv[0] * s + d[0] * d0;
        v[i][0][1] = nv[1] * s + d[1] * d0;
        v[i][1][0] = nv[0] * s + d[0] * d1;
        v[i][1][1] = nv[1] * s + d[1] * d1;
    }
    *out = v;
    *n = cnt;
    return 0;
}

/* ---- SVG ---- */

/* `{:.prec}` (prec >= 0) or `{}` (prec < 0) of any f64, with Rust's text for NaN and infinities. */
static void num(Buf *b, double x, int prec) {
    if (fmt_f64_fixed(b, x, prec) == 0) return;
    buf_puts(b, x != x ? "NaN" : x > 0 ? "inf" : "-inf");
}

static void hex(Buf *b, cc_rgb c) {
    static const char H[] = "0123456789abcdef";
    buf_putc(b, '#');
    for (int i = 0; i < 3; i++) {
        buf_putc(b, H[c.c[i] >> 4]);
        buf_putc(b, H[c.c[i] & 15]);
    }
}

static void escape(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': buf_puts(b, "&amp;"); break;
        case '<': buf_puts(b, "&lt;"); break;
        case '>': buf_puts(b, "&gt;"); break;
        case '"': buf_puts(b, "&quot;"); break;
        default: buf_putc(b, s[i]);
        }
    }
}

static void path_d(Buf *b, const cc_ring *loops, size_t n, int close) {
    for (size_t l = 0; l < n; l++) {
        for (size_t k = 0; k < loops[l].n; k++) {
            buf_putc(b, k == 0 ? 'M' : 'L');
            num(b, loops[l].p[k][0], 3);
            buf_putc(b, ' ');
            num(b, loops[l].p[k][1], 3);
        }
        if (close) buf_putc(b, 'Z');
    }
}

static void pen_attrs(Buf *b, const cc_pen *p) {
    buf_puts(b, "stroke=\"");
    hex(b, p->color);
    buf_puts(b, "\" stroke-width=\"");
    num(b, p->width_mm, 3);
    buf_puts(b, "\" stroke-linejoin=\"round\" stroke-linecap=\"butt\"");
    if (p->ndash) {
        buf_puts(b, " stroke-dasharray=\"");
        for (size_t i = 0; i < p->ndash; i++) {
            if (i) buf_putc(b, ' ');
            num(b, p->dash[i], 3);
        }
        buf_putc(b, '"');
    }
}

int cc_paper_svg(const cc_paper *p, Buf *b) {
    buf_puts(b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"");
    num(b, p->width_mm, -1);
    buf_puts(b, "mm\" height=\"");
    num(b, p->height_mm, -1);
    buf_puts(b, "mm\" viewBox=\"0 0 ");
    num(b, p->width_mm, -1);
    buf_putc(b, ' ');
    num(b, p->height_mm, -1);
    buf_puts(b, "\">\n<title>");
    escape(b, p->title, p->title_len);
    buf_puts(b, "</title>\n<rect x=\"0\" y=\"0\" width=\"");
    num(b, p->width_mm, -1);
    buf_puts(b, "\" height=\"");
    num(b, p->height_mm, -1);
    buf_puts(b, "\" fill=\"#ffffff\"/>\n");
    unsigned clip_id = 0;
    for (size_t i = 0; i < p->nitems; i++) {
        const cc_item *it = &p->items[i];
        switch (it->kind) {
        case CC_ITEM_AREA:
            if (it->has_fill) {
                buf_puts(b, "<path d=\"");
                path_d(b, it->rings, it->nrings, 1);
                buf_puts(b, "\" fill=\"");
                hex(b, it->fill);
                buf_puts(b, "\" fill-rule=\"evenodd\"/>\n");
            }
            if (it->nhatches) {
                clip_id++;
                buf_puts(b, "<clipPath id=\"c");
                buf_u64(b, clip_id);
                buf_puts(b, "\"><path d=\"");
                path_d(b, it->rings, it->nrings, 1);
                buf_puts(b, "\" clip-rule=\"evenodd\"/></clipPath><g clip-path=\"url(#c");
                buf_u64(b, clip_id);
                buf_puts(b, ")\">\n");
                for (size_t h = 0; h < it->nhatches; h++) {
                    double(*seg)[2][2];
                    size_t ns;
                    if (cc_hatch_lines(it->rings, it->nrings, &it->hatches[h], &seg, &ns)) return -1;
                    buf_puts(b, "<path d=\"");
                    for (size_t k = 0; k < ns; k++) {
                        cc_ring r = {seg[k], 2};
                        path_d(b, &r, 1, 0);
                    }
                    free(seg);
                    buf_puts(b, "\" fill=\"none\" ");
                    pen_attrs(b, &it->hatches[h].pen);
                    buf_puts(b, "/>\n");
                }
                buf_puts(b, "</g>\n");
            }
            if (it->has_pen) {
                buf_puts(b, "<path d=\"");
                path_d(b, it->rings, it->nrings, 1);
                buf_puts(b, "\" fill=\"none\" ");
                pen_attrs(b, &it->pen);
                buf_puts(b, "/>\n");
            }
            break;
        case CC_ITEM_LINES:
            buf_puts(b, "<path d=\"");
            path_d(b, it->rings, it->nrings, 0);
            buf_puts(b, "\" fill=\"none\" ");
            pen_attrs(b, &it->pen);
            buf_puts(b, "/>\n");
            break;
        case CC_ITEM_TEXT: {
            double s[2], u[2];
            text_start(it, p->font, s, u);
            buf_puts(b, "<text x=\"");
            num(b, s[0], 3);
            buf_puts(b, "\" y=\"");
            num(b, s[1], 3);
            buf_puts(b, p->font == CC_FONT_ROBOTO ? "\" font-family=\"Roboto, Helvetica, Arial, sans-serif\" font-size=\""
                                                  : "\" font-family=\"Helvetica, Arial, sans-serif\" font-size=\"");
            num(b, it->size_mm, 3);
            buf_putc(b, '"');
            if (it->bold) buf_puts(b, " font-weight=\"bold\"");
            buf_puts(b, " fill=\"");
            hex(b, it->color);
            buf_putc(b, '"');
            if (it->angle_deg != 0.0) {
                buf_puts(b, " transform=\"rotate(");
                num(b, -it->angle_deg, 3);
                buf_putc(b, ' ');
                num(b, s[0], 3);
                buf_putc(b, ' ');
                num(b, s[1], 3);
                buf_puts(b, ")\"");
            }
            buf_putc(b, '>');
            escape(b, it->text, it->text_len);
            buf_puts(b, "</text>\n");
            break;
        }
        }
    }
    buf_puts(b, "</svg>\n");
    return b->oom ? -1 : 0;
}
