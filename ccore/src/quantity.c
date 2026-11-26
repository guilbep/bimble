/* See quantity.h. Ports crates/bimble-drawing/src/schedule.rs (measure, crossings, density,
 * cell, totals, schedule, to_csv, table_json) and the helpers it takes from plan.rs (cm,
 * section_text, letter_tag, clusters). Every expression keeps Rust's types and order of
 * operations, sums run in Rust's order: that is what makes the texts equal.
 * Floating-point contraction (a*b+c into fma) would change bits: it is off. */
#ifdef __clang__
#pragma clang fp contract(off)
#endif
#include "quantity.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "json_w.h"
#include "library.h"
#include "math.h"

static const double PI = 3.14159265358979323846264338327950288;

static uint64_t bits64(double x) {
    uint64_t u;
    memcpy(&u, &x, 8);
    return u;
}
static double fabs_(double x) { return x < 0 || (x == 0 && bits64(x) >> 63) ? -x : x; }
static int is_nan(double x) { return x != x; }
/* f64::min / f64::max (llvm minnum/maxnum; aarch64 fminnm/fmaxnm: -0.0 below 0.0). */
static double fmin_(double a, double b) {
    if (is_nan(a)) return b;
    if (is_nan(b)) return a;
    if (a == b) return bits64(a) >> 63 ? a : b;
    return b < a ? b : a;
}
static double fmax_(double a, double b) {
    if (is_nan(a)) return b;
    if (is_nan(b)) return a;
    if (a == b) return bits64(a) >> 63 ? b : a;
    return b > a ? b : a;
}
/* f64::round: half away from zero. */
static double round_(double x) {
    double a = fabs_(x);
    if (is_nan(x) || a >= 4503599627370496.0) return x; /* 2^52: already whole */
    double t = (double)(int64_t)a;                         /* trunc */
    double r = a - t >= 0.5 ? t + 1.0 : t;
    return bits64(x) >> 63 ? -r : r;
}
/* `as i64`: saturating, NaN -> 0. */
static int64_t to_i64(double x) {
    if (is_nan(x)) return 0;
    if (x >= 9223372036854775808.0) return INT64_MAX;
    if (x <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)x;
}
/* f64::total_cmp */
static int total_cmp(double a, double b) {
    int64_t x = (int64_t)bits64(a), y = (int64_t)bits64(b);
    x ^= (int64_t)((uint64_t)(x >> 63) >> 1);
    y ^= (int64_t)((uint64_t)(y >> 63) >> 1);
    return x < y ? -1 : x > y;
}

