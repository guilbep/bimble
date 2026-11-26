/* STEP parser, a port of crates/bimble-ifc/src/step.rs (same chunk split, chunks parsed in
 * parallel with cc_jobs and merged in file order). See step.h. */
#include "step.h"
#include <stdlib.h>
#include <string.h>
#include "jobs.h"
#ifdef CC_IFC_PROFILE
#include <stdio.h>
#include <time.h>
static double sp_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
#define SPROF(what, t0) fprintf(stderr, "PROF    step %-14s %8.1f ms\n", what, sp_now() - t0)
#else
#define SPROF(what, t0) ((void)0)
#endif

/* arena_dup aligned to 8 (arena_dup is byte-aligned). */
static void *st_dup8(Arena *a, const void *src, size_t n) {
    void *d = arena_alloc(a, n ? n : 1, 8);
    if (d && n) memcpy(d, src, n);
    return d;
}

/* ------------------------------------------------------------------ reals */

#ifdef __wasm__
/* Freestanding wasm has no strtod: the C core's own, correctly rounded and bit-equal to glibc
 * (src/strtod.c, test/strtod_test.c). Overflow gives +-inf, as strtod. */
#include "strtod.h"
static double st_strtod(const char *s, size_t n) { return cc_strtod(s, n); }
#else
static double st_strtod(const char *s, size_t n) {
    char small[128];
    char *p = n < sizeof small ? small : malloc(n + 1);
    if (!p) return 0.0;
    memcpy(p, s, n);
    p[n] = 0;
    double x = strtod(p, NULL);
    if (p != small) free(p);
    return x;
}
#endif

/* Rust's f64 grammar restricted to the STEP number alphabet [0-9+-.Ee]:
 * [sign] digits* ['.' digits*] [(e|E) [sign] digits+], with at least one mantissa digit. */
int cc_step_parse_real(const char *s, size_t n, double *out) {
    size_t i = 0, md = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) i++;
    while (i < n && s[i] >= '0' && s[i] <= '9') i++, md++;
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++, md++;
    }
    if (!md) return -1;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t ed = 0;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++, ed++;
        if (!ed) return -1;
    }
    if (i != n) return -1;
    *out = st_strtod(s, n);
    return 0;
}

static int parse_i64(const char *s, size_t n, int64_t *out) {
    size_t i = 0;
    int neg = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i] == '-', i++;
    if (i == n) return -1;
    uint64_t v = 0, lim = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        unsigned d = (unsigned)(s[i] - '0');
        if (v > (lim - d) / 10) return -1;
        v = v * 10 + d;
    }
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return 0;
}

/* ------------------------------------------------------------------ strings */

static void put_cp(Buf *b, uint32_t c) {
    if (c < 0x80) buf_putc(b, (char)c);
    else if (c < 0x800) buf_putc(b, (char)(0xC0 | c >> 6)), buf_putc(b, (char)(0x80 | (c & 0x3F)));
    else if (c < 0x10000) {
        buf_putc(b, (char)(0xE0 | c >> 12));
        buf_putc(b, (char)(0x80 | ((c >> 6) & 0x3F)));
        buf_putc(b, (char)(0x80 | (c & 0x3F)));
    } else {
        buf_putc(b, (char)(0xF0 | c >> 18));
        buf_putc(b, (char)(0x80 | ((c >> 12) & 0x3F)));
        buf_putc(b, (char)(0x80 | ((c >> 6) & 0x3F)));
        buf_putc(b, (char)(0x80 | (c & 0x3F)));
    }
}

/* String::from_utf8_lossy: one U+FFFD per maximal invalid subpart. */
static void utf8_lossy(Buf *b, const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        if (c < 0x80) { buf_putc(b, (char)c); i++; continue; }
        size_t w = 0;
        unsigned lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) w = 2;
        else if (c >= 0xE0 && c <= 0xEF) { w = 3; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
        else if (c >= 0xF0 && c <= 0xF4) { w = 4; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
        if (!w) { put_cp(b, 0xFFFD); i++; continue; }
        size_t k = 1;
        while (k < w && i + k < n) {
            unsigned d = s[i + k];
            if (d < (k == 1 ? lo : 0x80) || d > (k == 1 ? hi : 0xBF)) break;
            k++;
        }
        if (k == w) buf_append(b, s + i, w);
        else put_cp(b, 0xFFFD);
        i += k;
    }
}

