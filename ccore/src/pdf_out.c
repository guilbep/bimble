/* Sheet::to_pdf of bimble-drawing's sheet.rs, as pdf-writer 0.15 writes it (pdf_out.h). */
#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */
#include "pdf_out.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ifc_math.h" /* cc_sin, cc_cos: correctly rounded */
#include "inflate.h"

/* Ryu's f2s.c, included here to reach its static f2d(): the ryu crate's digits for f32. */
#include "ryu/f2s.c"

static double f64_bits(uint64_t u) {
    double x;
    memcpy(&x, &u, 8);
    return x;
}

static float f32_bits(uint32_t u) {
    float x;
    memcpy(&x, &u, 4);
    return x;
}

#define DEG_TO_RAD f64_bits(0x3f91df46a2529d39ull) /* PI / 180.0, as f64::to_radians */
#define PT f64_bits(0x4006ad5ab56ad5acull)         /* 72.0 / 25.4: points per mm */

/* ---- numbers ---- */

/* Rust's `x as f32`: rounded to nearest; past the largest float (and its half step), infinite. */
static float to_f32(double x) {
    const double edge = f64_bits(0x47effffff0000000ull); /* 2^128 - 2^103 */
    if (x >= edge) return f32_bits(0x7f800000u);
    if (x <= -edge) return f32_bits(0xff800000u);
    return (float)x;
}

/* Rust's `f as i32`: saturating, NaN 0. */
static int32_t sat_i32(float f) {
    if (f != f) return 0;
    if (f >= 2147483648.0f) return INT32_MAX;
    if (f <= -2147483648.0f) return INT32_MIN;
    return (int32_t)f;
}

/* Shortest digits of a finite nonzero |f| (Ryu's f2d): value = mantissa * 10^exponent. */
static void f32_digits(float f, char d[10], int *n, int *e) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    floating_decimal_32 v = f2d(bits & ((1u << FLOAT_MANTISSA_BITS) - 1), (bits >> FLOAT_MANTISSA_BITS) & 0xffu);
    char t[10];
    int k = 0;
    uint32_t m = v.mantissa;
    do t[k++] = (char)('0' + m % 10), m /= 10;
    while (m);
    for (int i = 0; i < k; i++) d[i] = t[k - 1 - i];
    *n = k;
    *e = v.exponent;
}

/* Buf::push_float: an integer without a point when `f as i32 as f32 == f`; else push_decimal:
 * the ryu crate's format32 for 1e-6 < |f| < 1e12, Rust's Display (`{}`) outside. */
static void num(Buf *b, float f) {
    int32_t i = sat_i32(f);
    if ((float)i == f) {
        buf_i64(b, i);
        return;
    }
    if (f != f) {
        buf_puts(b, "NaN");
        return;
    }
    float a = f < 0 ? -f : f;
    if (a == f32_bits(0x7f800000u)) {
        buf_puts(b, f < 0 ? "-inf" : "inf");
        return;
    }
    if (f < 0) buf_putc(b, '-');
    char d[10];
    int n, e;
    f32_digits(a, d, &n, &e);
    int kk = n + e; /* 10^(kk-1) <= |f| < 10^kk */
    if (a > f32_bits(0x358637bdu) && a < f32_bits(0x5368d4a5u)) { /* 1e-6, 1e12: ryu */
        if (e >= 0 && kk <= 13) {
            buf_append(b, d, (size_t)n);
            buf_fill(b, '0', (size_t)e);
            buf_puts(b, ".0");
        } else if (kk > 0 && kk <= 13) {
            buf_append(b, d, (size_t)kk);
            buf_putc(b, '.');
            buf_append(b, d + kk, (size_t)(n - kk));
        } else if (kk > -6 && kk <= 0) {
            buf_puts(b, "0.");
            buf_fill(b, '0', (size_t)-kk);
            buf_append(b, d, (size_t)n);
        } else { /* not reached in this range; the ryu crate's scientific form */
            buf_putc(b, d[0]);
            if (n > 1) {
                buf_putc(b, '.');
                buf_append(b, d + 1, (size_t)(n - 1));
            }
            buf_putc(b, 'e');
            buf_i64(b, kk - 1);
        }
        return;
    }
    /* Display: plain digits, never an exponent. (Rust breaks an exact tie between two shortest
     * candidates upwards where Ryu takes the even one: an integer >= 1e12 only.) */
    if (e >= 0) {
        buf_append(b, d, (size_t)n);
        buf_fill(b, '0', (size_t)e);
    } else if (kk > 0) {
        buf_append(b, d, (size_t)kk);
        buf_putc(b, '.');
        buf_append(b, d + kk, (size_t)(n - kk));
    } else {
        buf_puts(b, "0.");
        buf_fill(b, '0', (size_t)-kk);
        buf_append(b, d, (size_t)n);
    }
}

