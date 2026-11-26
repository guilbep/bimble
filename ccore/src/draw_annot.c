/* plan.rs's annotations (draw_annot.h). Expressions keep Rust's order of operations: each
 * product and sum rounds as it does there. */
#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */
#include "draw_annot.h"
#include <stdlib.h>
#include <string.h>
#include "annot.h"
#include "draw_cut.h"
#include "fmt.h"
#include "georef.h"
#include "grid.h"
#include "ifc_math.h"
#include "math.h"

/* plan.rs's constants (draw_plan.c has the same). */
#define MARGIN 10.0
#define PAD 5.0
#define BUBBLE 8.0
#define DIM_STEP 7.0
#define LEGEND_ROWS 4
#define LEGEND_ROW 6.0
#define LEGEND_COL 62.0

static const double PI = 3.14159265358979323846;
static const double TAU = 6.28318530717958647692;
/* f64::to_degrees and to_radians multiply by these (folded at compile time in Rust). */
static const double TO_DEG = 57.2957795130823208768;
static const double TO_RAD = 0.0174532925199432957692;

static double fmax_rs(double a, double b) { return a != a ? b : b != b ? a : a > b ? a : b; }
static double fmin_rs(double a, double b) { return a != a ? b : b != b ? a : a < b ? a : b; }
static double fabs_(double x) { return x < 0 ? -x : x == 0 ? 0.0 : x; }