/* u32::from_str_radix(text, 16) on bytes (accepts a leading '+'). */
static int hex_u32(const char *s, size_t n, uint32_t *out) {
    size_t i = 0;
    if (n && s[0] == '+') i = 1;
    if (i == n) return -1;
    uint64_t v = 0;
    for (; i < n; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return -1;
        v = v * 16 + d;
        if (v > 0xFFFFFFFFu) return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static int starts(const char *s, size_t n, const char *pat) {
    size_t k = strlen(pat);
    return n >= k && memcmp(s, pat, k) == 0;
}

void cc_step_decode_string(const char *raw, size_t n, Buf *out) {
    size_t i = 0;
    while (i < n) {
        const char *rest = raw + i;
        size_t rn = n - i;
        if (starts(rest, rn, "\\X2\\") || starts(rest, rn, "\\X4\\")) {
            size_t width = rest[2] == '2' ? 4 : 8, j = i + 4;
            uint32_t pend = 0; /* UTF-16: a high surrogate waiting for its pair */
            int has_pend = 0;
            while (j + width <= n && !starts(raw + j, n - j, "\\X0\\")) {
                uint32_t u;
                if (hex_u32(raw + j, width, &u)) break;
                if (width == 4) {
                    u &= 0xFFFF;
                    if (has_pend) {
                        if (u >= 0xDC00 && u <= 0xDFFF) {
                            put_cp(out, 0x10000 + ((pend - 0xD800) << 10) + (u - 0xDC00));
                            has_pend = 0;
                            j += width;
                            continue;
                        }
                        put_cp(out, 0xFFFD);
                        has_pend = 0;
                    }
                    if (u >= 0xD800 && u <= 0xDBFF) pend = u, has_pend = 1;
                    else if (u >= 0xDC00 && u <= 0xDFFF) put_cp(out, 0xFFFD);
                    else put_cp(out, u);
                } else if (u <= 0x10FFFF && !(u >= 0xD800 && u <= 0xDFFF)) {
                    put_cp(out, u);
                }
                j += width;
            }
            if (has_pend) put_cp(out, 0xFFFD);
            i = starts(raw + j, n - j, "\\X0\\") ? j + 4 : j;
        } else if (starts(rest, rn, "\\X\\") && rn >= 5) {
            uint32_t u;
            if (!hex_u32(rest + 3, 2, &u)) put_cp(out, u);
            i += 5;
        } else if (starts(rest, rn, "\\S\\") && rn >= 4) {
            put_cp(out, (uint32_t)(unsigned char)rest[3] + 128);
            i += 4;
        } else if (starts(rest, rn, "\\\\")) {
            buf_putc(out, '\\');
            i += 2;
        } else {
            size_t p = 0;
            while (p < rn && rest[p] != '\\') p++;
            size_t end = p < rn ? i + (p > 1 ? p : 1) : n;
            utf8_lossy(out, (const unsigned char *)raw + i, end - i);
            i = end;
        }
    }
}

/* ------------------------------------------------------------------ parser */

/* Upper-case keywords (entity, type and enum names) interned per parser: one copy each. */
typedef struct {
    const char *p;
    size_t n;
    uint64_t h;
} Name;
#define NAMES_MAX 8192 /* distinct names kept; past it, copied each time */

typedef struct {
    const char *s;
    size_t n, i, line;
    Arena *a;
    cc_sv *stk; /* scratch stack of list items */
    size_t nstk, capstk;
    Buf tmp, dec;
    Name *names; /* open addressing, 2 * NAMES_MAX slots (NULL: no interning) */
    size_t nnames;
    /* An unterminated comment: a scan from open_from found no closing star-slash to the end of
     * the text (whose line is open_line_end), so neither does one from any later place. */
    size_t open_from, open_line_end;
    int oom;
    char msg[160];
} P;

static int perr(P *p, const char *m) {
    size_t k = strlen(m);
    if (k >= sizeof p->msg) k = sizeof p->msg - 1;
    memcpy(p->msg, m, k);
    p->msg[k] = 0;
    return -1;
}

static int peek(P *p) { return p->i < p->n ? (unsigned char)p->s[p->i] : -1; }
static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int is_alnum(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

static void skip_ws(P *p) {
    for (;;) {
        int c = peek(p);
        if (c == '\n') p->line++, p->i++;
        else if (is_ws(c)) p->i++;
        else if (c == '/' && p->i + 1 < p->n && p->s[p->i + 1] == '*') {
            p->i += 2;
            if (p->i >= p->open_from) { /* known to run to the end: the same answer, at once */
                p->i = p->n, p->line = p->open_line_end;
                break;
            }
            size_t from = p->i;
            while (p->i < p->n && !(p->s[p->i] == '*' && p->i + 1 < p->n && p->s[p->i + 1] == '/')) {
                if (p->s[p->i] == '\n') p->line++;
                p->i++;
            }
            if (p->i >= p->n) { /* unterminated: each later statement found it again (quadratic) */
                p->open_from = from, p->open_line_end = p->line;
                break;
            }
            p->i = p->i + 2 < p->n ? p->i + 2 : p->n;
        } else break;
    }
}

static int expect(P *p, char c) {
    skip_ws(p);
    if (peek(p) == (unsigned char)c) { p->i++; return 0; }
    char m[64] = "expected 'x', found ...";
    m[10] = c;
    return perr(p, m);
}

/* The keyword at i (alnum and '_'); its span. */
static void keyword_span(P *p, size_t *st, size_t *en) {
    skip_ws(p);
    *st = p->i;
    while (is_alnum(peek(p)) || peek(p) == '_') p->i++;
    *en = p->i;
}

static int keyword_is(P *p, const char *w) {
    size_t st, en;
    keyword_span(p, &st, &en);
    size_t k = strlen(w);
    if (en - st != k) return 0;
    for (size_t j = 0; j < k; j++)
        if (up(p->s[st + j]) != w[j]) return 0;
    return 1;
}

static const char *upper_dup(P *p, size_t st, size_t en) {
    size_t n = en - st;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t j = st; j < en; j++) h = (h ^ (unsigned char)up(p->s[j])) * 0x100000001b3ull;
    size_t mask = 2 * NAMES_MAX - 1, k = (size_t)h & mask;
    if (p->names) {
        for (; p->names[k].p; k = (k + 1) & mask) {
            const Name *x = &p->names[k];
            if (x->h != h || x->n != n) continue;
            size_t j = 0;
            while (j < n && x->p[j] == up(p->s[st + j])) j++;
            if (j == n) return x->p;
        }
    }
    char *d = arena_alloc(p->a, n + 1, 1);
    if (!d) { p->oom = 1; return ""; }
    for (size_t j = st; j < en; j++) d[j - st] = up(p->s[j]);
    if (p->names && p->nnames < NAMES_MAX) {
        p->names[k].p = d, p->names[k].n = n, p->names[k].h = h;
        p->nnames++;
    }
    return d;
}

static int number(P *p, cc_sv *v) {
    size_t st = p->i;
    for (;;) {
        int c = peek(p);
        if (is_digit(c) || c == '+' || c == '-' || c == '.' || c == 'E' || c == 'e') p->i++;
        else break;
    }
    const char *t = p->s + st;
    size_t n = p->i - st;
    int real = 0;
    for (size_t j = 0; j < n; j++) real |= t[j] == '.' || t[j] == 'E' || t[j] == 'e';
    if (real) {
        v->kind = CC_SV_REAL;
        if (cc_step_parse_real(t, n, &v->u.r)) return perr(p, "bad real");
    } else {
        v->kind = CC_SV_INT;
        if (parse_i64(t, n, &v->u.i)) return perr(p, "bad integer");
    }
    return 0;
}

static int string(P *p, cc_sv *v) {
    p->i++;
    buf_clear(&p->tmp);
    for (;;) {
        int c = peek(p);
        if (c < 0) return perr(p, "unterminated string");
        if (c == '\'' && p->i + 1 < p->n && p->s[p->i + 1] == '\'') {
            buf_putc(&p->tmp, '\'');
            p->i += 2;
        } else if (c == '\'') {
            p->i++;
            break;
        } else {
            if (c == '\n') p->line++;
            buf_putc(&p->tmp, (char)c);
            p->i++;
        }
    }
    buf_clear(&p->dec);
    cc_step_decode_string(p->tmp.p, p->tmp.len, &p->dec);
    if (p->tmp.oom || p->dec.oom) { p->oom = 1; return perr(p, "out of memory"); }
    char *d = arena_alloc(p->a, p->dec.len + 1, 1);
    if (!d) { p->oom = 1; return perr(p, "out of memory"); }
    if (p->dec.len) memcpy(d, p->dec.p, p->dec.len);
    v->kind = CC_SV_STR;
    v->u.s.p = d;
    v->u.s.n = p->dec.len;
    return 0;
}

static int items(P *p, size_t depth, cc_sv **out, size_t *nout);

static int value(P *p, size_t depth, cc_sv *v) {
    memset(v, 0, sizeof *v);
    if (depth > CC_STEP_MAX_DEPTH) return perr(p, "nesting too deep");
    skip_ws(p);
    int c = peek(p);
    if (c == '$') { p->i++; v->kind = CC_SV_NULL; return 0; }
    if (c == '*') { p->i++; v->kind = CC_SV_DERIVED; return 0; }
    if (c == '#') {
        p->i++;
        size_t st = p->i;
        while (is_digit(peek(p))) p->i++;
        uint64_t r = 0;
        if (p->i == st) return perr(p, "bad reference");
        for (size_t j = st; j < p->i; j++) {
            unsigned d = (unsigned)(p->s[j] - '0');
            if (r > (UINT64_MAX - d) / 10) return perr(p, "bad reference");
            r = r * 10 + d;
        }
        v->kind = CC_SV_REF;
        v->u.ref = r;
        return 0;
    }
    if (c == '\'') return string(p, v);
    if (c == '"' || c == '.') {
        p->i++;
        size_t st = p->i;
        while (peek(p) >= 0 && peek(p) != c) p->i++;
        size_t en = p->i;
        if (p->i < p->n) p->i++;
        if (c == '.') {
            v->kind = CC_SV_ENUM;
            v->u.s.p = upper_dup(p, st, en);
        } else {
            buf_clear(&p->dec);
            utf8_lossy(&p->dec, (const unsigned char *)p->s + st, en - st);
            char *d = arena_alloc(p->a, p->dec.len + 1, 1);
            if (!d || p->dec.oom) { p->oom = 1; return perr(p, "out of memory"); }
            if (p->dec.len) memcpy(d, p->dec.p, p->dec.len);
            v->kind = CC_SV_STR;
            v->u.s.p = d;
            en = st + p->dec.len;
        }
        v->u.s.n = en - st;
        return p->oom ? perr(p, "out of memory") : 0;
    }
    if (c == '(') {
        p->i++;
        v->kind = CC_SV_LIST;
        return items(p, depth + 1, &v->u.list.items, &v->u.list.n);
    }
    if (is_digit(c) || c == '-' || c == '+') return number(p, v);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        size_t st, en;
        keyword_span(p, &st, &en);
        const char *name = upper_dup(p, st, en);
        if (expect(p, '(')) return -1;
        cc_sv *its;
        size_t n;
        if (items(p, depth + 1, &its, &n)) return -1;
        cc_sv *inner = arena_alloc(p->a, sizeof *inner, 8);
        if (!inner) { p->oom = 1; return perr(p, "out of memory"); }
        if (n == 1) *inner = its[0];
        else inner->kind = CC_SV_LIST, inner->u.list.items = its, inner->u.list.n = n;
        v->kind = CC_SV_TYPED;
        v->u.typed.name = name;
        v->u.typed.inner = inner;
        return 0;
    }
    return perr(p, "unexpected character");
}

static int push(P *p, const cc_sv *v) {
    if (p->nstk == p->capstk) {
        size_t cap = p->capstk ? p->capstk * 2 : 256;
        cc_sv *s = realloc(p->stk, cap * sizeof *s);
        if (!s) { p->oom = 1; return perr(p, "out of memory"); }
        p->stk = s, p->capstk = cap;
    }
    p->stk[p->nstk++] = *v;
    return 0;
}

/* Comma-separated values up to and including ')'. */
static int items(P *p, size_t depth, cc_sv **out, size_t *nout) {
    size_t base = p->nstk;
    *out = NULL;
    *nout = 0;
    skip_ws(p);
    if (peek(p) == ')') { p->i++; return 0; }
    for (;;) {
        cc_sv v;
        if (value(p, depth, &v) || push(p, &v)) { p->nstk = base; return -1; }
        skip_ws(p);
        int c = peek(p);
        if (c == ',') { p->i++; continue; }
        if (c == ')') {
            p->i++;
            size_t n = p->nstk - base;
            cc_sv *a = arena_alloc_n(p->a, n, sizeof *a, 8);
            if (!a) { p->oom = 1; p->nstk = base; return perr(p, "out of memory"); }
            memcpy(a, p->stk + base, n * sizeof *a);
            p->nstk = base;
            *out = a, *nout = n;
            return 0;
        }
        p->nstk = base;
        return perr(p, "expected ',' or ')'");
    }
}

#ifdef __wasm__
static const char *st_memchr(const char *s, int c, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] == (char)c) return s + i;
    return NULL;
}
#else
static const char *st_memchr(const char *s, int c, size_t n) { return memchr(s, c, n); }
#endif