static void hex2(Buf *b, unsigned v) {
    static const char H[] = "0123456789ABCDEF";
    buf_putc(b, H[(v >> 4) & 15]);
    buf_putc(b, H[v & 15]);
}

/* Str: a literal string for ASCII (parentheses escaped unless balanced), else hex. */
static void str_obj(Buf *b, const unsigned char *s, size_t n) {
    int ascii = 1;
    for (size_t i = 0; i < n; i++) ascii &= s[i] < 0x80;
    if (!ascii) {
        buf_putc(b, '<');
        for (size_t i = 0; i < n; i++) hex2(b, s[i]);
        buf_putc(b, '>');
        return;
    }
    int balanced = -1; /* decided at the first parenthesis */
    buf_putc(b, '(');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '(' || c == ')') {
            if (balanced < 0) {
                if (c == ')') balanced = 0;
                else {
                    int depth = 0;
                    balanced = 1;
                    for (size_t k = 0; k < n && balanced; k++) {
                        if (s[k] == '(') depth++;
                        else if (s[k] == ')') {
                            if (depth > 0) depth--;
                            else balanced = 0;
                        }
                    }
                    if (depth) balanced = 0;
                }
            }
            if (!balanced) buf_putc(b, '\\');
            buf_putc(b, (char)c);
        } else if (c == '\\') buf_puts(b, "\\\\");
        else if (c >= ' ' && c <= '~') buf_putc(b, (char)c);
        else if (c == '\n') buf_puts(b, "\\n");
        else if (c == '\r') buf_puts(b, "\\r");
        else if (c == '\t') buf_puts(b, "\\t");
        else if (c == 8) buf_puts(b, "\\b");
        else if (c == 12) buf_puts(b, "\\f");
        else {
            buf_putc(b, '\\');
            buf_putc(b, (char)('0' + (c >> 6)));
            buf_putc(b, (char)('0' + ((c >> 3) & 7)));
            buf_putc(b, (char)('0' + (c & 7)));
        }
    }
    buf_putc(b, ')');
}

/* Next code point of UTF-8 text (invalid bytes: U+FFFD, one byte each), as paper.c. */
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

/* A code point as UTF-16 hex (a pair of surrogates past the BMP). */
static void utf16_hex(Buf *b, uint32_t c) {
    if (c >= 0x10000) {
        c -= 0x10000;
        uint32_t hi = 0xd800 + (c >> 10), lo = 0xdc00 + (c & 0x3ff);
        hex2(b, hi >> 8), hex2(b, hi & 0xff), hex2(b, lo >> 8), hex2(b, lo & 0xff);
    } else {
        hex2(b, c >> 8), hex2(b, c & 0xff);
    }
}

/* TextStr: bare ASCII (32..126) as a Str, else the byte order mark and UTF-16BE in hex. */
static void text_str(Buf *b, const char *s, size_t n) {
    int plain = 1;
    for (size_t i = 0; i < n; i++) plain &= (unsigned char)s[i] >= 32 && (unsigned char)s[i] <= 126;
    if (plain) {
        str_obj(b, (const unsigned char *)s, n);
        return;
    }
    buf_puts(b, "<FEFF");
    for (size_t i = 0; i < n;) utf16_hex(b, utf8_next(s, n, &i));
    buf_putc(b, '>');
}

/* ---- the content stream ---- */

/* Content ops in pretty mode: one per line ('\n' before each but the first), operands
 * separated by spaces, no newline at the end. */
static void op_start(Buf *c) {
    if (c->len) buf_putc(c, '\n');
}