static int str_eq(cc_str a, cc_str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }
static int str_cmp(cc_str a, cc_str b) { /* Ord for str: bytes, then length */
    size_t n = a.n < b.n ? a.n : b.n;
    int c = n ? memcmp(a.p, b.p, n) : 0;
    if (c) return c < 0 ? -1 : 1;
    return a.n < b.n ? -1 : a.n > b.n;
}
static int ascii_ieq(cc_str a, const char *b, size_t bn) { /* eq_ignore_ascii_case */
    if (a.n != bn) return 0;
    for (size_t i = 0; i < bn; i++) {
        unsigned char x = (unsigned char)a.p[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}
static cc_str cs(const char *s) {
    cc_str r = {s, strlen(s)};
    return r;
}
static cc_str arena_str(Arena *a, const char *p, size_t n) {
    cc_str r = {"", 0};
    if (!n) return r;
    char *d = arena_alloc(a, n, 1);
    if (!d) {
        r.p = NULL;
        return r;
    }
    memcpy(d, p, n);
    r.p = d, r.n = n;
    return r;
}

/* ---- UTF-8 ---- */

/* Next code point of s[*i..n); a byte that starts no valid sequence is returned as is (0x80..
 * 0xFF) and stands for itself. */
static uint32_t utf8_next(const char *s, size_t n, size_t *i, size_t *len) {
    const unsigned char *u = (const unsigned char *)s + *i;
    size_t left = n - *i;
    uint32_t c = u[0];
    size_t k = 1;
    if (c >= 0xC0 && c < 0xE0 && left >= 2 && (u[1] & 0xC0) == 0x80) c = (c & 0x1F) << 6 | (u[1] & 0x3F), k = 2;
    else if (c >= 0xE0 && c < 0xF0 && left >= 3 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80)
        c = (c & 0x0F) << 12 | (uint32_t)(u[1] & 0x3F) << 6 | (u[2] & 0x3F), k = 3;
    else if (c >= 0xF0 && c < 0xF8 && left >= 4 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 &&
             (u[3] & 0xC0) == 0x80)
        c = (c & 0x07) << 18 | (uint32_t)(u[1] & 0x3F) << 12 | (uint32_t)(u[2] & 0x3F) << 6 | (u[3] & 0x3F), k = 4;
    *i += k;
    if (len) *len = k;
    return c;
}

static void utf8_put(Buf *b, uint32_t c) {
    if (c < 0x80) buf_putc(b, (char)c);
    else if (c < 0x800) buf_putc(b, (char)(0xC0 | c >> 6)), buf_putc(b, (char)(0x80 | (c & 0x3F)));
    else if (c < 0x10000)
        buf_putc(b, (char)(0xE0 | c >> 12)), buf_putc(b, (char)(0x80 | (c >> 6 & 0x3F))),
            buf_putc(b, (char)(0x80 | (c & 0x3F)));
    else
        buf_putc(b, (char)(0xF0 | c >> 18)), buf_putc(b, (char)(0x80 | (c >> 12 & 0x3F))),
            buf_putc(b, (char)(0x80 | (c >> 6 & 0x3F))), buf_putc(b, (char)(0x80 | (c & 0x3F)));
}

/* char::to_lowercase for the Latin, Greek and Cyrillic blocks (see quantity.h); other code
 * points are kept. U+0130 lowers to two code points. */
static void lower_put(Buf *b, uint32_t c) {
    uint32_t l = c;
    if (c >= 'A' && c <= 'Z') l = c + 32;
    else if (c < 0xC0) l = c;
    else if ((c <= 0xD6) || (c >= 0xD8 && c <= 0xDE)) l = c + 32;
    else if (c == 0x130) {
        buf_putc(b, 'i');
        utf8_put(b, 0x307);
        return;
    } else if ((c >= 0x100 && c <= 0x12F) || (c >= 0x132 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        l = c & 1 ? c : c + 1;
    else if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) l = c & 1 ? c + 1 : c;
    else if (c == 0x178) l = 0xFF;
    else if (c == 0x386) l = 0x3AC;
    else if (c >= 0x388 && c <= 0x38A) l = c + 37;
    else if (c == 0x38C) l = 0x3CC;
    else if (c == 0x38E || c == 0x38F) l = c + 63;
    else if ((c >= 0x391 && c <= 0x3A1) || (c >= 0x3A3 && c <= 0x3AB)) l = c + 32;
    else if (c >= 0x400 && c <= 0x40F) l = c + 80;
    else if (c >= 0x410 && c <= 0x42F) l = c + 32;
    else if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F))
        l = c & 1 ? c : c + 1;
    else if (c == 0x4C0) l = 0x4CF;
    else if (c >= 0x4C1 && c <= 0x4CE) l = c & 1 ? c + 1 : c;
    else if ((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF)) l = c & 1 ? c : c + 1;
    else if (c == 0x1E9E) l = 0xDF;
    else if (c >= 0xFF21 && c <= 0xFF3A) l = c + 32;
    utf8_put(b, l);
}

/* char::is_whitespace (the White_Space property). */
static int is_ws(uint32_t c) {
    return (c >= 9 && c <= 13) || c == ' ' || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

/* str::trim */
static cc_str trim(cc_str s) {
    size_t i = 0, start = s.n, end = 0;
    while (i < s.n) {
        size_t at = i, len;
        uint32_t c = utf8_next(s.p, s.n, &i, &len);
        if (!is_ws(c)) {
            if (start == s.n) start = at;
            end = at + len;
        }
    }
    cc_str r = {s.p + (start < end ? start : 0), start < end ? end - start : 0};
    return r;
}

static size_t char_count(cc_str s) {
    size_t i = 0, k = 0;
    while (i < s.n) utf8_next(s.p, s.n, &i, NULL), k++;
    return k;
}

/* ---- exact fixed-point formatting ---- */

#define BIG_LIMBS 80 /* 2560 bits: 2^53 * 10^40 * 2^971 fits */
typedef struct {
    uint32_t d[BIG_LIMBS]; /* little-endian */
    int n;                  /* limbs in use */
} Big;

static void big_mul_small(Big *a, uint32_t m) {
    uint64_t carry = 0;
    for (int i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->d[i] * m + carry;
        a->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry && a->n < BIG_LIMBS) a->d[a->n++] = (uint32_t)carry;
}
static int big_bit(const Big *a, int k) { return k / 32 < a->n ? (int)(a->d[k / 32] >> (k % 32) & 1) : 0; }
static int big_low_nonzero(const Big *a, int k) { /* any of bits 0..k set */
    for (int i = 0; i < k / 32 && i < a->n; i++)
        if (a->d[i]) return 1;
    if (k / 32 < a->n && (a->d[k / 32] & ((1u << (k % 32)) - 1))) return 1;
    return 0;
}
static void big_shr(Big *a, int s) {
    int w = s / 32, r = s % 32;
    if (w >= a->n) {
        a->n = 0;
        return;
    }
    for (int i = 0; i + w < a->n; i++) {
        uint64_t lo = a->d[i + w], hi = i + w + 1 < a->n ? a->d[i + w + 1] : 0;
        a->d[i] = (uint32_t)((lo | hi << 32) >> r);
    }
    a->n -= w;
    while (a->n && !a->d[a->n - 1]) a->n--;
}
static void big_shl(Big *a, int s) {
    int w = s / 32, r = s % 32;
    int n = a->n + w + 1;
    if (n > BIG_LIMBS) n = BIG_LIMBS;
    for (int i = n - 1; i >= 0; i--) {
        uint64_t hi = i - w >= 0 && i - w < a->n ? a->d[i - w] : 0;
        uint64_t lo = i - w - 1 >= 0 && i - w - 1 < a->n ? a->d[i - w - 1] : 0;
        a->d[i] = (uint32_t)(((hi << 32 | lo) << r) >> 32);
    }
    a->n = n;
    while (a->n && !a->d[a->n - 1]) a->n--;
}
static void big_add1(Big *a) {
    for (int i = 0; i < a->n; i++)
        if (++a->d[i]) return;
    if (a->n < BIG_LIMBS) a->d[a->n++] = 1;
}
static uint32_t big_divmod(Big *a, uint32_t m) {
    uint64_t r = 0;
    for (int i = a->n - 1; i >= 0; i--) {
        uint64_t t = r << 32 | a->d[i];
        a->d[i] = (uint32_t)(t / m);
        r = t % m;
    }
    while (a->n && !a->d[a->n - 1]) a->n--;
    return (uint32_t)r;
}

void cc_fmt_fixed(Buf *b, double x, int p) {
    if (p < 0) p = 0;
    if (p > 40) p = 40;
    if (is_nan(x)) {
        buf_puts(b, "NaN");
        return;
    }
    uint64_t u = bits64(x);
    if (u >> 63) buf_putc(b, '-');
    int ex = (int)(u >> 52 & 0x7FF);
    uint64_t m = u & ((1ull << 52) - 1);
    if (ex == 0x7FF) {
        buf_puts(b, "inf");
        return;
    }
    int e;
    if (ex == 0) e = -1074;
    else m |= 1ull << 52, e = ex - 1075;
    Big a;
    memset(&a, 0, sizeof a);
    a.d[0] = (uint32_t)m, a.d[1] = (uint32_t)(m >> 32);
    a.n = a.d[1] ? 2 : a.d[0] ? 1 : 0;
    for (int i = 0; i < p; i++) big_mul_small(&a, 10);
    if (e >= 0) big_shl(&a, e);
    else {
        int s = -e;
        int half = big_bit(&a, s - 1), below = big_low_nonzero(&a, s - 1);
        big_shr(&a, s);
        if (half && (below || (a.n && (a.d[0] & 1)))) big_add1(&a);
    }
    char dig[800];
    int nd = 0;
    while (a.n && nd + 9 < (int)sizeof dig) {
        uint32_t r = big_divmod(&a, 1000000000u);
        for (int k = 0; k < 9; k++) dig[nd++] = (char)('0' + r % 10), r /= 10;
    }
    while (nd > 1 && dig[nd - 1] == '0') nd--;
    while (nd < p + 1) dig[nd++] = '0';
    for (int i = nd - 1; i >= p; i--) buf_putc(b, dig[i]);
    if (p) {
        buf_putc(b, '.');
        for (int i = p - 1; i >= 0; i--) buf_putc(b, dig[i]);
    }
}

/* ---- measure ---- */

/* plan.rs cm: whole cm, or one decimal. */
static void cm(Buf *b, double mm) {
    double c = mm / 10.0;
    if (fabs_(c - round_(c)) < 0.05) buf_i64(b, to_i64(round_(c)));
    else cc_fmt_fixed(b, c, 1);
}

/* plan.rs section_text: "30/30", or for timber with a material name "GL24h 24x46". */
static void section_text(Buf *b, cc_category cat, cc_str material, double w, double h) {
    cc_str m = trim(material);
    if (cat == CC_CAT_TIMBER && m.n && char_count(m) <= 40) {
        buf_append(b, m.p, m.n);
        buf_putc(b, ' ');
        cm(b, w);
        buf_putc(b, 'x');
        cm(b, h);
    } else {
        cm(b, w);
        buf_putc(b, '/');
        cm(b, h);
    }
}

static void ascii_lower(char *d, cc_str s, size_t cap) {
    size_t n = s.n < cap - 1 ? s.n : cap - 1;
    for (size_t i = 0; i < n; i++) {
        char c = s.p[i];
        d[i] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    d[n] = 0;
}

int cc_measure(const cc_scene *s, const cc_viewfile_meta *mt, size_t k, Arena *a, cc_measures *out) {
    memset(out, 0, sizeof *out);
    out->section.p = "";
    cc_mesh m;
    if (cc_scene_world_mesh(s, k, a, &m)) return -1;
    const double *o = s->origin;
    if (!m.nv) return 0;
    double dir[2] = {1.0, 0.0}, best = 0.0;
    for (uint32_t i = 0; i < m.ne; i++) {
        const float *e = m.edges[i];
        double dx = (double)e[3] - (double)e[0], dy = (double)e[4] - (double)e[1], dz = (double)e[5] - (double)e[2];
        double l = cc_hypot(dx, dy);
        if (l > best && fabs_(dz) < 1.0) {
            best = l;
            dir[0] = dx / l, dir[1] = dy / l;
        }
    }
    double n[2] = {-dir[1], dir[0]};
    double s0 = 1.0 / 0.0, s1 = -1.0 / 0.0, t0 = 1.0 / 0.0, t1 = -1.0 / 0.0, z0 = 1.0 / 0.0, z1 = -1.0 / 0.0;
    double lo[2] = {1.0 / 0.0, 1.0 / 0.0}, hi[2] = {-1.0 / 0.0, -1.0 / 0.0};
    for (uint32_t i = 0; i < m.nv; i++) {
        double p[3] = {m.pos[i][0], m.pos[i][1], m.pos[i][2]};
        double sv = p[0] * dir[0] + p[1] * dir[1], tv = p[0] * n[0] + p[1] * n[1];
        s0 = fmin_(s0, sv), s1 = fmax_(s1, sv), t0 = fmin_(t0, tv), t1 = fmax_(t1, tv);
        z0 = fmin_(z0, p[2]), z1 = fmax_(z1, p[2]);
        for (int j = 0; j < 2; j++) lo[j] = fmin_(lo[j], p[j]), hi[j] = fmax_(hi[j], p[j]);
    }
    double along = s1 - s0, across = t1 - t0, height = z1 - z0;
    /* Mesh::volume: about the first vertex, summed in triangle order, then / 6. */
    double vol = -0.0;
    {
        double q[3] = {m.pos[0][0], m.pos[0][1], m.pos[0][2]};
        for (uint32_t t = 0; t + 3 <= m.ni; t += 3) {
            double v[3][3];
            for (int c = 0; c < 3; c++) {
                const float *pp = m.pos[m.idx[t + c]];
                for (int j = 0; j < 3; j++) v[c][j] = (double)pp[j] - q[j];
            }
            const double *A = v[0], *B = v[1], *C = v[2];
            vol += A[0] * (B[1] * C[2] - B[2] * C[1]) - A[1] * (B[0] * C[2] - B[2] * C[0]) +
                   A[2] * (B[0] * C[1] - B[1] * C[0]);
        }
        vol = vol / 6.0;
    }
    double volume_m3 = fabs_(vol) / 1e9;
    out->centre[0] = (lo[0] + hi[0]) / 2.0 + o[0];
    out->centre[1] = (lo[1] + hi[1]) / 2.0 + o[1];
    const cc_scene_element *el = &s->elements[k];
    char cls[64];
    ascii_lower(cls, el->class_name, sizeof cls);
    int long_class = el->class_name.n >= sizeof cls; /* longer than any class compared below */
    int standing = !long_class && (!strcmp(cls, "ifccolumn") || !strcmp(cls, "ifcpile") || !strcmp(cls, "ifcmember")) &&
                   height > fmax_(along, across);
    int wall = !strncmp(cls, "ifcwall", 7);
    double sx, sy;
    if (fabs_(dir[0]) >= fabs_(dir[1])) sx = along, sy = across;
    else sx = across, sy = along;
    double round = PI * (sx / 2.0) * (sx / 2.0) * height / 1e9;
    int circular = standing && m.nv >= 24 && fabs_(sx - sy) <= 0.02 * sx && fabs_(round - volume_m3) <= 0.03 * round;
    cc_str material = k < mt->ninfo ? mt->info[k].material : cs("");
    Buf sec;
    buf_init(&sec);
    if (circular) {
        buf_puts(&sec, "\xc3\x98");
        cm(&sec, sx);
    } else if (standing) section_text(&sec, el->category, material, sx, sy);
    else if (!long_class && (!strcmp(cls, "ifcbeam") || !strcmp(cls, "ifcfooting")))
        section_text(&sec, el->category, material, across, height);
    if (sec.oom) {
        buf_free(&sec);
        return -1;
    }
    out->section = arena_str(a, sec.p, sec.len);
    buf_free(&sec);
    if (!out->section.p) return -1;
    if (standing) out->length = height, out->width = fmin_(sx, sy);
    else out->length = along, out->width = across;
    out->height = height;
    if (wall) out->area_m2 = along * height / 1e6;
    else if (standing) out->area_m2 = volume_m3 * 1e9 / fmax_(height, 1e-9) / 1e6;
    else if (height > 1e-6) out->area_m2 = volume_m3 * 1e9 / height / 1e6;
    else out->area_m2 = 0.0;
    out->volume_m3 = volume_m3 + (k < mt->ninfo ? mt->info[k].volume_fix_m3 : 0.0); /* WAVE1B (B1): sleeves' exact cylinders */
    return 1;
}

int cc_density(const cc_viewfile_meta *mt, cc_str material, double *out) {
    for (size_t i = 0; i < mt->ndensities; i++)
        if (str_eq(mt->densities[i].material, material)) {
            *out = mt->densities[i].density;
            return 1;
        }
    const cc_library_material *l = cc_library_lookup(material.p ? material.p : "", material.n);
    if (!l) return 0;
    *out = l->density_kg_m3;
    return 1;
}

/* ---- grid crossings ---- */

typedef struct {
    cc_str tag;
    double a[2], b[2];
} Axis;

typedef struct {
    double p[2];
    uint32_t x, y; /* axis indices: the name is "<x>-<y>" */
} Crossing;

static int dbl_qcmp(const void *a, const void *b) { return total_cmp(*(const double *)a, *(const double *)b); }

/* plan.rs clusters: positions shared by at least two values (within tol), averaged, sorted.
 * Sorts `v` in place; writes the means to out (room for n/2) and returns their count. */
static size_t clusters(double *v, size_t n, double tol, double *out) {
    /* a stable sort is not needed: equal keys under total_cmp have equal bits */
    if (n > 1) qsort(v, n, sizeof *v, dbl_qcmp);
    size_t k = 0, i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && v[j] - v[j - 1] <= tol) j++;
        if (j - i >= 2) {
            double s = -0.0;
            for (size_t q = i; q < j; q++) s += v[q];
            out[k++] = s / (double)(j - i);
        }
        i = j;
    }
    return k;
}

/* plan.rs letter_tag: A..Z, AA, AB… */
static cc_str letter_tag(Arena *a, size_t i) {
    char t[32];
    int n = 0;
    for (;;) {
        t[n++] = (char)('A' + i % 26);
        if (i < 26) break;
        i = i / 26 - 1;
    }
    char r[32];
    for (int k = 0; k < n; k++) r[k] = t[n - 1 - k];
    return arena_str(a, r, (size_t)n);
}

static int starts_digit(cc_str s) { return s.n && s.p[0] >= '0' && s.p[0] <= '9'; }

/* crossings(scene). -1 out of memory. */
static int crossings(const cc_scene *s, const cc_viewfile_meta *mt, Arena *a, Axis **axes_out, Crossing **out,
                     size_t *nout) {
    Axis *axes;
    size_t na = 0;
    if (mt->ngrids) {
        axes = arena_alloc(a, mt->ngrids * sizeof *axes, 8);
        if (!axes) return -1;
        for (size_t i = 0; i < mt->ngrids; i++) {
            axes[i].tag = mt->grids[i].tag;
            memcpy(axes[i].a, mt->grids[i].start, sizeof axes[i].a);
            memcpy(axes[i].b, mt->grids[i].end, sizeof axes[i].b);
        }
        na = mt->ngrids;
    } else {
        const double *o = s->origin;
        size_t ne = s->nelements;
        double *xs = arena_alloc(a, (ne + 1) * sizeof *xs, 8), *ys = arena_alloc(a, (ne + 1) * sizeof *ys, 8);
        double *cx = arena_alloc(a, (ne + 1) * sizeof *cx, 8), *cy = arena_alloc(a, (ne + 1) * sizeof *cy, 8);
        if (!xs || !ys || !cx || !cy) return -1;
        size_t nc = 0;
        Arena scratch;
        arena_init(&scratch, 0);
        for (size_t k = 0; k < ne; k++) {
            if (!ascii_ieq(s->elements[k].class_name, "IfcColumn", 9)) continue;
            cc_mesh m;
            arena_reset(&scratch);
            if (cc_scene_world_mesh(s, k, &scratch, &m)) {
                arena_free(&scratch);
                return -1;
            }
            double lo[2] = {1.0 / 0.0, 1.0 / 0.0}, hi[2] = {-1.0 / 0.0, -1.0 / 0.0};
            for (uint32_t i = 0; i < m.nv; i++)
                for (int j = 0; j < 2; j++) {
                    double v = (double)m.pos[i][j] + o[j];
                    lo[j] = fmin_(lo[j], v), hi[j] = fmax_(hi[j], v);
                }
            if (lo[0] - lo[0] != 0.0) continue; /* not finite */
            xs[nc] = (lo[0] + hi[0]) / 2.0, ys[nc] = (lo[1] + hi[1]) / 2.0;
            nc++;
        }
        arena_free(&scratch);
        size_t nx = clusters(xs, nc, 50.0, cx), ny = clusters(ys, nc, 50.0, cy);
        axes = arena_alloc(a, (nx + ny + 1) * sizeof *axes, 8);
        if (!axes) return -1;
        for (size_t i = 0; i < nx; i++) {
            Axis *x = &axes[na++];
            x->tag = letter_tag(a, i);
            if (!x->tag.p) return -1;
            x->a[0] = cx[i], x->a[1] = 0.0, x->b[0] = cx[i], x->b[1] = 1.0;
        }
        for (size_t i = 0; i < ny; i++) {
            Axis *y = &axes[na++];
            Buf t;
            buf_init(&t);
            buf_u64(&t, i + 1);
            y->tag = arena_str(a, t.p, t.len);
            buf_free(&t);
            if (!y->tag.p) return -1;
            y->a[0] = 0.0, y->a[1] = cy[i], y->b[0] = 1.0, y->b[1] = cy[i];
        }
    }
    /* na (na - 1) / 2 pairs at most; a count that overflows is out of memory */
    if (na > 1 && na - 1 > SIZE_MAX / na) return -1;
    size_t cap = na > 1 ? na * (na - 1) / 2 : 0;
    if (cap >= SIZE_MAX / sizeof(Crossing)) return -1;
    Crossing *c = arena_alloc(a, (cap + 1) * sizeof *c, 8);
    if (!c) return -1;
    size_t nc = 0;
    for (size_t i = 0; i < na; i++) {
        const Axis *A = &axes[i];
        for (size_t j = i + 1; j < na; j++) {
            const Axis *B = &axes[j];
            double d1[2] = {A->b[0] - A->a[0], A->b[1] - A->a[1]};
            double d2[2] = {B->b[0] - B->a[0], B->b[1] - B->a[1]};
            double den = d1[0] * d2[1] - d1[1] * d2[0];
            if (fabs_(den) <= 1e-9 * cc_hypot(d1[0], d1[1]) * cc_hypot(d2[0], d2[1])) continue;
            double w[2] = {B->a[0] - A->a[0], B->a[1] - A->a[1]};
            double t = (w[0] * d2[1] - w[1] * d2[0]) / den;
            Crossing *x = &c[nc++];
            x->p[0] = A->a[0] + d1[0] * t, x->p[1] = A->a[1] + d1[1] * t;
            /* Letters first: "B-2", whichever axis is which. */
            if (starts_digit(A->tag)) x->x = (uint32_t)j, x->y = (uint32_t)i;
            else x->x = (uint32_t)i, x->y = (uint32_t)j;
        }
    }
    *axes_out = axes;
    *out = c;
    *nout = nc;
    return 0;
}

/* ---- natural order ---- */

typedef struct {
    cc_str text; /* lower case */
    uint64_t num;
} Seg;

typedef struct {
    Seg *v;
    size_t n;
} Natural;

/* schedule.rs natural: runs of non-digits (lower case) and numbers. */
static int natural(Arena *a, cc_str t, Natural *out) {
    size_t cap = 1;
    for (size_t i = 0; i < t.n; i++) cap += t.p[i] >= '0' && t.p[i] <= '9';
    Seg *v = arena_alloc(a, cap * sizeof *v, 8);
    if (!v) return -1;
    size_t n = 0;
    Buf text;
    buf_init(&text);
    uint64_t num = 0;
    int has_num = 0, overflow = 0;
    size_t i = 0;
    while (i < t.n) {
        unsigned char ch = (unsigned char)t.p[i];
        if (ch >= '0' && ch <= '9') {
            i++;
            if (num > (UINT64_MAX - (ch - '0')) / 10) overflow = 1;
            else num = num * 10 + (ch - '0');
            has_num = 1;
            continue;
        }
        if (has_num) {
            v[n].text = arena_str(a, text.p, text.len);
            v[n].num = overflow ? UINT64_MAX : num;
            if (!v[n].text.p) break;
            n++;
            buf_clear(&text);
            num = 0, has_num = 0, overflow = 0;
        }
        lower_put(&text, utf8_next(t.p, t.n, &i, NULL));
    }
    v[n].text = arena_str(a, text.p, text.len);
    v[n].num = has_num && !overflow ? num : 0; /* parse().unwrap_or(0): empty or too big */
    int bad = text.oom || !v[n].text.p;
    n++;
    buf_free(&text);
    out->v = v, out->n = n;
    return bad ? -1 : 0;
}

static int natural_cmp(const Natural *a, const Natural *b) {
    size_t n = a->n < b->n ? a->n : b->n;
    for (size_t i = 0; i < n; i++) {
        int c = str_cmp(a->v[i].text, b->v[i].text);
        if (c) return c;
        if (a->v[i].num != b->v[i].num) return a->v[i].num < b->v[i].num ? -1 : 1;
    }
    return a->n < b->n ? -1 : a->n > b->n;
}

/* ---- schedule ---- */

typedef struct {
    cc_str storey;
    cc_sched_row row;
    Natural key[2]; /* location, name */
} Pending;

static int pending_cmp(const Pending *a, const Pending *b) {
    int c = natural_cmp(&a->key[0], &b->key[0]);
    return c ? c : natural_cmp(&a->key[1], &b->key[1]);
}

/* slice::sort_by: stable (merge sort on indices). */
static void stable_sort(const Pending *p, size_t *idx, size_t *tmp, size_t n) {
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = pending_cmp(&p[idx[j]], &p[idx[i]]) < 0 ? idx[j++] : idx[i++];
            while (i < mid) tmp[k++] = idx[i++];
            while (j < hi) tmp[k++] = idx[j++];
        }
        memcpy(idx, tmp, n * sizeof *idx);
    }
}

