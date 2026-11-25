/* style.rs, pens.rs, hatches.rs of bimble-core and the pens and looks of plan.rs (draw_style.h). */
#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */
#include "draw_style.h"
#include <string.h>
#include "fmt.h"
#include "json_w.h"

static const char *const CAT_LABEL[CC_CAT_COUNT] = {"Concrete", "Steel",  "Timber", "Masonry",
                                                    "Insulation", "Earth", "Glass", "Other"};
const char *cc_category_label(cc_category c) { return (unsigned)c < CC_CAT_COUNT ? CAT_LABEL[c] : "?"; }

static const char *const ROLE_NAME[CC_ROLE_COUNT] = {"Cut", "Seen", "Above", "Hatch", "Axis", "Dimension", "Frame"};
/* serde keys of LineRoles, in the same order. */
static const char *const ROLE_KEY[CC_ROLE_COUNT] = {"cut", "seen", "above", "hatch", "axis", "dimension", "frame"};
const char *cc_role_name(cc_role r) { return (unsigned)r < CC_ROLE_COUNT ? ROLE_NAME[r] : "?"; }

/* ---- f32 numbers ---- */

/* A small unsigned big integer, enough for D * 10^22 * 2^150 (f32 against a decimal). */
#define BIG_LIMBS 20
typedef struct {
    uint32_t w[BIG_LIMBS];
    int n;
} Big;

static void big_set(Big *b, uint64_t v) {
    b->n = 0;
    while (v) {
        b->w[b->n++] = (uint32_t)v;
        v >>= 32;
    }
}

static int big_mul(Big *b, uint32_t m) {
    uint64_t c = 0;
    for (int i = 0; i < b->n; i++) {
        c += (uint64_t)b->w[i] * m;
        b->w[i] = (uint32_t)c;
        c >>= 32;
    }
    if (c) {
        if (b->n == BIG_LIMBS) return -1;
        b->w[b->n++] = (uint32_t)c;
    }
    return 0;
}

static int big_shl(Big *b, int s) {
    for (; s >= 32; s -= 32) {
        if (!b->n) return 0;
        if (b->n == BIG_LIMBS) return -1;
        memmove(b->w + 1, b->w, (size_t)b->n * 4);
        b->w[0] = 0;
        b->n++;
    }
    for (; s > 0; s--)
        if (big_mul(b, 2)) return -1;
    return 0;
}