static void op_nums(Buf *c, const float *v, int n, const char *op) {
    op_start(c);
    for (int i = 0; i < n; i++) {
        num(c, v[i]);
        buf_putc(c, ' ');
    }
    buf_puts(c, op);
}

static void op0(Buf *c, const char *op) {
    op_start(c);
    buf_puts(c, op);
}

static void rgb_op(Buf *c, cc_rgb col, const char *op) {
    float v[3];
    for (int i = 0; i < 3; i++) v[i] = (float)col.c[i] / 255.0f; /* Rgb::unit */
    op_nums(c, v, 3, op);
}

static void point_op(Buf *c, const double p[2], const char *op) {
    float v[2] = {to_f32(p[0]), to_f32(p[1])};
    op_nums(c, v, 2, op);
}

static void path(Buf *c, const cc_ring *loops, size_t n, int close) {
    for (size_t l = 0; l < n; l++) {
        for (size_t k = 0; k < loops[l].n; k++) point_op(c, loops[l].p[k], k == 0 ? "m" : "l");
        if (close) op0(c, "h");
    }
}

static int set_pen(Buf *c, const cc_pen *p) {
    rgb_op(c, p->color, "RG");
    float w = to_f32(p->width_mm);
    if (!(w >= 0.0f)) return CC_PDF_ERR_PEN;
    op_nums(c, &w, 1, "w");
    op_start(c);
    buf_putc(c, '[');
    for (size_t i = 0; i < p->ndash; i++) {
        if (i) buf_putc(c, ' ');
        num(c, to_f32(p->dash[i]));
    }
    buf_puts(c, "] 0 d");
    return 0;
}

/* text_start (as paper.c): the left end of the baseline and the baseline direction (y down). */
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

/* win_ansi: Latin-1 as is, three typographic marks, '?' for anything else. */
static unsigned char win_ansi(uint32_t c) {
    if ((c >= 0x20 && c <= 0x7e) || (c >= 0xa0 && c <= 0xff)) return (unsigned char)c;
    if (c == 0x2013) return 0x96; /* en dash */
    if (c == 0x2014) return 0x97; /* em dash */
    if (c == 0x2019) return 0x92; /* right single quote */
    return '?';
}

static int text(Buf *c, const cc_item *t, cc_sheet_font font) {
    rgb_op(c, t->color, "rg");
    op0(c, "BT");
    op_start(c);
    buf_puts(c, t->bold ? "/F2 1 Tf" : "/F1 1 Tf");
    double p[2], u[2];
    text_start(t, font, p, u);
    double s = t->size_mm;
    double m[6] = {s * u[0], s * u[1], s * u[1], -s * u[0], p[0], p[1]};
    float mf[6];
    for (int i = 0; i < 6; i++) mf[i] = to_f32(m[i]);
    op_nums(c, mf, 6, "Tm");
    Buf bytes;
    buf_init(&bytes);
    for (size_t i = 0; i < t->text_len;) {
        uint32_t cp = utf8_next(t->text, t->text_len, &i);
        if (font == CC_FONT_ROBOTO) {
            uint16_t gid, units;
            cc_roboto_glyph(cp, t->bold, &gid, &units);
            buf_putc(&bytes, (char)(gid >> 8));
            buf_putc(&bytes, (char)(gid & 0xff));
        } else {
            buf_putc(&bytes, (char)win_ansi(cp));
        }
    }
    op_start(c);
    str_obj(c, (const unsigned char *)bytes.p, bytes.len);
    buf_puts(c, " Tj");
    int oom = bytes.oom;
    buf_free(&bytes);
    op0(c, "ET");
    return oom ? CC_PDF_ERR_OOM : 0;
}