static int field_summed(int f) {
    return f == CC_FIELD_LENGTH || f == CC_FIELD_AREA || f == CC_FIELD_VOLUME || f == CC_FIELD_MASS;
}

static cc_cell text_cell(cc_str s) {
    cc_cell c;
    memset(&c, 0, sizeof c);
    c.kind = s.n ? CC_CELL_TEXT : CC_CELL_EMPTY;
    c.text = s;
    return c;
}
static cc_cell num_cell(double v, int dec) {
    cc_cell c;
    memset(&c, 0, sizeof c);
    c.kind = CC_CELL_NUM;
    c.v = v, c.dec = dec;
    return c;
}

/* totals: sums of the summed fields (in row order), the label in the first other column. */
static cc_cell *totals(Arena *a, const uint8_t *fields, size_t nf, cc_sched_row *const *rows, size_t nrows,
                       cc_str label) {
    cc_cell *out = arena_alloc(a, (nf + 1) * sizeof *out, 8);
    if (!out) return NULL;
    for (size_t k = 0; k < nf; k++) {
        if (!field_summed(fields[k])) {
            out[k].kind = CC_CELL_EMPTY;
            continue;
        }
        double s = -0.0;
        for (size_t r = 0; r < nrows; r++)
            if (rows[r]->cells[k].kind == CC_CELL_NUM) s += rows[r]->cells[k].v;
        s = s + 0.0; /* an empty sum is -0.0: written "0.000" */
        int dec = fields[k] == CC_FIELD_AREA ? 2 : fields[k] == CC_FIELD_MASS ? 0 : 3;
        out[k] = num_cell(s, dec);
    }
    for (size_t k = 0; k < nf; k++)
        if (!field_summed(fields[k])) {
            out[k].kind = CC_CELL_TEXT; /* Cell::Text(label), even when empty */
            out[k].text = label;
            break;
        }
    return out;
}