static size_t count_nl(const char *s, size_t n) {
    size_t k = 0;
    for (const char *e = s + n; (s = st_memchr(s, '\n', (size_t)(e - s))) != NULL; s++) k++;
    return k;
}

/* To just after the next ';' outside strings (newlines inside strings are not counted). By
 * memchr: the ';', a quote before it, and the newlines before that. */
static void skip_statement(P *p) {
    while (p->i < p->n) {
        const char *s = p->s + p->i;
        size_t rem = p->n - p->i;
        const char *semi = st_memchr(s, ';', rem);
        size_t lim = semi ? (size_t)(semi - s) : rem;
        const char *q = st_memchr(s, '\'', lim);
        size_t upto = q ? (size_t)(q - s) : lim;
        p->line += count_nl(s, upto);
        if (!q) {
            p->i += semi ? lim + 1 : rem;
            return;
        }
        p->i += upto + 1; /* a string: to just after its closing quote */
        const char *e = st_memchr(p->s + p->i, '\'', p->n - p->i);
        p->i = e ? (size_t)(e - p->s) + 1 : p->n;
    }
}

/* ------------------------------------------------------------------ file */

static const unsigned char *find(const unsigned char *s, size_t n, const char *pat) {
    size_t k = strlen(pat);
    for (size_t i = 0; i + k <= n; i++)
        if (s[i] == (unsigned char)pat[0] && memcmp(s + i, pat, k) == 0) return s + i;
    return NULL;
}