int cc_paper_pdf_content(const cc_paper *p, Buf *c) {
    size_t start = c->len;
    Buf own;
    buf_init(&own);
    /* Work in a buffer of its own: "first operation" means an empty stream. */
    Buf *b = start ? &own : c;
    int rc = 0;
    /* Work in mm with y down, like the sheet: scale to points and flip. */
    float cm[6] = {to_f32(PT), 0.0f, 0.0f, to_f32(-PT), 0.0f, to_f32(p->height_mm * PT)};
    op_nums(b, cm, 6, "cm");
    for (size_t i = 0; i < p->nitems && !rc; i++) {
        const cc_item *it = &p->items[i];
        switch (it->kind) {
        case CC_ITEM_AREA:
            if (it->has_fill) {
                rgb_op(b, it->fill, "rg");
                path(b, it->rings, it->nrings, 1);
                op0(b, "f*");
            }
            if (it->nhatches) {
                op0(b, "q");
                path(b, it->rings, it->nrings, 1);
                op0(b, "W*");
                op0(b, "n");
                for (size_t h = 0; h < it->nhatches && !rc; h++) {
                    if ((rc = set_pen(b, &it->hatches[h].pen))) break;
                    double(*seg)[2][2];
                    size_t ns;
                    if (cc_hatch_lines(it->rings, it->nrings, &it->hatches[h], &seg, &ns)) {
                        rc = CC_PDF_ERR_OOM;
                        break;
                    }
                    for (size_t k = 0; k < ns; k++) {
                        point_op(b, seg[k][0], "m");
                        point_op(b, seg[k][1], "l");
                    }
                    free(seg);
                    op0(b, "S");
                }
                op0(b, "Q");
            }
            if (it->has_pen && !rc && !(rc = set_pen(b, &it->pen))) {
                path(b, it->rings, it->nrings, 1);
                op0(b, "S");
            }
            break;
        case CC_ITEM_LINES:
            if ((rc = set_pen(b, &it->pen))) break;
            path(b, it->rings, it->nrings, 0);
            op0(b, "S");
            break;
        case CC_ITEM_TEXT: rc = text(b, it, p->font); break;
        }
    }
    if (b == &own) buf_append(c, own.p, own.len);
    if (!rc && (b->oom || c->oom)) rc = CC_PDF_ERR_OOM;
    buf_free(&own);
    return rc;
}

/* ---- the file ---- */

#define NOBJ 16 /* ids 1..15: 1-7 fixed, 8-11 Roboto regular, 12-15 bold */

typedef struct {
    Buf *b;
    size_t base;        /* where the file starts in b */
    size_t off[NOBJ];   /* offset + 1 of each object written (0: none) */
} Pdf;

static void obj_begin(Pdf *w, int id) {
    w->off[id] = w->b->len - w->base + 1;
    buf_i64(w->b, id);
    buf_puts(w->b, " 0 obj\n");
}

static void obj_end(Pdf *w) { buf_puts(w->b, "\nendobj\n\n"); }

static void ref(Buf *b, int id) {
    buf_i64(b, id);
    buf_puts(b, " 0 R");
}

static void key_ref(Buf *b, const char *key, int id) {
    buf_puts(b, key);
    ref(b, id);
}

/* A stream object: its dictionary (/Length then `extra`, lines indented by two) and data. */
static void stream_obj(Pdf *w, int id, const char *data, size_t n, const char *extra) {
    obj_begin(w, id);
    buf_puts(w->b, "<<\n  /Length ");
    buf_i64(w->b, (int64_t)n);
    buf_puts(w->b, extra);
    buf_puts(w->b, "\n>>\nstream\n");
    buf_append(w->b, data, n);
    buf_puts(w->b, "\nendstream");
    obj_end(w);
}

/* ---- Roboto ---- */

/* Face metrics of the shipped files, font units (2048 per em; PDF wants 1/1000 em, `* 1000/2048`
 * exact in f32): ascent, descent, cap height, bbox. Golden roboto.txt has them in 1/1000 em. */
static const int16_t ROBOTO_METRICS[2][7] = {
    {1900, -500, 1456, -1825, -555, 4188, 2163},
    {1900, -500, 1456, -1870, -555, 4220, 2163},
};
static const char *const ROBOTO_NAME[2] = {"Roboto-Regular", "Roboto-Bold"};
static const uint32_t ROBOTO_SIZE[2] = {349400, 351108};
static const uint32_t ROBOTO_CHECK[2] = {0x79ee70a5u, 0xd0be291bu}; /* head.checkSumAdjustment */

