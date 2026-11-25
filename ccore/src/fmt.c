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

/* ---- fixed point: exact, with a small bignum (32-bit limbs, little end first) ---- */

#define BIG_LIMBS 40 /* 2^1024 * 5^20 < 2^1071 < 2^(32 * 34) */

typedef struct {
    uint32_t w[BIG_LIMBS];
    int n; /* limbs in use; w[n-1] != 0 unless n == 0 */
} Big;

static void big_trim(Big *a) {
    while (a->n > 0 && a->w[a->n - 1] == 0) a->n--;
}

static void big_mul_small(Big *a, uint32_t k) {
    uint64_t carry = 0;
    for (int i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->w[i] * k + carry;
        a->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) a->w[a->n++] = (uint32_t)carry;
}

static void big_shl(Big *a, int s) {
    int limbs = s / 32, bits = s % 32;
    if (a->n == 0) return;
    for (int i = a->n - 1; i >= 0; i--) {
        uint64_t v = (uint64_t)a->w[i] << bits;
        if (i + limbs + 1 < BIG_LIMBS) a->w[i + limbs + 1] |= (uint32_t)(v >> 32);
        a->w[i + limbs] = (uint32_t)v;
    }
    for (int i = 0; i < limbs; i++) a->w[i] = 0;
    a->n += limbs + 1;
    big_trim(a);
}

static int big_bit(const Big *a, int i) {
    int l = i / 32;
    return l < a->n ? (int)(a->w[l] >> (i % 32)) & 1 : 0;
}

/* Any bit below bit i set? */
static int big_any_below(const Big *a, int i) {
    int l = i / 32;
    for (int k = 0; k < l && k < a->n; k++)
        if (a->w[k]) return 1;
    if (l < a->n && (a->w[l] & ((1u << (i % 32)) - 1))) return 1;
    return 0;
}

static void big_shr(Big *a, int s) {
    int limbs = s / 32, bits = s % 32;
    if (limbs >= a->n) {
        a->n = 0;
        return;
    }
    for (int i = 0; i + limbs < a->n; i++) {
        uint64_t v = a->w[i + limbs];
        if (i + limbs + 1 < a->n) v |= (uint64_t)a->w[i + limbs + 1] << 32;
        a->w[i] = (uint32_t)(v >> bits);
    }
    a->n -= limbs;
    big_trim(a);
}

static void big_add1(Big *a) {
    for (int i = 0; i < a->n; i++)
        if (++a->w[i]) return;
    a->w[a->n++] = 1;
}

/* Decimal digits of a, most significant first, into d (no leading zeros; "" for 0). */
static int big_digits(Big *a, char *d) {
    char t[400];
    int n = 0;
    while (a->n > 0) {
        uint64_t rem = 0;
        for (int i = a->n - 1; i >= 0; i--) {
            uint64_t cur = (rem << 32) | a->w[i];
            a->w[i] = (uint32_t)(cur / 1000000000u);
            rem = cur % 1000000000u;
        }
        big_trim(a);
        for (int k = 0; k < 9 && (a->n > 0 || rem); k++) {
            t[n++] = (char)('0' + rem % 10);
            rem /= 10;
        }
    }
    for (int i = 0; i < n; i++) d[i] = t[n - 1 - i];
    return n;
}

int fmt_f64_fixed(Buf *b, double x, int prec) {
    if (prec > 20) return CCORE_ERR_NONFINITE;
    uint64_t u;
    memcpy(&u, &x, 8);
    if (((u >> 52) & 0x7ff) == 0x7ff) return CCORE_ERR_NONFINITE;
    uint64_t bits;
    int z = prologue(b, x, &bits);
    if (prec < 0) { /* Display: shortest digits, plain */
        if (z) {
            buf_putc(b, '0');
            return 0;
        }
        Digits g;
        shortest(bits, &g, 1);
        int kk = g.n + g.exp;
        if (g.exp >= 0) {
            buf_append(b, g.d, (size_t)g.n);
            buf_fill(b, '0', (size_t)g.exp);
        } else if (kk > 0) {
            buf_append(b, g.d, (size_t)kk);
            buf_putc(b, '.');
            buf_append(b, g.d + kk, (size_t)(g.n - kk));
        } else {
            buf_append(b, "0.", 2);
            buf_fill(b, '0', (size_t)-kk);
            buf_append(b, g.d, (size_t)g.n);
        }
        return 0;
    }
    /* x * 10^prec = m * 5^prec * 2^(e2 + prec), rounded half to even. */
    const uint32_t ex = (uint32_t)(bits >> DOUBLE_MANTISSA_BITS);
    uint64_t m = bits & ((1ull << DOUBLE_MANTISSA_BITS) - 1);
    int e2 = (ex ? (int)ex : 1) - DOUBLE_BIAS - DOUBLE_MANTISSA_BITS;
    if (ex) m |= 1ull << DOUBLE_MANTISSA_BITS;
    Big q;
    memset(&q, 0, sizeof q);
    q.w[0] = (uint32_t)m;
    q.w[1] = (uint32_t)(m >> 32);
    q.n = 2;
    big_trim(&q);
    for (int i = 0; i < prec; i++) big_mul_small(&q, 5);
    int s = e2 + prec;
    if (s >= 0) {
        big_shl(&q, s);
    } else {
        int sh = -s;
        int half = big_bit(&q, sh - 1), rest = sh > 1 && big_any_below(&q, sh - 1);
        big_shr(&q, sh);
        if (half && (rest || big_bit(&q, 0))) big_add1(&q);
    }
    char d[400];
    int n = big_digits(&q, d);
    if (n <= prec) { /* 0.00ddd */
        buf_putc(b, '0');
        if (prec) buf_putc(b, '.');
        buf_fill(b, '0', (size_t)(prec - n));
        buf_append(b, d, (size_t)n);
    } else {
        buf_append(b, d, (size_t)(n - prec));
        if (prec) {
            buf_putc(b, '.');
            buf_append(b, d + n - prec, (size_t)prec);
        }
    }
    return 0;
}