static int lists(const cc_schedule *d, cc_str cls, cc_category cat) {
    int ok = d->classes.n == 0;
    for (size_t i = 0; i < d->classes.n && !ok; i++) ok = ascii_ieq(cls, d->classes.v[i].p, d->classes.v[i].n);
    return ok && (!d->has_family || d->family == (int)cat);
}

static cc_str label_of(Arena *a, const char *what, cc_str name, size_t count) {
    Buf b;
    buf_init(&b);
    if (what) buf_puts(&b, what);
    else buf_append(&b, name.p, name.n);
    buf_puts(&b, " (");
    buf_u64(&b, count);
    buf_putc(&b, ')');
    cc_str r = b.oom ? (cc_str){NULL, 0} : arena_str(a, b.p, b.len);
    buf_free(&b);
    return r;
}

static int oom(cc_error *err) {
    if (err) {
        err->offset = CC_NO_OFFSET;
        memcpy(err->msg, "out of memory", 14);
    }
    return -1;
}

void cc_sched_table_free(cc_sched_table *t) { arena_free(&t->arena); }

int cc_schedule_table(cc_sched_table *t, const cc_scene *s, const cc_viewfile_meta *mt, const cc_schedule *def,
                      cc_error *err) {
    memset(t, 0, sizeof *t);
    arena_init(&t->arena, 0);
    Arena *a = &t->arena;
    Arena tmp, scratch;
    arena_init(&tmp, 0);
    arena_init(&scratch, 0);
    int rc = -1;
    size_t nf = def->nfields, ne = s->nelements;
    t->name = arena_str(a, def->name.p, def->name.n);
    t->fields = arena_alloc(a, nf + 1, 1);
    if (!t->name.p || !t->fields) goto done;
    if (nf) memcpy(t->fields, def->fields, nf);
    t->nfields = nf;
    int want_loc = 0;
    for (size_t k = 0; k < nf; k++) want_loc |= def->fields[k] == CC_FIELD_LOCATION;
    Axis *axes = NULL;
    Crossing *cross = NULL;
    size_t ncross = 0;
    if (want_loc && crossings(s, mt, &tmp, &axes, &cross, &ncross)) goto done;
    Pending *rows = arena_alloc(&tmp, (ne + 1) * sizeof *rows, 8);
    if (!rows) goto done;
    size_t nrows = 0;
    for (size_t i = 0; i < ne; i++) {
        const cc_scene_element *e = &s->elements[i];
        if (!lists(def, e->class_name, e->category)) continue;
        cc_measures m;
        arena_reset(&scratch);
        int r = cc_measure(s, mt, i, &scratch, &m);
        if (r < 0) goto done;
        if (r == 0) continue;
        cc_str location = {"", 0};
        if (ncross) {
            double bestd = 0;
            const Crossing *best = NULL;
            for (size_t c = 0; c < ncross; c++) {
                double d = cc_hypot(cross[c].p[0] - m.centre[0], cross[c].p[1] - m.centre[1]);
                if (!(d <= 1500.0)) continue;
                if (!best || total_cmp(d, bestd) < 0) best = &cross[c], bestd = d;
            }
            if (best) {
                Buf b;
                buf_init(&b);
                buf_append(&b, axes[best->x].tag.p, axes[best->x].tag.n);
                buf_putc(&b, '-');
                buf_append(&b, axes[best->y].tag.p, axes[best->y].tag.n);
                location = b.oom ? (cc_str){NULL, 0} : arena_str(a, b.p, b.len);
                buf_free(&b);
                if (!location.p) goto done;
            }
        }
        const cc_view_info *info = i < mt->ninfo ? &mt->info[i] : NULL;
        cc_str name = info ? info->name : cs(""), storey = info ? info->storey : cs("");
        cc_str material = info ? info->material : cs(""), gid = info ? info->global_id : cs("");
        double rho = 0;
        int has_rho = cc_density(mt, material, &rho);
        cc_str section = arena_str(a, m.section.p, m.section.n);
        cc_cell *cells = arena_alloc(a, (nf + 1) * sizeof *cells, 8);
        if (!cells || !section.p) goto done;
        for (size_t k = 0; k < nf; k++) {
            switch (def->fields[k]) {
            case CC_FIELD_LEVEL: cells[k] = text_cell(storey); break;
            case CC_FIELD_LOCATION: cells[k] = text_cell(location); break;
            case CC_FIELD_NAME: cells[k] = text_cell(name); break;
            case CC_FIELD_CLASS: cells[k] = text_cell(e->class_name); break;
            case CC_FIELD_MATERIAL: cells[k] = text_cell(material); break;
            case CC_FIELD_SECTION: cells[k] = text_cell(section); break;
            case CC_FIELD_LENGTH: cells[k] = num_cell(m.length / 1000.0, 3); break;
            case CC_FIELD_WIDTH: cells[k] = num_cell(m.width / 10.0, 1); break;
            case CC_FIELD_HEIGHT: cells[k] = num_cell(m.height / 10.0, 1); break;
            case CC_FIELD_AREA: cells[k] = num_cell(m.area_m2, 2); break;
            case CC_FIELD_VOLUME: cells[k] = num_cell(m.volume_m3, 3); break;
            case CC_FIELD_MASS:
                cells[k] = has_rho ? num_cell(m.volume_m3 * rho, 0) : text_cell(cs(""));
                break;
            case CC_FIELD_GLOBAL_ID: cells[k] = text_cell(gid); break;
            default: cells[k] = text_cell(cs("")); break;
            }
        }
        Pending *p = &rows[nrows++];
        p->storey = storey;
        p->row.element = i;
        p->row.cells = cells;
        if (natural(&tmp, location, &p->key[0]) || natural(&tmp, name, &p->key[1])) goto done;
    }
    t->count = nrows;
    size_t *idx = arena_alloc(&tmp, (nrows + 1) * sizeof *idx, 8), *ix2 = arena_alloc(&tmp, (nrows + 1) * sizeof *ix2, 8);
    cc_sched_row **all = arena_alloc(&tmp, (nrows + 1) * sizeof *all, 8);
    if (!idx || !ix2 || !all) goto done;
    for (size_t i = 0; i < nrows; i++) idx[i] = i;
    stable_sort(rows, idx, ix2, nrows);
    size_t nall = 0;
    if (def->group_by_level) {
        /* Levels lowest first (their place in the scene's storeys, by name), then by name;
         * elements on no known storey last. */
        size_t *order = arena_alloc(&tmp, (nrows + 1) * sizeof *order, 8);
        size_t *lv = arena_alloc(&tmp, (nrows + 1) * sizeof *lv, 8); /* a row index per distinct level */
        t->groups = arena_alloc(a, (nrows + 1) * sizeof *t->groups, 8);
        if (!order || !lv || !t->groups) goto done;
        for (size_t i = 0; i < nrows; i++) {
            order[i] = SIZE_MAX;
            for (size_t k = 0; k < mt->nstoreys; k++)
                if (str_eq(mt->storeys[k].name, rows[i].storey)) {
                    order[i] = k;
                    break;
                }
        }
        size_t nlv = 0;
        for (size_t i = 0; i < nrows; i++) {
            /* insertion into the sorted distinct list */
            size_t at = nlv;
            int dup = 0;
            for (size_t k = 0; k < nlv; k++) {
                size_t j = lv[k];
                int c = order[i] < order[j] ? -1 : order[i] > order[j];
                if (!c) c = str_cmp(rows[i].storey, rows[j].storey);
                if (c == 0) {
                    dup = 1;
                    break;
                }
                if (c < 0) {
                    at = k;
                    break;
                }
            }
            if (dup) continue;
            memmove(lv + at + 1, lv + at, (nlv - at) * sizeof *lv);
            lv[at] = i;
            nlv++;
        }
        for (size_t g = 0; g < nlv; g++) {
            cc_str l = rows[lv[g]].storey;
            cc_sched_group *G = &t->groups[t->ngroups++];
            G->level = arena_str(a, l.p, l.n);
            size_t cnt = 0;
            for (size_t i = 0; i < nrows; i++) cnt += str_eq(rows[idx[i]].storey, l);
            G->rows = arena_alloc(a, (cnt + 1) * sizeof *G->rows, 8);
            if (!G->level.p || !G->rows) goto done;
            for (size_t i = 0; i < nrows; i++)
                if (str_eq(rows[idx[i]].storey, l)) G->rows[G->nrows++] = rows[idx[i]].row;
            cc_sched_row **rp = arena_alloc(&tmp, (cnt + 1) * sizeof *rp, 8);
            if (!rp) goto done;
            for (size_t i = 0; i < cnt; i++) rp[i] = &G->rows[i], all[nall++] = &G->rows[i];
            cc_str label = label_of(a, l.n ? NULL : "No level", l, cnt);
            if (!label.p) goto done;
            G->subtotal = totals(a, def->fields, nf, rp, cnt, label);
            if (!G->subtotal) goto done;
        }
    } else {
        t->groups = arena_alloc(a, sizeof *t->groups, 8);
        if (!t->groups) goto done;
        cc_sched_group *G = &t->groups[t->ngroups++];
        G->level = cs("");
        G->rows = arena_alloc(a, (nrows + 1) * sizeof *G->rows, 8);
        if (!G->rows) goto done;
        for (size_t i = 0; i < nrows; i++) G->rows[G->nrows++] = rows[idx[i]].row, all[nall++] = &G->rows[i];
    }
    if (def->totals) {
        cc_str label = label_of(a, "Total", cs(""), nrows);
        if (!label.p) goto done;
        t->total = totals(a, def->fields, nf, all, nall, label);
        if (!t->total) goto done;
    }
    rc = 0;
done:
    arena_free(&tmp);
    arena_free(&scratch);
    if (rc) oom(err);
    return rc;
}