/* f64::round: half away from zero (the sign of a zero result kept). */
static double round_(double x) {
    if (!(fabs_(x) < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x;
    if (x - t >= 0.5) t += 1.0;
    else if (t - x >= 0.5) t -= 1.0;
    if (t == 0.0 && x < 0) t = -0.0;
    return t;
}

static int str_eq(cc_str a, cc_str b) { return a.n == b.n && (!a.n || !memcmp(a.p, b.p, a.n)); }

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static int eq_ignore_ascii_case(cc_str a, const char *b) {
    size_t nb = strlen(b);
    if (a.n != nb) return 0;
    for (size_t i = 0; i < nb; i++)
        if (lower(a.p[i]) != lower(b[i])) return 0;
    return 1;
}

/* One UTF-8 character at s[*i] (advanced); U+FFFD for a bad byte. */
static uint32_t utf8_next(const char *s, size_t n, size_t *i) {
    const unsigned char *u = (const unsigned char *)s;
    unsigned c = u[*i];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!len || *i + (size_t)len > n) {
        (*i)++;
        return 0xfffd;
    }
    uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
    for (int k = 1; k < len; k++) cp = (cp << 6) | (u[*i + (size_t)k] & 0x3f);
    *i += (size_t)len;
    return cp;
}

static size_t chars(cc_str s) {
    size_t c = 0;
    for (size_t i = 0; i < s.n; c++) utf8_next(s.p, s.n, &i);
    return c;
}

/* char::is_whitespace (Unicode White_Space). */
static int is_ws(uint32_t c) {
    return (c >= 9 && c <= 13) || c == 32 || c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
           c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

/* str::trim. */
static cc_str trim(cc_str s) {
    size_t i = 0, start = s.n, end = 0;
    while (i < s.n) {
        size_t at = i;
        if (!is_ws(utf8_next(s.p, s.n, &i))) {
            if (start == s.n) start = at;
            end = i;
        }
    }
    cc_str r = {s.p + (start < end ? start : 0), start < end ? end - start : 0};
    return r;
}

/* ---- the context ---- */

cc_item *cc_item_list_push(cc_item_list *v) {
    if (v->n == v->cap) {
        size_t c = v->cap ? 2 * v->cap : 64;
        cc_item *q = realloc(v->p, c * sizeof *q);
        if (!q) return NULL;
        v->p = q;
        v->cap = c;
    }
    cc_item *it = &v->p[v->n++];
    memset(it, 0, sizeof *it);
    return it;
}

void cc_annot_to_paper(const cc_annot_ctx *c, const double p[2], double out[2]) {
    out[0] = c->ox + (p[0] - c->lo[0]) * c->k;
    out[1] = c->oy + (c->hi[1] - p[1]) * c->k;
}

static double text_mm(const cc_annot_ctx *c) { return (double)c->style->paper.text_mm; }

static int pen_of(cc_annot_ctx *c, cc_role r, uint32_t scale, cc_pen *out) {
    return cc_role_pen(c->style, r, scale, c->a, out);
}

/* `thicker`: the role's pen k times as wide, continuous. */
static int thicker(cc_annot_ctx *c, cc_role r, double k, cc_pen *out) {
    if (pen_of(c, r, c->scale, out)) return -1;
    out->width_mm *= k;
    out->dash = NULL;
    out->ndash = 0;
    return 0;
}

static int text_full(cc_annot_ctx *c, const double pos[2], double size, const char *s, size_t n, cc_rgb color,
                     cc_text_anchor anchor, double angle, int bold) {
    char *t = arena_alloc(c->a, n + 1, 1);
    cc_item *it = t ? cc_item_list_push(c->items) : NULL;
    if (!it) return -1;
    if (n) memcpy(t, s, n);
    it->kind = CC_ITEM_TEXT;
    it->pos[0] = pos[0];
    it->pos[1] = pos[1];
    it->size_mm = size;
    it->text = t;
    it->text_len = n;
    it->color = color;
    it->anchor = anchor;
    it->angle_deg = angle;
    it->bold = bold;
    return 0;
}

static int text_at(cc_annot_ctx *c, const double pos[2], double size, const char *s, size_t n, cc_text_anchor anchor,
                   double angle, int bold) {
    return text_full(c, pos, size, s, n, c->style->edges, anchor, angle, bold);
}

/* Polylines: nrings rings of counts[i] points, back to back in pts, copied to the arena. */
static int lines_item(cc_annot_ctx *c, const double (*pts)[2], const size_t *counts, size_t nrings, const cc_pen *pen) {
    size_t total = 0;
    for (size_t i = 0; i < nrings; i++) total += counts[i];
    cc_ring *r = arena_alloc(c->a, (nrings ? nrings : 1) * sizeof *r, _Alignof(cc_ring));
    double(*p)[2] = arena_alloc(c->a, (total ? total : 1) * sizeof *p, _Alignof(double));
    cc_item *it = r && p ? cc_item_list_push(c->items) : NULL;
    if (!it) return -1;
    if (total) memcpy(p, pts, total * sizeof *p);
    for (size_t i = 0, at = 0; i < nrings; at += counts[i], i++) {
        r[i].p = p + at;
        r[i].n = counts[i];
    }
    it->kind = CC_ITEM_LINES;
    it->rings = r;
    it->nrings = nrings;
    it->has_pen = 1;
    it->pen = *pen;
    return 0;
}

/* Segments (2 points each) as one Lines item. */
static int segs_item(cc_annot_ctx *c, const double (*pts)[2], size_t nsegs, const cc_pen *pen) {
    size_t *counts = malloc((nsegs ? nsegs : 1) * sizeof *counts);
    if (!counts) return -1;
    for (size_t i = 0; i < nsegs; i++) counts[i] = 2;
    int rc = lines_item(c, pts, counts, nsegs, pen);
    free(counts);
    return rc;
}

static int one_line(cc_annot_ctx *c, const double (*pts)[2], size_t n, const cc_pen *pen) {
    return lines_item(c, pts, &n, 1, pen);
}

/* An Area of one loop. */
static int area1(cc_annot_ctx *c, const double (*pts)[2], size_t n, int has_fill, cc_rgb fill, const cc_hatch *h,
                 size_t nh, const cc_pen *outline) {
    cc_ring *r = arena_alloc(c->a, sizeof *r, _Alignof(cc_ring));
    double(*p)[2] = arena_alloc(c->a, (n ? n : 1) * sizeof *p, _Alignof(double));
    cc_item *it = r && p ? cc_item_list_push(c->items) : NULL;
    if (!it) return -1;
    memcpy(p, pts, n * sizeof *p);
    r->p = p;
    r->n = n;
    it->kind = CC_ITEM_AREA;
    it->rings = r;
    it->nrings = 1;
    it->has_fill = has_fill;
    it->fill = fill;
    it->hatches = h;
    it->nhatches = nh;
    if (outline) {
        it->has_pen = 1;
        it->pen = *outline;
    }
    return 0;
}

static const cc_rgb WHITE = {{255, 255, 255}};

/* ---- small texts ---- */

void cc_letter_tag(Buf *b, size_t i) {
    char s[24];
    size_t n = 0;
    for (;;) {
        s[n++] = (char)('A' + i % 26);
        if (i < 26) break;
        i = i / 26 - 1;
    }
    while (n) buf_putc(b, s[--n]);
}

void cc_cm(Buf *b, double mm) {
    double c = mm / 10.0;
    if (fabs_(c - round_(c)) < 0.05) {
        double r = round_(c); /* `as i64`: saturating, NaN 0 */
        int64_t v = r != r ? 0 : r >= 9223372036854775807.0 ? INT64_MAX : r <= -9223372036854775808.0 ? INT64_MIN : (int64_t)r;
        buf_i64(b, v);
    } else {
        fmt_f64_fixed(b, c, 1);
    }
}

void cc_section_text(Buf *b, cc_category cat, cc_str material, double w, double h) {
    cc_str m = trim(material);
    if (cat == CC_CAT_TIMBER && m.n && chars(m) <= 40) {
        buf_append(b, m.p, m.n);
        buf_putc(b, ' ');
        cc_cm(b, w);
        buf_putc(b, 'x');
        cc_cm(b, h);
    } else {
        cc_cm(b, w);
        buf_putc(b, '/');
        cc_cm(b, h);
    }
}

/* f64::total_cmp. */
static int total_cmp(double a, double b) {
    int64_t x, y;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    x ^= (int64_t)(((uint64_t)(x >> 63)) >> 1);
    y ^= (int64_t)(((uint64_t)(y >> 63)) >> 1);
    return x < y ? -1 : x > y;
}

/* sort_by(total_cmp): an insertion sort (stable; the lists are short). */
static void sort_total(double *v, size_t n) {
    for (size_t i = 1; i < n; i++) {
        double x = v[i];
        size_t j = i;
        while (j > 0 && total_cmp(v[j - 1], x) > 0) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

size_t cc_clusters(double *vals, size_t n, double tol, double *out) {
    sort_total(vals, n);
    size_t k = 0, i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && vals[j] - vals[j - 1] <= tol) j++;
        if (j - i >= 2) {
            double s = -0.0; /* Sum of f64 starts at -0.0 */
            for (size_t q = i; q < j; q++) s += vals[q];
            out[k++] = s / (double)(j - i);
        }
        i = j;
    }
    return k;
}

/* ---- grid axes ---- */

int cc_plan_axis_vertical(const cc_plan_axis *ax) { return fabs_(ax->b[1] - ax->a[1]) >= fabs_(ax->b[0] - ax->a[0]); }

static int dup_buf(Arena *a, const Buf *b, cc_str *out) {
    char *p = arena_alloc(a, b->len + 1, 1);
    if (!p) return -1;
    if (b->len) memcpy(p, b->p, b->len);
    out->p = p;
    out->n = b->len;
    return 0;
}

int cc_plan_axes(const cc_model *m, const double origin[3], cc_axes mode, const double (*centres)[2], size_t ncentres,
                 const double blo[2], const double bhi[2], Arena *a, cc_plan_axis **out, size_t *n, int *model_axes) {
    *out = NULL;
    *n = 0;
    *model_axes = 0;
    if (mode == CC_AXES_OFF) return 0;
    /* The model's grids, as bimble-ifc's read gives a scene its axes (read.rs grid_axes). */
    size_t total = 0;
    for (size_t g = 0; g < m->ngrids; g++) total += m->grids[g].naxes;
    size_t cap = total + 2 * ncentres + 1;
    cc_plan_axis *v = arena_alloc(a, cap * sizeof *v, _Alignof(cc_plan_axis));
    if (!v) return -1;
    size_t k = 0;
    for (size_t g = 0; g < m->ngrids && k < 10000; g++) {
        const cc_grid *gr = &m->grids[g];
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = 0; i < gr->naxes && k < 10000; i++) {
                if (cc_grid_axis_is_u(gr, i) != !pass) continue;
                const cc_grid_axis *ax = &gr->axes[i];
                int dup = 0;
                for (size_t j = 0; j < k && !dup; j++)
                    dup = str_eq(v[j].tag, ax->tag) && v[j].a[0] == ax->start[0] - origin[0] &&
                          v[j].a[1] == ax->start[1] - origin[1] && v[j].b[0] == ax->end[0] - origin[0] &&
                          v[j].b[1] == ax->end[1] - origin[1];
                if (dup || (ax->start[0] == ax->end[0] && ax->start[1] == ax->end[1])) continue;
                v[k].tag = ax->tag;
                v[k].a[0] = ax->start[0] - origin[0];
                v[k].a[1] = ax->start[1] - origin[1];
                v[k].b[0] = ax->end[0] - origin[0];
                v[k].b[1] = ax->end[1] - origin[1];
                k++;
            }
    }
    *model_axes = k > 0;
    if (!k && mode == CC_AXES_MODEL_OR_COLUMNS && ncentres) {
        double *xs = malloc(4 * ncentres * sizeof *xs);
        if (!xs) return -1;
        double *ys = xs + ncentres, *cx = xs + 2 * ncentres, *cy = xs + 3 * ncentres;
        for (size_t i = 0; i < ncentres; i++) xs[i] = centres[i][0], ys[i] = centres[i][1];
        size_t nx = cc_clusters(xs, ncentres, 50.0, cx), ny = cc_clusters(ys, ncentres, 50.0, cy);
        Buf t;
        buf_init(&t);
        int bad = 0;
        for (size_t i = 0; i < nx && !bad; i++, k++) {
            buf_clear(&t);
            cc_letter_tag(&t, i);
            bad = dup_buf(a, &t, &v[k].tag);
            v[k].a[0] = cx[i], v[k].a[1] = blo[1];
            v[k].b[0] = cx[i], v[k].b[1] = bhi[1];
        }
        for (size_t i = 0; i < ny && !bad; i++, k++) {
            buf_clear(&t);
            buf_u64(&t, (uint64_t)i + 1);
            bad = dup_buf(a, &t, &v[k].tag);
            v[k].a[0] = blo[0], v[k].a[1] = cy[i];
            v[k].b[0] = bhi[0], v[k].b[1] = cy[i];
        }
        buf_free(&t);
        free(xs);
        if (bad) return -1;
    }
    *out = v;
    *n = k;
    return 0;
}

static double chains_of(const cc_plan_template *t, int any) { return !t->dimensions ? 0.0 : any ? 2.0 : 1.0; }

int cc_annot_axis_lines(cc_annot_ctx *c, const cc_plan_axis *axes, size_t n, double chains_v, double chains_h,
                        cc_axes_drawn *out) {
    memset(out, 0, sizeof *out);
    double top_draw = c->oy, left_draw = c->ox;
    double bubble_v = top_draw - chains_v * DIM_STEP - 4.0 - BUBBLE / 2.0;
    double bubble_h = left_draw - chains_h * DIM_STEP - 4.0 - BUBBLE / 2.0;
    size_t cap = n ? n : 1;
    out->bubble = arena_alloc(c->a, cap * sizeof *out->bubble, _Alignof(double));
    out->tag = arena_alloc(c->a, cap * sizeof *out->tag, _Alignof(cc_str));
    out->at_top = arena_alloc(c->a, cap * sizeof *out->at_top, _Alignof(double));
    out->at_left = arena_alloc(c->a, cap * sizeof *out->at_left, _Alignof(double));
    double(*lines)[2] = malloc(2 * cap * sizeof *lines);
    if (!out->bubble || !out->tag || !out->at_top || !out->at_left || !lines) {
        free(lines);
        return -1;
    }
    size_t nl = 0;
    for (size_t i = 0; i < n; i++) {
        const cc_plan_axis *ax = &axes[i];
        double a[2], b[2];
        cc_annot_to_paper(c, ax->a, a);
        cc_annot_to_paper(c, ax->b, b);
        int v = cc_plan_axis_vertical(ax);
        /* Order so `a` is the end the bubble goes to (top or left). */
        if ((v && a[1] > b[1]) || (!v && a[0] > b[0])) {
            double t0 = a[0], t1 = a[1];
            a[0] = b[0], a[1] = b[1];
            b[0] = t0, b[1] = t1;
        }
        double d[2] = {b[0] - a[0], b[1] - a[1]};
        double len = cc_hypot(d[0], d[1]);
        if (len < 1e-9) continue;
        double u[2] = {d[0] / len, d[1] / len};
        /* Extend `a` along the axis to the bubble's edge. */
        double along = v ? (bubble_v + BUBBLE / 2.0 - a[1]) / u[1] : (bubble_h + BUBBLE / 2.0 - a[0]) / u[0];
        double a2[2] = {a[0] + u[0] * along, a[1] + u[1] * along};
        double b2[2] = {b[0] + u[0] * 3.0, b[1] + u[1] * 3.0};
        lines[2 * nl][0] = a2[0], lines[2 * nl][1] = a2[1];
        lines[2 * nl + 1][0] = b2[0], lines[2 * nl + 1][1] = b2[1];
        nl++;
        out->bubble[out->nbubbles][0] = a2[0] - u[0] * BUBBLE / 2.0;
        out->bubble[out->nbubbles][1] = a2[1] - u[1] * BUBBLE / 2.0;
        out->tag[out->nbubbles++] = ax->tag;
        /* Crossing with the drawing's top / left edge. */
        if (v && fabs_(u[1]) > 1e-9) {
            double s = (top_draw - a[1]) / u[1];
            out->at_top[out->ntop++] = a[0] + u[0] * s;
        } else if (!v && fabs_(u[0]) > 1e-9) {
            double s = (left_draw - a[0]) / u[0];
            out->at_left[out->nleft++] = a[1] + u[1] * s;
        }
    }
    int rc = 0;
    if (nl) {
        /* Dash-dot, the axis line of office drawings. */
        cc_pen pen;
        rc = pen_of(c, CC_ROLE_AXIS, c->scale, &pen) || segs_item(c, (const double(*)[2])lines, nl, &pen) ? -1 : 0;
    }
    free(lines);
    return rc;
}

/* plan::circle: a closed polyline of 37 points. */
static void circle(const double c[2], double r, double out[37][2]) {
    for (int i = 0; i <= 36; i++) {
        double a = (double)i / 36.0 * TAU;
        out[i][0] = c[0] + r * cc_cos(a);
        out[i][1] = c[1] + r * cc_sin(a);
    }
}

int cc_annot_bubbles(cc_annot_ctx *c, const cc_axes_drawn *d) {
    cc_pen seen;
    if (d->nbubbles && pen_of(c, CC_ROLE_SEEN, c->scale, &seen)) return -1;
    for (size_t i = 0; i < d->nbubbles; i++) {
        double pts[37][2];
        circle(d->bubble[i], BUBBLE / 2.0, pts);
        if (area1(c, (const double(*)[2])pts, 37, 1, WHITE, NULL, 0, &seen)) return -1;
        double size = chars(d->tag[i]) > 2 ? 3.0 : 4.0;
        double pos[2] = {d->bubble[i][0], d->bubble[i][1] + size * 0.36};
        if (text_at(c, pos, size, d->tag[i].p, d->tag[i].n, CC_ANCHOR_MIDDLE, 0.0, 0)) return -1;
    }
    return 0;
}

/* ---- dimensions ---- */

typedef struct {
    double (*lines)[2]; /* segments */
    size_t nlines, cap;
    double (*ticks)[2];
    size_t nticks, tcap;
    struct DimText {
        double pos[2], angle;
        cc_text_anchor anchor;
        char value[40];
        size_t n;
    } *texts;
    size_t ntexts, xcap;
} Chains;

static int grow(void **p, size_t *cap, size_t need, size_t sz) {
    if (need <= *cap) return 0;
    size_t c = *cap ? *cap : 16;
    while (c < need) c *= 2;
    void *q = realloc(*p, c * sz);
    if (!q) return -1;
    *p = q;
    *cap = c;
    return 0;
}

static void pt(int horizontal, double s, double q, double out[2]) {
    out[0] = horizontal ? s : q;
    out[1] = horizontal ? q : s;
}

static int chain(cc_annot_ctx *c, Chains *ch, double *stops, size_t n, double line_at, double from, int horizontal) {
    sort_total(stops, n);
    size_t m = 0;
    for (size_t i = 0; i < n; i++)
        if (!m || !(fabs_(stops[i] - stops[m - 1]) < 0.05)) stops[m++] = stops[i];
    if (m < 2) return 0;
    if (grow((void **)&ch->lines, &ch->cap, 2 * (ch->nlines + m + 1), sizeof *ch->lines) ||
        grow((void **)&ch->ticks, &ch->tcap, 2 * (ch->nticks + m), sizeof *ch->ticks) ||
        grow((void **)&ch->texts, &ch->xcap, ch->ntexts + m, sizeof *ch->texts))
        return -1;
    pt(horizontal, stops[0] - 1.5, line_at, ch->lines[2 * ch->nlines]);
    pt(horizontal, stops[m - 1] + 1.5, line_at, ch->lines[2 * ch->nlines + 1]);
    ch->nlines++;
    for (size_t i = 0; i < m; i++) {
        double s = stops[i];
        /* Extension line from near the drawing to just past the dimension line. */
        pt(horizontal, s, from - 1.5, ch->lines[2 * ch->nlines]);
        pt(horizontal, s, line_at - 1.5, ch->lines[2 * ch->nlines + 1]);
        ch->nlines++;
        /* Architect's tick: a short 45° stroke. */
        double p[2];
        pt(horizontal, s, line_at, p);
        double(*t)[2] = &ch->ticks[2 * ch->nticks++];
        t[0][0] = p[0] - 0.9, t[0][1] = p[1] + 0.9;
        t[1][0] = p[0] + 0.9, t[1][1] = p[1] - 0.9;
    }
    double angle = horizontal ? 0.0 : 90.0, dim_size = text_mm(c) * 0.9;
    for (size_t i = 0; i + 1 < m; i++) {
        double w0 = stops[i], w1 = stops[i + 1];
        struct DimText *dt = &ch->texts[ch->ntexts++];
        Buf v;
        buf_init(&v);
        cc_dim_format(&v, (w1 - w0) / c->k, (int)c->tpl->dimension_unit);
        dt->n = v.len < sizeof dt->value ? v.len : sizeof dt->value;
        memcpy(dt->value, v.p, dt->n);
        buf_free(&v);
        /* A text wider than its span goes outside it: before the first span, else after. */
        double s;
        if (cc_text_width_mm(dt->value, dt->n, dim_size, 0, c->style->paper.font) + 0.6 <= w1 - w0) {
            s = (w0 + w1) / 2.0;
            dt->anchor = CC_ANCHOR_MIDDLE;
        } else if (i == 0) {
            /* Vertical texts read upwards, towards smaller s. */
            s = w0 - 1.2;
            dt->anchor = horizontal ? CC_ANCHOR_END : CC_ANCHOR_START;
        } else {
            s = w1 + 1.2;
            dt->anchor = horizontal ? CC_ANCHOR_START : CC_ANCHOR_END;
        }
        pt(horizontal, s, line_at - 0.8, dt->pos);
        dt->angle = angle;
    }
    return 0;
}

int cc_annot_chains(cc_annot_ctx *c, const cc_axes_drawn *d, const double blo[2], const double bhi[2], int any_v,
                    int any_h) {
    if (!c->tpl->dimensions) return 0;
    double plo[2], phi[2];
    {
        double p[2] = {blo[0], bhi[1]}, q[2] = {bhi[0], blo[1]};
        cc_annot_to_paper(c, p, plo);
        cc_annot_to_paper(c, q, phi);
    }
    double top_draw = c->oy, left_draw = c->ox;
    Chains ch;
    memset(&ch, 0, sizeof ch);
    size_t cap = (d->ntop > d->nleft ? d->ntop : d->nleft) + 2;
    double *s = malloc(cap * sizeof *s);
    int rc = s ? 0 : -1;
    for (int side = 0; side < 2 && !rc; side++) {
        int horizontal = side == 0, any = horizontal ? any_v : any_h;
        double line = (horizontal ? top_draw : left_draw) - DIM_STEP, from = horizontal ? top_draw : left_draw;
        const double *at = horizontal ? d->at_top : d->at_left;
        size_t nat = horizontal ? d->ntop : d->nleft, k = horizontal ? 0 : 1;
        if (any) {
            memcpy(s, at, nat * sizeof *s);
            s[nat] = plo[k];
            s[nat + 1] = phi[k];
            rc = chain(c, &ch, s, nat + 2, line, from, horizontal);
            line -= DIM_STEP;
        }
        if (!rc) {
            s[0] = plo[k];
            s[1] = phi[k];
            rc = chain(c, &ch, s, 2, line, from, horizontal);
        }
    }
    free(s);
    if (!rc && ch.nlines) {
        cc_pen note, tick;
        rc = pen_of(c, CC_ROLE_DIMENSION, c->scale, &note) || thicker(c, CC_ROLE_DIMENSION, 2.0, &tick) ||
                     segs_item(c, (const double(*)[2])ch.lines, ch.nlines, &note) ||
                     segs_item(c, (const double(*)[2])ch.ticks, ch.nticks, &tick)
                 ? -1
                 : 0;
        for (size_t i = 0; i < ch.ntexts && !rc; i++)
            rc = text_at(c, ch.texts[i].pos, text_mm(c) * 0.9, ch.texts[i].value, ch.texts[i].n, ch.texts[i].anchor,
                         ch.texts[i].angle, 0);
    }
    free(ch.lines);
    free(ch.ticks);
    free(ch.texts);
    return rc;
}

/* -(atan2(u1, u0) in degrees), kept upright (within -90..=90). */
static double upright_angle(const double u[2]) {
    double angle = -(cc_atan2(u[1], u[0]) * TO_DEG);
    if (!(angle >= -90.0 && angle <= 90.0)) angle += angle > 0.0 ? -180.0 : 180.0;
    return angle;
}

int cc_dimension_on_paper(const double pa[2], const double pb[2], double off, cc_dim_axis axis, double u[2],
                          double qa[2], double qb[2], double *len) {
    double d0[2] = {pb[0] - pa[0], pb[1] - pa[1]};
    if (cc_hypot(d0[0], d0[1]) < 1e-6) return 1;
    double d[2];
    if (axis == CC_AXIS_HORIZONTAL) {
        d[0] = d0[0] < 0.0 ? -1.0 : 1.0;
        d[1] = 0.0;
    } else if (axis == CC_AXIS_VERTICAL) {
        d[0] = 0.0;
        d[1] = d0[1] < 0.0 ? -1.0 : 1.0;
    } else {
        d[0] = d0[0];
        d[1] = d0[1];
    }
    double dl = cc_hypot(d[0], d[1]);
    u[0] = d[0] / dl;
    u[1] = d[1] / dl;
    double n[2] = {u[1], -u[0]};
    qa[0] = pa[0] + n[0] * off;
    qa[1] = pa[1] + n[1] * off;
    double along = (pb[0] - qa[0]) * u[0] + (pb[1] - qa[1]) * u[1];
    qb[0] = qa[0] + u[0] * along;
    qb[1] = qa[1] + u[1] * along;
    *len = fabs_(along);
    return fabs_(along) > 1e-6 ? 0 : 1;
}

/* plan::dimension_items: its lines, ticks and value. */
static int dimension_items(cc_annot_ctx *c, const double pa[2], const double pb[2], const double u[2],
                           const double qa[2], const double qb[2], const cc_pen *pen, const char *value, size_t vn,
                           double size, cc_rgb color) {
    cc_pen tick = *pen;
    tick.width_mm *= 2.0;
    tick.dash = NULL;
    tick.ndash = 0;
    double n[2] = {u[1], -u[0]};
    double lines[6][2];
    const double *ends[2][2] = {{pa, qa}, {pb, qb}};
    for (int i = 0; i < 2; i++) {
        const double *p = ends[i][0], *q = ends[i][1];
        double e[2] = {q[0] - p[0], q[1] - p[1]};
        double el = cc_hypot(e[0], e[1]);
        if (el > 1e-9) {
            e[0] = e[0] / el;
            e[1] = e[1] / el;
        } else {
            e[0] = n[0];
            e[1] = n[1];
        }
        lines[2 * i][0] = p[0] + e[0], lines[2 * i][1] = p[1] + e[1];
        lines[2 * i + 1][0] = q[0] + e[0] * 1.5, lines[2 * i + 1][1] = q[1] + e[1] * 1.5;
    }
    lines[4][0] = qa[0], lines[4][1] = qa[1];
    lines[5][0] = qb[0], lines[5][1] = qb[1];
    double ticks[4][2];
    double s = 0.9, t[2] = {(u[0] - n[0]) * s, (u[1] - n[1]) * s};
    const double *qs[2] = {qa, qb};
    for (int i = 0; i < 2; i++) {
        ticks[2 * i][0] = qs[i][0] - t[0], ticks[2 * i][1] = qs[i][1] - t[1];
        ticks[2 * i + 1][0] = qs[i][0] + t[0], ticks[2 * i + 1][1] = qs[i][1] + t[1];
    }
    double angle = upright_angle(u);
    double mid[2] = {(qa[0] + qb[0]) / 2.0, (qa[1] + qb[1]) / 2.0};
    /* The value just above its line, upright. */
    double r = angle * TO_RAD;
    double pos[2] = {mid[0] - cc_sin(r) * 0.8, mid[1] - cc_cos(r) * 0.8};
    if (segs_item(c, (const double(*)[2])lines, 3, pen) || segs_item(c, (const double(*)[2])ticks, 2, &tick)) return -1;
    return text_full(c, pos, size, value, vn, color, CC_ANCHOR_MIDDLE, angle, 0);
}

/* The value of a length as the template's unit writes it, then dimension_items. */
static int unit_dimension(cc_annot_ctx *c, const double pa[2], const double pb[2], double off, cc_dim_axis axis,
                          const cc_pen *pen) {
    double u[2], qa[2], qb[2], len;
    if (cc_dimension_on_paper(pa, pb, off, axis, u, qa, qb, &len)) return 0;
    Buf v;
    buf_init(&v);
    cc_dim_format(&v, len / c->k, (int)c->tpl->dimension_unit);
    int rc = dimension_items(c, pa, pb, u, qa, qb, pen, v.p, v.len, text_mm(c) * 0.8, c->style->edges);
    buf_free(&v);
    return rc;
}

/* The nearest of an axis to one of the two sides lo, hi: (axis position, side); 0 none. */
static int nearest(const cc_plan_axis *axes, size_t naxes, int v, double lo, double hi, double *x, double *e) {
    int found = 0;
    double best = 0.0;
    for (size_t i = 0; i < naxes; i++) {
        if (cc_plan_axis_vertical(&axes[i]) != v) continue;
        double p = v ? axes[i].a[0] : axes[i].a[1];
        double sides[2] = {lo, hi};
        for (int j = 0; j < 2; j++) {
            if (!(fabs_(p - sides[j]) > 1.0)) continue;
            double d = fabs_(p - sides[j]);
            if (!found || total_cmp(d, best) < 0) {
                found = 1;
                best = d;
                *x = p;
                *e = sides[j];
            }
        }
    }
    return found;
}

int cc_annot_opening(cc_annot_ctx *c, const double a[2], const double b[2], const cc_plan_axis *axes, size_t naxes) {
    double p[2], q[2];
    cc_annot_to_paper(c, a, p);
    cc_annot_to_paper(c, b, q);
    double w = b[0] - a[0], h = b[1] - a[1];
    Buf l;
    buf_init(&l);
    cc_cm(&l, w);
    buf_putc(&l, 'x');
    cc_cm(&l, h);
    double size = text_mm(c) * 0.8;
    int fits = cc_text_width_mm(l.p, l.len, size, 0, c->style->paper.font) + 1.0 < q[0] - p[0] &&
               size + 1.0 < p[1] - q[1];
    double pos[2] = {(p[0] + q[0]) / 2.0, fits ? (p[1] + q[1]) / 2.0 + size * 0.35 : p[1] + size + 0.8};
    int rc = text_at(c, pos, size, l.p, l.len, CC_ANCHOR_MIDDLE, 0.0, 0);
    buf_free(&l);
    if (rc) return -1;
    cc_pen note;
    if (pen_of(c, CC_ROLE_DIMENSION, c->scale, &note)) return -1;
    double x, e;
    /* Along x, on the top edge: from the nearest vertical axis to the nearer side. */
    if (nearest(axes, naxes, 1, a[0], b[0], &x, &e)) {
        double pl[2] = {fmin_rs(x, e), b[1]}, pr[2] = {fmax_rs(x, e), b[1]}, pa[2], pb[2];
        cc_annot_to_paper(c, pl, pa);
        cc_annot_to_paper(c, pr, pb);
        if (unit_dimension(c, pa, pb, 2.5, CC_AXIS_HORIZONTAL, &note)) return -1;
    }
    /* Along y, on the left edge: from the nearest horizontal axis to the nearer side. */
    if (nearest(axes, naxes, 0, a[1], b[1], &x, &e)) {
        double pl[2] = {a[0], fmin_rs(x, e)}, ph[2] = {a[0], fmax_rs(x, e)}, pa[2], pb[2];
        cc_annot_to_paper(c, pl, pa);
        cc_annot_to_paper(c, ph, pb);
        if (unit_dimension(c, pa, pb, 2.5, CC_AXIS_VERTICAL, &note)) return -1;
    }
    return 0;
}

/* ---- section labels ---- */

static const char *const LABELLED[4] = {"IfcColumn", "IfcBeam", "IfcMember", "IfcPile"};

int cc_cut_label(cc_str cls, cc_category cat, cc_str material, const cc_ring *loops, size_t nloops, Buf *out) {
    int labelled = 0;
    for (int i = 0; i < 4; i++) labelled |= eq_ignore_ascii_case(cls, LABELLED[i]);
    if (!labelled || nloops != 1) return 0;
    const cc_ring *l = &loops[0];
    if (!l->n) return 0;
    double lo[2] = {1.0 / 0.0, 1.0 / 0.0}, hi[2] = {-1.0 / 0.0, -1.0 / 0.0};
    for (size_t i = 0; i < l->n; i++)
        for (int k = 0; k < 2; k++) {
            lo[k] = fmin_rs(lo[k], l->p[i][k]);
            hi[k] = fmax_rs(hi[k], l->p[i][k]);
        }
    if (!(lo[0] <= hi[0])) return 0;
    double w = hi[0] - lo[0], h = hi[1] - lo[1];
    if (w < 1.0 || h < 1.0) return 0;
    double area = fabs_(cc_cut_area(l));
    if (fabs_(w * h - area) <= 0.01 * w * h) {
        cc_section_text(out, cat, material, w, h);
        return 1;
    }
    double r = (w + h) / 4.0;
    if (l->n >= 12 && fabs_(w - h) <= 0.02 * w && fabs_(PI * r * r - area) <= 0.03 * area) {
        buf_puts(out, "\xc3\x98");
        cc_cm(out, 2.0 * r);
        return 1;
    }
    return 0;
}

int cc_annot_cut_label(cc_annot_ctx *c, cc_str cls, cc_category cat, cc_str material, const cc_ring *loops,
                       size_t nloops) {
    Buf l;
    buf_init(&l);
    if (!cc_cut_label(cls, cat, material, loops, nloops, &l)) {
        buf_free(&l);
        return 0;
    }
    double hi[2] = {-1.0 / 0.0, -1.0 / 0.0};
    int any = 0;
    for (size_t j = 0; j < nloops; j++)
        for (size_t i = 0; i < loops[j].n; i++, any = 1)
            for (int k = 0; k < 2; k++) hi[k] = fmax_rs(hi[k], loops[j].p[i][k]);
    int rc = 0;
    if (any) {
        double q[2];
        cc_annot_to_paper(c, hi, q);
        /* Above-right of it, off the axes that run through its centre. */
        double pos[2] = {q[0] + 0.6, q[1] - 0.6};
        rc = text_at(c, pos, text_mm(c) * 0.8, l.p, l.len, CC_ANCHOR_START, 0.0, 0);
    }
    buf_free(&l);
    return rc;
}

int cc_beam_label_of(const cc_mesh *m, cc_category cat, cc_str material, Arena *a, cc_beam_label *out) {
    int has = 0;
    double d[2] = {0, 0}, best = 0.0;
    for (uint32_t i = 0; i < m->ne; i++) {
        const float *e = m->edges[i];
        double dx = (double)e[3] - (double)e[0], dy = (double)e[4] - (double)e[1], dz = (double)e[5] - (double)e[2];
        double len = cc_hypot(dx, dy);
        if (len > best && fabs_(dz) < 1.0) {
            best = len;
            d[0] = dx / len;
            d[1] = dy / len;
            has = 1;
        }
    }
    if (!has) return 0;
    double n[2] = {-d[1], d[0]};
    double s0 = 1.0 / 0.0, s1 = -1.0 / 0.0, t0 = 1.0 / 0.0, t1 = -1.0 / 0.0, z0 = 1.0 / 0.0, z1 = -1.0 / 0.0;
    for (uint32_t i = 0; i < m->nv; i++) {
        double p[3] = {m->pos[i][0], m->pos[i][1], m->pos[i][2]};
        double s = p[0] * d[0] + p[1] * d[1], t = p[0] * n[0] + p[1] * n[1];
        s0 = fmin_rs(s0, s), s1 = fmax_rs(s1, s), t0 = fmin_rs(t0, t), t1 = fmax_rs(t1, t);
        z0 = fmin_rs(z0, p[2]), z1 = fmax_rs(z1, p[2]);
    }
    double w = t1 - t0, h = z1 - z0, len = s1 - s0;
    if (!(w >= 1.0 && h >= 1.0 && len > 2.0 * w)) return 0;
    double tm = (t0 + t1) / 2.0;
    Buf b;
    buf_init(&b);
    cc_section_text(&b, cat, material, w, h);
    int bad = dup_buf(a, &b, &out->text);
    buf_free(&b);
    if (bad) return -1;
    out->a[0] = d[0] * s0 + n[0] * tm;
    out->a[1] = d[1] * s0 + n[1] * tm;
    out->b[0] = d[0] * s1 + n[0] * tm;
    out->b[1] = d[1] * s1 + n[1] * tm;
    out->half = w / 2.0;
    return 1;
}

int cc_annot_beam_label(cc_annot_ctx *c, const cc_beam_label *l) {
    double pa[2], pb[2], t = text_mm(c);
    cc_annot_to_paper(c, l->a, pa);
    cc_annot_to_paper(c, l->b, pb);
    double d[2] = {pb[0] - pa[0], pb[1] - pa[1]};
    double len = cc_hypot(d[0], d[1]);
    if (len < t * 4.0) return 0;
    double u[2] = {d[0] / len, d[1] / len};
    double angle = upright_angle(u);
    /* Just beside the beam, on the side above its text. */
    double r = angle * TO_RAD;
    double off = l->half * c->k + 0.8;
    double mid[2] = {(pa[0] + pb[0]) / 2.0, (pa[1] + pb[1]) / 2.0};
    double pos[2] = {mid[0] - cc_sin(r) * off, mid[1] - cc_cos(r) * off};
    return text_at(c, pos, t * 0.8, l->text.p, l->text.n, CC_ANCHOR_MIDDLE, angle, 0);
}

/* ---- section marks ---- */

int cc_section_marks(const cc_view *views, size_t n, Arena *a, cc_section_mark **out, size_t *nout) {
    *out = arena_alloc(a, (n ? n : 1) * sizeof **out, _Alignof(cc_section_mark));
    *nout = 0;
    if (!*out) return -1;
    for (size_t k = 0; k < n; k++) {
        const cc_view *v = &views[k];
        if (v->kind != CC_VIEW_SECTION) continue;
        size_t i = *nout;
        /* The last word of the name (split_whitespace), up to its first '-'. */
        cc_str last = {v->name.p, 0};
        size_t at = 0;
        while (at < v->name.n) {
            size_t start = at;
            uint32_t ch = utf8_next(v->name.p, v->name.n, &at);
            if (is_ws(ch)) continue;
            size_t end = at;
            while (end < v->name.n) {
                size_t q = end;
                if (is_ws(utf8_next(v->name.p, v->name.n, &q))) break;
                end = q;
            }
            last.p = v->name.p + start;
            last.n = end - start;
            at = end;
        }
        for (size_t j = 0; j < last.n; j++)
            if (last.p[j] == '-') {
                last.n = j;
                break;
            }
        cc_section_mark *m = &(*out)[i];
        if (last.n && chars(last) <= 3 && !str_eq(last, v->name)) {
            m->tag = last;
        } else {
            Buf t;
            buf_init(&t);
            cc_letter_tag(&t, i);
            int bad = dup_buf(a, &t, &m->tag);
            buf_free(&t);
            if (bad) return -1;
        }
        m->origin[0] = v->origin[0];
        m->origin[1] = v->origin[1];
        m->facing = v->facing;
        m->true_north = v->true_north;
        (*nout)++;
    }
    return 0;
}

void cc_turn_apply_point(const cc_turn *t, const double p[3], double out[3]) {
    if (t->kind == CC_TURN_SECTION) {
        double dx = t->d[0], dy = t->d[1];
        out[0] = dy * p[0] - dx * p[1];
        out[1] = p[2];
        out[2] = -(dx * p[0] + dy * p[1]);
        return;
    }
    double q[3] = {p[0], p[1], p[2]};
    double a = -t->a;
    if (a != 0.0) {
        double s = cc_sin(a), c = cc_cos(a);
        q[0] = c * p[0] - s * p[1];
        q[1] = s * p[0] + c * p[1];
    }
    out[0] = q[0];
    out[1] = q[1];
    out[2] = t->kind == CC_TURN_REFLECT ? -q[2] : q[2];
}

/* Facing::dir_turned. */
static void facing_dir_turned(cc_facing f, double a, double out[2]) {
    double x = f == CC_FACING_EAST ? 1.0 : f == CC_FACING_WEST ? -1.0 : 0.0;
    double y = f == CC_FACING_NORTH ? 1.0 : f == CC_FACING_SOUTH ? -1.0 : 0.0;
    double s = cc_sin(a), c = cc_cos(a);
    out[0] = c * x - s * y + 0.0;
    out[1] = s * x + c * y + 0.0;
}

static void mark_at(cc_annot_ctx *c, const cc_turn *turn, const double so[3], double x, double y, double out[2]) {
    double p[3] = {x - so[0], y - so[1], 0.0}, q[3];
    cc_turn_apply_point(turn, p, q);
    cc_annot_to_paper(c, q, out);
}

int cc_annot_section_marks(cc_annot_ctx *c, const cc_section_mark *marks, size_t n, const cc_turn *turn,
                           const double so[3], const cc_georef *geo, const double blo[2], const double bhi[2]) {
    if (!n) return 0;
    double lo_p[2], hi_p[2];
    cc_annot_to_paper(c, blo, lo_p);
    cc_annot_to_paper(c, bhi, hi_p);
    double x0 = fmin_rs(lo_p[0], hi_p[0]) - 6.0, x1 = fmax_rs(lo_p[0], hi_p[0]) + 6.0;
    double y0 = fmin_rs(lo_p[1], hi_p[1]) - 6.0, y1 = fmax_rs(lo_p[1], hi_p[1]) + 6.0;
    cc_pen axis, thick, seen;
    if (pen_of(c, CC_ROLE_AXIS, c->scale, &axis) || thicker(c, CC_ROLE_CUT, 1.5, &thick) ||
        pen_of(c, CC_ROLE_SEEN, c->scale, &seen))
        return -1;
    double t = text_mm(c);
    for (size_t i = 0; i < n; i++) {
        const cc_section_mark *m = &marks[i];
        double f[2], ce[2], ahead[2];
        facing_dir_turned(m->facing, cc_georef_plan_turn(geo, m->true_north), f);
        mark_at(c, turn, so, m->origin[0], m->origin[1], ce);
        mark_at(c, turn, so, m->origin[0] + f[0], m->origin[1] + f[1], ahead);
        double fd[2] = {ahead[0] - ce[0], ahead[1] - ce[1]};
        double fl = cc_hypot(fd[0], fd[1]);
        if (fl < 1e-12) continue;
        fd[0] = fd[0] / fl;
        fd[1] = fd[1] / fl;
        /* Across the drawing, perpendicular to where it looks (facings are along the axes). */
        double g[2] = {-fd[1], fd[0]};
        double e[2][2];
        if (fabs_(g[0]) > 0.5) {
            if (!(ce[1] >= y0 && ce[1] <= y1)) continue;
            e[0][0] = x0, e[0][1] = ce[1], e[1][0] = x1, e[1][1] = ce[1];
        } else {
            if (!(ce[0] >= x0 && ce[0] <= x1)) continue;
            e[0][0] = ce[0], e[0][1] = y0, e[1][0] = ce[0], e[1][1] = y1;
        }
        if (one_line(c, (const double(*)[2])e, 2, &axis)) return -1;
        for (int k = 0; k < 2; k++) {
            const double *p = e[k], *o = e[1 - k];
            double inward[2] = {o[0] - p[0], o[1] - p[1]};
            double il = fmax_rs(cc_hypot(inward[0], inward[1]), 1e-9);
            double iw[2] = {inward[0] / il, inward[1] / il};
            double seg[2][2] = {{p[0], p[1]}, {p[0] + iw[0] * 6.0, p[1] + iw[1] * 6.0}};
            if (one_line(c, (const double(*)[2])seg, 2, &thick)) return -1;
            /* The arrow from the end, looking along the section. */
            double base[2] = {p[0] + iw[0] * 1.5, p[1] + iw[1] * 1.5};
            double head[2] = {base[0] + fd[0] * 4.0, base[1] + fd[1] * 4.0};
            double arrow[2][2] = {{base[0], base[1]}, {head[0], head[1]}};
            if (one_line(c, (const double(*)[2])arrow, 2, &seen)) return -1;
            double tri[3][2] = {{head[0], head[1]},
                                {head[0] - fd[0] * 2.0 + iw[0] * 1.0, head[1] - fd[1] * 2.0 + iw[1] * 1.0},
                                {head[0] - fd[0] * 2.0 - iw[0] * 1.0, head[1] - fd[1] * 2.0 - iw[1] * 1.0}};
            if (area1(c, (const double(*)[2])tri, 3, 1, c->style->edges, NULL, 0, NULL)) return -1;
            /* The tag beyond the end of the line. */
            double size = t * 1.2;
            double pos[2] = {p[0] - iw[0] * 3.0, p[1] - iw[1] * 3.0 + size * 0.36};
            if (text_at(c, pos, size, m->tag.p, m->tag.n, CC_ANCHOR_MIDDLE, 0.0, 1)) return -1;
        }
    }
    return 0;
}

/* ---- a view's own dimensions and texts ---- */

void cc_dim_type_format(Buf *b, const cc_dim_type *d, double mm) {
    double v;
    const char *u;
    if (d->unit == CC_DIM_M) v = mm / 1000.0, u = "m";
    else if (d->unit == CC_DIM_CM) v = mm / 10.0, u = "cm";
    else v = mm, u = "mm";
    int dec = d->decimals < 6 ? d->decimals : 6;
    if (fmt_f64_fixed(b, v, dec)) buf_puts(b, v != v ? "NaN" : v > 0 ? "inf" : "-inf");
    if (d->show_unit) {
        buf_putc(b, ' ');
        buf_puts(b, u);
    }
}

static void view_at(cc_annot_ctx *c, const cc_turn *turn, const double so[3], const double p[3], double out[2]) {
    double q[3] = {p[0] - so[0], p[1] - so[1], p[2] - so[2]}, r[3];
    cc_turn_apply_point(turn, q, r);
    cc_annot_to_paper(c, r, out);
}

int cc_annot_view(cc_annot_ctx *c, const cc_model *m, const cc_view *v, const cc_turn *turn, const double so[3]) {
    const cc_paper_style *ps = &c->style->paper;
    double t = text_mm(c);
    cc_rgb ink = c->style->edges;
    cc_pen note;
    if (pen_of(c, CC_ROLE_DIMENSION, c->scale, &note)) return -1;
    for (size_t i = 0; i < v->nannotations; i++) {
        const cc_annotation *an = &v->annotations[i];
        if (an->type == CC_ANN_DIMENSION) {
            const cc_dim_type *dt = NULL;
            for (size_t j = 0; an->has_kind && j < ps->ndim_types && !dt; j++)
                if (str_eq(ps->dim_types[j].name, an->kind)) dt = &ps->dim_types[j];
            double wa[3], wb[3], pa[2], pb[2];
            cc_anchor_point(m, &an->a, wa);
            cc_anchor_point(m, &an->b, wb);
            view_at(c, turn, so, wa, pa);
            view_at(c, turn, so, wb, pb);
            double u[2], qa[2], qb[2], len;
            if (cc_dimension_on_paper(pa, pb, an->offset_mm * c->k, an->axis, u, qa, qb, &len)) continue;
            /* Its type's line (annotation pen) in its colour, else the dimension line. */
            cc_pen pen = note;
            if (dt) {
                const cc_line_type *lt = cc_style_line_type(ps, dt->line_type.p, dt->line_type.n);
                float w;
                if (lt && cc_pen_annotation_mm(&ps->pens, lt->pen, &w)) {
                    double *dash = arena_alloc(c->a, (lt->ndash ? lt->ndash : 1) * sizeof *dash, _Alignof(double));
                    if (!dash) return -1;
                    for (size_t k = 0; k < lt->ndash; k++) dash[k] = (double)lt->dash_mm[k];
                    pen.width_mm = (double)w;
                    pen.dash = dash;
                    pen.ndash = lt->ndash;
                }
                pen.color = dt->color;
            }
            Buf val;
            buf_init(&val);
            if (dt)
                cc_dim_type_format(&val, dt, len / c->k);
            else
                cc_dim_format(&val, len / c->k, (int)c->tpl->dimension_unit);
            int rc = dimension_items(c, pa, pb, u, qa, qb, &pen, val.p, val.len, dt ? (double)dt->text_mm : t * 0.9,
                                     dt ? dt->color : ink);
            buf_free(&val);
            if (rc) return -1;
        } else {
            const cc_text_type *tt = NULL;
            for (size_t j = 0; an->has_kind && j < ps->ntext_types && !tt; j++)
                if (str_eq(ps->text_types[j].name, an->kind)) tt = &ps->text_types[j];
            double size = tt ? (double)tt->size_mm : an->size_mm;
            cc_rgb color = tt ? tt->color : ink;
            size_t start = 0, line = 0;
            for (size_t j = 0; j <= an->text.n; j++) {
                if (j < an->text.n && an->text.p[j] != '\n') continue;
                double q[2];
                view_at(c, turn, so, an->at, q);
                double pos[2] = {q[0], q[1] + (double)line * size * 1.25};
                if (text_full(c, pos, size, an->text.p + start, j - start, color, CC_ANCHOR_START, an->angle_deg, 0))
                    return -1;
                start = j + 1;
                line++;
            }
        }
    }
    return 0;
}

/* ---- whole sheets ---- */

/* 10^e as 10f64.powf(e) gives it for a small whole e (glibc's pow is correctly rounded there,
 * and so are these: exact products, then one division). */
static double pow10_(int e) {
    double q = 1.0;
    for (int i = 0; i < (e < 0 ? -e : e); i++) q *= 10.0;
    return e < 0 ? 1.0 / q : q;
}

/* a * b = hi + lo exactly (Dekker, Veltkamp's split; no FMA). */
static void two_prod(double a, double b, double *hi, double *lo) {
    double ca = 134217729.0 * a, cb = 134217729.0 * b;
    double ah = ca - (ca - a), al = a - ah, bh = cb - (cb - b), bl = b - bh;
    *hi = a * b;
    *lo = ((ah * bh - *hi) + ah * bl + al * bh) + al * bl;
}

/* 10^floor(log10(x)) as Rust computes it (glibc's log10, then powf): the power of ten at or
 * below x, or the next one when log10(x) rounds to that whole number (x a hair under it). x
 * positive and finite. glibc's log10 is not always correctly rounded, so within about 1e-16 of a
 * power of ten this can still differ from it; for x = 50 / (1 / n) / 1000, every scale n in
 * 1..=5000, it gives Python's 10 ** floor(log10(x)) (glibc) exactly (checked by hand). */
static double magnitude(double x) {
    int e = 0;
    while (e < 300 && pow10_(e + 1) <= x) e++;
    while (e > -300 && pow10_(e) > x) e--;
    int t = e + 1, n = t < 0 ? -t : t;
    if (!t) return pow10_(e);
    /* log10(x) = t - d, d ~ (1 - x / 10^t) / ln 10 (10^t the true power: for t < 0 x * 10^-t is
     * taken exactly). */
    double d;
    if (t > 0) {
        d = (pow10_(t) - x) / pow10_(t) / 2.302585092994046;
    } else {
        double hi, lo;
        two_prod(x, pow10_(n), &hi, &lo);
        d = ((1.0 - hi) - lo) / 2.302585092994046;
    }
    /* Rounds to t when d is under half the spacing of doubles on its side of |t|: ulp(|t|),
     * halved below a power of two. */
    double u = 2.220446049250313e-16;
    for (int k = n; k >= 2; k /= 2) u *= 2.0;
    if (t > 0 && !(n & (n - 1))) u /= 2.0;
    return d < u / 2.0 ? pow10_(t) : pow10_(e);
}

int cc_annot_scale_bar(cc_annot_ctx *c, double w, double title_y) {
    double k = c->k, t = text_mm(c);
    cc_rgb ink = c->style->edges;
    /* A round length about 50 mm long. */
    double target_m = 50.0 / k / 1000.0;
    double mag = magnitude(target_m);
    static const double F[4] = {1.0, 2.0, 5.0, 10.0};
    double len_m = mag;
    for (int i = 3; i >= 0; i--)
        if (F[i] * mag <= target_m) {
            len_m = F[i] * mag;
            break;
        }
    double bar = len_m * 1000.0 * k;
    double bx = w - MARGIN - PAD - bar, by = title_y - 2.0;
    cc_pen proj;
    if (pen_of(c, CC_ROLE_SEEN, c->scale, &proj)) return -1;
    for (int i = 0; i < 4; i++) {
        double x0 = bx + bar * (double)i / 4.0, x1 = bx + bar * (double)(i + 1) / 4.0;
        double r[4][2] = {{x0, by}, {x1, by}, {x1, by + 2.0}, {x0, by + 2.0}};
        if (area1(c, (const double(*)[2])r, 4, 1, i % 2 == 0 ? ink : WHITE, NULL, 0, &proj)) return -1;
    }
    double p0[2] = {bx, by - 1.5}, p1[2] = {bx + bar, by - 1.5};
    if (text_at(c, p0, t * 0.8, "0", 1, CC_ANCHOR_START, 0.0, 0)) return -1;
    Buf l;
    buf_init(&l);
    if (len_m >= 1.0) {
        fmt_f64_fixed(&l, len_m, -1);
        buf_puts(&l, " m");
    } else {
        fmt_f64_fixed(&l, len_m * 1000.0, -1);
        buf_puts(&l, " mm");
    }
    int rc = text_at(c, p1, t * 0.8, l.p, l.len, CC_ANCHOR_END, 0.0, 0);
    buf_free(&l);
    return rc;
}

static void fill(Buf *b, cc_str s, const cc_fill_values *fv) {
    cc_template_fill(b, s.p, s.n, fv->values, fv->nvalues, fv->fields, fv->nfields);
}

int cc_annot_level_note(cc_annot_ctx *c, double w, double h, const cc_fill_values *fv) {
    double tb_top = h - MARGIN - c->tpl->title_block.height_mm;
    Buf note;
    buf_init(&note);
    fill(&note, c->tpl->level_note, fv);
    int rc = 0;
    if (note.len) {
        double pos[2] = {w - MARGIN - PAD, tb_top - 2.5};
        rc = text_at(c, pos, text_mm(c) * 0.9, note.p, note.len, CC_ANCHOR_END, 0.0, 0);
    }
    buf_free(&note);
    return rc;
}

static double legend_height(size_t n) {
    return n == 0 ? 0.0 : 10.0 + (double)(n < LEGEND_ROWS ? n : LEGEND_ROWS) * LEGEND_ROW + PAD;
}
static double legend_width(size_t n) { return (double)((n + LEGEND_ROWS - 1) / LEGEND_ROWS) * LEGEND_COL + 4.0; }

int cc_draw_legend(cc_annot_ctx *c, double x0, double bottom, const cc_legend_entry *legend, size_t n) {
    if (!n) return 0;
    const cc_style *style = c->style;
    double t = text_mm(c);
    /* Swatches as at 1:100. */
    cc_pen proj, cut, above;
    if (pen_of(c, CC_ROLE_SEEN, 100, &proj) || pen_of(c, CC_ROLE_CUT, 100, &cut) ||
        pen_of(c, CC_ROLE_ABOVE, 100, &above))
        return -1;
    double y0 = bottom - legend_height(n) + PAD, x1 = x0 + legend_width(n), y1 = bottom;
    double box[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    if (one_line(c, (const double(*)[2])box, 4, &proj)) return -1;
    double tp[2] = {x0 + 3.0, y0 + 5.5};
    if (text_at(c, tp, t, c->tpl->legend.title.p, c->tpl->legend.title.n, CC_ANCHOR_START, 0.0, 1)) return -1;
    for (size_t i = 0; i < n; i++) {
        size_t col = i / LEGEND_ROWS, row = i % LEGEND_ROWS;
        double sx = x0 + 3.0 + (double)col * LEGEND_COL;
        double sy = y0 + 9.0 + (double)row * LEGEND_ROW;
        double rect[4][2] = {{sx, sy}, {sx + 10.0, sy}, {sx + 10.0, sy + 4.0}, {sx, sy + 4.0}};
        const cc_legend_entry *e = &legend[i];
        if (e->kind == CC_SWATCH_ABOVE) {
            double l[2][2] = {{sx, sy + 2.0}, {sx + 10.0, sy + 2.0}};
            if (one_line(c, (const double(*)[2])l, 2, &above)) return -1;
        } else {
            cc_area_look look;
            int cutk = e->kind == CC_SWATCH_CUT;
            if ((cutk ? cc_cut_look(style, e->cat, 100, c->a, &look) : cc_seen_look(style, e->cat, 100, c->a, &look)) ||
                area1(c, (const double(*)[2])rect, 4, look.has_fill, look.fill, look.hatches, look.nhatches,
                      cutk ? &cut : &proj))
                return -1;
        }
        double lp[2] = {sx + 13.0, sy + 3.3};
        if (text_at(c, lp, t * 0.9, e->label.p, e->label.n, CC_ANCHOR_START, 0.0, 0)) return -1;
    }
    return 0;
}

int cc_frame_and_title_block(cc_annot_ctx *c, double w, double h, const cc_fill_values *fv, cc_str version) {
    const cc_plan_template *tpl = c->tpl;
    cc_rgb ink = c->style->edges;
    double t = text_mm(c), tb_h = tpl->title_block.height_mm, tb_top = h - MARGIN - tb_h;
    cc_pen frame;
    if (pen_of(c, CC_ROLE_FRAME, 100, &frame)) return -1;
    double r[5][2] = {{MARGIN, MARGIN}, {w - MARGIN, MARGIN}, {w - MARGIN, h - MARGIN}, {MARGIN, h - MARGIN}, {MARGIN, MARGIN}};
    if (one_line(c, (const double(*)[2])r, 5, &frame)) return -1;
    double frame_w = w - 2.0 * MARGIN;
    size_t ncols = tpl->title_block.ncolumns;
    if (ncols) {
        const cc_tb_column *cols = tpl->title_block.columns;
        double fixed = -0.0;
        size_t flexible = 0;
        for (size_t i = 0; i < ncols; i++) {
            if (cols[i].has_width) fixed += cols[i].width_mm;
            else flexible++;
        }
        double share = flexible > 0 ? fmax_rs((frame_w - fixed) / (double)flexible, 20.0) : 0.0;
        double total = fixed + share * (double)flexible;
        /* Squeeze proportionally when the columns are wider than the frame; stretch the last
         * column when they are narrower. */
        double squeeze = fmin_rs(frame_w / total, 1.0);
        double(*lines)[2] = malloc(2 * (ncols + 1) * sizeof *lines);
        if (!lines) return -1;
        size_t nl = 0;
        lines[0][0] = MARGIN, lines[0][1] = tb_top, lines[1][0] = w - MARGIN, lines[1][1] = tb_top;
        nl = 1;
        double x = MARGIN;
        Buf b;
        buf_init(&b);
        int rc = 0;
        for (size_t i = 0; i < ncols && !rc; i++) {
            const cc_tb_column *col = &cols[i];
            double cw = (col->has_width ? col->width_mm : share) * squeeze;
            if (i + 1 == ncols) cw = w - MARGIN - x;
            if (i > 0) {
                lines[2 * nl][0] = x, lines[2 * nl][1] = tb_top;
                lines[2 * nl + 1][0] = x, lines[2 * nl + 1][1] = h - MARGIN;
                nl++;
            }
            double eh = tb_h / (double)(col->nentries > 1 ? col->nentries : 1);
            for (size_t j = 0; j < col->nentries && !rc; j++) {
                const cc_tb_entry *e = &col->entries[j];
                double ey = tb_top + (double)j * eh;
                double y = ey + 2.0 + t * 1.1;
                if (e->label.n) {
                    buf_clear(&b);
                    fill(&b, e->label, fv);
                    double pos[2] = {x + 2.5, y};
                    rc = text_at(c, pos, t * 1.1, b.p, b.len, CC_ANCHOR_START, 0.0, 1);
                    y += t * 0.5;
                }
                double size = t * e->size;
                buf_clear(&b);
                fill(&b, e->value, fv);
                size_t start = 0;
                for (size_t q = 0; q <= b.len && !rc; q++) {
                    if (q < b.len && b.p[q] != '\n') continue;
                    y += size * 1.25;
                    if (y > ey + eh) break;
                    double pos[2] = {x + 2.5, y};
                    rc = text_full(c, pos, size, b.p + start, q - start, e->has_color ? e->color : ink, CC_ANCHOR_START,
                                   0.0, 0);
                    start = q + 1;
                }
            }
            x += cw;
        }
        buf_free(&b);
        if (!rc) rc = segs_item(c, (const double(*)[2])lines, nl, &frame);
        free(lines);
        if (rc) return -1;
    }
    Buf v;
    buf_init(&v);
    buf_puts(&v, "Bimble ");
    buf_append(&v, version.p, version.n);
    double pos[2] = {w - MARGIN, h - MARGIN + 3.5};
    int rc = text_at(c, pos, 2.0, v.p, v.len, CC_ANCHOR_END, 0.0, 0);
    buf_free(&v);
    return rc;
}
