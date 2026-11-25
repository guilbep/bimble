/* JSON reader: recursive descent into an arena DOM. See json_r.h. */
#include "json_r.h"
#include <stdlib.h>
#include <string.h>

#ifdef __wasm__
/* Freestanding wasm has no strtod: the C core's own, correctly rounded and bit-equal to glibc
 * (src/strtod.c, test/strtod_test.c). Overflow gives +-inf, as strtod. */
#include "strtod.h"
static double jr_strtod(const char *s, size_t n) { return cc_strtod(s, n); }
#else
/* glibc's strtod is correctly rounded, as serde_json with float_roundtrip. */
static double jr_strtod(const char *s, size_t n) {
    char small[128];
    char *p = n < sizeof small ? small : malloc(n + 1);
    if (!p) return 1e308 * 10.0; /* out of memory: reported as out of range */
    memcpy(p, s, n);
    p[n] = 0;
    double x = strtod(p, NULL);
    if (p != small) free(p);
    return x;
}
#endif

typedef struct {
    const char *s;
    size_t n, i;
    Arena *a;
    int depth;
    JrError *err;
    int failed;
} P;

static int fail(P *p, size_t off, const char *msg) {
    if (!p->failed) {
        size_t k = strlen(msg);
        if (k >= sizeof p->err->msg) k = sizeof p->err->msg - 1;
        memcpy(p->err->msg, msg, k);
        p->err->msg[k] = 0;
        p->err->offset = off;
        p->failed = 1;
    }
    return -1;
}

static void ws(P *p) {
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

long long jr_utf8_check(const char *str, size_t n) {
    const unsigned char *s = (const unsigned char *)str;
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t need;
        unsigned lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c == 0xE0) need = 2, lo = 0xA0;
        else if (c == 0xED) need = 2, hi = 0x9F;
        else if (c >= 0xE1 && c <= 0xEF) need = 2;
        else if (c == 0xF0) need = 3, lo = 0x90;
        else if (c >= 0xF1 && c <= 0xF3) need = 3;
        else if (c == 0xF4) need = 3, hi = 0x8F;
        else return (long long)i;
        if (n - i - 1 < need) return (long long)i;
        if (s[i + 1] < lo || s[i + 1] > hi) return (long long)i;
        for (size_t k = 2; k <= need; k++)
            if (s[i + k] < 0x80 || s[i + k] > 0xBF) return (long long)i;
        i += need + 1;
    }
    return -1;
}

static int hex4(P *p, size_t at, unsigned *out) {
    if (p->n - at < 4) return fail(p, at, "EOF while parsing a string");
    unsigned v = 0;
    for (int k = 0; k < 4; k++) {
        char c = p->s[at + k];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return fail(p, at + k, "invalid escape");
        v = v * 16 + (unsigned)d;
    }
    *out = v;
    return 0;
}