static uint32_t be32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int cc_pdf_roboto_is_shipped(const unsigned char *data, size_t n, int bold) {
    bold = bold ? 1 : 0;
    if (!data || n != ROBOTO_SIZE[bold] || n < 12) return 0;
    size_t tables = (size_t)data[4] << 8 | data[5];
    for (size_t i = 0; i < tables && 12 + 16 * i + 16 <= n; i++) {
        const unsigned char *r = data + 12 + 16 * i;
        if (memcmp(r, "head", 4)) continue;
        uint32_t off = be32(r + 8);
        return off <= n - 12 && be32(data + off + 8) == ROBOTO_CHECK[bold];
    }
    return 0;
}

static float em(int units) { return (float)units * 0.48828125f; } /* * (1000 / 2048) */

static void put_em(Buf *b, int units) { num(b, em(units)); }

/* zlib (RFC 1950) of `data`: the raw deflate of cc_gzip between a zlib head and the Adler-32. */
static int zlib(const unsigned char *data, size_t n, Buf *out) {
    Buf gz;
    buf_init(&gz);
    if (cc_gzip(data, n, &gz) || gz.oom || gz.len < 18) {
        buf_free(&gz);
        return CC_PDF_ERR_OOM;
    }
    buf_putc(out, 0x78);
    buf_putc(out, 0x01); /* deflate, 32 KiB window, "fastest" (the level is a hint) */
    buf_append(out, gz.p + 10, gz.len - 18);
    buf_free(&gz);
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < n;) {
        size_t end = n - i > 5552 ? i + 5552 : n;
        for (; i < end; i++) {
            s1 += data[i];
            s2 += s1;
        }
        s1 %= 65521;
        s2 %= 65521;
    }
    uint32_t a = s2 << 16 | s1;
    for (int k = 3; k >= 0; k--) buf_putc(out, (char)(a >> (8 * k)));
    return out->oom ? CC_PDF_ERR_OOM : 0;
}

/* embed_font: font `id` (5 or 7) as a Type 0 font over the whole file, its text as 2-byte glyph
 * ids, with the widths and the characters of the glyphs used (`first[g]`: the first character
 * drawn with glyph g, or UINT32_MAX). Its four objects take the ids from *next on. */