typedef struct {
    cc_step_entity *v;
    size_t n, cap;
} EntVec;

static int ent_push(EntVec *ev, const cc_step_entity *e) {
    if (ev->n == ev->cap) {
        size_t cap = ev->cap ? ev->cap * 2 : 1024;
        cc_step_entity *v = realloc(ev->v, cap * sizeof *v);
        if (!v) return -1;
        ev->v = v, ev->cap = cap;
    }
    ev->v[ev->n++] = *e;
    return 0;
}

typedef struct {
    const char *const *skip; /* entity types not stored (upper case) */
    size_t nskip;
    int only; /* M1LINKS: `skip` lists the only types stored (cc_step_parse_only) */
} Skip;

static int skipped_type(const Skip *sk, const char *name) {
    for (size_t k = 0; k < sk->nskip; k++)
        if (strcmp(sk->skip[k], name) == 0) return !sk->only;
    return sk->only;
}

static void parse_chunk(P *p, size_t start, size_t line, size_t stop, const Skip *sk, EntVec *ev, size_t *skipped) {
    p->i = start;
    p->line = line;
    for (;;) {
        skip_ws(p);
        if (p->i >= stop || p->i >= p->n) break;
        if (peek(p) != '#') {
            if (keyword_is(p, "ENDSEC")) break;
            skip_statement(p);
            continue;
        }
        size_t s0 = p->i, l0 = p->line;
        cc_step_entity e;
        memset(&e, 0, sizeof e);
        cc_sv idv;
        int bad = value(p, 0, &idv), skip = 0;
        if (!bad && idv.kind != CC_SV_REF) bad = perr(p, "expected entity id");
        if (!bad) bad = expect(p, '=');
        if (!bad) {
            e.id = idv.u.ref;
            skip_ws(p);
            if (peek(p) == '(' && sk->only) { /* M1LINKS: a complex instance is none of the types asked */
                skip_statement(p);
                skip = 1;
            } else if (peek(p) == '(') {
                p->i++;
                size_t st, en;
                keyword_span(p, &st, &en);
                e.name = upper_dup(p, st, en);
                bad = expect(p, '(') || items(p, 1, &e.args, &e.nargs);
                if (!bad) skip_statement(p);
            } else {
                size_t st, en;
                keyword_span(p, &st, &en);
                if (st == en) bad = perr(p, "expected entity name");
                else {
                    e.name = upper_dup(p, st, en);
                    /* step.rs parse_chunk: a skipped type is stepped over, never parsed. */
                    if (sk->nskip && skipped_type(sk, e.name)) {
                        skip_statement(p);
                        skip = 1;
                    } else bad = expect(p, '(') || items(p, 1, &e.args, &e.nargs) || expect(p, ';');
                }
            }
        }
        if (p->oom) return;
        if (skip) continue;
        if (!bad) {
            if (ent_push(ev, &e)) { p->oom = 1; return; }
        } else {
            (*skipped)++;
            p->i = s0, p->line = l0;
            skip_statement(p);
        }
    }
}