static size_t put_utf8(char *o, unsigned cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12);
        o[1] = (char)(0x80 | ((cp >> 6) & 63));
        o[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    o[0] = (char)(0xF0 | cp >> 18);
    o[1] = (char)(0x80 | ((cp >> 12) & 63));
    o[2] = (char)(0x80 | ((cp >> 6) & 63));
    o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

/* At the opening quote. Decoded bytes go to the arena; the input is already valid UTF-8. */
static int string(P *p, const char **out, size_t *out_n) {
    size_t start = ++p->i, j = start;
    int esc = 0;
    for (;;) {
        if (j >= p->n) return fail(p, j, "EOF while parsing a string");
        unsigned char c = (unsigned char)p->s[j];
        if (c == '"') break;
        if (c < 0x20) return fail(p, j, "control character (\\u0000-\\u001F) found while parsing a string");
        if (c == '\\') {
            esc = 1;
            j++;
            if (j >= p->n) return fail(p, j, "EOF while parsing a string");
        }
        j++;
    }
    size_t raw = j - start;
    char *o = arena_alloc(p->a, raw + 1, 1);
    if (!o) return fail(p, start, "out of memory");
    if (!esc) {
        memcpy(o, p->s + start, raw);
        *out = o;
        *out_n = raw;
        p->i = j + 1;
        return 0;
    }
    size_t k = 0;
    for (size_t i = start; i < j;) {
        char c = p->s[i];
        if (c != '\\') { o[k++] = c; i++; continue; }
        char e = p->s[i + 1];
        i += 2;
        switch (e) {
        case '"': o[k++] = '"'; break;
        case '\\': o[k++] = '\\'; break;
        case '/': o[k++] = '/'; break;
        case 'b': o[k++] = '\b'; break;
        case 'f': o[k++] = '\f'; break;
        case 'n': o[k++] = '\n'; break;
        case 'r': o[k++] = '\r'; break;
        case 't': o[k++] = '\t'; break;
        case 'u': {
            unsigned cp, lo;
            if (hex4(p, i, &cp)) return -1;
            size_t at = i - 2;
            i += 4;
            if (cp >= 0xDC00 && cp <= 0xDFFF) return fail(p, at, "lone leading surrogate in hex escape");
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (i + 1 >= j || p->s[i] != '\\' || p->s[i + 1] != 'u')
                    return fail(p, at, "unexpected end of hex escape");
                if (hex4(p, i + 2, &lo)) return -1;
                if (lo < 0xDC00 || lo > 0xDFFF) return fail(p, i, "lone leading surrogate in hex escape");
                i += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            k += put_utf8(o + k, cp); /* never longer than its escape */
            break;
        }
        default: return fail(p, i - 1, "invalid escape");
        }
    }
    o[k] = 0;
    *out = o;
    *out_n = k;
    p->i = j + 1;
    return 0;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int number(P *p, JrVal *v) {
    size_t st = p->i, i = p->i;
    v->type = JR_NUM;
    v->num_int = 1;
    v->num_neg = 0;
    if (i < p->n && p->s[i] == '-') v->num_neg = 1, i++;
    if (i >= p->n || !is_digit(p->s[i])) return fail(p, i, "invalid number");
    if (p->s[i] == '0') i++;
    else while (i < p->n && is_digit(p->s[i])) i++;
    if (i < p->n && p->s[i] == '.') {
        v->num_int = 0;
        i++;
        if (i >= p->n || !is_digit(p->s[i])) return fail(p, i, "invalid number");
        while (i < p->n && is_digit(p->s[i])) i++;
    }
    if (i < p->n && (p->s[i] == 'e' || p->s[i] == 'E')) {
        v->num_int = 0;
        i++;
        if (i < p->n && (p->s[i] == '+' || p->s[i] == '-')) i++;
        if (i >= p->n || !is_digit(p->s[i])) return fail(p, i, "invalid number");
        while (i < p->n && is_digit(p->s[i])) i++;
    }
    v->s = p->s + st;
    v->n = i - st;
    v->num = jr_strtod(v->s, v->n);
    p->i = i;
    return 0;
}

static int lit(P *p, const char *w) {
    size_t k = strlen(w);
    if (p->n - p->i < k || memcmp(p->s + p->i, w, k) != 0) return fail(p, p->i, "expected value");
    p->i += k;
    return 0;
}

/* Growable scratch for a container's children, copied to the arena when it closes. */
typedef struct {
    char *p;
    size_t len, cap;
} Vec;

static void *vec_push(Vec *v, size_t sz) {
    if (v->len + sz > v->cap) {
        size_t c = v->cap ? v->cap * 2 : 8 * sz;
        while (c < v->len + sz) c *= 2;
        char *q = realloc(v->p, c);
        if (!q) return NULL;
        v->p = q;
        v->cap = c;
    }
    void *r = v->p + v->len;
    memset(r, 0, sz);
    v->len += sz;
    return r;
}

static int value(P *p, JrVal *v);

static int container(P *p, JrVal *v, int obj) {
    if (p->depth >= JR_MAX_DEPTH) return fail(p, p->i, "recursion limit exceeded");
    p->depth++;
    p->i++;
    Vec sc = {0};
    size_t sz = obj ? sizeof(JrMember) : sizeof(JrVal), count = 0;
    int rc = 0;
    ws(p);
    char close = obj ? '}' : ']';
    if (p->i < p->n && p->s[p->i] == close) {
        p->i++;
    } else {
        for (;;) {
            void *slot = vec_push(&sc, sz);
            if (!slot) { rc = fail(p, p->i, "out of memory"); break; }
            JrVal *child = slot;
            if (obj) {
                JrMember *m = slot;
                ws(p);
                if (p->i >= p->n) { rc = fail(p, p->i, "EOF while parsing an object"); break; }
                if (p->s[p->i] != '"') { rc = fail(p, p->i, "key must be a string"); break; }
                if (string(p, &m->key, &m->key_n)) { rc = -1; break; }
                ws(p);
                if (p->i >= p->n || p->s[p->i] != ':') {
                    rc = fail(p, p->i, p->i >= p->n ? "EOF while parsing an object" : "expected `:`");
                    break;
                }
                p->i++;
                child = &m->val;
            }
            count++;
            if (value(p, child)) { rc = -1; break; }
            ws(p);
            if (p->i >= p->n) {
                rc = fail(p, p->i, obj ? "EOF while parsing an object" : "EOF while parsing a list");
                break;
            }
            char c = p->s[p->i++];
            if (c == close) break;
            if (c != ',') {
                rc = fail(p, p->i - 1, obj ? "expected `,` or `}`" : "expected `,` or `]`");
                break;
            }
            ws(p);
            if (p->i < p->n && p->s[p->i] == close) { rc = fail(p, p->i, "trailing comma"); break; }
        }
    }
    if (!rc && count) {
        void *dst = arena_alloc(p->a, sc.len, 16);
        if (dst) memcpy(dst, sc.p, sc.len);
        if (!dst) rc = fail(p, p->i, "out of memory");
        else if (obj) v->members = dst;
        else v->items = dst;
    }
    free(sc.p);
    v->type = obj ? JR_OBJ : JR_ARR;
    v->n = count;
    p->depth--;
    return rc;
}

static int value(P *p, JrVal *v) {
    ws(p);
    if (p->i >= p->n) return fail(p, p->i, "EOF while parsing a value");
    v->off = p->i;
    switch (p->s[p->i]) {
    case '{': return container(p, v, 1);
    case '[': return container(p, v, 0);
    case '"': v->type = JR_STR; return string(p, &v->s, &v->n);
    case 't': v->type = JR_BOOL; v->b = 1; return lit(p, "true");
    case 'f': v->type = JR_BOOL; v->b = 0; return lit(p, "false");
    case 'n': v->type = JR_NULL; return lit(p, "null");
    default:
        if (p->s[p->i] == '-' || is_digit(p->s[p->i])) return number(p, v);
        return fail(p, p->i, "expected value");
    }
}

JrVal *jr_parse(Arena *a, const char *text, size_t n, JrError *err) {
    P p = {text, n, 0, a, 0, err, 0};
    err->offset = 0;
    err->msg[0] = 0;
    if (n > JR_MAX_BYTES) {
        fail(&p, 0, "input too large");
        return NULL;
    }
    long long bad = jr_utf8_check(text, n);
    if (bad >= 0) {
        fail(&p, (size_t)bad, "invalid UTF-8");
        return NULL;
    }
    JrVal *root = arena_alloc(a, sizeof *root, 16);
    if (!root) {
        fail(&p, 0, "out of memory");
        return NULL;
    }
    if (value(&p, root)) return NULL;
    ws(&p);
    if (p.i != n) {
        fail(&p, p.i, "trailing characters");
        return NULL;
    }
    return root;
}

int jr_key_is(const JrMember *m, const char *key) {
    size_t k = strlen(key);
    return m->key_n == k && memcmp(m->key, key, k) == 0;
}

const JrVal *jr_get(const JrVal *obj, const char *key) {
    if (!obj || obj->type != JR_OBJ) return NULL;
    for (size_t i = 0; i < obj->n; i++)
        if (jr_key_is(&obj->members[i], key)) return &obj->members[i].val;
    return NULL;
}