static int big_cmp(const Big *a, const Big *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

/* Sign of D * 10^k - N * 2^f, exactly (|k| <= 22, f within the f32 range). */
static int cmp_dec_bin(uint64_t d, int k, uint64_t n, int f) {
    Big l, r;
    big_set(&l, d);
    big_set(&r, n);
    for (int i = 0; i < (k < 0 ? -k : k); i++) big_mul(k >= 0 ? &l : &r, 10);
    big_shl(f >= 0 ? &r : &l, f >= 0 ? f : -f);
    return big_cmp(&l, &r);
}

static const double P10[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                               1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

/* The shortest decimal D * 10^k inside v's rounding interval (the bounds count when v's mantissa
 * is even, as Ryu), the nearest to v among the shortest (a tie: the even one), as Ryu's f2s. */
int cc_f32_shortest(float v, double *out) {
    uint32_t u;
    memcpy(&u, &v, 4);
    uint32_t ie = (u >> 23) & 0xff, im = u & 0x7fffff;
    if (ie == 0xff) return -1;
    int neg = (int)(u >> 31);
    if (!ie && !im) {
        *out = neg ? -0.0 : 0.0;
        return 0;
    }
    uint64_t m = ie ? (im | 0x800000u) : im;
    int e = ie ? (int)ie - 150 : -149;
    /* The interval: (2m - 1) 2^(e-1) .. (2m + 1) 2^(e-1); below a power of two the lower half is
     * half as wide: (4m - 1) 2^(e-2). */
    uint64_t lo_n = 2 * m - 1, hi_n = 2 * m + 1;
    int lo_f = e - 1, hi_f = e - 1;
    if (!im && ie > 1) {
        lo_n = 4 * m - 1;
        lo_f = e - 2;
    }
    int accept = (m & 1) == 0;
    double vd = neg ? -(double)v : (double)v;
    for (int k = 22; k >= -22; k--) {
        double q = k >= 0 ? vd / P10[k] : vd * P10[-k];
        if (q >= 1e10) break; /* more than 10 digits: an f32 never needs them */
        int64_t d0 = (int64_t)q;
        uint64_t best = 0;
        for (int64_t d = d0 - 1; d <= d0 + 2; d++) {
            if (d < 1) continue;
            int cl = cmp_dec_bin((uint64_t)d, k, lo_n, lo_f), ch = cmp_dec_bin((uint64_t)d, k, hi_n, hi_f);
            int inside = (cl > 0 || (cl == 0 && accept)) && (ch < 0 || (ch == 0 && accept));
            if (!inside) continue;
            if (!best) {
                best = (uint64_t)d;
                continue;
            }
            /* Both inside: the nearer to v, by the midpoint (2 best + 1) 10^k against 2v. */
            int c = cmp_dec_bin(2 * best + 1, k, m, e + 1);
            if (c < 0 || (c == 0 && (d % 2) == 0)) best = (uint64_t)d;
        }
        if (best) {
            double x = k >= 0 ? (double)best * P10[k] : (double)best / P10[-k];
            *out = neg ? -x : x;
            return 0;
        }
    }
    return -1;
}

/* ---- colours ---- */

/* f32::round (half away from zero) and `as u8` (saturating, NaN 0). */
static uint8_t round_u8(float x) {
    if (!(x > 0.0f)) return 0;
    if (x >= 255.5f) return 255;
    float t = (float)(int32_t)x;
    float d = x - t; /* exact */
    if (d >= 0.5f) t += 1.0f;
    return t >= 255.0f ? 255 : (uint8_t)t;
}

cc_rgb cc_rgb_darken(cc_rgb c, float k) {
    cc_rgb o;
    float f = 1.0f - k;
    for (int i = 0; i < 3; i++) {
        float x = (float)c.c[i] * f;
        o.c[i] = round_u8(x);
    }
    return o;
}

cc_rgb cc_rgb_lighten(cc_rgb c, float k) {
    cc_rgb o;
    for (int i = 0; i < 3; i++) {
        float v = (float)c.c[i];
        float t = (255.0f - v) * k;
        float x = v + t;
        o.c[i] = round_u8(x);
    }
    return o;
}

void cc_rgb_hex(Buf *b, cc_rgb c) {
    static const char H[] = "0123456789abcdef";
    buf_putc(b, '#');
    for (int i = 0; i < 3; i++) {
        buf_putc(b, H[c.c[i] >> 4]);
        buf_putc(b, H[c.c[i] & 15]);
    }
}

/* ---- the defaults ---- */

static cc_rgb rgb(uint8_t r, uint8_t g, uint8_t b) {
    cc_rgb c = {{r, g, b}};
    return c;
}

static int str_of(Arena *a, const char *s, size_t n, cc_str *out) {
    char *p = arena_alloc(a, n + 1, 1);
    if (!p) return -1;
    memcpy(p, s, n);
    out->p = p;
    out->n = n;
    return 0;
}

static int strz(Arena *a, const char *s, cc_str *out) { return str_of(a, s, strlen(s), out); }

static cc_style_lines lines(float angle, float spacing, float width) {
    cc_style_lines l = {angle, spacing, width, 0, 0.0f, 1.0f};
    return l;
}

static cc_fill plain(cc_rgb c) {
    cc_fill f;
    memset(&f, 0, sizeof f);
    f.color = c;
    return f;
}

static void pen_table_default(cc_pen_table *t) {
    static const float model[CC_PENS][7] = {
        {0.10f, 0.10f, 0.10f, 0.10f, 0.10f, 0.10f, 0.10f}, {0.20f, 0.20f, 0.20f, 0.20f, 0.20f, 0.10f, 0.10f},
        {0.30f, 0.25f, 0.35f, 0.35f, 0.25f, 0.18f, 0.15f}, {0.70f, 0.70f, 0.50f, 0.50f, 0.35f, 0.25f, 0.18f},
        {1.00f, 1.00f, 0.70f, 0.70f, 0.50f, 0.35f, 0.25f}, {1.40f, 1.40f, 1.00f, 1.00f, 0.70f, 0.50f, 0.35f},
        {2.00f, 2.00f, 1.40f, 1.40f, 1.00f, 0.70f, 0.50f}, {2.80f, 2.80f, 2.00f, 2.00f, 1.40f, 1.00f, 0.70f},
        {4.00f, 4.00f, 2.80f, 2.80f, 2.00f, 1.40f, 1.00f}, {5.00f, 5.00f, 4.00f, 4.00f, 2.80f, 2.00f, 1.40f},
        {6.00f, 6.00f, 5.00f, 5.00f, 4.00f, 2.80f, 2.00f}, {7.00f, 7.00f, 6.00f, 6.00f, 5.00f, 4.00f, 2.80f},
        {8.00f, 8.00f, 7.00f, 7.00f, 6.00f, 5.00f, 4.00f}, {9.00f, 9.00f, 8.00f, 8.00f, 7.00f, 6.00f, 5.00f},
        {9.00f, 9.00f, 9.00f, 9.00f, 8.00f, 7.00f, 6.00f}, {0.05f, 0.05f, 0.18f, 0.18f, 0.10f, 0.10f, 0.10f},
    };
    static const float annotation[CC_PENS] = {0.10f, 0.18f, 0.25f, 0.35f, 0.50f, 0.70f, 1.00f, 1.40f,
                                              2.00f, 2.80f, 4.00f, 5.00f, 6.00f, 7.00f, 8.00f, 10.00f};
    static const uint32_t scales[7] = {5, 10, 20, 50, 100, 200, 500};
    memset(t, 0, sizeof *t);
    t->nscales = 7;
    for (int i = 0; i < 7; i++) t->scales[i] = scales[i];
    for (int p = 0; p < CC_PENS; p++) {
        for (int i = 0; i < 7; i++) t->model[p][i] = model[p][i];
        t->perspective[p] = annotation[p];
        t->annotation[p] = annotation[p];
    }
}

static const struct {
    const char *name;
    uint8_t c[3];
} LINE_COLORS[9] = {{"black", {0, 0, 0}},       {"red", {230, 40, 30}},    {"grey", {128, 128, 128}},
                    {"green", {40, 160, 40}},   {"brown", {150, 90, 40}},  {"orange", {240, 130, 40}},
                    {"blue", {30, 110, 240}},   {"pink", {230, 30, 230}},  {"yellow", {230, 210, 20}}};

/* pens::default_line_types: colour x pattern x pen. */
static int line_types_default(cc_paper_style *p, Arena *a) {
    static const float DASH[2] = {2.0f, 1.0f}, AXES[4] = {12.0f, 1.5f, 0.5f, 1.5f}, AXES_DETAIL[4] = {3.0f, 1.0f, 0.5f, 1.0f};
    static const uint8_t PEN4[4] = {1, 2, 3, 5};
    p->nline_types = 9 * 11;
    p->line_types = arena_alloc(a, p->nline_types * sizeof *p->line_types, _Alignof(cc_line_type));
    if (!p->line_types) return -1;
    size_t n = 0;
    char name[64];
    for (int c = 0; c < 9; c++) {
        cc_rgb col = rgb(LINE_COLORS[c].c[0], LINE_COLORS[c].c[1], LINE_COLORS[c].c[2]);
        for (int kind = 0; kind < 4; kind++) {
            int npen = kind < 2 ? 4 : kind == 2 ? 2 : 1;
            for (int i = 0; i < npen; i++) {
                cc_line_type *t = &p->line_types[n++];
                t->color = col;
                t->pen = kind == 3 ? 1 : PEN4[i];
                const float *d = kind == 0 ? NULL : kind == 1 ? DASH : kind == 2 ? AXES : AXES_DETAIL;
                t->ndash = kind == 0 ? 0 : kind == 1 ? 2 : 4;
                for (size_t j = 0; j < t->ndash; j++) t->dash_mm[j] = d[j];
                const char *pat = kind == 0 ? "continu" : kind == 1 ? "dash" : kind == 2 ? "axes" : "axes_detail";
                Buf b;
                buf_init(&b);
                buf_puts(&b, LINE_COLORS[c].name);
                buf_putc(&b, '_');
                buf_puts(&b, pat);
                buf_putc(&b, '_');
                buf_u64(&b, t->pen);
                int rc = b.len < sizeof name ? str_of(a, b.p, b.len, &t->name) : -1;
                buf_free(&b);
                if (rc) return -1;
            }
        }
    }
    return 0;
}

static const struct {
    const char *name;
    uint8_t c[3];
} ANNOTATION_COLORS[5] = {{"black", {0, 0, 0}},    {"red", {230, 40, 30}},   {"green", {40, 160, 40}},
                          {"brown", {190, 110, 40}}, {"blue", {30, 110, 240}}};

/* f32 Display ({}): its shortest digits. */
static int f32_display(Buf *b, float x) {
    double d;
    return cc_f32_shortest(x, &d) || fmt_f64_fixed(b, d, -1) ? -1 : 0;
}

static int text_types_default(cc_paper_style *p, Arena *a) {
    static const float SIZES[5] = {5.0f, 4.0f, 3.5f, 2.5f, 2.0f};
    p->ntext_types = 25;
    p->text_types = arena_alloc(a, 25 * sizeof *p->text_types, _Alignof(cc_text_type));
    if (!p->text_types) return -1;
    size_t n = 0;
    for (int c = 0; c < 5; c++)
        for (int s = 0; s < 5; s++) {
            cc_text_type *t = &p->text_types[n++];
            Buf b;
            buf_init(&b);
            buf_puts(&b, "Arial ");
            int rc = f32_display(&b, SIZES[s]);
            buf_puts(&b, " mm ");
            buf_puts(&b, ANNOTATION_COLORS[c].name);
            rc = rc || str_of(a, b.p, b.len, &t->name) || strz(a, "Arial", &t->font);
            buf_free(&b);
            if (rc) return -1;
            t->size_mm = SIZES[s];
            t->color = rgb(ANNOTATION_COLORS[c].c[0], ANNOTATION_COLORS[c].c[1], ANNOTATION_COLORS[c].c[2]);
        }
    return 0;
}

static int dim_types_default(cc_paper_style *p, Arena *a) {
    static const int COLS[4] = {0, 1, 4, 2};
    static const struct {
        cc_dim_unit unit;
        uint8_t decimals;
        int show;
        const char *label;
    } KINDS[3] = {{CC_DIM_CM, 0, 0, "cm"}, {CC_DIM_M, 2, 1, "m"}, {CC_DIM_MM, 0, 1, "mm"}};
    p->ndim_types = 12;
    p->dim_types = arena_alloc(a, 12 * sizeof *p->dim_types, _Alignof(cc_dim_type));
    if (!p->dim_types) return -1;
    size_t n = 0;
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 3; k++) {
            cc_dim_type *t = &p->dim_types[n++];
            char name[32];
            size_t cl = strlen(ANNOTATION_COLORS[COLS[c]].name);
            memcpy(name, ANNOTATION_COLORS[COLS[c]].name, cl);
            name[cl] = ' ';
            strcpy(name + cl + 1, KINDS[k].label);
            if (strz(a, name, &t->name) || strz(a, "black_continu_1", &t->line_type)) return -1;
            t->unit = KINDS[k].unit;
            t->decimals = KINDS[k].decimals;
            t->show_unit = KINDS[k].show;
            const uint8_t *col = ANNOTATION_COLORS[COLS[c]].c;
            t->color = rgb(col[0], col[1], col[2]);
            t->text_mm = 2.5f;
        }
    return 0;
}

/* hatches::default_hatch_types. */
static int hatch_types_default(cc_paper_style *p, Arena *a) {
    typedef struct {
        float angle, spacing, offset;
        int ndash;
        float dash[2];
    } L;
    static const struct {
        const char *name;
        int has_fill;
        uint8_t fill[3];
        int nlines;
        L lines[3];
    } BASE[10] = {
        {"concrete cut", 0, {0, 0, 0}, 1, {{45.0f, 1.5f, 0.0f, 0, {0, 0}}}},
        {"concrete seen", 1, {235, 235, 235}, 0, {{0}}},
        {"masonry cut", 0, {0, 0, 0}, 2, {{45.0f, 1.0f, 0.0f, 0, {0, 0}}, {135.0f, 3.0f, 0.0f, 0, {0, 0}}}},
        {"masonry seen", 0, {0, 0, 0}, 1, {{45.0f, 3.0f, 0.0f, 0, {0, 0}}}},
        {"steel cut", 1, {0, 0, 0}, 0, {{0}}},
        {"timber cut", 0, {0, 0, 0}, 2, {{45.0f, 2.0f, 0.0f, 0, {0, 0}}, {135.0f, 2.0f, 0.0f, 0, {0, 0}}}},
        {"timber seen", 0, {0, 0, 0}, 1, {{0.0f, 2.0f, 0.0f, 0, {0, 0}}}},
        {"earth", 0, {0, 0, 0}, 3, {{45.0f, 4.0f, 0.0f, 0, {0, 0}}, {45.0f, 4.0f, 0.5f, 0, {0, 0}}, {45.0f, 4.0f, 1.0f, 0, {0, 0}}}},
        {"drainage", 0, {0, 0, 0}, 2, {{0.0f, 1.2f, 0.0f, 2, {0.2f, 1.0f}}, {0.0f, 1.2f, 0.6f, 2, {0.2f, 1.0f}}}},
        {"insulation", 0, {0, 0, 0}, 2, {{45.0f, 3.0f, 0.0f, 2, {1.0f, 1.0f}}, {135.0f, 3.0f, 0.0f, 2, {1.0f, 1.0f}}}},
    };
    static const struct {
        const char *name;
        uint8_t c[3];
    } SOLID[3] = {{"black", {0, 0, 0}}, {"grey", {190, 190, 190}}, {"light grey", {225, 225, 225}}};
    p->nhatch_types = 10 + 5 + 3;
    p->hatch_types = arena_alloc(a, p->nhatch_types * sizeof *p->hatch_types, _Alignof(cc_hatch_type));
    if (!p->hatch_types) return -1;
    size_t n = 0;
    for (int i = 0; i < 10; i++) {
        cc_hatch_type *t = &p->hatch_types[n++];
        if (strz(a, BASE[i].name, &t->name)) return -1;
        t->has_fill = BASE[i].has_fill;
        t->fill = rgb(BASE[i].fill[0], BASE[i].fill[1], BASE[i].fill[2]);
        t->color = rgb(0, 0, 0);
        t->pen = 1;
        t->nlines = (size_t)BASE[i].nlines;
        if (t->nlines) {
            t->lines = arena_alloc(a, t->nlines * sizeof *t->lines, _Alignof(cc_hatch_line));
            if (!t->lines) return -1;
        }
        for (size_t j = 0; j < t->nlines; j++) {
            const L *l = &BASE[i].lines[j];
            t->lines[j].angle_deg = l->angle;
            t->lines[j].spacing_mm = l->spacing;
            t->lines[j].offset_mm = l->offset;
            t->lines[j].ndash = (size_t)l->ndash;
            for (int d = 0; d < l->ndash; d++) t->lines[j].dash_mm[d] = l->dash[d];
        }
    }
    for (int c = 0; c < 5; c++) {
        cc_hatch_type *t = &p->hatch_types[n++];
        char name[32] = "diagonals ";
        strcpy(name + 10, ANNOTATION_COLORS[c].name);
        if (strz(a, name, &t->name)) return -1;
        t->color = rgb(ANNOTATION_COLORS[c].c[0], ANNOTATION_COLORS[c].c[1], ANNOTATION_COLORS[c].c[2]);
        t->pen = 1;
        t->nlines = 1;
        t->lines = arena_alloc(a, sizeof *t->lines, _Alignof(cc_hatch_line));
        if (!t->lines) return -1;
        t->lines[0].angle_deg = 45.0f;
        t->lines[0].spacing_mm = 2.0f;
    }
    for (int c = 0; c < 3; c++) {
        cc_hatch_type *t = &p->hatch_types[n++];
        char name[32] = "solid ";
        strcpy(name + 6, SOLID[c].name);
        if (strz(a, name, &t->name)) return -1;
        t->has_fill = 1;
        t->fill = rgb(SOLID[c].c[0], SOLID[c].c[1], SOLID[c].c[2]);
        t->color = rgb(0, 0, 0);
        t->pen = 1;
    }
    return 0;
}

int cc_style_default(cc_style *s, Arena *a) {
    memset(s, 0, sizeof *s);
    /* Spacings are measured across the lines (pixels): a family at 45 degrees with spacing
     * 9/sqrt(2) repeats every 9 px along x. f32 division, as Rust. */
    const float s2 = 1.41421356237309504880f;
    s->background = rgb(247, 247, 245);
    s->edges = rgb(26, 26, 28);
    s->selection = rgb(0xD7, 0xAC, 0xB6);
    s->hover = rgb(30, 144, 255);
    s->dimensions = rgb(20, 110, 220);
    s->reference = rgb(190, 214, 238);
    s->reference_edges = rgb(52, 110, 180);
    s->only_here = rgb(224, 75, 58);
    s->only_in_reference = rgb(46, 160, 90);
    s->hatch_strength = 0.5f;
    s->hatch_darken = 0.45f;
    s->ambient = 0.62f;
    s->cut_shade = 0.72f;
    cc_fill *f = s->families;
    f[CC_CAT_CONCRETE] = plain(rgb(214, 214, 210));
    f[CC_CAT_CONCRETE].has_stipple = 1;
    f[CC_CAT_CONCRETE].stipple = (cc_stipple){0.625f, 0.07f, 0.8f};
    float q;
    f[CC_CAT_STEEL] = plain(rgb(150, 170, 196));
    q = 5.0f / s2;
    f[CC_CAT_STEEL].lines[0] = lines(135.0f, q, 0.12f);
    f[CC_CAT_STEEL].nlines = 1;
    f[CC_CAT_TIMBER] = plain(rgb(226, 196, 150));
    q = 9.0f / s2;
    f[CC_CAT_TIMBER].lines[0] = lines(45.0f, q, 0.1f);
    f[CC_CAT_TIMBER].nlines = 1;
    f[CC_CAT_MASONRY] = plain(rgb(206, 150, 128));
    q = 12.0f / s2;
    f[CC_CAT_MASONRY].lines[0] = lines(45.0f, q, 0.08f);
    q = 24.0f / s2;
    f[CC_CAT_MASONRY].lines[1] = lines(135.0f, q, 0.05f);
    f[CC_CAT_MASONRY].nlines = 2;
    f[CC_CAT_INSULATION] = plain(rgb(236, 220, 120));
    f[CC_CAT_INSULATION].lines[0] = (cc_style_lines){0.0f, 8.0f, 0.12f, 1, 10.0f, 0.6f};
    f[CC_CAT_INSULATION].nlines = 1;
    f[CC_CAT_EARTH] = plain(rgb(176, 160, 132));
    q = 10.0f / s2;
    f[CC_CAT_EARTH].lines[0] = lines(45.0f, q, 0.08f);
    f[CC_CAT_EARTH].lines[1] = lines(135.0f, q, 0.08f);
    f[CC_CAT_EARTH].nlines = 2;
    f[CC_CAT_GLASS] = plain(rgb(176, 214, 226));
    f[CC_CAT_OTHER] = plain(rgb(238, 238, 236));

    cc_paper_style *p = &s->paper;
    p->cut_pen_mm = 0.5f;
    p->projection_pen_mm = 0.18f;
    p->above_pen_mm = 0.13f;
    p->above_dash_mm[0] = 2.0f;
    p->above_dash_mm[1] = 1.0f;
    p->hatch_pen_mm = 0.13f;
    p->hatch_mm_per_px = 0.35f;
    p->frame_pen_mm = 0.5f;
    p->text_mm = 2.5f;
    p->annotation_pen_mm = 0.13f;
    p->axis_pen_mm = 0.13f;
    p->axis_dash_mm[0] = 12.0f;
    p->axis_dash_mm[1] = 1.5f;
    p->axis_dash_mm[2] = 0.5f;
    p->axis_dash_mm[3] = 1.5f;
    p->seen_lighten = 0.6f;
    pen_table_default(&p->pens);
    static const char *const ROLES[CC_ROLE_COUNT] = {"black_continu_5", "black_continu_2", "black_dash_1",
                                                     "black_continu_1", "black_axes_1",    "black_continu_1",
                                                     "black_continu_5"};
    for (int r = 0; r < CC_ROLE_COUNT; r++)
        if (strz(a, ROLES[r], &p->lines[r])) return -1;
    if (line_types_default(p, a) || text_types_default(p, a) || dim_types_default(p, a) || hatch_types_default(p, a))
        return -1;
    p->font = CC_FONT_ROBOTO;
    return 0;
}

/* ---- lookups and pens ---- */

static int str_eq(cc_str s, const char *p, size_t n) { return s.n == n && !memcmp(s.p, p, n); }

int cc_pen_model_mm(const cc_pen_table *t, int pen, uint32_t scale, float *mm) {
    if (pen < 1 || pen > CC_PENS || !t->nscales) return 0;
    /* The nearest listed scale in ratio: |ln s - ln x| compared exactly as max/min ratios
     * (cross-multiplied in u64); the first one wins a tie, as min_by. */
    uint64_t s = scale ? scale : 1;
    size_t col = 0;
    uint64_t bhi = 0, blo = 1;
    for (size_t i = 0; i < t->nscales; i++) {
        uint64_t x = t->scales[i] ? t->scales[i] : 1;
        uint64_t hi = s > x ? s : x, lo = s > x ? x : s;
        if (i == 0 || hi * blo < bhi * lo) {
            col = i;
            bhi = hi;
            blo = lo;
        }
    }
    *mm = t->model[pen - 1][col];
    return 1;
}

int cc_pen_annotation_mm(const cc_pen_table *t, int pen, float *mm) {
    if (pen < 1 || pen > CC_PENS) return 0;
    *mm = t->annotation[pen - 1];
    return 1;
}

const cc_line_type *cc_style_line_type(const cc_paper_style *p, const char *name, size_t n) {
    for (size_t i = 0; i < p->nline_types; i++)
        if (str_eq(p->line_types[i].name, name, n)) return &p->line_types[i];
    return NULL;
}

const cc_hatch_type *cc_style_hatch_type(const cc_paper_style *p, const char *name, size_t n) {
    for (size_t i = 0; i < p->nhatch_types; i++)
        if (str_eq(p->hatch_types[i].name, name, n)) return &p->hatch_types[i];
    return NULL;
}

const cc_hatch_type *cc_style_material_hatch(const cc_paper_style *p, cc_category c, int cut) {
    for (size_t i = 0; i < p->nmaterial_hatches; i++) {
        const cc_material_hatch *u = &p->material_hatches[i];
        if (u->category != c) continue;
        cc_str n = cut ? u->cut : u->seen;
        return n.n ? cc_style_hatch_type(p, n.p, n.n) : NULL;
    }
    return NULL;
}

static double *dashes(Arena *a, const float *d, size_t n) {
    double *o = arena_alloc(a, (n ? n : 1) * sizeof *o, _Alignof(double));
    if (o)
        for (size_t i = 0; i < n; i++) o[i] = (double)d[i];
    return o;
}

int cc_role_pen(const cc_style *s, cc_role r, uint32_t scale, Arena *a, cc_pen *out) {
    const cc_paper_style *p = &s->paper;
    int annotation = r == CC_ROLE_AXIS || r == CC_ROLE_DIMENSION || r == CC_ROLE_FRAME;
    const cc_line_type *lt = cc_style_line_type(p, p->lines[r].p, p->lines[r].n);
    if (lt) {
        float w;
        if (annotation ? cc_pen_annotation_mm(&p->pens, lt->pen, &w) : cc_pen_model_mm(&p->pens, lt->pen, scale, &w)) {
            out->width_mm = (double)w;
            out->color = lt->color;
            out->ndash = lt->ndash;
            out->dash = lt->ndash ? dashes(a, lt->dash_mm, lt->ndash) : NULL;
            return lt->ndash && !out->dash ? -1 : 0;
        }
    }
    float w;
    const float *d = NULL;
    size_t nd = 0;
    switch (r) {
    case CC_ROLE_CUT: w = p->cut_pen_mm; break;
    case CC_ROLE_SEEN: w = p->projection_pen_mm; break;
    case CC_ROLE_ABOVE: w = p->above_pen_mm, d = p->above_dash_mm, nd = 2; break;
    case CC_ROLE_HATCH: w = p->hatch_pen_mm; break;
    case CC_ROLE_AXIS: w = p->axis_pen_mm, d = p->axis_dash_mm, nd = 4; break;
    case CC_ROLE_DIMENSION: w = p->annotation_pen_mm; break;
    default: w = p->frame_pen_mm; break;
    }
    out->width_mm = (double)w;
    out->color = s->edges;
    out->ndash = nd;
    out->dash = nd ? dashes(a, d, nd) : NULL;
    return nd && !out->dash ? -1 : 0;
}

/* plan::hatches_for: a family's own pattern, scaled to paper. */
static int hatches_for(const cc_style *s, cc_category c, uint32_t scale, Arena *a, cc_area_look *out) {
    double mm_per_px = (double)s->paper.hatch_mm_per_px;
    const cc_fill *fam = &s->families[c];
    cc_rgb ink = fam->has_ink ? fam->ink : cc_rgb_darken(fam->color, s->hatch_darken);
    out->nhatches = (size_t)fam->nlines;
    out->hatches = NULL;
    if (!fam->nlines) return 0;
    cc_pen hp;
    if (cc_role_pen(s, CC_ROLE_HATCH, scale, a, &hp)) return -1;
    out->hatches = arena_alloc(a, out->nhatches * sizeof *out->hatches, _Alignof(cc_hatch));
    if (!out->hatches) return -1;
    for (int i = 0; i < fam->nlines; i++) {
        const cc_style_lines *l = &fam->lines[i];
        cc_hatch *h = &out->hatches[i];
        h->angle_deg = (double)l->angle_deg;
        h->spacing_mm = (double)l->spacing_px * mm_per_px;
        h->offset_mm = 0.0;
        h->pen.width_mm = hp.width_mm;
        h->pen.color = ink;
        h->pen.dash = NULL;
        h->pen.ndash = 0;
        if (l->has_dash) {
            double d = (double)l->dash_px * mm_per_px, *v = arena_alloc(a, 2 * sizeof *v, _Alignof(double));
            if (!v) return -1;
            v[0] = d * (double)l->dash_on;
            v[1] = d * (1.0 - (double)l->dash_on);
            h->pen.dash = v;
            h->pen.ndash = 2;
        }
    }
    return 0;
}

/* plan::hatch_type_lines: a hatch type's lines, their width from the model pens at the scale. */
static int hatch_type_lines(const cc_style *s, const cc_hatch_type *t, uint32_t scale, Arena *a, cc_area_look *out) {
    float w32;
    double width;
    if (cc_pen_model_mm(&s->paper.pens, t->pen, scale, &w32)) {
        width = (double)w32;
    } else {
        cc_pen hp;
        if (cc_role_pen(s, CC_ROLE_HATCH, scale, a, &hp)) return -1;
        width = hp.width_mm;
    }
    out->nhatches = t->nlines;
    out->hatches = NULL;
    if (!t->nlines) return 0;
    out->hatches = arena_alloc(a, t->nlines * sizeof *out->hatches, _Alignof(cc_hatch));
    if (!out->hatches) return -1;
    for (size_t i = 0; i < t->nlines; i++) {
        const cc_hatch_line *l = &t->lines[i];
        cc_hatch *h = &out->hatches[i];
        h->angle_deg = (double)l->angle_deg;
        h->spacing_mm = (double)l->spacing_mm;
        h->offset_mm = (double)l->offset_mm;
        h->pen.width_mm = width;
        h->pen.color = t->color;
        h->pen.ndash = l->ndash;
        h->pen.dash = l->ndash ? dashes(a, l->dash_mm, l->ndash) : NULL;
        if (l->ndash && !h->pen.dash) return -1;
    }
    return 0;
}

static int type_look(const cc_style *s, const cc_hatch_type *h, uint32_t scale, Arena *a, cc_area_look *out) {
    /* No fill: white paper, hiding what lies under it. */
    out->has_fill = 1;
    out->fill = h->has_fill ? h->fill : rgb(255, 255, 255);
    return hatch_type_lines(s, h, scale, a, out);
}

int cc_cut_look(const cc_style *s, cc_category c, uint32_t scale, Arena *a, cc_area_look *out) {
    const cc_hatch_type *h = cc_style_material_hatch(&s->paper, c, 1);
    if (h) return type_look(s, h, scale, a, out);
    out->has_fill = 1;
    out->fill = s->families[c].color;
    return hatches_for(s, c, scale, a, out);
}

int cc_seen_look(const cc_style *s, cc_category c, uint32_t scale, Arena *a, cc_area_look *out) {
    const cc_hatch_type *h = cc_style_material_hatch(&s->paper, c, 0);
    if (h) return type_look(s, h, scale, a, out);
    /* plan::seen_fill_for: the family colour, lighter. */
    out->has_fill = 1;
    out->fill = cc_rgb_lighten(s->families[c].color, s->paper.seen_lighten);
    out->hatches = NULL;
    out->nhatches = 0;
    return 0;
}

/* ---- JSON (serde's field order) ---- */

static void jf32(JsonW *w, float x, int *err) {
    double d;
    if (cc_f32_shortest(x, &d)) {
        *err = 1;
        jw_null(w);
        return;
    }
    jw_f64(w, d);
}

static void jrgb(JsonW *w, cc_rgb c) {
    char t[8];
    static const char H[] = "0123456789abcdef";
    t[0] = '#';
    for (int i = 0; i < 3; i++) {
        t[1 + 2 * i] = H[c.c[i] >> 4];
        t[2 + 2 * i] = H[c.c[i] & 15];
    }
    jw_str_n(w, t, 7);
}

static void jstr(JsonW *w, cc_str s) { jw_str_n(w, s.p ? s.p : "", s.n); }

static void jf32s(JsonW *w, const float *v, size_t n, int *err) {
    jw_arr_begin(w);
    for (size_t i = 0; i < n; i++) jf32(w, v[i], err);
    jw_arr_end(w);
}

static void json_fill(JsonW *w, const cc_fill *f, int *err) {
    jw_obj_begin(w);
    jw_key(w, "color");
    jrgb(w, f->color);
    jw_key(w, "lines");
    jw_arr_begin(w);
    for (int i = 0; i < f->nlines; i++) {
        const cc_style_lines *l = &f->lines[i];
        jw_obj_begin(w);
        jw_key(w, "angle_deg");
        jf32(w, l->angle_deg, err);
        jw_key(w, "spacing_px");
        jf32(w, l->spacing_px, err);
        jw_key(w, "width");
        jf32(w, l->width, err);
        if (l->has_dash) {
            jw_key(w, "dash_px");
            jf32(w, l->dash_px, err);
        }
        if (l->dash_on != 1.0f) {
            jw_key(w, "dash_on");
            jf32(w, l->dash_on, err);
        }
        jw_obj_end(w);
    }
    jw_arr_end(w);
    if (f->has_stipple) {
        jw_key(w, "stipple");
        jw_obj_begin(w);
        jw_key(w, "cell_px");
        jf32(w, f->stipple.cell_px, err);
        jw_key(w, "density");
        jf32(w, f->stipple.density, err);
        jw_key(w, "strength");
        jf32(w, f->stipple.strength, err);
        jw_obj_end(w);
    }
    if (f->has_ink) {
        jw_key(w, "ink");
        jrgb(w, f->ink);
    }
    jw_obj_end(w);
}

static const char *const CAT_KEY[CC_CAT_COUNT] = {"concrete", "steel", "timber", "masonry",
                                                  "insulation", "earth", "glass", "other"};

static void json_paper(JsonW *w, const cc_paper_style *p, int *err) {
    jw_obj_begin(w);
    const struct {
        const char *k;
        float v;
    } pens1[] = {{"cut_pen_mm", p->cut_pen_mm}, {"projection_pen_mm", p->projection_pen_mm}, {"above_pen_mm", p->above_pen_mm}};
    for (int i = 0; i < 3; i++) {
        jw_key(w, pens1[i].k);
        jf32(w, pens1[i].v, err);
    }
    jw_key(w, "above_dash_mm");
    jf32s(w, p->above_dash_mm, 2, err);
    const struct {
        const char *k;
        float v;
    } pens2[] = {{"hatch_pen_mm", p->hatch_pen_mm}, {"hatch_mm_per_px", p->hatch_mm_per_px}, {"frame_pen_mm", p->frame_pen_mm},
                 {"text_mm", p->text_mm},           {"annotation_pen_mm", p->annotation_pen_mm}, {"axis_pen_mm", p->axis_pen_mm}};
    for (int i = 0; i < 6; i++) {
        jw_key(w, pens2[i].k);
        jf32(w, pens2[i].v, err);
    }
    jw_key(w, "axis_dash_mm");
    jf32s(w, p->axis_dash_mm, 4, err);
    jw_key(w, "seen_lighten");
    jf32(w, p->seen_lighten, err);
    jw_key(w, "pens");
    jw_obj_begin(w);
    jw_key(w, "scales");
    jw_arr_begin(w);
    for (size_t i = 0; i < p->pens.nscales; i++) jw_u64(w, p->pens.scales[i]);
    jw_arr_end(w);
    jw_key(w, "model");
    jw_arr_begin(w);
    for (int r = 0; r < CC_PENS; r++) jf32s(w, p->pens.model[r], p->pens.nscales, err);
    jw_arr_end(w);
    jw_key(w, "perspective");
    jf32s(w, p->pens.perspective, CC_PENS, err);
    jw_key(w, "annotation");
    jf32s(w, p->pens.annotation, CC_PENS, err);
    jw_obj_end(w);
    jw_key(w, "line_types");
    jw_arr_begin(w);
    for (size_t i = 0; i < p->nline_types; i++) {
        const cc_line_type *t = &p->line_types[i];
        jw_obj_begin(w);
        jw_key(w, "name");
        jstr(w, t->name);
        jw_key(w, "color");
        jrgb(w, t->color);
        if (t->ndash) {
            jw_key(w, "dash_mm");
            jf32s(w, t->dash_mm, t->ndash, err);
        }
        jw_key(w, "pen");
        jw_u64(w, t->pen);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_key(w, "lines");
    jw_obj_begin(w);
    for (int r = 0; r < CC_ROLE_COUNT; r++) {
        jw_key(w, ROLE_KEY[r]);
        jstr(w, p->lines[r]);
    }
    jw_obj_end(w);
    jw_key(w, "text_types");
    jw_arr_begin(w);
    for (size_t i = 0; i < p->ntext_types; i++) {
        const cc_text_type *t = &p->text_types[i];
        jw_obj_begin(w);
        jw_key(w, "name");
        jstr(w, t->name);
        jw_key(w, "font");
        jstr(w, t->font);
        jw_key(w, "size_mm");
        jf32(w, t->size_mm, err);
        jw_key(w, "color");
        jrgb(w, t->color);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_key(w, "dimension_types");
    jw_arr_begin(w);
    static const char *const UNIT[3] = {"m", "cm", "mm"};
    for (size_t i = 0; i < p->ndim_types; i++) {
        const cc_dim_type *t = &p->dim_types[i];
        jw_obj_begin(w);
        jw_key(w, "name");
        jstr(w, t->name);
        jw_key(w, "unit");
        jw_str(w, UNIT[t->unit]);
        jw_key(w, "decimals");
        jw_u64(w, t->decimals);
        jw_key(w, "show_unit");
        jw_bool(w, t->show_unit);
        jw_key(w, "color");
        jrgb(w, t->color);
        jw_key(w, "text_mm");
        jf32(w, t->text_mm, err);
        jw_key(w, "line_type");
        jstr(w, t->line_type);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_key(w, "hatch_types");
    jw_arr_begin(w);
    for (size_t i = 0; i < p->nhatch_types; i++) {
        const cc_hatch_type *t = &p->hatch_types[i];
        jw_obj_begin(w);
        jw_key(w, "name");
        jstr(w, t->name);
        if (t->has_fill) {
            jw_key(w, "fill");
            jrgb(w, t->fill);
        }
        if (t->nlines) {
            jw_key(w, "lines");
            jw_arr_begin(w);
            for (size_t j = 0; j < t->nlines; j++) {
                const cc_hatch_line *l = &t->lines[j];
                jw_obj_begin(w);
                jw_key(w, "angle_deg");
                jf32(w, l->angle_deg, err);
                jw_key(w, "spacing_mm");
                jf32(w, l->spacing_mm, err);
                if (l->offset_mm != 0.0f) {
                    jw_key(w, "offset_mm");
                    jf32(w, l->offset_mm, err);
                }
                if (l->ndash) {
                    jw_key(w, "dash_mm");
                    jf32s(w, l->dash_mm, l->ndash, err);
                }
                jw_obj_end(w);
            }
            jw_arr_end(w);
        }
        jw_key(w, "color");
        jrgb(w, t->color);
        jw_key(w, "pen");
        jw_u64(w, t->pen);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    if (p->nmaterial_hatches) {
        jw_key(w, "material_hatches");
        jw_arr_begin(w);
        for (size_t i = 0; i < p->nmaterial_hatches; i++) {
            const cc_material_hatch *u = &p->material_hatches[i];
            jw_obj_begin(w);
            jw_key(w, "category");
            jw_str(w, cc_category_name(u->category));
            if (u->cut.n) {
                jw_key(w, "cut");
                jstr(w, u->cut);
            }
            if (u->seen.n) {
                jw_key(w, "seen");
                jstr(w, u->seen);
            }
            jw_obj_end(w);
        }
        jw_arr_end(w);
    }
    jw_key(w, "font");
    jw_str(w, p->font == CC_FONT_HELVETICA ? "helvetica" : "roboto");
    jw_obj_end(w);
}

int cc_style_json(const cc_style *s, Buf *b) {
    JsonW w;
    int err = 0;
    jw_init(&w, b, 1);
    jw_obj_begin(&w);
    const struct {
        const char *k;
        cc_rgb c;
    } cols[] = {{"background", s->background},
                {"edges", s->edges},
                {"selection", s->selection},
                {"hover", s->hover},
                {"dimensions", s->dimensions},
                {"reference", s->reference},
                {"reference_edges", s->reference_edges},
                {"only_here", s->only_here},
                {"only_in_reference", s->only_in_reference}};
    for (size_t i = 0; i < sizeof cols / sizeof cols[0]; i++) {
        jw_key(&w, cols[i].k);
        jrgb(&w, cols[i].c);
    }
    const struct {
        const char *k;
        float v;
    } units[] = {{"hatch_strength", s->hatch_strength}, {"hatch_darken", s->hatch_darken}, {"ambient", s->ambient}, {"cut_shade", s->cut_shade}};
    for (int i = 0; i < 4; i++) {
        jw_key(&w, units[i].k);
        jf32(&w, units[i].v, &err);
    }
    jw_key(&w, "families");
    jw_obj_begin(&w);
    for (int c = 0; c < CC_CAT_COUNT; c++) {
        jw_key(&w, CAT_KEY[c]);
        json_fill(&w, &s->families[c], &err);
    }
    jw_obj_end(&w);
    if (s->nmaterial_families) {
        jw_key(&w, "material_families");
        jw_obj_begin(&w);
        for (size_t i = 0; i < s->nmaterial_families; i++) {
            jw_key_n(&w, s->material_family_names[i].p, s->material_family_names[i].n);
            jw_str(&w, cc_category_name(s->material_family_cats[i]));
        }
        jw_obj_end(&w);
    }
    jw_key(&w, "paper");
    json_paper(&w, &s->paper, &err);
    jw_obj_end(&w);
    if (jw_finish(&w) != JW_OK || err) return -1;
    buf_putc(b, '\n');
    return 0;
}