/* ---- writers ---- */

void cc_cell_text(Buf *b, const cc_cell *c) {
    if (c->kind == CC_CELL_TEXT) buf_append(b, c->text.p, c->text.n);
    else if (c->kind == CC_CELL_NUM) cc_fmt_fixed(b, c->v, c->dec);
}

static const char *const TITLE[CC_FIELD_COUNT] = {
    "Level", "Location", "Name", "IFC class", "Material", "Section", "Length (m)", "Width (cm)", "Height (cm)",
    "Area (m\xc2\xb2)", "Volume (m\xc2\xb3)", "Mass (kg)", "GlobalId",
};

const char *cc_sched_field_title(int f) { return (unsigned)f < CC_FIELD_COUNT ? TITLE[f] : "?"; }

static void csv_cell(Buf *out, const char *s, size_t n) {
    int q = 0;
    for (size_t i = 0; i < n && !q; i++) q = s[i] == ',' || s[i] == '"' || s[i] == '\n';
    if (!q) {
        buf_append(out, s, n);
        return;
    }
    buf_putc(out, '"');
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '"') buf_putc(out, '"');
        buf_putc(out, s[i]);
    }
    buf_putc(out, '"');
}

static void csv_line(Buf *out, const cc_cell *cells, size_t n) {
    Buf t;
    buf_init(&t);
    for (size_t k = 0; k < n; k++) {
        if (k) buf_putc(out, ',');
        buf_clear(&t);
        cc_cell_text(&t, &cells[k]);
        if (t.oom) out->oom = 1;
        csv_cell(out, t.p, t.len);
    }
    buf_free(&t);
    buf_putc(out, '\n');
}

