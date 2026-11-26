/* Elementary functions in double-double, rounded once. See ifc_math.h. */
#include "ifc_math.h"
#include <stdint.h>
#include <string.h>
#include "scene.h" /* cc_sqrt */

#ifndef __TINYC__ /* tcc never contracts and warns on the pragma */
#pragma STDC FP_CONTRACT OFF
#endif
#ifdef __clang__
#pragma clang fp contract(off)
#endif

typedef struct {
    double hi, lo;
} DD;

static const double PI_D = 3.141592653589793;
static const double PIO2_D = 1.5707963267948966;
/* pi/2 = P1 + P2 + P3 (each the double nearest the remainder) */
static const double P1 = 1.5707963267948966, P2 = 6.123233995736766e-17, P3 = -1.4973849048591698e-33;

static int is_nan(double x) { return x != x; }
static int is_inf(double x) { return x == x && x - x != 0; }
static double fabs_(double x) { return x < 0 ? -x : x; }
static double copysign_(double m, double s) {
    uint64_t a, b;
    memcpy(&a, &m, 8);
    memcpy(&b, &s, 8);
    a = (a & ~(1ull << 63)) | (b & (1ull << 63));
    memcpy(&m, &a, 8);
    return m;
}

static DD two_sum(double a, double b) {
    double s = a + b, bb = s - a;
    DD r = {s, (a - (s - bb)) + (b - bb)};
    return r;
}
static DD quick_two_sum(double a, double b) {
    double s = a + b;
    DD r = {s, b - (s - a)};
    return r;
}
static void split(double a, double *hi, double *lo) {
    double c = 134217729.0 * a;
    *hi = c - (c - a);
    *lo = a - *hi;
}
static DD two_prod(double a, double b) {
    double p = a * b, ah, al, bh, bl;
    split(a, &ah, &al);
    split(b, &bh, &bl);
    DD r = {p, ((ah * bh - p) + ah * bl + al * bh) + al * bl};
    return r;
}
static DD dd_add(DD a, DD b) {
    DD s = two_sum(a.hi, b.hi), t = two_sum(a.lo, b.lo);
    s.lo += t.hi;
    s = quick_two_sum(s.hi, s.lo);
    s.lo += t.lo;
    return quick_two_sum(s.hi, s.lo);
}
static DD dd_neg(DD a) {
    DD r = {-a.hi, -a.lo};
    return r;
}
static DD dd_sub(DD a, DD b) { return dd_add(a, dd_neg(b)); }
static DD dd_mul(DD a, DD b) {
    DD p = two_prod(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return quick_two_sum(p.hi, p.lo);
}
static DD dd_mul_d(DD a, double b) {
    DD p = two_prod(a.hi, b);
    p.lo += a.lo * b;
    return quick_two_sum(p.hi, p.lo);
}
static DD dd_div(DD a, DD b) {
    double q1 = a.hi / b.hi;
    DD r = dd_sub(a, dd_mul_d(b, q1));
    double q2 = r.hi / b.hi;
    r = dd_sub(r, dd_mul_d(b, q2));
    double q3 = r.hi / b.hi;
    DD q = quick_two_sum(q1, q2);
    DD t = {q3, 0.0};
    return dd_add(q, t);
}
static DD dd(double x) {
    DD r = {x, 0.0};
    return r;
}
static double dd_round(DD a) { return a.hi + a.lo; }

/* sin and cos of a dd argument; quadrant reduction by pi/2 in three parts. */
static void dd_sincos(DD x, DD *s, DD *c) {
    double kd = x.hi * 0.6366197723675814;
    kd = kd < 0 ? -(double)(int64_t)(-kd + 0.5) : (double)(int64_t)(kd + 0.5);
    DD r = dd_sub(x, two_prod(kd, P1));
    r = dd_sub(r, two_prod(kd, P2));
    r = dd_sub(r, two_prod(kd, P3));
    DD r2 = dd_mul(r, r);
    /* sin r = r - r^3/3! + ..., cos r = 1 - r^2/2! + ... (|r| <= pi/4: 30 terms are plenty) */
    DD ss = r, term = r;
    for (int i = 3; i <= 41; i += 2) {
        term = dd_div(dd_mul(term, r2), dd(-(double)(i * (i - 1))));
        ss = dd_add(ss, term);
        if (fabs_(term.hi) < 1e-40 * fabs_(ss.hi)) break;
    }
    DD cc = dd(1.0);
    term = dd(1.0);
    for (int i = 2; i <= 40; i += 2) {
        term = dd_div(dd_mul(term, r2), dd(-(double)(i * (i - 1))));
        cc = dd_add(cc, term);
        if (fabs_(term.hi) < 1e-40) break;
    }
    int64_t k = (int64_t)kd;
    switch ((int)(k & 3)) {
    case 0: *s = ss, *c = cc; break;
    case 1: *s = cc, *c = dd_neg(ss); break;
    case 2: *s = dd_neg(ss), *c = dd_neg(cc); break;
    default: *s = dd_neg(cc), *c = ss; break;
    }
}

double cc_sin(double x) {
    if (is_nan(x) || is_inf(x)) return x - x;
    if (x == 0.0 || fabs_(x) > 1e15) return x == 0.0 ? x : 0.0;
    DD s, c;
    dd_sincos(dd(x), &s, &c);
    return dd_round(s);
}

double cc_cos(double x) {
    if (is_nan(x) || is_inf(x)) return x - x;
    if (x == 0.0) return 1.0;
    if (fabs_(x) > 1e15) return 1.0;
    DD s, c;
    dd_sincos(dd(x), &s, &c);
    return dd_round(c);
}

void cc_sincos(double x, double *s, double *c) {
    if (is_nan(x) || is_inf(x)) {
        *s = *c = x - x;
        return;
    }
    if (x == 0.0 || fabs_(x) > 1e15) {
        *s = x == 0.0 ? x : 0.0;
        *c = 1.0;
        return;
    }
    DD ds, dc;
    dd_sincos(dd(x), &ds, &dc);
    *s = dd_round(ds);
    *c = dd_round(dc);
}

/* A plain double atan, a few ulp: the start of the Newton steps. */
static double atan_rough(double t) {
    int neg = t < 0;
    if (neg) t = -t;
    int inv = t > 1.0;
    if (inv) t = 1.0 / t;
    t = t / (1.0 + cc_sqrt(1.0 + t * t));
    t = t / (1.0 + cc_sqrt(1.0 + t * t));
    double t2 = t * t, sum = 0.0;
    for (int i = 25; i >= 1; i -= 2) sum = 1.0 / i - t2 * sum;
    double a = 4.0 * t * sum;
    if (inv) a = PIO2_D - a;
    return neg ? -a : a;
}

/* Scale both by the same power of two so the larger is in [1, 2). */
static void scale_pair(double *a, double *b) {
    double m = fabs_(*a) > fabs_(*b) ? fabs_(*a) : fabs_(*b);
    uint64_t bits;
    memcpy(&bits, &m, 8);
    int e = (int)((bits >> 52) & 0x7FF);
    if (e > 2000) *a *= 0x1p-100, *b *= 0x1p-100, m *= 0x1p-100, memcpy(&bits, &m, 8), e = (int)((bits >> 52) & 0x7FF);
    if (e == 0) { /* subnormal: up by 2^600 first */
        double big = 1.0;
        for (int i = 0; i < 12; i++) big *= 1125899906842624.0; /* 2^50 */
        *a *= big, *b *= big;
        m = fabs_(*a) > fabs_(*b) ? fabs_(*a) : fabs_(*b);
        memcpy(&bits, &m, 8);
        e = (int)((bits >> 52) & 0x7FF);
    }
    uint64_t f = (uint64_t)(2046 - e) << 52; /* 2^(1023-e+...) : 2^(-(e-1023)) */
    double s;
    memcpy(&s, &f, 8);
    *a *= s, *b *= s;
}

double cc_atan2(double y, double x) {
    if (is_nan(x) || is_nan(y)) return x + y;
    if (y == 0.0) {
        uint64_t xb;
        memcpy(&xb, &x, 8);
        return (x > 0 || (x == 0 && !(xb >> 63))) ? y : copysign_(PI_D, y);
    }
    if (x == 0.0) return copysign_(PIO2_D, y);
    if (is_inf(x)) {
        if (is_inf(y)) return copysign_(x > 0 ? 0.7853981633974483 : 2.356194490192345, y);
        return x > 0 ? copysign_(0.0, y) : copysign_(PI_D, y);
    }
    if (is_inf(y)) return copysign_(PIO2_D, y);
    scale_pair(&y, &x);
    double q = y / x;
    double t0;
    if (x > 0) {
        if (fabs_(q) < 1e-300) return q; /* tiny angle */
        t0 = atan_rough(q);
    } else t0 = atan_rough(q) + (y > 0 ? PI_D : -PI_D);
    if (is_nan(t0)) t0 = copysign_(PIO2_D, y);
    DD th = dd(t0);
    for (int it = 0; it < 2; it++) {
        DD s, c;
        dd_sincos(th, &s, &c);
        DD num = dd_sub(dd_mul_d(c, y), dd_mul_d(s, x));
        DD den = dd_add(dd_mul_d(c, x), dd_mul_d(s, y));
        th = dd_add(th, dd_div(num, den));
    }
    return dd_round(th);
}

double cc_acos(double x) {
    if (is_nan(x)) return x;
    if (x > 1.0 || x < -1.0) return (x - x) / (x - x);
    if (x == 1.0) return 0.0;
    if (x == -1.0) return PI_D;
    double t0 = cc_atan2(cc_sqrt((1.0 - x) * (1.0 + x)), x);
    DD th = dd(t0);
    for (int it = 0; it < 2; it++) {
        DD s, c;
        dd_sincos(th, &s, &c);
        th = dd_add(th, dd_div(dd_sub(c, dd(x)), s));
    }
    return dd_round(th);
}

double cc_fmod(double x, double y) {
    uint64_t ux, uy, i;
    memcpy(&ux, &x, 8);
    memcpy(&uy, &y, 8);
    int ex = (int)(ux >> 52 & 0x7ff), ey = (int)(uy >> 52 & 0x7ff);
    uint64_t sx = ux >> 63;
    if (uy << 1 == 0 || is_nan(y) || ex == 0x7ff) return (x * y) / (x * y);
    if (ux << 1 <= uy << 1) {
        if (ux << 1 == uy << 1) return 0 * x;
        return x;
    }
    if (!ex) {
        for (i = ux << 12; i >> 63 == 0; ex--, i <<= 1) {}
        ux <<= -ex + 1;
    } else {
        ux &= -1ULL >> 12;
        ux |= 1ULL << 52;
    }
    if (!ey) {
        for (i = uy << 12; i >> 63 == 0; ey--, i <<= 1) {}
        uy <<= -ey + 1;
    } else {
        uy &= -1ULL >> 12;
        uy |= 1ULL << 52;
    }
    for (; ex > ey; ex--) {
        i = ux - uy;
        if (i >> 63 == 0) {
            if (i == 0) return 0 * x;
            ux = i;
        }
        ux <<= 1;
    }
    i = ux - uy;
    if (i >> 63 == 0) {
        if (i == 0) return 0 * x;
        ux = i;
    }
    for (; ux >> 52 == 0; ux <<= 1, ex--) {}
    if (ex > 0) {
        ux -= 1ULL << 52;
        ux |= (uint64_t)ex << 52;
    } else ux >>= -ex + 1;
    ux |= sx << 63;
    double r;
    memcpy(&r, &ux, 8);
    return r;
}