static int embed_font(Pdf *w, int id, int bold, const uint32_t *first, const cc_pdf_options *o, int *next) {
    Buf *b = w->b;
    int cid = *next, descriptor = *next + 1, cmap = *next + 2, file = *next + 3;
    *next += 4;
    const char *name = ROBOTO_NAME[bold];
    int used = 0;
    for (uint32_t g = 0; g < 65536 && !used; g++) used = first[g] != UINT32_MAX;
    obj_begin(w, id);
    buf_puts(b, "<<\n  /Type /Font\n  /Subtype /Type0\n  /BaseFont /");
    buf_puts(b, name);
    buf_puts(b, "\n  /Encoding /Identity-H\n  /DescendantFonts [");
    ref(b, cid);
    buf_puts(b, "]\n");
    key_ref(b, "  /ToUnicode ", cmap);
    buf_puts(b, "\n>>");
    obj_end(w);

    obj_begin(w, cid);
    buf_puts(b, "<<\n  /Type /Font\n  /Subtype /CIDFontType2\n  /BaseFont /");
    buf_puts(b, name);
    buf_puts(b, "\n  /CIDSystemInfo <<\n    /Registry (Adobe)\n    /Ordering (Identity)\n    /Supplement 0\n  >>\n");
    key_ref(b, "  /FontDescriptor ", descriptor);
    buf_puts(b, "\n  /DW 0\n  /CIDToGIDMap /Identity\n  /W [");
    int any = 0;
    for (uint32_t g = 0; g < 65536; g++) {
        if (first[g] == UINT32_MAX) continue;
        uint16_t gid, units;
        cc_roboto_glyph(first[g], bold, &gid, &units);
        if (any++) buf_putc(b, ' ');
        buf_i64(b, g);
        buf_puts(b, " [");
        put_em(b, units);
        buf_putc(b, ']');
    }
    buf_puts(b, "]\n>>");
    obj_end(w);

    const int16_t *m = ROBOTO_METRICS[bold];
    obj_begin(w, descriptor);
    buf_puts(b, "<<\n  /Type /FontDescriptor\n  /FontName /");
    buf_puts(b, name);
    buf_puts(b, "\n  /Flags 32\n  /FontBBox [");
    for (int k = 0; k < 4; k++) {
        if (k) buf_putc(b, ' ');
        put_em(b, m[3 + k]);
    }
    buf_puts(b, "]\n  /ItalicAngle 0\n  /Ascent ");
    put_em(b, m[0]);
    buf_puts(b, "\n  /Descent ");
    put_em(b, m[1]);
    buf_puts(b, "\n  /CapHeight ");
    put_em(b, m[2]);
    buf_puts(b, "\n  /StemV 80");
    if (used) key_ref(b, "\n  /FontFile2 ", file);
    buf_puts(b, "\n>>");
    obj_end(w);

    /* UnicodeCmap: the static header, then at most 100 pairs per bfchar block. */
    Buf u;
    buf_init(&u);
    buf_puts(&u, "%!PS-Adobe-3.0 Resource-CMap\n%%DocumentNeededResources: procset CIDInit\n"
                 "%%IncludeResource: procset CIDInit\n%%BeginResource: CMap Bimble-UTF16\n"
                 "%%Title: (Bimble-UTF16 Adobe Identity 0)\n%%Version: 1\n%%EndComments\n"
                 "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                 "/CIDSystemInfo 3 dict dup begin\n    /Registry (Adobe) def\n    /Ordering (Identity) def\n"
                 "    /Supplement 0 def\nend def\n/CMapName /Bimble-UTF16 def\n/CMapVersion 1 def\n"
                 "/CMapType 0 def\n/WMode 0 def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n");
    uint32_t g = 0;
    for (;;) {
        uint32_t block[100], nb = 0;
        for (; g < 65536 && nb < 100; g++)
            if (first[g] != UINT32_MAX) block[nb++] = g;
        if (!nb) break;
        buf_i64(&u, nb);
        buf_puts(&u, " beginbfchar\n");
        for (uint32_t k = 0; k < nb; k++) {
            buf_putc(&u, '<');
            hex2(&u, block[k] >> 8);
            hex2(&u, block[k] & 0xff);
            buf_puts(&u, "> <");
            utf16_hex(&u, first[block[k]]);
            buf_puts(&u, ">\n");
        }
        buf_puts(&u, "endbfchar\n");
    }
    buf_puts(&u, "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n%%EndResource\n%%EOF");
    int rc = u.oom ? CC_PDF_ERR_OOM : 0;
    if (!rc) stream_obj(w, cmap, u.p, u.len, "\n  /Type /CMap");
    buf_free(&u);
    if (rc || !used) return rc;

    const unsigned char *data = o ? o->roboto[bold] : NULL;
    size_t n = o ? o->roboto_len[bold] : 0;
    if (!cc_pdf_roboto_is_shipped(data, n, bold)) return CC_PDF_ERR_FONT;
    Buf z;
    buf_init(&z);
    rc = zlib(data, n, &z);
    Buf e;
    buf_init(&e);
    buf_puts(&e, "\n  /Filter /FlateDecode\n  /Length1 ");
    buf_i64(&e, (int64_t)n);
    if (!rc && e.oom) rc = CC_PDF_ERR_OOM;
    if (!rc) stream_obj(w, file, z.p, z.len, buf_cstr(&e));
    buf_free(&e);
    buf_free(&z);
    return rc;
}

