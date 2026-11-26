/* L0 maths without libm (see math.h): cc_sqrt, cc_sqrtf (from scene.c), cc_hypot (from verbs.c).
 * Floating-point contraction (a*b+c into fma) would change bits: it is off. */
#ifdef __clang__
#pragma clang fp contract(off)
#endif
#include "math.h"
#include <stdint.h>
#include <string.h>

static uint64_t bits64(double x) { uint64_t u; memcpy(&u, &x, 8); return u; }
static double from64(uint64_t u) { double x; memcpy(&x, &u, 8); return x; }

/* Correctly rounded square root: digit by digit on the 108-bit radicand m << 54. IEEE 754 sqrt
 * is correctly rounded too, so with clang or GCC on aarch64 and x86-64 the instruction gives the
 * same bits, about 20 times faster (ticket IFC2: a third of a rebar shape's read; ifc_in_test
 * compares the two on 200k random values, the digit version runs under tcc and wasm). */
static double sqrt_digits(double x);
double cc_sqrt(double x) {
#if (defined(__clang__) || (defined(__GNUC__) && !defined(__TINYC__))) && defined(__aarch64__)
    if (x >= 0.0) {
        double r;
        __asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(x));
        return r;
    }
#elif (defined(__clang__) || (defined(__GNUC__) && !defined(__TINYC__))) && defined(__x86_64__)
    if (x >= 0.0) {
        double r;
        __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
        return r;
    }
#endif
    return sqrt_digits(x);
}
static double sqrt_digits(double x) {
    uint64_t u = bits64(x);
    if (x != x) return x;
    if (x == 0.0 || x == 1.0 / 0.0) return x; /* ±0, +inf */
    if (u >> 63) return 0.0 / 0.0;
    int e = (int)(u >> 52);
    uint64_t m = u & ((1ull << 52) - 1);
    if (e == 0) { /* subnormal */
        e = 1;
        while (!(m & (1ull << 52))) m <<= 1, e--;
    } else {
        m |= 1ull << 52;
    }
    int ex = e - 1075; /* x = m * 2^ex */
    if (ex & 1) m <<= 1, ex--;
    uint64_t hi = m >> 10, lo = m << 54, root = 0, rem = 0;
    for (int i = 53; i >= 0; i--) {
        int b = 2 * i;
        uint64_t pair = b >= 64 ? (hi >> (b - 64)) & 3 : (lo >> b) & 3;
        rem = (rem << 2) | pair;
        uint64_t trial = (root << 2) | 1;
        if (rem >= trial) {
            rem -= trial;
            root = (root << 1) | 1;
        } else {
            root <<= 1;
        }
    }
    /* root in [2^53, 2^54): 53 bits and a guard bit; rem != 0 is the sticky bit. */
    uint64_t mant = root >> 1;
    if ((root & 1) && (rem != 0 || (mant & 1))) mant++;
    int exp = ex / 2 - 26;
    if (mant == (1ull << 53)) mant >>= 1, exp++;
    return from64((uint64_t)(exp + 52 + 1023) << 52 | (mant & ((1ull << 52) - 1)));
}

/* Rounding the exact f64 root to f32 is correctly rounded (53 >= 2 * 24 + 2). */
float cc_sqrtf(float x) { return (float)cc_sqrt((double)x); }

/* ---- f64 maths without libm ----
 * Neither test/run.sh nor build.sh links libm, and freestanding wasm has none. cc_sqrt is exact
 * (IEEE sqrt is correctly rounded, so any correct method gives the same bits). cc_hypot is
 * glibc's (sysdeps/ieee754/dbl-64/e_hypot.c, glibc 2.35+) with its FMA kernel, the one glibc
 * uses where FMA is native (aarch64, where the goldens were made); the FMA is emulated in
 * software (exact product, then round-to-odd, Boldo and Melquiond 2008), so every compiler and
 * wasm give the same bits. Checked natively against glibc 2.43 on aarch64: 0 differences in
 * 2x10^7 random mm pairs; 1 in 2x10^7 random bit patterns, near 2^-511 where the exact product
 * underflows. An x86_64 glibc without FMA uses another kernel
 * and may differ from Rust there in the last bit. Ticket 13 (wasm) must check it bit-exact.
 * No contraction: a*b+c must round twice, as in Rust (tcc never contracts). */
#ifndef __TINYC__
#pragma STDC FP_CONTRACT OFF
#endif

static uint64_t bits_of(double x) {
    uint64_t u;
    memcpy(&u, &x, 8);
    return u;
}
static double of_bits(uint64_t u) {
    double x;
    memcpy(&x, &u, 8);
    return x;
}

/* a*b = p + *e exactly (Veltkamp split, Dekker product; no overflow in hypot's range). */
static double two_prod(double a, double b, double *e) {
    double p = a * b;
    double ca = 134217729.0 * a, cb = 134217729.0 * b;
    double ah = ca - (ca - a), al = a - ah, bh = cb - (cb - b), bl = b - bh;
    *e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    return p;
}

/* a + b = s + *e exactly (Knuth). */
static double two_sum(double a, double b, double *e) {
    double s = a + b, bb = s - a;
    *e = (a - (s - bb)) + (b - bb);
    return s;
}

/* a + b rounded to odd. */
static double add_odd(double a, double b) {
    double e, s = two_sum(a, b, &e);
    uint64_t u = bits_of(s);
    if (e != 0.0 && !(u & 1)) u += (e > 0) == (s > 0) ? 1 : (uint64_t)-1;
    return of_bits(u);
}

/* fma(a, b, c), correctly rounded. */
static double fma_(double a, double b, double c) {
    double ul, uh = two_prod(a, b, &ul);
    double tl, th = two_sum(c, uh, &tl);
    return th + add_odd(tl, ul);
}

static double hypot_kernel(double ax, double ay) {
    double t1 = ay + ay, t2 = ax - ay;
    if (t1 >= ax) return cc_sqrt(fma_(t1, ax, t2 * t2));
    return cc_sqrt(fma_(ax, ax, ay * ay));
}

double cc_hypot(double x, double y) {
    const double SCALE = 0x1p-600, LARGE_VAL = 0x1p+511, TINY_VAL = 0x1p-511, EPS = 0x1p-54;
    int xinf = x - x != 0.0 && x == x, yinf = y - y != 0.0 && y == y;
    if (x - x != 0.0 || y - y != 0.0) { /* not finite */
        if (xinf || yinf) return of_bits(0x7FF0000000000000ull);
        return x + y;
    }
    x = x < 0 ? -x : x;
    y = y < 0 ? -y : y;
    double ax = x < y ? y : x, ay = x < y ? x : y;
    if (ax > LARGE_VAL) {
        if (ay <= ax * EPS) return ax + ay;
        return hypot_kernel(ax * SCALE, ay * SCALE) / SCALE;
    }
    if (ay < TINY_VAL) {
        if (ax >= ay / EPS) return ax + ay;
        return hypot_kernel(ax / SCALE, ay / SCALE) * SCALE;
    }
    if (ay <= ax * EPS) return ax + ay;
    return hypot_kernel(ax, ay);
}