void cc_schedule_csv(Buf *out, const cc_sched_table *t) {
    for (size_t k = 0; k < t->nfields; k++) {
        if (k) buf_putc(out, ',');
        const char *s = cc_sched_field_title(t->fields[k]);
        csv_cell(out, s, strlen(s));
    }
    buf_putc(out, '\n');
    for (size_t g = 0; g < t->ngroups; g++) {
        const cc_sched_group *G = &t->groups[g];
        for (size_t r = 0; r < G->nrows; r++) csv_line(out, G->rows[r].cells, t->nfields);
        if (G->subtotal && t->nfields) csv_line(out, G->subtotal, t->nfields);
    }
    if (t->total) csv_line(out, t->total, t->nfields);
}

static void json_cells(JsonW *w, Buf *tmp, const cc_cell *cells, size_t n) {
    jw_arr_begin(w);
    for (size_t k = 0; cells && k < n; k++) {
        buf_clear(tmp);
        cc_cell_text(tmp, &cells[k]);
        jw_str_n(w, tmp->p ? tmp->p : "", tmp->len);
    }
    jw_arr_end(w);
}

void cc_schedule_json(Buf *out, const cc_sched_table *t, const cc_viewfile_meta *mt, size_t max) {
    JsonW w;
    Buf tmp;
    buf_init(&tmp);
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "columns");
    jw_arr_begin(&w);
    for (size_t k = 0; k < t->nfields; k++) jw_str(&w, cc_sched_field_title(t->fields[k]));
    jw_arr_end(&w);
    jw_key(&w, "count");
    jw_u64(&w, t->count);
    jw_key(&w, "groups");
    jw_arr_begin(&w);
    size_t left = max;
    for (size_t g = 0; g < t->ngroups; g++) {
        const cc_sched_group *G = &t->groups[g];
        size_t n = G->nrows < left ? G->nrows : left;
        left -= n;
        jw_obj_begin(&w);
        jw_key(&w, "ids");
        jw_arr_begin(&w);
        for (size_t r = 0; r < n; r++) {
            size_t e = G->rows[r].element;
            if (e < mt->ninfo) jw_str_n(&w, mt->info[e].global_id.p, mt->info[e].global_id.n);
            else jw_null(&w);
        }
        jw_arr_end(&w);
        jw_key(&w, "level");
        jw_str_n(&w, G->level.p, G->level.n);
        jw_key(&w, "rows");
        jw_arr_begin(&w);
        for (size_t r = 0; r < n; r++) json_cells(&w, &tmp, G->rows[r].cells, t->nfields);
        jw_arr_end(&w);
        jw_key(&w, "subtotal");
        json_cells(&w, &tmp, G->subtotal, G->subtotal ? t->nfields : 0);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "name");
    jw_str_n(&w, t->name.p, t->name.n);
    jw_key(&w, "total");
    if (t->total) json_cells(&w, &tmp, t->total, t->nfields);
    else jw_null(&w);
    jw_key(&w, "truncated");
    jw_bool(&w, t->count > max);
    jw_obj_end(&w);
    if (jw_finish(&w) != JW_OK || tmp.oom) out->oom = 1;
    buf_free(&tmp);
}