/* Stable merge sort of entities by id. */
static void sort_ents(cc_step_entity *v, cc_step_entity *tmp, size_t n) {
    if (n < 2) return;
    size_t h = n / 2;
    sort_ents(v, tmp, h);
    sort_ents(v + h, tmp, n - h);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) tmp[k++] = v[j].id < v[i].id ? v[j++] : v[i++];
    while (i < h) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof *v);
}

static int fail(cc_error *err, size_t line, size_t off, const char *msg) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "line ");
    buf_u64(&b, line);
    buf_puts(&b, ": ");
    buf_puts(&b, msg);
    size_t k = b.len < sizeof err->msg - 1 ? b.len : sizeof err->msg - 1;
    if (k) memcpy(err->msg, b.p, k);
    err->msg[k] = 0;
    err->offset = off;
    buf_free(&b);
    return -1;
}

/* One chunk of the DATA section per task; one parser (scratch and arena) per worker. */
typedef struct {
    const char *text;
    size_t n;
    size_t (*starts)[2]; /* chunk start offset and line */
    size_t nst, end;
    Skip sk;
    P *ps;        /* per worker */
    Arena *arenas; /* per worker */
    EntVec *out;  /* per chunk */
    size_t *skipped; /* per chunk */
} Chunks;

static void chunk_job(void *ctx, size_t k, size_t w) {
    Chunks *c = ctx;
    P *p = &c->ps[w];
    if (p->oom) return;
    parse_chunk(p, c->starts[k][0], c->starts[k][1], k + 1 < c->nst ? c->starts[k + 1][0] : c->end, &c->sk,
                &c->out[k], &c->skipped[k]);
}

