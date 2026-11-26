/* Suite strtod: cc_strtod (src/strtod.c, the exact parser of the freestanding wasm build) against
 * glibc strtod, bit for bit: 1,000,000 random doubles printed with %.17g (and %.16g, shortest
 * forms vary), every value of golden/floats/floats.txt in both its texts, 10,000 random digit
 * strings of 1-40 digits with random exponents, exact halfway points between neighbours (and
 * just above, just below), subnormals, overflow to inf, underflow to 0, and hand-made edges. */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "../src/strtod.h"
#include "check.h"

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) { /* splitmix64 */
    uint64_t z = (rng += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint64_t bits_of(double x) {
    uint64_t b;
    memcpy(&b, &x, 8);
    return b;
}
static double of_bits(uint64_t b) {
    double x;
    memcpy(&x, &b, 8);
    return x;
}

static int is_finite(double x) { return (bits_of(x) & 0x7ff0000000000000ull) != 0x7ff0000000000000ull; }
static double pow2(int e) { /* 2^e, -1074 <= e <= 1023 */
    return e >= -1022 ? of_bits((uint64_t)(e + 1023) << 52) : of_bits((uint64_t)1 << (e + 1074));
}

static int nfail_shown;
#if defined(_WIN32) || defined(STRTOD_EXACT_REF)
static int exact_ok(const char *s, double g);
static int crt_wrong, crt_shown; /* WIN2: the C runtime's strtod off, cc_strtod proven right */
#endif
/* Compares cc_strtod(s) with glibc strtod(s); counts one check. */
static void cmp(const char *s) {
    double want = strtod(s, NULL), got = cc_strtod(s, strlen(s));
    if (bits_of(want) == bits_of(got)) {
        passed++;
        return;
    }
#if defined(_WIN32) || defined(STRTOD_EXACT_REF)
    /* WIN2: the C runtime is not the reference on Windows (UCRT's strtod rounds some inputs just
     * below a power of two up to it: run 45726496943, 44 checks, each one ulp above the exact
     * value). When the two differ, an exact decimal comparison decides: cc_strtod's result must
     * be the correctly rounded one (ties to even) and the runtime's must not be; else a FAIL. */
    int ok_got = exact_ok(s, got), ok_want = exact_ok(s, want);
    if (ok_got == 1 && ok_want == 0) {
        passed++, crt_wrong++;
        if (crt_shown++ < 20)
            printf("win32: C runtime strtod %.80s%s -> %016llx (off), exact -> %016llx (cc_strtod)\n", s,
                   strlen(s) > 80 ? "..." : "", (unsigned long long)bits_of(want), (unsigned long long)bits_of(got));
        return;
    }
    if (nfail_shown < 20) printf("exact check: cc_strtod %d, C runtime %d (1 right, 0 wrong, -1 undecided)\n", ok_got, ok_want);
#endif
    failed++;
    if (nfail_shown++ < 20)
        printf("FAIL %.120s%s\n  got:  %016llx\n  want: %016llx\n", s, strlen(s) > 120 ? "..." : "",
               (unsigned long long)bits_of(got), (unsigned long long)bits_of(want));
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* A random finite double: all bit patterns but NaN and inf, a share of subnormals. */
static double rand_double(void) {
    for (;;) {
        uint64_t b = next();
        if ((next() & 15) == 0) b &= 0x800fffffffffffffull; /* subnormal (or zero) */
        double x = of_bits(b);
        if (is_finite(x)) return x;
    }
}

/* Exact decimal expansion of a finite x >= 0 as "<int>.<1080 digits>" (glibc printf is exact). */
#define FRAC 1080
static void exact(double x, char *out, size_t cap) { snprintf(out, cap, "%.*f", FRAC, x); }

/* (a + b) / 2 of two exact expansions with the same number of fraction digits, as a decimal
 * string with one more fraction digit; exact. */
static void midpoint(const char *a, const char *b, char *out) {
    size_t la = strlen(a), lb = strlen(b);
    size_t L = la > lb ? la : lb;
    static char A[2048], B[2048], S[2048];
    /* right-align (same fraction length) */
    memset(A, '0', L), memset(B, '0', L);
    memcpy(A + L - la, a, la), memcpy(B + L - lb, b, lb);
    A[L] = B[L] = 0;
    int carry = 0;
    for (size_t i = L; i-- > 0;) {
        if (A[i] == '.') { S[i + 1] = '.'; continue; }
        int d = (A[i] - '0') + (B[i] - '0') + carry;
        S[i + 1] = (char)('0' + d % 10);
        carry = d / 10;
    }
    S[0] = (char)('0' + carry);
    S[L + 1] = '0', S[L + 2] = 0; /* one more digit for the halving */
    /* halve, left to right */
    int rem = 0;
    size_t o = 0;
    for (size_t i = 0; S[i]; i++) {
        if (S[i] == '.') { out[o++] = '.'; continue; }
        int d = rem * 10 + (S[i] - '0');
        out[o++] = (char)('0' + d / 2);
        rem = d % 2;
    }
    out[o] = 0;
}

#if defined(_WIN32) || defined(STRTOD_EXACT_REF)
/* WIN2: exact decimal comparison, independent of any strtod. A decimal text as digits D (no
 * leading or trailing zero) and exponent E: the value 0.D x 10^E; D empty is zero. Returns the
 * sign (0 +, 1 -) or -1 when the text is not a plain decimal. */
#define DEC_MAX 4096
static int dec_parse(const char *s, char *D, long *E) {
    int neg = 0;
    if (*s == '+' || *s == '-') neg = *s++ == '-';
    size_t n = 0;
    long e = 0, lead = 0;
    int seen = 0, point = 0;
    for (; (*s >= '0' && *s <= '9') || *s == '.'; s++) {
        if (*s == '.') {
            if (point) return -1;
            point = 1;
            continue;
        }
        seen = 1;
        if (n == 0 && *s == '0') { /* leading zero: after the point it shifts the exponent */
            if (point) lead++;
            continue;
        }
        if (n + 1 >= DEC_MAX) return -1;
        D[n++] = *s;
        if (!point) e++;
    }
    if (!seen) return -1;
    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0;
        if (*s == '+' || *s == '-') eneg = *s++ == '-';
        if (!(*s >= '0' && *s <= '9')) return -1;
        long x = 0;
        for (; *s >= '0' && *s <= '9'; s++) x = x < 100000000 ? x * 10 + (*s - '0') : x;
        e += eneg ? -x : x;
    }
    if (*s) return -1;
    while (n > 0 && D[n - 1] == '0') n--;
    D[n] = 0;
    *E = n ? e - lead : 0;
    return neg;
}
/* Sign of a - b, both >= 0 in the form above. */
static int dec_cmp(const char *A, long EA, const char *B, long EB) {
    if (!*A || !*B) return (*A != 0) - (*B != 0);
    if (EA != EB) return EA < EB ? -1 : 1;
    for (;; A++, B++) {
        if (!*A || !*B) return (*A != 0) - (*B != 0);
        if (*A != *B) return *A < *B ? -1 : 1;
    }
}
/* The midpoint of two neighbouring finite doubles a < b, as D, E. */
static void dec_mid(double a, double b, char *D, long *E) {
    static char ea[2048], eb[2048], m[2100];
    exact(a, ea, sizeof ea), exact(b, eb, sizeof eb);
    midpoint(ea, eb, m);
    dec_parse(m, D, E);
}
/* 1 when g is the correctly rounded value of the decimal text s (ties to even), 0 when not, -1
 * when undecided (not a plain decimal; g infinite or the largest finite double). */
static int exact_ok(const char *s, double g) {
    static char D[DEC_MAX], M[DEC_MAX];
    long E, EM;
    int neg = dec_parse(s, D, &E);
    if (neg < 0) return -1;
    uint64_t b = bits_of(g);
    if ((int)(b >> 63) != neg) return 0;
    b &= 0x7fffffffffffffffull;
    if (b >= 0x7fefffffffffffffull) return -1;
    int even = (b & 1) == 0;
    if (b > 0) { /* above the midpoint with the neighbour below */
        dec_mid(of_bits(b - 1), of_bits(b), M, &EM);
        int c = dec_cmp(D, E, M, EM);
        if (c < 0 || (c == 0 && !even)) return 0;
    }
    dec_mid(of_bits(b), of_bits(b + 1), M, &EM); /* below the midpoint with the one above */
    int c = dec_cmp(D, E, M, EM);
    return c < 0 || (c == 0 && even);
}
#endif

static void halfway(double x) {
    if (!(x >= 0) || !is_finite(x)) return;
    double y = of_bits(bits_of(x) + 1);
    if (!is_finite(y)) return;
    static char a[2048], b[2048], m[2100], t[2200];
    exact(x, a, sizeof a), exact(y, b, sizeof b);
    midpoint(a, b, m);
    cmp(m); /* exactly halfway: to even */
    snprintf(t, sizeof t, "%s0000000000000000000000001", m); /* just above */
    cmp(t);
    /* just below: m minus one unit in its last place, then many 9s */
    strcpy(t, m);
    size_t k = strlen(t);
    while (k-- > 0) {
        if (t[k] == '.') continue;
        if (t[k] > '0') { t[k]--; break; }
        t[k] = '9';
    }
    strcat(t, "99999999999999999999");
    cmp(t);
    /* the same halfway point with an exponent: "<digits>e-<frac>" (no point) */
    static char u[2300];
    size_t o = 0, frac = 0;
    int after = 0;
    for (size_t i = 0; m[i]; i++) {
        if (m[i] == '.') { after = 1; continue; }
        u[o++] = m[i];
        frac += after;
    }
    snprintf(u + o, sizeof u - o, "e-%zu", frac);
    cmp(u);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "ccore/golden/floats";
    char s[256];
    double t0 = now_ms();

    /* 1. random doubles, %.17g (round trip) and %.16g / %.15g (another neighbour, often) */
    int before = passed + failed;
    for (int i = 0; i < 1000000; i++) {
        double x = rand_double();
        snprintf(s, sizeof s, "%.17g", x);
        cmp(s);
        if (i % 10 == 0) {
            snprintf(s, sizeof s, "%.16g", x);
            cmp(s);
            snprintf(s, sizeof s, "%.15e", x);
            cmp(s);
        }
    }
    printf("random doubles: %d compared\n", passed + failed - before);

    /* 2. floats.txt: both texts of every line */
    size_t n;
    char *text = slurp(dir, "floats.txt", &n);
    int lines = 0;
    before = passed + failed;
    for (char *line = text; *line;) {
        char *eol = strchr(line, '\n');
        if (!eol) eol = line + strlen(line);
        char *sp1 = memchr(line, ' ', (size_t)(eol - line));
        char *sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1)) : NULL;
        if (!sp2) { printf("bad line %d\n", lines + 1); return 2; }
        uint64_t bits = strtoull(line, NULL, 16);
        snprintf(s, sizeof s, "%.*s", (int)(sp2 - sp1 - 1), sp1 + 1);
        cmp(s);
        check_true("floats.txt json text = its bits", bits_of(cc_strtod(s, strlen(s))) == bits);
        snprintf(s, sizeof s, "%.*s", (int)(eol - sp2 - 1), sp2 + 1);
        cmp(s);
        check_true("floats.txt debug text = its bits", bits_of(cc_strtod(s, strlen(s))) == bits);
        lines++;
        line = *eol ? eol + 1 : eol;
    }
    free(text);
    check_true("6247 lines in floats.txt", lines == 6247);
    printf("floats.txt: %d lines, %d compared\n", lines, passed + failed - before);

    /* 3. random digit strings: 1-40 digits, a point somewhere (or none), an exponent */
    before = passed + failed;
    for (int i = 0; i < 10000; i++) {
        int nd = 1 + (int)(next() % 40), point = (int)(next() % (uint64_t)(nd + 2)) - 1;
        size_t o = 0;
        if (next() & 1) s[o++] = '-';
        for (int k = 0; k < nd; k++) {
            if (k == point) s[o++] = '.';
            int d = (int)(next() % 10);
            /* runs of 0s and 9s find carries and halfway-like tails */
            uint64_t r = next() % 8;
            if (r == 0) d = 0;
            else if (r == 1) d = 9;
            s[o++] = (char)('0' + d);
        }
        if (point == nd) s[o++] = '.', s[o++] = '0';
        int ex = (int)(next() % 700) - 350;
        o += (size_t)snprintf(s + o, sizeof s - o, "e%d", ex);
        cmp(s);
    }
    printf("random digit strings: %d compared\n", passed + failed - before);

    /* 4. halfway points (and just above / below): random doubles, subnormals, powers of two */
    before = passed + failed;
    for (int i = 0; i < 2500; i++) halfway(of_bits(bits_of(rand_double()) & 0x7fffffffffffffffull));
    for (int i = 0; i < 300; i++) halfway(of_bits(next() & 0x000fffffffffffffull)); /* subnormal */
    for (int e = -1074; e <= 1023; e += 7) halfway(pow2(e));
    halfway(0.0);
    halfway(of_bits(0x7fefffffffffffffull - 1));
    halfway(of_bits(0x000fffffffffffffull)); /* largest subnormal */
    halfway(1.0), halfway(9007199254740992.0), halfway(0.1);
    printf("halfway cases: %d compared\n", passed + failed - before);

    /* 5. hand-made edges: overflow, underflow, zeros, long inputs, signs */
    static const char *edges[] = {
        "0", "-0", "+0", "0.0", "-0.0", "0e0", "0e-999999", "-0e999999", "0.000000000000000000000000",
        "1", "+1", "-1", "1.", ".5", "-.5", "1e0", "1E+0", "1e-0", "000000123.4560000", "0.1", "0.3",
        "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308",
        "-1.7976931348623159e308", "1e309", "-1e309", "1e400", "1e99999999999", "-1e99999999999",
        "179769313486231580793728971405303415079934132710037826936173778980444968292764750946649017977587207096330286416692887910946555547851940402630657488671505820681908902000708383676273854845817711531764475730270069855571366959622842914819860834936475292719074168444365510704342711559699508093042880177904174497791.9999999",
        "179769313486231580793728971405303415079934132710037826936173778980444968292764750946649017977587207096330286416692887910946555547851940402630657488671505820681908902000708383676273854845817711531764475730270069855571366959622842914819860834936475292719074168444365510704342711559699508093042880177904174497792",
        "2.2250738585072014e-308", "2.2250738585072011e-308", "2.2250738585072012e-308", "4.9406564584124654e-324",
        "2.4703282292062327e-324", "2.4703282292062328e-324", "-2.4703282292062328e-324", "1e-324", "1e-400",
        "1e-99999999999", "3e-324", "7.4109846876186982e-324", "9007199254740993", "9007199254740992.5",
        "9007199254740993.0000000000000000000000000000001", "123456789012345678901234567890e-30",
        "0.000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001",
        "1000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
        "4.4501477170144022721148195934182639518696390927032912960468522194496444440421538910330590478162701758282983178260792422137401728773891892910553144148156412434867599762821265346585071045737627442980259622449029037796981144446145705102663115100318287949527959668236039986479250965780342141637013812613333119898765515451440315261253813266652951306000184917766328660755595837392240989947807556594098101021612198814605258742579179000071675999344145086087205681577915435923018910334964869420614052182892431445797605163650903606514140377217442262561590244668525767372446430075513332450079650686719491377688478005309963967709758965844137894433796621993967316936280457084866613206797017728916080020698679408551343728867675409720757232455434770912461317493580281734466552734375e-308",
    };
    for (size_t k = 0; k < sizeof edges / sizeof *edges; k++) cmp(edges[k]);
    /* 2^-1075 (half the smallest subnormal) with a long tail: just above rounds up */
    static char big[3000];
    exact(of_bits(1), big, sizeof big); /* smallest subnormal, exact */
    cmp(big);
    /* 1000 digits of 1s, a value needing the sticky flag past 800 digits */
    for (int k = 0; k < 1000; k++) big[k] = '1';
    big[1000] = 0;
    cmp(big);
    strcpy(big + 1000, "e-1300");
    cmp(big);
    printf("edges: done\n");

    /* timing: cc_strtod vs glibc on %.17g texts */
    static char texts[100000][32];
    for (int i = 0; i < 100000; i++) snprintf(texts[i], 32, "%.17g", rand_double());
    double a0 = now_ms();
    volatile double acc = 0;
    for (int i = 0; i < 100000; i++) acc += cc_strtod(texts[i], strlen(texts[i]));
    double a1 = now_ms();
    for (int i = 0; i < 100000; i++) acc -= strtod(texts[i], NULL);
    double a2 = now_ms();
    printf("100000 %%.17g parses: cc_strtod %.1f ms, glibc strtod %.1f ms\n", a1 - a0, a2 - a1);
#if defined(_WIN32) || defined(STRTOD_EXACT_REF)
    printf("win32: %d texts where the C runtime's strtod is off and cc_strtod is exact (decided by the exact decimal check)\n",
           crt_wrong);
#endif
    printf("strtod suite: %.0f ms\n", now_ms() - t0);
    return report("strtod");
}