/* ---- definitions (schedule.rs default_schedules, effective) ---- */

static cc_str CLS_COLUMN[] = {{"IfcColumn", 9}};
static cc_str CLS_BEAM[] = {{"IfcBeam", 7}};
static cc_str CLS_SLAB[] = {{"IfcSlab", 7}};
static cc_str CLS_WALL[] = {{"IfcWall", 7}, {"IfcWallStandardCase", 19}};
static cc_str CLS_FOOTING[] = {{"IfcFooting", 10}};
enum {
    F_LEVEL = CC_FIELD_LEVEL, F_LOC = CC_FIELD_LOCATION, F_NAME = CC_FIELD_NAME, F_CLASS = CC_FIELD_CLASS,
    F_MAT = CC_FIELD_MATERIAL, F_SEC = CC_FIELD_SECTION, F_LEN = CC_FIELD_LENGTH, F_WID = CC_FIELD_WIDTH,
    F_HEI = CC_FIELD_HEIGHT, F_AREA = CC_FIELD_AREA, F_VOL = CC_FIELD_VOLUME, F_MASS = CC_FIELD_MASS
};
static uint8_t FD_COL[] = {F_LEVEL, F_LOC, F_NAME, F_MAT, F_SEC, F_LEN, F_VOL};
static uint8_t FD_BEAM[] = {F_LEVEL, F_NAME, F_MAT, F_SEC, F_LEN, F_WID, F_HEI, F_VOL};
static uint8_t FD_STEEL_COL[] = {F_LEVEL, F_LOC, F_NAME, F_MAT, F_SEC, F_LEN, F_VOL, F_MASS};
static uint8_t FD_STEEL_BEAM[] = {F_LEVEL, F_NAME, F_MAT, F_SEC, F_LEN, F_WID, F_HEI, F_VOL, F_MASS};
static uint8_t FD_SLAB[] = {F_LEVEL, F_NAME, F_MAT, F_HEI, F_AREA, F_VOL};
static uint8_t FD_WALL[] = {F_LEVEL, F_NAME, F_MAT, F_WID, F_LEN, F_HEI, F_AREA, F_VOL};
static uint8_t FD_FOOTING[] = {F_LEVEL, F_LOC, F_NAME, F_MAT, F_SEC, F_HEI, F_VOL};
static uint8_t FD_ALL[] = {F_LEVEL, F_NAME, F_CLASS, F_MAT, F_VOL, F_MASS};
#define DEF(nm, cls, fam, fd)                                                                                    \
    {{nm, sizeof nm - 1}, {cls, sizeof cls / sizeof cls[0]}, fam >= 0, fam >= 0 ? fam : 0, fd, sizeof fd, 1, 1}