static void p_init(P *p, const char *text, size_t n, Arena *a) {
    memset(p, 0, sizeof *p);
    p->s = text, p->n = n, p->line = 1, p->a = a;
    p->open_from = (size_t)-1;
    buf_init(&p->tmp);
    buf_init(&p->dec);
    p->names = calloc(2 * NAMES_MAX, sizeof *p->names); /* NULL: names copied each time */
}

static void p_free(P *p) {
    free(p->stk);
    free(p->names);
    buf_free(&p->tmp);
    buf_free(&p->dec);
}

int cc_step_parse(cc_step_file *f, const char *text, size_t n, cc_error *err) {
    return cc_step_parse_skipping(f, text, n, NULL, 0, err);
}

static int parse_listed(cc_step_file *f, const char *text, size_t n, const char *const *skip, size_t nskip, int only,
                        cc_error *err);
int cc_step_parse_skipping(cc_step_file *f, const char *text, size_t n, const char *const *skip, size_t nskip,
                           cc_error *err) {
    return parse_listed(f, text, n, skip, nskip, 0, err);
}
int cc_step_parse_only(cc_step_file *f, const char *text, size_t n, const char *const *keep, size_t nkeep, cc_error *err) {
    return parse_listed(f, text, n, keep, nkeep, 1, err);
}
static int parse_listed(cc_step_file *f, const char *text, size_t n, const char *const *skip, size_t nskip, int only,
                        cc_error *err) {
    memset(f, 0, sizeof *f);
    arena_init(&f->arena, 0);
    if ((uint64_t)n > CC_STEP_MAX_BYTES) return fail(err, 0, 0, "file too large for the C core");
    P p;
    p_init(&p, text, n, &f->arena);
    int rc = 0;
    EntVec ev = {0};
    Chunks c;
    memset(&c, 0, sizeof c);
    size_t nw = 0;
    const unsigned char *u = (const unsigned char *)text;

    skip_ws(&p);
    if (!keyword_is(&p, "ISO")) { rc = fail(err, p.line, p.i, "not a STEP file (missing ISO-10303-21 header)"); goto done; }
    const unsigned char *dp = find(u, n, "\nDATA;");
    if (!dp) dp = find(u, n, "DATA;");
    if (!dp) { rc = fail(err, p.line, p.i, "no DATA section"); goto done; }
    size_t data_pos = (size_t)(dp - u);
    /* FILE_SCHEMA names, in Rust's order (a stack popped from the end). */
    const unsigned char *fs = find(u, data_pos, "FILE_SCHEMA");
    if (fs) {
        P h = p;
        h.n = data_pos, h.i = (size_t)(fs - u) + 11, h.line = 1;
        cc_sv *its;
        size_t ni;
        if (!expect(&h, '(') && !items(&h, 1, &its, &ni)) {
            const cc_sv **stack = malloc((ni + 1) * sizeof *stack);
            size_t sn = 0, sc = ni + 1, cnt = 0;
            const char **names = NULL;
            for (size_t k = 0; stack && k < ni; k++) stack[sn++] = &its[k];
            while (stack && sn) {
                const cc_sv *x = stack[--sn];
                if (x->kind == CC_SV_STR) {
                    const char **nn = realloc(names, (cnt + 1) * sizeof *nn);
                    if (!nn) break;
                    names = nn;
                    char *d = arena_alloc(&f->arena, x->u.s.n + 1, 1);
                    if (!d) break;
                    for (size_t j = 0; j < x->u.s.n; j++) d[j] = up(x->u.s.p[j]);
                    names[cnt++] = d;
                } else if (x->kind == CC_SV_LIST) {
                    if (sn + x->u.list.n > sc) {
                        size_t c2 = (sn + x->u.list.n) * 2;
                        const cc_sv **s2 = realloc(stack, c2 * sizeof *s2);
                        if (!s2) break;
                        stack = s2, sc = c2;
                    }
                    for (size_t j = 0; j < x->u.list.n; j++) stack[sn++] = &x->u.list.items[j];
                }
            }
            free(stack);
            if (cnt) {
                f->schemas = st_dup8(&f->arena, names, cnt * sizeof *names);
                if (f->schemas) f->nschemas = cnt;
            }
            free(names);
        }
        p.stk = h.stk, p.capstk = h.capstk, p.nstk = 0;
        p.tmp = h.tmp, p.dec = h.dec;
    }
    size_t line = 1;
    for (size_t k = 0; k < data_pos; k++) line += u[k] == '\n';
    const unsigned char *d5 = find(u + data_pos, n - data_pos, "DATA;");
    size_t data = data_pos + (d5 ? (size_t)(d5 - (u + data_pos)) : 0) + 5;

#ifdef CC_IFC_PROFILE
    double t0 = sp_now();
#endif
    /* data_chunks: statement starts beginning a chunk, and the end (ENDSEC or end of text). */
    size_t (*starts_v)[2] = NULL, nst = 0, cst = 0, end;
    p.i = data, p.line = line;
    for (;;) {
        skip_ws(&p);
        size_t hi = p.i, hl = p.line;
        if (peek(&p) < 0) break;
        if (peek(&p) != '#') {
            int endsec = keyword_is(&p, "ENDSEC");
            p.i = hi, p.line = hl;
            if (endsec) break;
        }
        if (!nst || hi - starts_v[nst - 1][0] >= CC_STEP_CHUNK_BYTES) {
            if (nst == cst) {
                size_t c2 = cst ? cst * 2 : 16;
                void *s2 = realloc(starts_v, c2 * sizeof *starts_v);
                if (!s2) { rc = fail(err, hl, hi, "out of memory"); goto done; }
                starts_v = s2, cst = c2;
            }
            starts_v[nst][0] = hi, starts_v[nst][1] = hl;
            nst++;
        }
        skip_statement(&p);
    }
    end = p.i;
    SPROF("split", t0);

    /* The chunks on all workers, each into its own vector; merged in file order. */
    c.text = text, c.n = n, c.starts = starts_v, c.nst = nst, c.end = end;
    c.sk.skip = skip, c.sk.nskip = skip ? nskip : 0, c.sk.only = only;
    nw = cc_jobs_workers(nst);
    c.ps = calloc(nw, sizeof *c.ps);
    c.arenas = calloc(nw, sizeof *c.arenas);
    c.out = calloc(nst ? nst : 1, sizeof *c.out);
    c.skipped = calloc(nst ? nst : 1, sizeof *c.skipped);
    if (!c.ps || !c.arenas || !c.out || !c.skipped) { free(starts_v); rc = fail(err, 0, 0, "out of memory"); goto done; }
    for (size_t w = 0; w < nw; w++) {
        arena_init(&c.arenas[w], (size_t)1 << 20);
        p_init(&c.ps[w], text, n, &c.arenas[w]);
    }
    cc_jobs_run(nst, chunk_job, &c);
    SPROF("chunks", t0);
    free(starts_v);
    int oom = 0;
    size_t total = 0;
    for (size_t w = 0; w < nw; w++) {
        oom |= c.ps[w].oom;
        arena_adopt(&f->arena, &c.arenas[w]);
    }
    for (size_t k = 0; k < nst; k++) total += c.out[k].n, f->skipped += c.skipped[k];
    if (oom) { rc = fail(err, 0, 0, "out of memory"); goto done; }
    ev.v = malloc((total ? total : 1) * sizeof *ev.v);
    if (!ev.v) { rc = fail(err, 0, 0, "out of memory"); goto done; }
    for (size_t k = 0; k < nst; k++) {
        if (c.out[k].n) memcpy(ev.v + ev.n, c.out[k].v, c.out[k].n * sizeof *ev.v);
        ev.n += c.out[k].n;
        free(c.out[k].v);
        c.out[k].v = NULL;
    }

    SPROF("merge", t0);
    /* Sort by id (stable; usually already sorted); a later duplicate wins. */
    if (ev.n) {
        int sorted = 1;
        for (size_t k = 1; k < ev.n && sorted; k++) sorted = ev.v[k - 1].id < ev.v[k].id;
        size_t w = ev.n;
        if (!sorted) {
            cc_step_entity *tmp = malloc(ev.n * sizeof *tmp);
            if (!tmp) { rc = fail(err, 0, 0, "out of memory"); goto done; }
            sort_ents(ev.v, tmp, ev.n);
            free(tmp);
            w = 0;
            for (size_t k = 0; k < ev.n; k++) {
                if (w && ev.v[w - 1].id == ev.v[k].id) ev.v[w - 1] = ev.v[k];
                else ev.v[w++] = ev.v[k];
            }
        }
        f->ents = ev.v; /* owned (cc_step_free) */
        f->n = w;
        ev.v = NULL;
    }
done:
    for (size_t w = 0; c.ps && w < nw; w++) p_free(&c.ps[w]);
    for (size_t w = 0; c.arenas && w < nw; w++) arena_free(&c.arenas[w]);
    for (size_t k = 0; c.out && k < c.nst; k++) free(c.out[k].v);
    free(c.ps);
    free(c.arenas);
    free(c.out);
    free(c.skipped);
    free(ev.v);
    p_free(&p);
    return rc;
}