int cc_paper_pdf(const cc_paper *p, const cc_pdf_options *o, Buf *b) {
    Pdf w;
    memset(&w, 0, sizeof w);
    w.b = b;
    w.base = b->len;
    /* The content first: a bad pen fails before anything is written. */
    Buf content;
    buf_init(&content);
    int rc = cc_paper_pdf_content(p, &content);
    if (rc) {
        buf_free(&content);
        return rc;
    }
    float wh[2] = {to_f32(p->width_mm * PT), to_f32(p->height_mm * PT)};
    buf_append(b, "%PDF-1.7\n%\x80\x80\x80\x80\n\n", 16);
    obj_begin(&w, 1);
    buf_puts(b, "<<\n  /Type /Catalog\n  /Pages 2 0 R\n>>");
    obj_end(&w);
    obj_begin(&w, 2);
    buf_puts(b, "<<\n  /Type /Pages\n  /Kids [3 0 R]\n  /Count 1\n>>");
    obj_end(&w);
    obj_begin(&w, 3);
    buf_puts(b, "<<\n  /Type /Page\n  /MediaBox [0 0 ");
    num(b, wh[0]);
    buf_putc(b, ' ');
    num(b, wh[1]);
    buf_puts(b, "]\n  /Parent 2 0 R\n  /Contents 4 0 R\n  /Resources <<\n    /Font <<\n      /F1 5 0 R\n"
                 "      /F2 7 0 R\n    >>\n  >>\n>>");
    obj_end(&w);
    if (p->font == CC_FONT_ROBOTO) {
        /* The glyphs each weight's texts use, each with its first character. */
        uint32_t *first = malloc(2 * 65536 * sizeof *first);
        if (!first) rc = CC_PDF_ERR_OOM;
        else {
            memset(first, 0xff, 2 * 65536 * sizeof *first);
            for (size_t i = 0; i < p->nitems; i++) {
                const cc_item *t = &p->items[i];
                if (t->kind != CC_ITEM_TEXT) continue;
                for (size_t k = 0; k < t->text_len;) {
                    uint32_t cp = utf8_next(t->text, t->text_len, &k);
                    uint16_t gid, units;
                    cc_roboto_glyph(cp, t->bold, &gid, &units);
                    uint32_t *f = &first[(t->bold ? 65536 : 0) + gid];
                    if (*f == UINT32_MAX) *f = cp;
                }
            }
            int next = 8;
            rc = embed_font(&w, 5, 0, first, o, &next);
            if (!rc) rc = embed_font(&w, 7, 1, first + 65536, o, &next);
            free(first);
        }
    } else {
        obj_begin(&w, 5);
        buf_puts(b, "<<\n  /Type /Font\n  /Subtype /Type1\n  /BaseFont /Helvetica\n  /Encoding /WinAnsiEncoding\n>>");
        obj_end(&w);
        obj_begin(&w, 7);
        buf_puts(b, "<<\n  /Type /Font\n  /Subtype /Type1\n  /BaseFont /Helvetica-Bold\n  /Encoding /WinAnsiEncoding\n>>");
        obj_end(&w);
    }
    if (rc) {
        buf_free(&content);
        return rc;
    }
    obj_begin(&w, 6);
    buf_puts(b, "<<\n  /Title ");
    text_str(b, p->title, p->title_len);
    buf_puts(b, "\n  /Producer ");
    {
        Buf v;
        buf_init(&v);
        buf_puts(&v, "Bimble ");
        if (o && o->version) buf_append(&v, o->version, o->version_len);
        text_str(b, v.p ? v.p : "", v.len);
        if (v.oom) rc = CC_PDF_ERR_OOM;
        buf_free(&v);
    }
    buf_puts(b, "\n>>");
    obj_end(&w);
    stream_obj(&w, 4, content.p, content.len, "");
    buf_free(&content);
    /* The cross-reference table: a free entry points to the next free id (0 after the last). */
    size_t xref = b->len - w.base;
    int size = 1;
    for (int id = 1; id < NOBJ; id++)
        if (w.off[id]) size = id + 1;
    buf_puts(b, "xref\n0 ");
    buf_i64(b, size);
    buf_putc(b, '\n');
    for (int id = 0; id < size; id++) {
        char line[21];
        uint64_t v;
        const char *tail;
        if (id && w.off[id]) {
            v = w.off[id] - 1;
            tail = " 00000 n\r\n";
        } else {
            int nx = id + 1;
            while (nx < size && w.off[nx]) nx++;
            v = (uint64_t)(nx % size);
            tail = id ? " 00000 f\r\n" : " 65535 f\r\n";
        }
        for (int k = 9; k >= 0; k--, v /= 10) line[k] = (char)('0' + v % 10);
        memcpy(line + 10, tail, 10);
        buf_append(b, line, 20);
    }
    buf_puts(b, "trailer\n<<\n  /Size ");
    buf_i64(b, size);
    buf_puts(b, "\n  /Root 1 0 R\n  /Info 6 0 R\n>>\nstartxref\n");
    buf_i64(b, (int64_t)xref);
    buf_puts(b, "\n%%EOF");
    if (!rc && b->oom) rc = CC_PDF_ERR_OOM;
    return rc;
}