#define DEF0(nm, fam, fd) {{nm, sizeof nm - 1}, {NULL, 0}, fam >= 0, fam >= 0 ? fam : 0, fd, sizeof fd, 1, 1}
static const cc_schedule BUILTIN[] = {
    DEF("Concrete columns", CLS_COLUMN, CC_CAT_CONCRETE, FD_COL),
    DEF("Steel columns", CLS_COLUMN, CC_CAT_STEEL, FD_STEEL_COL),
    DEF("Timber columns", CLS_COLUMN, CC_CAT_TIMBER, FD_COL),
    DEF("Concrete beams", CLS_BEAM, CC_CAT_CONCRETE, FD_BEAM),
    DEF("Steel beams", CLS_BEAM, CC_CAT_STEEL, FD_STEEL_BEAM),
    DEF("Timber beams", CLS_BEAM, CC_CAT_TIMBER, FD_BEAM),
    DEF("Slabs", CLS_SLAB, -1, FD_SLAB),
    DEF("Walls", CLS_WALL, -1, FD_WALL),
    DEF("Footings", CLS_FOOTING, -1, FD_FOOTING),
    DEF0("All elements", -1, FD_ALL),
};
#undef DEF
#undef DEF0

const cc_schedule *cc_schedule_builtins(size_t *n) {
    *n = sizeof BUILTIN / sizeof BUILTIN[0];
    return BUILTIN;
}

size_t cc_schedules_effective(const cc_schedule *stored, size_t nstored, const cc_schedule **out, size_t cap) {
    size_t nb = sizeof BUILTIN / sizeof BUILTIN[0], n = 0;
    for (size_t i = 0; i < nb; i++) {
        const cc_schedule *d = &BUILTIN[i];
        for (size_t k = 0; k < nstored; k++)
            if (ascii_ieq(stored[k].name, d->name.p, d->name.n)) {
                d = &stored[k];
                break;
            }
        if (n < cap) out[n] = d;
        n++;
    }
    for (size_t k = 0; k < nstored; k++) {
        /* in the list already: a built-in name (replaced or not), or an earlier stored one */
        int seen = 0;
        for (size_t i = 0; i < nb && !seen; i++) seen = ascii_ieq(BUILTIN[i].name, stored[k].name.p, stored[k].name.n);
        for (size_t i = 0; i < k && !seen; i++) seen = ascii_ieq(stored[i].name, stored[k].name.p, stored[k].name.n);
        if (seen) continue;
        if (n < cap) out[n] = &stored[k];
        n++;
    }
    return n;
}

/* ---- WAVE1B (B5): the foundations list (quantity_rows.h) ---- */
#include "quantity_rows.h"
int cc_foundation_rows_json(const cc_model *m, const cc_scene *sc, JsonW *w) {
    cc_viewfile_meta mt;
    if (cc_viewfile_meta_from_model(&mt, m, sc)) return -1;
    int rc = 0;
    jw_arr_begin(w);
    for (size_t k = 0; k < sc->nelements && !rc; k++) {
        size_t mi = sc->elements[k].model_index;
        if (mi >= m->nelements) continue;
        const cc_element *e = &m->elements[mi];
        const cc_str *pd = cc_element_predefined(e);
        int raft = e->cls == CC_IFC_SLAB && pd && pd->n == 8 && !memcmp(pd->p, "BASESLAB", 8);
        if (e->cls != CC_IFC_FOOTING && !raft) continue;
        const char *kind = raft ? "raft"
                           : e->generator.kind == CC_GEN_PAD_FOOTING ? "pad"
                           : (pd && pd->n == 13 && !memcmp(pd->p, "STRIP_FOOTING", 13)) || e->generator.kind == CC_GEN_WALL ? "strip"
                                                                                                                         : "footing";
        Arena a;
        arena_init(&a, 0);
        cc_measures q;
        int r = cc_measure(sc, &mt, k, &a, &q);
        if (r < 0) rc = -1;
        if (r == 1) {
            const cc_storey *st = cc_model_storey(m, &e->storey);
            jw_obj_begin(w);
            jw_key(w, "id"), jw_str_n(w, e->id.c, 22);
            jw_key(w, "kind"), jw_str(w, kind);
            jw_key(w, "level"), jw_str_n(w, st && st->name.n ? st->name.p : "", st ? st->name.n : 0);
            jw_key(w, "name"), jw_str_n(w, e->name.n ? e->name.p : "", e->name.n);
            jw_key(w, "section"), jw_str_n(w, q.section.n ? q.section.p : "", q.section.n);
            jw_key(w, "volume_m3"), jw_f64(w, q.volume_m3);
            jw_obj_end(w);
        }
        arena_free(&a);
    }
    jw_arr_end(w);
    cc_viewfile_meta_free(&mt);
    return rc;
}