void cc_step_free(cc_step_file *f) {
    free(f->ents);
    arena_free(&f->arena);
    memset(f, 0, sizeof *f);
}

const cc_step_entity *cc_step_get(const cc_step_file *f, uint64_t id) {
    size_t lo = 0, hi = f->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (f->ents[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < f->n && f->ents[lo].id == id ? &f->ents[lo] : NULL;
}

static const cc_sv null_sv = {CC_SV_NULL, {0}};

const cc_sv *cc_step_arg(const cc_step_entity *e, size_t i) {
    return e && i < e->nargs ? &e->args[i] : &null_sv;
}

const cc_step_entity *cc_step_deref(const cc_step_file *f, const cc_sv *v) {
    uint64_t r;
    return cc_sv_as_ref(v, &r) ? cc_step_get(f, r) : NULL;
}

size_t cc_step_index(const cc_step_file *f, const cc_step_entity *e) { return (size_t)(e - f->ents); }

int cc_sv_as_ref(const cc_sv *v, uint64_t *out) {
    if (v->kind != CC_SV_REF) return 0;
    *out = v->u.ref;
    return 1;
}

int cc_sv_as_f64(const cc_sv *v, double *out) {
    for (int k = 0; k < 256 && v->kind == CC_SV_TYPED; k++) v = v->u.typed.inner;
    if (v->kind == CC_SV_REAL) { *out = v->u.r; return 1; }
    if (v->kind == CC_SV_INT) { *out = (double)v->u.i; return 1; }
    return 0;
}

int cc_sv_as_i64(const cc_sv *v, int64_t *out) {
    for (int k = 0; k < 256 && v->kind == CC_SV_TYPED; k++) v = v->u.typed.inner;
    if (v->kind != CC_SV_INT) return 0;
    *out = v->u.i;
    return 1;
}

int cc_sv_as_str(const cc_sv *v, const char **p, size_t *n) {
    for (int k = 0; k < 256 && v->kind == CC_SV_TYPED; k++) v = v->u.typed.inner;
    if (v->kind != CC_SV_STR) return 0;
    *p = v->u.s.p, *n = v->u.s.n;
    return 1;
}

int cc_sv_as_enum(const cc_sv *v, const char **p) {
    if (v->kind != CC_SV_ENUM) return 0;
    *p = v->u.s.p;
    return 1;
}

int cc_sv_as_list(const cc_sv *v, const cc_sv **items, size_t *n) {
    if (v->kind != CC_SV_LIST) return 0;
    *items = v->u.list.items, *n = v->u.list.n;
    return 1;
}
