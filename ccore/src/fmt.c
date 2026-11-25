#include "fmt.h"
#include <string.h>

/* Ryu's d2s.c, included here to reach its static d2d() and d2d_small_int(). */
#include "ryu/d2s.c"

/* Shortest digits of a finite nonzero |x|: value = digits * 10^exp, no trailing zeros. */
typedef struct {
    char d[17];
    int n;   /* number of digits, 1..17 */
    int exp; /* decimal exponent of the last digit */
} Digits;

/* a * 5^k without overflow; 0 on overflow. */
static uint64_t mul_pow5(uint64_t a, int k) {
    while (k-- > 0) {
        if (a > UINT64_MAX / 5) return 0;
        a *= 5;
    }
    return a;
}

/* True if x = m2 * 2^e2 is exactly (m + 1/2) * 10^e, the midpoint between two shortest
 * candidates. Both sides as odd * 5^i * 2^j: (2m+1) * 5^e * 2^(e-1) = m2' * 2^e2' (m2' odd). */
static int is_tie_above(uint64_t m2, int e2, uint64_t m, int e) {
    while (!(m2 & 1)) {
        m2 >>= 1;
        e2++;
    }
    if (e2 != e - 1 || m >= (1ull << 62)) return 0;
    uint64_t lhs = mul_pow5(m2, -e), rhs = mul_pow5(2 * m + 1, e);
    return lhs && lhs == rhs;
}

/* ties_up: Ryu breaks an exact tie to the even digit, as serde_json (zmij) does. Rust's
 * {:?} (core::fmt, Grisu with Dragon fallback) rounds a tie up, e.g. 1000000000000000.25
 * prints as 1000000000000000.3. */
static void shortest(uint64_t bits, Digits *out, int ties_up) {
    const uint64_t mant = bits & ((1ull << DOUBLE_MANTISSA_BITS) - 1);
    const uint32_t ex = (uint32_t)((bits >> DOUBLE_MANTISSA_BITS) & ((1u << DOUBLE_EXPONENT_BITS) - 1));
    floating_decimal_64 v;
    if (!d2d_small_int(mant, ex, &v)) {
        v = d2d(mant, ex);
        if (ties_up) {
            uint64_t m2 = ex ? mant | (1ull << DOUBLE_MANTISSA_BITS) : mant;
            int e2 = (ex ? (int)ex : 1) - DOUBLE_BIAS - DOUBLE_MANTISSA_BITS;
            if (is_tie_above(m2, e2, v.mantissa, v.exponent)) v.mantissa++;
        }
    }
    uint64_t m = v.mantissa;
    int e = v.exponent;
    while (m % 10 == 0) { /* small ints keep trailing zeros; d2d may too */
        m /= 10;
        e++;
    }
    char t[20];
    int n = 0;
    while (m) {
        t[n++] = (char)('0' + m % 10);
        m /= 10;
    }
    for (int i = 0; i < n; i++) out->d[i] = t[n - 1 - i];
    out->n = n;
    out->exp = e;
}

/* Returns 1 if x is finite and zero, 0 if finite nonzero, -1 if not finite. Appends the sign. */
static int prologue(Buf *b, double x, uint64_t *bits) {
    uint64_t u;
    memcpy(&u, &x, 8);
    if (((u >> 52) & 0x7ff) == 0x7ff) return -1;
    if (u >> 63) buf_putc(b, '-');
    *bits = u & ~(1ull << 63);
    return *bits == 0;
}

/* Plain decimal: digits with the point placed by kk = n + exp; ".0" for integers. */
static void plain(Buf *b, const Digits *g) {
    int kk = g->n + g->exp;
    if (g->exp >= 0) {
        buf_append(b, g->d, (size_t)g->n);
        buf_fill(b, '0', (size_t)g->exp);
        buf_append(b, ".0", 2);
    } else if (kk > 0) {
        buf_append(b, g->d, (size_t)kk);
        buf_putc(b, '.');
        buf_append(b, g->d + kk, (size_t)(g->n - kk));
    } else {
        buf_append(b, "0.", 2);
        buf_fill(b, '0', (size_t)-kk);
        buf_append(b, g->d, (size_t)g->n);
    }
}

/* d[.ddd]e[sign]exp; `plus` writes '+' for exponents >= 0. */
static void sci(Buf *b, const Digits *g, int plus) {
    buf_putc(b, g->d[0]);
    if (g->n > 1) {
        buf_putc(b, '.');
        buf_append(b, g->d + 1, (size_t)(g->n - 1));
    }
    buf_putc(b, 'e');
    int e = g->n + g->exp - 1;
    if (e < 0) buf_putc(b, '-');
    else if (plus) buf_putc(b, '+');
    buf_i64(b, e < 0 ? -e : e);
}

int fmt_f64_json(Buf *b, double x) {
    uint64_t bits;
    int z = prologue(b, x, &bits);
    if (z < 0) return CCORE_ERR_NONFINITE;
    if (z) {
        buf_append(b, "0.0", 3);
        return 0;
    }
    Digits g;
    shortest(bits, &g, 0);
    /* serde_json (zmij, same layout as the ryu crate): plain while the point position
     * kk = n + exp is in -5 < kk <= 16, else scientific with an explicit '+'. */
    int kk = g.n + g.exp;
    if (kk > -5 && kk <= 16) plain(b, &g);
    else sci(b, &g, 1);
    return 0;
}

int fmt_f64_debug(Buf *b, double x) {
    uint64_t bits;
    int z = prologue(b, x, &bits);
    if (z < 0) return CCORE_ERR_NONFINITE;
    if (z) {
        buf_append(b, "0.0", 3);
        return 0;
    }
    Digits g;
    shortest(bits, &g, 1);
    /* Rust core::fmt Debug for f64: scientific when |x| < 1e-4 or |x| >= 1e16, compared on the
     * value. Positive doubles order like their bits; the bits avoid any compiler strtod. */
    const uint64_t ONE_E_MINUS_4 = 0x3f1a36e2eb1c432dull, ONE_E_16 = 0x4341c37937e08000ull;
    if (bits < ONE_E_MINUS_4 || bits >= ONE_E_16) sci(b, &g, 0);
    else plain(b, &g);
    return 0;
}
