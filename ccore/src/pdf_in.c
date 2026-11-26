/* The first page of a vector PDF as polylines (ticket L1). See pdf_in.h; Rust:
 * crates/bimble-drawing/src/reference.rs:322-558 over lopdf 0.47 (reader.rs, parser/mod.rs,
 * document.rs). */
#pragma STDC FP_CONTRACT OFF /* the floats Rust computes, no fused multiply-add */
#include "pdf_in.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "arena.h"
#include "fmt.h"
#include "inflate.h"
#include "strtod.h"

#define MAX_NEST 100          /* lopdf reader::MAX_NESTING_DEPTH */
#define MAX_BRACKET 100       /* lopdf reader::MAX_BRACKET: nested ( ) in a string */
#define MAX_OBJECTS 1000000   /* lopdf MAX_RECONSTRUCTED_OBJECTS, also the table's cap here */
#define MAX_SECTIONS 64       /* cross-reference sections followed through /Prev */
#define DEREF_LIMIT 128       /* lopdf Document::DEREF_LIMIT */
#define Q_STACK 64            /* reference.rs: q pushes while the stack is under 64 */

/* ---- objects ---- */

enum { O_NULL, O_BOOL, O_INT, O_REAL, O_NAME, O_STR, O_ARR, O_DICT, O_REF, O_STREAM };

typedef struct PObj PObj;
typedef struct {
    const unsigned char *k; /* a name's bytes (escapes decoded) */
    size_t kn;
    PObj *v;
} PKey;

struct PObj {
    int t;
    int64_t i;              /* O_INT, O_BOOL */
    float r;                /* O_REAL (lopdf Object::Real(f32)) */
    const unsigned char *s; /* O_NAME, O_STR bytes (a string's escapes are not decoded: unused) */
    size_t n;               /* O_NAME, O_STR bytes; O_ARR items; O_DICT, O_STREAM keys */
    PObj *items;            /* O_ARR */
    PKey *keys;             /* O_DICT, O_STREAM: the dictionary */
    uint32_t num;           /* O_REF */
    uint32_t gen;
    const unsigned char *data; /* O_STREAM: its bytes as in the file */
    size_t ndata;
};

typedef struct {
    uint32_t num;
    int type;          /* 0 free, 1 at an offset, 2 in an object stream */
    uint64_t a, b;     /* offset, generation; or container, index */
    uint64_t seq;      /* the order entries win in: lowest first */
} XEntry;

typedef struct {
    const unsigned char *b; /* the file from "%PDF-" */
    size_t n;
    Arena *a;
    /* the cross-reference table, sorted by number after loading */
    XEntry *x;
    size_t nx, capx;
    int x_sorted; /* sorted by number, one entry each (x_finish) */
    uint64_t seq;
    PObj trailer;
    int has_trailer;
    /* the objects loaded: number -> object (generation ignored, as lopdf keys by (num, gen)
     * but a file has one live generation per number) */
    uint32_t *onum;
    PObj **oval;
    size_t nobj, capobj;
    int oom;
    int depth_left; /* bounds the recursion of /Length lookups */
} Doc;

static const PObj NULL_OBJ = {O_NULL, 0, 0, NULL, 0, NULL, NULL, 0, 0, NULL, 0};

static int is_ws(unsigned c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0 || c == 0x0C; }
static int is_delim(unsigned c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}
static int is_regular(unsigned c) { return !is_ws(c) && !is_delim(c); }
static int is_digit(unsigned c) { return c >= '0' && c <= '9'; }

/* ---- the parser (lopdf parser/mod.rs) ---- */

typedef struct {
    const unsigned char *b;
    size_t n, pos;
    Arena *a;
    int *oom;
} P;

static int at(const P *p, size_t k) { return p->pos + k < p->n ? p->b[p->pos + k] : -1; }

static int starts(const P *p, const char *t) {
    size_t k = strlen(t);
    return p->n - p->pos >= k && !memcmp(p->b + p->pos, t, k);
}

static void ws_only(P *p) { /* content_space, white_space */
    while (p->pos < p->n && is_ws(p->b[p->pos])) p->pos++;
}

static int eol(P *p) {
    if (starts(p, "\r\n")) return p->pos += 2, 1;
    if (at(p, 0) == '\n' || at(p, 0) == '\r') return p->pos++, 1;
    return 0;
}

static int comment(P *p) { /* % … eol (the eol is required) */
    size_t s = p->pos;
    if (at(p, 0) != '%') return 0;
    p->pos++;
    while (p->pos < p->n && p->b[p->pos] != '\r' && p->b[p->pos] != '\n') p->pos++;
    if (!eol(p)) return p->pos = s, 0;
    return 1;
}

static void space(P *p) { /* white space and comments */
    for (;;) {
        size_t s = p->pos;
        ws_only(p);
        if (at(p, 0) == '%') comment(p);
        if (p->pos == s) return;
    }
}

static void *alloc(P *p, size_t n) {
    void *q = arena_alloc(p->a, n ? n : 1, 8);
    if (!q) *p->oom = 1;
    return q;
}

/* A run of digits as u64; 0 overflow. */
static int digits_u64(const unsigned char *s, size_t n, uint64_t *v) {
    uint64_t x = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned d = s[i] - '0';
        if (x > (UINT64_MAX - d) / 10) return 0;
        x = x * 10 + d;
    }
    *v = x;
    return 1;
}

/* integer: [+-] digit1, i64. */
static int p_integer(P *p, int64_t *v) {
    size_t s = p->pos;
    int neg = 0;
    if (at(p, 0) == '+' || at(p, 0) == '-') neg = at(p, 0) == '-', p->pos++;
    size_t d = p->pos;
    while (p->pos < p->n && is_digit(p->b[p->pos])) p->pos++;
    uint64_t m;
    if (p->pos == d || !digits_u64(p->b + d, p->pos - d, &m)) return p->pos = s, 0;
    if (neg) {
        if (m > (uint64_t)INT64_MAX + 1) return p->pos = s, 0;
        *v = m == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)m;
    } else {
        if (m > (uint64_t)INT64_MAX) return p->pos = s, 0;
        *v = (int64_t)m;
    }
    return 1;
}

/* real: [+-] (digit1 '.' digit0 | '.' digit1), then digits or '-' (a '-' inside the fraction is
 * dropped, as lopdf does); the value is f32::from_str of the text. */
static int p_real(P *p, float *v) {
    size_t s = p->pos;
    if (at(p, 0) == '+' || at(p, 0) == '-') p->pos++;
    size_t d = p->pos;
    while (p->pos < p->n && is_digit(p->b[p->pos])) p->pos++;
    if (p->pos > d) {
        if (at(p, 0) != '.') return p->pos = s, 0;
        p->pos++;
    } else {
        if (at(p, 0) != '.' || at(p, 1) < 0 || !is_digit((unsigned)at(p, 1))) return p->pos = s, 0;
        p->pos++;
    }
    int dash = 0;
    while (p->pos < p->n && (is_digit(p->b[p->pos]) || p->b[p->pos] == '-')) dash |= p->b[p->pos++] == '-';
    char tmp[128];
    size_t k = 0, len = p->pos - s;
    const unsigned char *t = p->b + s;
    /* f32::from_str: long texts are exact in cc_strtod; the f64 is then rounded to f32. */
    if (len >= sizeof tmp) {
        /* Very long numbers: keep the sign, the digits and the point; any count works for
         * cc_strtod, but the stack copy is bounded: parse from the arena. */
        char *big = alloc(p, len);
        if (!big) return p->pos = s, 0;
        for (size_t i = 0; i < len; i++)
            if (!(dash && i > 0 && t[i] == '-')) big[k++] = (char)t[i];
        *v = (float)cc_strtod(big, k);
        return 1;
    }
    for (size_t i = 0; i < len; i++)
        if (!(dash && i > 0 && t[i] == '-')) tmp[k++] = (char)t[i];
    *v = (float)cc_strtod(tmp, k);
    return 1;
}

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int p_name(P *p, PObj *o) {
    if (at(p, 0) != '/') return 0;
    p->pos++;
    size_t s = p->pos, esc = 0;
    for (;;) {
        while (p->pos < p->n && p->b[p->pos] != '#' && is_regular(p->b[p->pos])) p->pos++;
        if (at(p, 0) == '#' && hexv(at(p, 1)) >= 0 && hexv(at(p, 2)) >= 0) {
            p->pos += 3, esc++;
            continue;
        }
        break;
    }
    o->t = O_NAME;
    if (!esc) {
        o->s = p->b + s, o->n = p->pos - s;
        return 1;
    }
    unsigned char *d = alloc(p, p->pos - s);
    if (!d) return 0;
    size_t k = 0;
    for (size_t i = s; i < p->pos;)
        if (p->b[i] == '#') d[k++] = (unsigned char)(hexv(p->b[i + 1]) << 4 | hexv(p->b[i + 2])), i += 3;
        else d[k++] = p->b[i++];
    o->s = d, o->n = k;
    return 1;
}

/* A literal string: balanced parentheses (at most MAX_BRACKET deep), backslash escapes. */
static int p_literal(P *p, PObj *o) {
    if (at(p, 0) != '(') return 0;
    size_t s = p->pos++;
    int depth = 1;
    while (p->pos < p->n) {
        unsigned c = p->b[p->pos];
        if (c == '\\') {
            p->pos += p->pos + 1 < p->n ? 2 : 1;
            continue;
        }
        if (c == '(') {
            if (depth > MAX_BRACKET) return p->pos = s, 0;
            depth++;
        } else if (c == ')' && !--depth) {
            p->pos++;
            o->t = O_STR, o->s = p->b + s + 1, o->n = p->pos - s - 2;
            return 1;
        }
        p->pos++;
    }
    return p->pos = s, 0;
}

static int p_hexstr(P *p, PObj *o) {
    if (at(p, 0) != '<') return 0;
    size_t s = p->pos++;
    for (;;) {
        ws_only(p);
        if (p->pos < p->n && hexv(p->b[p->pos]) >= 0) {
            p->pos++;
            continue;
        }
        break;
    }
    if (at(p, 0) != '>') return p->pos = s, 0;
    p->pos++;
    o->t = O_STR, o->s = p->b + s, o->n = p->pos - s;
    return 1;
}

static int p_object(P *p, int depth, PObj *o);

/* lopdf Dictionary::set for every pair, in the document's order: a key again replaces its value,
 * in the first one's place. A linear search per key was quadratic (100,000 keys: 12.5 s); past
 * DICT_SMALL keys the pairs are sorted once by (key, position) instead. 0, -1 out of memory. */
#define DICT_SMALL 32
typedef struct {
    const unsigned char *k;
    size_t kn, at;
} KeyAt;
static int key_at_cmp(const void *a, const void *b) {
    const KeyAt *x = a, *y = b;
    size_t m = x->kn < y->kn ? x->kn : y->kn;
    int c = m ? memcmp(x->k, y->k, m) : 0;
    if (c) return c;
    if (x->kn != y->kn) return x->kn < y->kn ? -1 : 1;
    return x->at < y->at ? -1 : x->at > y->at;
}
static int dict_dedup(PKey *keys, size_t *n) {
    size_t out = 0;
    if (*n <= DICT_SMALL) {
        for (size_t i = 0; i < *n; i++) {
            size_t j = 0;
            while (j < out && !(keys[j].kn == keys[i].kn && !memcmp(keys[j].k, keys[i].k, keys[i].kn))) j++;
            if (j < out) keys[j].v = keys[i].v;
            else keys[out++] = keys[i];
        }
        *n = out;
        return 0;
    }
    KeyAt *s = malloc(*n * sizeof *s);
    unsigned char *drop = calloc(*n, 1);
    if (!s || !drop) return free(s), free(drop), -1;
    for (size_t i = 0; i < *n; i++) s[i].k = keys[i].k, s[i].kn = keys[i].kn, s[i].at = i;
    qsort(s, *n, sizeof *s, key_at_cmp);
    for (size_t i = 0; i < *n;) {
        size_t e = i + 1;
        while (e < *n && s[e].kn == s[i].kn && !memcmp(s[e].k, s[i].k, s[i].kn)) e++;
        keys[s[i].at].v = keys[s[e - 1].at].v; /* the first place, the last value */
        for (size_t k = i + 1; k < e; k++) drop[s[k].at] = 1;
        i = e;
    }
    for (size_t i = 0; i < *n; i++)
        if (!drop[i]) keys[out++] = keys[i];
    *n = out;
    free(s), free(drop);
    return 0;
}

/* Growable arrays of the parser, in the arena (doubling; the old copy is left behind). */
static int grow(P *p, void **v, size_t *cap, size_t n, size_t size) {
    if (n < *cap) return 0;
    size_t c = *cap ? *cap * 2 : 8;
    void *w = alloc(p, c * size);
    if (!w) return -1;
    if (n) memcpy(w, *v, n * size);
    *v = w, *cap = c;
    return 0;
}

/* << (name object)* >> */
static int p_dict(P *p, int depth, PObj *o) {
    if (!starts(p, "<<")) return 0;
    size_t s = p->pos;
    p->pos += 2;
    space(p);
    PKey *keys = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        PObj k;
        size_t ks = p->pos;
        if (!p_name(p, &k)) break;
        space(p);
        PObj *v = alloc(p, sizeof *v);
        if (!v) return p->pos = s, 0;
        if (!p_object(p, depth, v)) {
            p->pos = ks;
            break;
        }
        if (grow(p, (void **)&keys, &cap, n, sizeof *keys)) return p->pos = s, 0;
        keys[n].k = k.s, keys[n].kn = k.n, keys[n].v = v;
        n++;
    }
    if (!starts(p, ">>")) return p->pos = s, 0;
    if (dict_dedup(keys, &n)) return *p->oom = 1, p->pos = s, 0;
    p->pos += 2;
    memset(o, 0, sizeof *o);
    o->t = O_DICT, o->keys = keys, o->n = n;
    return 1;
}

static int p_array(P *p, int depth, PObj *o) {
    if (at(p, 0) != '[') return 0;
    size_t s = p->pos++;
    space(p);
    PObj *items = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        PObj v;
        if (!p_object(p, depth, &v)) break;
        if (grow(p, (void **)&items, &cap, n, sizeof *items)) return p->pos = s, 0;
        items[n++] = v;
    }
    if (at(p, 0) != ']') return p->pos = s, 0;
    p->pos++;
    memset(o, 0, sizeof *o);
    o->t = O_ARR, o->items = items, o->n = n;
    return 1;
}

/* unsigned_int space unsigned_int space 'R' */
static int p_ref(P *p, PObj *o) {
    size_t s = p->pos, d = p->pos;
    uint64_t a, g;
    while (p->pos < p->n && is_digit(p->b[p->pos])) p->pos++;
    if (p->pos == d || !digits_u64(p->b + d, p->pos - d, &a) || a > UINT32_MAX) return p->pos = s, 0;
    space(p);
    d = p->pos;
    while (p->pos < p->n && is_digit(p->b[p->pos])) p->pos++;
    if (p->pos == d || !digits_u64(p->b + d, p->pos - d, &g) || g > 65535) return p->pos = s, 0;
    space(p);
    if (at(p, 0) != 'R') return p->pos = s, 0;
    p->pos++;
    memset(o, 0, sizeof *o);
    o->t = O_REF, o->num = (uint32_t)a, o->gen = (uint32_t)g;
    return 1;
}

/* _direct_objects by the first byte, then `space` (lopdf _direct_object). */
static int p_object(P *p, int depth, PObj *o) {
    if (depth <= 0 || *p->oom) return 0;
    depth--;
    memset(o, 0, sizeof *o);
    int c = at(p, 0), ok = 0;
    if (c == 'n') {
        if ((ok = starts(p, "null"))) p->pos += 4, o->t = O_NULL;
    } else if (c == 't' || c == 'f') {
        if (starts(p, "true")) p->pos += 4, o->t = O_BOOL, o->i = 1, ok = 1;
        else if (starts(p, "false")) p->pos += 5, o->t = O_BOOL, ok = 1;
    } else if (c >= '0' && c <= '9') {
        ok = p_ref(p, o);
        if (!ok && (ok = p_real(p, &o->r))) o->t = O_REAL;
        if (!ok && (ok = p_integer(p, &o->i))) o->t = O_INT;
    } else if (c == '+' || c == '-' || c == '.') {
        if ((ok = p_real(p, &o->r))) o->t = O_REAL;
        else if ((ok = p_integer(p, &o->i))) o->t = O_INT;
    } else if (c == '/') ok = p_name(p, o);
    else if (c == '(') ok = p_literal(p, o);
    else if (c == '<') ok = p_hexstr(p, o) || p_dict(p, depth, o);
    else if (c == '[') ok = p_array(p, depth, o);
    if (ok) space(p);
    return ok;
}

/* ---- dictionaries and the object table ---- */

static PObj *dget(const PObj *d, const char *key) {
    if (!d || (d->t != O_DICT && d->t != O_STREAM)) return NULL;
    size_t kn = strlen(key);
    for (size_t i = 0; i < d->n; i++)
        if (d->keys[i].kn == kn && !memcmp(d->keys[i].k, key, kn)) return d->keys[i].v;
    return NULL;
}

static int name_is(const PObj *o, const char *s) {
    return o && o->t == O_NAME && o->n == strlen(s) && !memcmp(o->s, s, o->n);
}

static PObj *obj_by_num(const Doc *d, uint32_t num) { /* objects.get */
    size_t lo = 0, hi = d->nobj;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (d->onum[m] < num) lo = m + 1;
        else hi = m;
    }
    return lo < d->nobj && d->onum[lo] == num ? d->oval[lo] : NULL;
}

/* lopdf Document::dereference: follows references (at most DEREF_LIMIT); NULL when missing. */
static PObj *deref(const Doc *d, PObj *o) {
    for (int k = 0; o && o->t == O_REF; k++) {
        if (k >= DEREF_LIMIT) return NULL;
        o = obj_by_num(d, o->num);
    }
    return o;
}

static PObj *dget_deref(const Doc *d, const PObj *dict, const char *key) { return deref(d, dget(dict, key)); }

static int as_num(const PObj *o, double *v) { /* reference.rs num */
    if (!o) return 0;
    if (o->t == O_INT) return *v = (double)o->i, 1;
    if (o->t == O_REAL) return *v = (double)o->r, 1;
    return 0;
}

/* ---- streams ---- */

/* PNG predictors (10-15) of FlateDecode's DecodeParms, in place over `b` (rows of 1 + columns
 * bytes): the decoded rows replace it. -1 damaged. */
static int png_unpredict(Buf *b, size_t colors, size_t bpc, size_t columns) {
    size_t bpp = (colors * bpc + 7) / 8, row = (colors * bpc * columns + 7) / 8;
    if (!bpp) bpp = 1;
    if (!row) return -1;
    size_t nrows = b->len / (row + 1);
    unsigned char *prev = calloc(row, 1), *out = (unsigned char *)b->p;
    if (!prev) return -1;
    size_t w = 0;
    for (size_t r = 0; r < nrows; r++) {
        const unsigned char *in = (const unsigned char *)b->p + r * (row + 1);
        int f = in[0];
        in++;
        unsigned char *cur = out + w;
        for (size_t i = 0; i < row; i++) {
            unsigned a = i >= bpp ? cur[i - bpp] : 0, up = prev[i], c = i >= bpp ? prev[i - bpp] : 0, x = in[i];
            switch (f) {
            case 0: break;
            case 1: x += a; break;
            case 2: x += up; break;
            case 3: x += (a + up) / 2; break;
            case 4: {
                int pa = (int)up - (int)c, pb = (int)a - (int)c, pc = pa + pb;
                pa = pa < 0 ? -pa : pa, pb = pb < 0 ? -pb : pb, pc = pc < 0 ? -pc : pc;
                x += pa <= pb && pa <= pc ? a : pb <= pc ? up : c;
                break;
            }
            default: free(prev); return -1;
            }
            cur[i] = (unsigned char)x; /* w <= r * (row + 1): never ahead of the input */
        }
        memcpy(prev, cur, row);
        w += row;
    }
    free(prev);
    b->len = w;
    return 0;
}

static int64_t int_or(const Doc *d, const PObj *dict, const char *key, int64_t dflt) {
    PObj *o = dget_deref(d, dict, key);
    return o && o->t == O_INT ? o->i : dflt;
}

/* Stream::decompressed_content_with_limit: no /Filter: the bytes as they are; FlateDecode (one
 * or a chain of them), with a PNG predictor. 0 ok (appended to out), -1 not decodable, -2 over
 * the limit. */
static int stream_decode(const Doc *d, const PObj *s, size_t limit, Buf *out) {
    PObj *f = dget_deref(d, s, "Filter");
    if (!f) {
        if (s->ndata > limit) return -2;
        buf_append(out, s->data, s->ndata);
        return out->oom ? -1 : 0;
    }
    size_t nf = f->t == O_ARR ? f->n : 1;
    if (nf > 8) return -1;
    Buf cur, next;
    buf_init(&cur), buf_init(&next);
    buf_append(&cur, s->data, s->ndata);
    PObj *parms = dget_deref(d, s, "DecodeParms");
    int rc = 0;
    for (size_t k = 0; k < nf && !rc; k++) {
        PObj *fk = f->t == O_ARR ? deref(d, &f->items[k]) : f;
        PObj *pk = parms && parms->t == O_ARR ? (k < parms->n ? deref(d, &parms->items[k]) : NULL) : parms;
        if (!name_is(fk, "FlateDecode") && !name_is(fk, "Fl")) {
            rc = -1;
            break;
        }
        const char *why;
        buf_clear(&next);
        if (cc_zinflate((const unsigned char *)cur.p, cur.len, limit, &next, &why)) {
            rc = why && !strcmp(why, "gzip: decompressed data too large") ? -2 : -1; /* inflate.c put_room */
            break;
        }
        int64_t pred = pk && pk->t == O_DICT ? int_or(d, pk, "Predictor", 1) : 1;
        if (pred >= 10) {
            int64_t colors = int_or(d, pk, "Colors", 1), bpc = int_or(d, pk, "BitsPerComponent", 8),
                    cols = int_or(d, pk, "Columns", 1);
            if (colors < 1 || colors > 32 || bpc < 1 || bpc > 16 || cols < 1 || cols > (1 << 24) ||
                png_unpredict(&next, (size_t)colors, (size_t)bpc, (size_t)cols))
                rc = -1;
        } else if (pred != 1) rc = -1; /* TIFF predictor 2: not read */
        Buf t = cur;
        cur = next, next = t;
    }
    if (!rc) {
        buf_append(out, cur.p, cur.len);
        if (out->oom) rc = -1;
    }
    buf_free(&cur), buf_free(&next);
    return rc;
}

/* ---- loading ---- */

static size_t find_fwd(const unsigned char *b, size_t n, size_t from, const char *t) {
    size_t k = strlen(t);
    for (size_t i = from; i + k <= n; i++)
        if (b[i] == (unsigned char)t[0] && !memcmp(b + i, t, k)) return i;
    return (size_t)-1;
}

static size_t find_back(const unsigned char *b, size_t n, const char *t) {
    size_t k = strlen(t);
    if (n < k) return (size_t)-1;
    for (size_t i = n - k + 1; i-- > 0;)
        if (b[i] == (unsigned char)t[0] && !memcmp(b + i, t, k)) return i;
    return (size_t)-1;
}

static int load_object_at(Doc *d, size_t off, size_t bound, uint32_t *num, PObj **out);

/* The table's entry of an object: the winning one (lowest seq) before x_finish. */
static const XEntry *x_find(const Doc *d, uint32_t num) {
    if (d->x_sorted) {
        size_t lo = 0, hi = d->nx;
        while (lo < hi) {
            size_t m = lo + (hi - lo) / 2;
            if (d->x[m].num < num) lo = m + 1;
            else hi = m;
        }
        return lo < d->nx && d->x[lo].num == num ? &d->x[lo] : NULL;
    }
    const XEntry *best = NULL;
    for (size_t i = 0; i < d->nx; i++)
        if (d->x[i].num == num && (!best || d->x[i].seq < best->seq)) best = &d->x[i];
    return best;
}

/* The stream after its dictionary: /Length (direct, or an object loaded on the way), its bytes,
 * then [eol] "endstream"; else the single "endstream" + "endobj" before `bound` (lopdf
 * recover_stream_length). */
static int stream_body(Doc *d, P *p, PObj *o, size_t bound) {
    PObj *len = dget(o, "Length");
    int64_t L = -1;
    if (len && len->t == O_INT) L = len->i;
    else if (len && len->t == O_REF && d->depth_left > 0) {
        PObj *v = obj_by_num(d, len->num);
        if (!v) { /* not loaded yet: load it from its table entry */
            const XEntry *e = x_find(d, len->num);
            if (e && e->type == 1 && e->a < d->n) {
                uint32_t nn;
                d->depth_left--;
                if (load_object_at(d, (size_t)e->a, d->n, &nn, &v) || nn != len->num) v = NULL;
                d->depth_left++;
            }
        }
        if (v && v->t == O_INT) L = v->i;
    }
    size_t start = p->pos;
    if (L >= 0 && (uint64_t)L <= p->n - start) {
        P q = *p;
        q.pos = start + (size_t)L;
        eol(&q);
        if (starts(&q, "endstream")) {
            o->data = p->b + start, o->ndata = (size_t)L;
            p->pos = q.pos + 9;
            return 1;
        }
    }
    /* recover: one "endstream" after an eol, then space "endobj", before the bound */
    size_t end = bound < p->n ? bound : p->n, found = (size_t)-1, after = 0;
    for (size_t i = find_fwd(p->b, end, start, "endstream"); i != (size_t)-1; i = find_fwd(p->b, end, i + 1, "endstream")) {
        size_t de;
        if (i >= start + 2 && p->b[i - 2] == '\r' && p->b[i - 1] == '\n') de = i - 2;
        else if (i >= start + 1 && (p->b[i - 1] == '\n' || p->b[i - 1] == '\r')) de = i - 1;
        else continue;
        P q = *p;
        q.pos = i + 9;
        space(&q);
        if (!starts(&q, "endobj")) continue;
        if (q.pos + 6 < q.n && !is_ws(q.b[q.pos + 6])) continue;
        if (found != (size_t)-1) return 0; /* ambiguous */
        found = de, after = i + 9;
    }
    if (found == (size_t)-1) return 0;
    o->data = p->b + start, o->ndata = found - start;
    p->pos = after;
    return 1;
}

/* "N G obj" object [stream] ["endobj"] at `off`. 0 ok, -1 not an object there. */
static int load_object_at(Doc *d, size_t off, size_t bound, uint32_t *num, PObj **out) {
    P p = {d->b, d->n, off, d->a, &d->oom};
    space(&p);
    size_t s = p.pos;
    uint64_t a, g;
    while (p.pos < p.n && is_digit(p.b[p.pos])) p.pos++;
    if (p.pos == s || !digits_u64(p.b + s, p.pos - s, &a) || a > UINT32_MAX) return -1;
    space(&p);
    s = p.pos;
    while (p.pos < p.n && is_digit(p.b[p.pos])) p.pos++;
    if (p.pos == s || !digits_u64(p.b + s, p.pos - s, &g) || g > 65535) return -1;
    space(&p);
    if (!starts(&p, "obj")) return -1;
    p.pos += 3;
    space(&p);
    PObj *o = alloc(&p, sizeof *o);
    if (!o) return -1;
    if (starts(&p, "<<")) {
        if (!p_dict(&p, MAX_NEST, o)) return -1;
        /* stream_keyword: space "stream" space0 eol */
        P q = p;
        space(&q);
        if (starts(&q, "stream")) {
            q.pos += 6;
            while (q.pos < q.n && (q.b[q.pos] == ' ' || q.b[q.pos] == '\t')) q.pos++;
            if (eol(&q)) {
                o->t = O_STREAM;
                if (!stream_body(d, &q, o, bound)) return -1;
                p = q;
            }
        }
    } else if (!p_object(&p, MAX_NEST, o)) return -1;
    *num = (uint32_t)a;
    *out = o;
    return 0;
}

static int add_obj(Doc *d, uint32_t num, PObj *o) {
    if (d->nobj == d->capobj) {
        size_t c = d->capobj ? d->capobj * 2 : 64;
        uint32_t *nn = realloc(d->onum, c * sizeof *nn);
        if (!nn) return d->oom = 1, -1;
        d->onum = nn;
        PObj **vv = realloc(d->oval, c * sizeof *vv);
        if (!vv) return d->oom = 1, -1;
        d->oval = vv, d->capobj = c;
    }
    d->onum[d->nobj] = num, d->oval[d->nobj] = o, d->nobj++;
    return 0;
}

/* An entry; the first one added for a number wins (sections are read newest first), unless
 * `seq` says otherwise (x_finish keeps the lowest). */
static int add_x_seq(Doc *d, uint32_t num, int type, uint64_t a, uint64_t b, uint64_t seq) {
    if (d->nx >= 4 * (size_t)MAX_OBJECTS) return 0;
    if (d->nx == d->capx) {
        size_t c = d->capx ? d->capx * 2 : 64;
        XEntry *x = realloc(d->x, c * sizeof *x);
        if (!x) return d->oom = 1, -1;
        d->x = x, d->capx = c;
    }
    d->x[d->nx].num = num, d->x[d->nx].type = type, d->x[d->nx].a = a, d->x[d->nx].b = b, d->x[d->nx].seq = seq;
    d->nx++;
    return 0;
}

static int add_x(Doc *d, uint32_t num, int type, uint64_t a, uint64_t b) { return add_x_seq(d, num, type, a, b, d->seq++); }

/* A classic table at p ("xref" … "trailer" <<…>>). 0 ok, -1 not one. */
static int read_xref_table(Doc *d, size_t off, PObj *trailer) {
    P p = {d->b, d->n, off, d->a, &d->oom};
    if (!starts(&p, "xref")) return -1;
    p.pos += 4;
    if (at(&p, 0) == ' ') p.pos++;
    if (!eol(&p)) return -1;
    int sections = 0;
    for (;; sections++) {
        P q = p;
        uint64_t start, count;
        size_t s = q.pos;
        while (q.pos < q.n && is_digit(q.b[q.pos])) q.pos++;
        if (q.pos == s || !digits_u64(q.b + s, q.pos - s, &start)) break;
        if (at(&q, 0) != ' ') break;
        q.pos++;
        s = q.pos;
        while (q.pos < q.n && is_digit(q.b[q.pos])) q.pos++;
        if (q.pos == s || !digits_u64(q.b + s, q.pos - s, &count)) break;
        if (at(&q, 0) == ' ') q.pos++;
        if (!eol(&q)) break;
        p = q;
        for (uint64_t idx = 0;; idx++) { /* many0(xref_entry) */
            q = p;
            uint64_t o, g;
            s = q.pos;
            while (q.pos < q.n && is_digit(q.b[q.pos])) q.pos++;
            if (q.pos == s || !digits_u64(q.b + s, q.pos - s, &o) || at(&q, 0) != ' ') break;
            q.pos++;
            s = q.pos;
            while (q.pos < q.n && is_digit(q.b[q.pos])) q.pos++;
            if (q.pos == s || !digits_u64(q.b + s, q.pos - s, &g) || g > UINT32_MAX || at(&q, 0) != ' ') break;
            q.pos++;
            int k = at(&q, 0);
            if (k != 'n' && k != 'f') break;
            q.pos++;
            if (starts(&q, " \r") || starts(&q, " \n") || starts(&q, "\r\n")) q.pos += 2;
            else if (at(&q, 0) == '\n' || at(&q, 0) == '\r') q.pos++;
            else break;
            p = q;
            uint64_t num = start + idx;
            if (num <= UINT32_MAX && (k == 'f' || g <= 65535)) add_x(d, (uint32_t)num, k == 'n' ? 1 : 0, o, g);
            if (idx > MAX_OBJECTS) break;
        }
    }
    if (!sections) return -1;
    space(&p);
    if (!starts(&p, "trailer")) return -1;
    p.pos += 7;
    space(&p);
    if (!p_dict(&p, MAX_NEST, trailer)) return -1;
    PObj *size = dget(trailer, "Size");
    if (!size || size->t != O_INT) return -1;
    return 0;
}

/* A cross-reference stream object at off. 0 ok (its dictionary is the trailer), -1 not one. */
static int read_xref_stream(Doc *d, size_t off, PObj *trailer) {
    uint32_t num;
    PObj *o;
    if (load_object_at(d, off, d->n, &num, &o) || o->t != O_STREAM) return -1;
    PObj *w = dget(o, "W"), *size = dget(o, "Size"), *index = dget(o, "Index");
    if (!w || w->t != O_ARR || w->n < 3 || !size || size->t != O_INT) return -1;
    int64_t W[3];
    for (int k = 0; k < 3; k++) {
        if (w->items[k].t != O_INT || w->items[k].i < 0 || w->items[k].i > 8) return -1;
        W[k] = w->items[k].i;
    }
    size_t row = (size_t)(W[0] + W[1] + W[2]);
    if (!row) return -1;
    Buf b;
    buf_init(&b);
    if (stream_decode(d, o, CC_REF_MAX_FILE_BYTES, &b)) return buf_free(&b), -1;
    size_t nrows = b.len / row, r = 0;
    size_t nidx = index && index->t == O_ARR ? index->n / 2 : 1;
    for (size_t k = 0; k < nidx; k++) {
        int64_t first = 0, count = size->i;
        if (index && index->t == O_ARR) {
            if (index->items[2 * k].t != O_INT || index->items[2 * k + 1].t != O_INT) break;
            first = index->items[2 * k].i, count = index->items[2 * k + 1].i;
        }
        if (first < 0 || count < 0) break;
        for (int64_t i = 0; i < count && r < nrows; i++, r++) {
            const unsigned char *e = (const unsigned char *)b.p + r * row;
            uint64_t f[3] = {W[0] ? 0 : 1, 0, 0};
            size_t q = 0;
            for (int c = 0; c < 3; c++) {
                if (W[c]) f[c] = 0;
                for (int64_t j = 0; j < W[c]; j++) f[c] = f[c] << 8 | e[q++];
            }
            uint64_t n = (uint64_t)first + (uint64_t)i;
            if (n > UINT32_MAX) continue;
            if (f[0] == 0) add_x(d, (uint32_t)n, 0, 0, 0);
            else if (f[0] == 1) add_x(d, (uint32_t)n, 1, f[1], f[2]);
            else if (f[0] == 2) add_x(d, (uint32_t)n, 2, f[1], f[2]);
        }
    }
    buf_free(&b);
    *trailer = *o;
    trailer->t = O_DICT;
    return 0;
}

/* "startxref" N, then the sections through /Prev (and /XRefStm). 0 ok, -1 not readable. */
static int resolve_xref(Doc *d) {
    size_t sx = find_back(d->b, d->n, "startxref");
    if (sx == (size_t)-1) return -1;
    P p = {d->b, d->n, sx + 9, d->a, &d->oom};
    if (at(&p, 0) == ' ') p.pos++;
    if (!eol(&p)) return -1;
    while (at(&p, 0) == ' ') p.pos++;
    int64_t off;
    if (!p_integer(&p, &off) || off < 0 || (uint64_t)off >= d->n) return -1;
    size_t offs[MAX_SECTIONS];
    int nsec = 0;
    for (;;) {
        for (int k = 0; k < nsec; k++)
            if (offs[k] == (size_t)off) return d->has_trailer ? 0 : -1; /* a /Prev loop */
        if (nsec == MAX_SECTIONS) break;
        offs[nsec++] = (size_t)off;
        PObj t;
        memset(&t, 0, sizeof t);
        P q = {d->b, d->n, (size_t)off, d->a, &d->oom};
        space(&q);
        int r = read_xref_table(d, q.pos, &t);
        if (r) r = read_xref_stream(d, q.pos, &t);
        if (r) return nsec == 1 ? -1 : 0;
        if (!d->has_trailer) d->trailer = t, d->has_trailer = 1;
        PObj *stm = dget(&t, "XRefStm");
        if (stm && stm->t == O_INT && stm->i >= 0 && (uint64_t)stm->i < d->n) {
            PObj t2;
            read_xref_stream(d, (size_t)stm->i, &t2);
        }
        PObj *prev = dget(&t, "Prev");
        if (!prev || prev->t != O_INT || prev->i < 0 || (uint64_t)prev->i >= d->n) break;
        off = prev->i;
    }
    return 0;
}

/* lopdf reconstruct_xref_and_trailer: every "N G obj" at a line start (streams skipped), the
 * last such header of each number wins; the trailer: the last "trailer" dictionary with /Root,
 * else the last cross-reference stream's, else {Root: the last /Catalog}. */
static int reconstruct(Doc *d) {
    d->nx = 0, d->x_sorted = 0;
    int line = 1;
    for (size_t pos = 0; pos < d->n;) {
        const unsigned char *b = d->b;
        if (d->n - pos >= 6 && !memcmp(b + pos, "stream", 6) && !(pos >= 3 && !memcmp(b + pos - 3, "end", 3)) &&
            pos + 6 < d->n && (b[pos + 6] == '\r' || b[pos + 6] == '\n')) {
            size_t e = find_fwd(b, d->n, pos + 6, "endstream");
            if (e != (size_t)-1) {
                pos = e + 9, line = 0;
                continue;
            }
        }
        if (line && is_digit(b[pos])) {
            P p = {b, d->n, pos, d->a, &d->oom};
            uint64_t a, g;
            size_t s = p.pos;
            while (p.pos < p.n && is_digit(b[p.pos])) p.pos++;
            if (digits_u64(b + s, p.pos - s, &a) && a <= MAX_OBJECTS) {
                space(&p);
                s = p.pos;
                while (p.pos < p.n && is_digit(b[p.pos])) p.pos++;
                if (p.pos > s && digits_u64(b + s, p.pos - s, &g) && g <= 65535) {
                    space(&p);
                    if (starts(&p, "obj") && add_x_seq(d, (uint32_t)a, 1, pos, g, UINT64_MAX - pos)) return -1;
                }
            }
        }
        unsigned c = b[pos];
        if (c == '\r' || c == '\n') line = 1;
        else if (c != ' ' && c != '\t') line = 0;
        pos++;
    }
    if (!d->nx) return -1;
    for (size_t t = find_back(d->b, d->n, "trailer"); t != (size_t)-1; t = t ? find_back(d->b, t, "trailer") : (size_t)-1) {
        P p = {d->b, d->n, t + 7, d->a, &d->oom};
        space(&p);
        PObj tr;
        if (p_dict(&p, MAX_NEST, &tr) && dget(&tr, "Root")) {
            d->trailer = tr, d->has_trailer = 1;
            return 0;
        }
    }
    return 1; /* the caller looks for a cross-reference stream or a catalog among the objects */
}

static int cmp_x(const void *a, const void *b) {
    const XEntry *x = a, *y = b;
    if (x->num != y->num) return x->num < y->num ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* Sorted by number, the winning entry of each kept. */
static void x_finish(Doc *d) {
    if (d->nx) qsort(d->x, d->nx, sizeof *d->x, cmp_x);
    size_t k = 0;
    for (size_t i = 0; i < d->nx; i++)
        if (!k || d->x[k - 1].num != d->x[i].num) d->x[k++] = d->x[i];
    d->nx = k;
    if (d->nx > MAX_OBJECTS) d->nx = MAX_OBJECTS;
    d->x_sorted = 1;
}

typedef struct {
    uint32_t num;
    PObj *obj;
} Pair;

static int cmp_pair(const void *a, const void *b) {
    uint32_t x = ((const Pair *)a)->num, y = ((const Pair *)b)->num;
    return x < y ? -1 : x > y;
}

/* Every object of the table: those at offsets, then those in object streams. */
static int load_objects(Doc *d) {
    x_finish(d);
    /* object boundaries: the next offset after each (the recovery scan stays inside it) */
    size_t *offs = malloc((d->nx + 1) * sizeof *offs), no = 0;
    if (!offs) return d->oom = 1, -1;
    for (size_t i = 0; i < d->nx; i++)
        if (d->x[i].type == 1 && d->x[i].a < d->n) offs[no++] = (size_t)d->x[i].a;
    {
        /* sort offsets (shell sort) */
        size_t n = no;
        for (size_t gap = n / 2; gap > 0; gap /= 2)
            for (size_t i = gap; i < n; i++) {
                size_t t = offs[i], j = i;
                while (j >= gap && offs[j - gap] > t) offs[j] = offs[j - gap], j -= gap;
                offs[j] = t;
            }
    }
    for (size_t i = 0; i < d->nx && !d->oom; i++) {
        if (d->x[i].type != 1 || d->x[i].a >= d->n) continue;
        if (obj_by_num(d, d->x[i].num)) continue;
        size_t off = (size_t)d->x[i].a, lo = 0, hi = no;
        while (lo < hi) {
            size_t m = lo + (hi - lo) / 2;
            if (offs[m] <= off) lo = m + 1;
            else hi = m;
        }
        size_t bound = lo < no ? offs[lo] : d->n;
        uint32_t num;
        PObj *o;
        if (load_object_at(d, off, bound, &num, &o) || num != d->x[i].num) continue;
        /* keep the table sorted: insert in place (numbers come in order; Length lookups may
         * have added later ones) */
        if (add_obj(d, num, o)) break;
        for (size_t k = d->nobj - 1; k > 0 && d->onum[k - 1] > d->onum[k]; k--) {
            uint32_t tn = d->onum[k];
            PObj *to = d->oval[k];
            d->onum[k] = d->onum[k - 1], d->oval[k] = d->oval[k - 1];
            d->onum[k - 1] = tn, d->oval[k - 1] = to;
        }
    }
    free(offs);
    /* object streams */
    Pair *more = NULL;
    size_t nmore = 0, capmore = 0;
    uint32_t last_container = UINT32_MAX;
    for (size_t i = 0; i < d->nx && !d->oom; i++) {
        if (d->x[i].type != 2 || d->x[i].a > UINT32_MAX || d->x[i].a == last_container) continue;
        uint32_t cn = (uint32_t)d->x[i].a;
        last_container = cn;
        PObj *c = obj_by_num(d, cn);
        if (!c || c->t != O_STREAM) continue;
        PObj *N = dget_deref(d, c, "N"), *F = dget_deref(d, c, "First");
        if (!N || N->t != O_INT || !F || F->t != O_INT || N->i < 0 || F->i < 0 || N->i > MAX_OBJECTS) continue;
        Buf b;
        buf_init(&b);
        if (stream_decode(d, c, CC_REF_MAX_FILE_BYTES, &b) || (uint64_t)F->i > b.len) {
            buf_free(&b);
            continue;
        }
        /* the decoded bytes live as long as the objects: copy them to the arena */
        unsigned char *data = arena_dup(d->a, b.p, b.len ? b.len : 1);
        size_t dn = b.len;
        buf_free(&b);
        if (!data) return d->oom = 1, -1;
        P p = {data, dn, 0, d->a, &d->oom};
        space(&p);
        for (int64_t k = 0; k < N->i; k++) {
            int64_t on, oo;
            if (!p_integer(&p, &on)) break;
            space(&p);
            if (!p_integer(&p, &oo)) break;
            space(&p);
            if (on < 0 || on > UINT32_MAX || oo < 0 || (uint64_t)F->i + (uint64_t)oo >= dn) continue;
            P q = {data, dn, (size_t)F->i + (size_t)oo, d->a, &d->oom};
            space(&q);
            PObj *o = alloc(&q, sizeof *o);
            if (!o || !p_object(&q, MAX_NEST, o)) continue;
            /* only members the table places in this stream (a free or newer entry wins) */
            size_t lo = 0, hi = d->nx;
            while (lo < hi) {
                size_t m = lo + (hi - lo) / 2;
                if (d->x[m].num < (uint32_t)on) lo = m + 1;
                else hi = m;
            }
            if (lo >= d->nx || d->x[lo].num != (uint32_t)on || d->x[lo].type != 2 || d->x[lo].a != cn) continue;
            if (nmore == capmore) {
                size_t cap = capmore ? capmore * 2 : 64;
                Pair *m = realloc(more, cap * sizeof *m);
                if (!m) return free(more), d->oom = 1, -1;
                more = m, capmore = cap;
            }
            more[nmore].num = (uint32_t)on, more[nmore].obj = o, nmore++;
        }
    }
    for (size_t i = 0; i < nmore; i++)
        if (!obj_by_num(d, more[i].num)) {
            if (add_obj(d, more[i].num, more[i].obj)) break;
            for (size_t k = d->nobj - 1; k > 0 && d->onum[k - 1] > d->onum[k]; k--) {
                uint32_t tn = d->onum[k];
                PObj *to = d->oval[k];
                d->onum[k] = d->onum[k - 1], d->oval[k] = d->oval[k - 1];
                d->onum[k - 1] = tn, d->oval[k - 1] = to;
            }
        }
    free(more);
    return d->oom ? -1 : 0;
}

/* After a rebuild that found no "trailer": the last cross-reference stream's dictionary, else
 * {Root: the last catalog}. */
static void trailer_from_objects(Doc *d) {
    for (size_t i = d->nobj; i-- > 0;) {
        PObj *o = d->oval[i];
        if (o->t == O_STREAM && name_is(dget(o, "Type"), "XRef") && dget(o, "Root")) {
            d->trailer = *o, d->trailer.t = O_DICT, d->has_trailer = 1;
            return;
        }
    }
    for (size_t i = d->nobj; i-- > 0;) {
        PObj *o = d->oval[i];
        if (o->t == O_DICT && name_is(dget(o, "Type"), "Catalog")) {
            PObj *r = arena_alloc(d->a, sizeof *r, 8);
            PKey *k = arena_alloc(d->a, sizeof *k, 8);
            if (!r || !k) return;
            memset(r, 0, sizeof *r);
            r->t = O_REF, r->num = d->onum[i];
            k->k = (const unsigned char *)"Root", k->kn = 4, k->v = r;
            memset(&d->trailer, 0, sizeof d->trailer);
            d->trailer.t = O_DICT, d->trailer.keys = k, d->trailer.n = 1, d->has_trailer = 1;
            return;
        }
    }
}

/* Dictionary::get_type: /Type as a name, else "Linearized" when that key is there. */
static int type_is(const PObj *dict, const char *t) {
    if (!dict || (dict->t != O_DICT && dict->t != O_STREAM)) return -1; /* not a dictionary */
    PObj *ty = dget(dict, "Type");
    if (ty) return ty->t == O_NAME ? name_is(ty, t) : -1;
    return dget(dict, "Linearized") ? !strcmp(t, "Linearized") : -1;
}

/* PageTreeIter: the first page and the count (at most as many steps as objects). */
static PObj *first_page(Doc *d, size_t *count, uint32_t *first_num) {
    *count = 0;
    PObj *cat = deref(d, dget(&d->trailer, "Root"));
    PObj *pr = cat && cat->t == O_DICT ? dget(cat, "Pages") : NULL;
    if (!pr || pr->t != O_REF) return NULL;
    PObj *pages = obj_by_num(d, pr->num);
    pages = deref(d, pages);
    PObj *kids = pages && pages->t == O_DICT ? dget_deref(d, pages, "Kids") : NULL;
    if (!kids || kids->t != O_ARR) return NULL;
    struct {
        PObj *v;
        size_t n;
    } stack[256], cur = {kids->items, kids->n};
    size_t sp = 0, limit = d->nobj;
    PObj *first = NULL;
    for (;;) {
        while (cur.n) {
            if (!limit) return first;
            limit--;
            PObj *kid = cur.v;
            cur.v++, cur.n--;
            if (kid->t != O_REF) continue;
            PObj *kd = deref(d, obj_by_num(d, kid->num));
            int page = type_is(kd, "Page"), node = type_is(kd, "Pages");
            if (page == 1 || page < 0) { /* a page, or a kid that does not load: a stand-in page */
                if (!first) first = kd && (kd->t == O_DICT || kd->t == O_STREAM) ? kd : (PObj *)&NULL_OBJ, *first_num = kid->num;
                ++*count;
            } else if (node == 1 && sp < 256) {
                if (cur.n) stack[sp].v = cur.v, stack[sp].n = cur.n, sp++;
                PObj *k2 = dget_deref(d, kd, "Kids");
                cur.v = k2 && k2->t == O_ARR ? k2->items : NULL;
                cur.n = k2 && k2->t == O_ARR ? k2->n : 0;
            }
        }
        if (!sp) return first;
        cur.v = stack[sp - 1].v, cur.n = stack[sp - 1].n, sp--;
    }
}

/* get_page_resources: the nearest /Resources written as a dictionary, else the first referenced
 * one's dictionary; none when the /Parent chain loops, is too long or does not load. */
static PObj *page_resources(Doc *d, PObj *page) {
    PObj *dict = NULL;
    uint32_t first_id = 0, seen[MAX_NEST];
    int has_id = 0;
    size_t nseen = 0;
    for (PObj *node = page; node;) {
        PObj *r = dget(node, "Resources");
        if (r && r->t == O_REF) {
            if (!has_id) first_id = r->num;
            has_id = 1;
        } else if (r && r->t == O_DICT && !dict && !has_id) dict = r;
        PObj *par = dget(node, "Parent");
        if (!par || par->t != O_REF) break;
        for (size_t i = 0; i < nseen; i++)
            if (seen[i] == par->num) return NULL; /* ReferenceCycle */
        if (nseen >= MAX_NEST) return NULL;        /* RecursionLimit */
        seen[nseen++] = par->num;
        PObj *pd = deref(d, obj_by_num(d, par->num));
        if (!pd || pd->t != O_DICT) return NULL;
        node = pd;
    }
    if (dict) return dict;
    if (has_id) {
        PObj *o = deref(d, obj_by_num(d, first_id));
        if (o && o->t == O_DICT) return o;
    }
    return NULL;
}

/* get_page_content_with_limit: the content streams joined, each followed by '\n'. 0 ok, -2 the
 * limit passed. */
static int page_content(Doc *d, PObj *page, Buf *out) {
    PObj *c = dget(page, "Contents");
    uint32_t ids[1];
    PObj *list = NULL;
    size_t n = 0;
    if (c && c->t == O_REF) { /* a stream, a missing object, or (through references) an array */
        PObj *o = c;
        int hops = 0;
        for (;;) {
            PObj *t = obj_by_num(d, o->num);
            if (!t || t->t == O_STREAM) {
                ids[0] = o->num, n = 1;
                break;
            }
            if (++hops >= DEREF_LIMIT) break;
            if (t->t == O_REF) {
                o = t;
                continue;
            }
            if (t->t == O_ARR) list = t;
            break;
        }
    } else if (c && c->t == O_ARR) list = c;
    size_t count = list ? list->n : n;
    for (size_t k = 0; k < count; k++) {
        uint32_t id;
        if (list) {
            if (list->items[k].t != O_REF) continue;
            id = list->items[k].num;
        } else id = ids[0];
        PObj *s = deref(d, obj_by_num(d, id));
        if (!s || s->t != O_STREAM) continue;
        size_t remaining = CC_REF_MAX_FILE_BYTES > out->len ? CC_REF_MAX_FILE_BYTES - out->len : 0;
        size_t before = out->len;
        int r = stream_decode(d, s, remaining, out);
        if (r == -2) return -2;
        if (r) {
            out->len = before;
            if (s->ndata > remaining) return -2;
            buf_append(out, s->data, s->ndata);
        }
        buf_putc(out, '\n');
        if (out->oom) return -1;
    }
    return 0;
}

/* ---- the content stream (lopdf parser::content) ---- */

typedef struct {
    double v[6]; /* the numbers among the operands (reference.rs `a`), the first 6 kept */
    size_t nnum;
    char op[8];
    size_t opn;
    PObj first; /* the first operand (Do's name) */
    int has_first;
} Op;

/* content operand: null, boolean, real, integer, name, literal, hex, array, dictionary, then
 * white space (no comments). */
static int c_operand(P *p, PObj *o) {
    memset(o, 0, sizeof *o);
    int ok = 0;
    if (starts(p, "null")) p->pos += 4, o->t = O_NULL, ok = 1;
    else if (starts(p, "true")) p->pos += 4, o->t = O_BOOL, o->i = 1, ok = 1;
    else if (starts(p, "false")) p->pos += 5, o->t = O_BOOL, ok = 1;
    else if (p_real(p, &o->r)) o->t = O_REAL, ok = 1;
    else if (p_integer(p, &o->i)) o->t = O_INT, ok = 1;
    else if (p_name(p, o)) ok = 1;
    else if (p_literal(p, o)) ok = 1;
    else if (p_hexstr(p, o)) ok = 1;
    else if (p_array(p, MAX_NEST, o)) ok = 1;
    else if (p_dict(p, MAX_NEST, o)) ok = 1;
    if (ok) ws_only(p);
    return ok;
}

static int is_op_char(unsigned c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '*' || c == '\'' || c == '"';
}

/* One operation. 1 read, 0 the end (or where lopdf's `many0` stops), -1 lopdf fails the whole
 * content (Content::decode gives None: the caller counts it unreadable). */
static int c_operation(P *p, Op *op) {
    size_t s = p->pos;
    while (at(p, 0) == '%') {
        if (!comment(p)) return p->pos = s, 0;
        ws_only(p);
    }
    op->nnum = 0, op->opn = 0, op->has_first = 0;
    if (starts(p, "BI")) { /* inline image: its dictionary, ID, data to white space + EI */
        p->pos += 2;
        ws_only(p);
        for (;;) {
            PObj k, v;
            size_t ks = p->pos;
            if (!p_name(p, &k)) break;
            space(p);
            if (!p_object(p, MAX_NEST, &v)) {
                p->pos = ks;
                break;
            }
        }
        if (!starts(p, "ID")) return -1;
        p->pos += 2;
        if (!eol(p) && p->pos < p->n && is_ws(p->b[p->pos])) p->pos++;
        size_t i;
        for (i = p->pos; i + 3 <= p->n; i++)
            if (is_ws(p->b[i]) && p->b[i + 1] == 'E' && p->b[i + 2] == 'I' && (i + 3 == p->n || is_ws(p->b[i + 3]))) break;
        if (i + 3 > p->n) return -1;
        p->pos = i + 3;
        ws_only(p);
        memcpy(op->op, "BI", 3), op->opn = 2;
        return 1;
    }
    for (;;) {
        PObj o;
        size_t before = p->pos;
        if (!c_operand(p, &o)) break;
        if (*p->oom) return -1;
        if (!op->has_first) op->first = o, op->has_first = 1;
        double v;
        if (as_num(&o, &v)) {
            if (op->nnum < 6) op->v[op->nnum] = v;
            op->nnum++;
        }
        if (p->pos == before) break;
    }
    size_t os = p->pos;
    while (p->pos < p->n && is_op_char(p->b[p->pos])) p->pos++;
    if (p->pos == os || is_digit(p->b[os])) return p->pos = s, 0;
    size_t len = p->pos - os;
    memcpy(op->op, p->b + os, len < sizeof op->op - 1 ? len : sizeof op->op - 1);
    op->opn = len < sizeof op->op - 1 ? len : sizeof op->op - 1;
    op->op[op->opn] = 0;
    if (len >= sizeof op->op - 1) op->op[0] = '~'; /* a long unknown operator: matches none */
    ws_only(p);
    return 1;
}

/* ---- the interpreter (reference.rs:384-558) ---- */

typedef struct {
    double m[6];
} M;

static void m_apply(const M *m, const double p[2], double out[2]) {
    double x = m->m[0] * p[0] + m->m[2] * p[1] + m->m[4];
    double y = m->m[1] * p[0] + m->m[3] * p[1] + m->m[5];
    out[0] = x, out[1] = y;
}

/* `self` applied after `inner` */
static M m_after(const M *self, const M *inner) {
    const double *a = inner->m, *b = self->m;
    M r = {{a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2], a[2] * b[1] + a[3] * b[3],
            a[4] * b[0] + a[5] * b[2] + b[4], a[4] * b[1] + a[5] * b[3] + b[5]}};
    return r;
}

typedef struct {
    double (*p)[2];
    size_t n, cap;
} Line;

static int line_push(Line *l, const double q[2]) {
    if (l->n == l->cap) {
        size_t c = l->cap ? l->cap * 2 : 16;
        if (c > CC_REF_MAX_POINTS + 64) return -1;
        void *w = realloc(l->p, c * sizeof *l->p);
        if (!w) return -1;
        l->p = w, l->cap = c;
    }
    l->p[l->n][0] = q[0], l->p[l->n][1] = q[1];
    l->n++;
    return 0;
}

typedef struct {
    Line *v;
    size_t n, cap;
} Path;

typedef struct {
    Doc *d;
    cc_refread *out;
    size_t skipped;
    int oom;
} Ctx;

static int is(const Op *o, const char *s) { return !strcmp(o->op, s); }

static void path_clear(Path *path) {
    for (size_t i = 0; i < path->n; i++) free(path->v[i].p);
    path->n = 0;
}

/* flush: cur (2 points or more) joins the path; else it is cleared. */
static int flush(Line *cur, Path *path) {
    if (cur->n >= 2) {
        if (path->n == path->cap) {
            size_t c = path->cap ? path->cap * 2 : 8;
            Line *w = realloc(path->v, c * sizeof *w);
            if (!w) return -1;
            path->v = w, path->cap = c;
        }
        path->v[path->n++] = *cur;
        memset(cur, 0, sizeof *cur);
    } else cur->n = 0;
    return 0;
}

static void content(Ctx *x, const unsigned char *data, size_t n, PObj *res, M base, int depth);

/* Do: a form XObject of the resources, drawn with its /Matrix. 1 stop (truncated or oom). */
static int do_form(Ctx *x, const Op *op, PObj *res, const M *ctm, int depth) {
    Doc *d = x->d;
    if (!op->has_first || op->first.t != O_NAME) return 0;
    if (depth >= CC_REF_MAX_DEPTH) {
        x->skipped++;
        return 0;
    }
    PObj *xo = res ? dget_deref(d, res, "XObject") : NULL;
    if (!xo || xo->t != O_DICT) return 0;
    PObj *s = NULL;
    for (size_t i = 0; i < xo->n; i++)
        if (xo->keys[i].kn == op->first.n && !memcmp(xo->keys[i].k, op->first.s, op->first.n)) s = deref(d, xo->keys[i].v);
    if (!s || s->t != O_STREAM) return 0;
    if (!name_is(dget(s, "Subtype"), "Form")) return 0;
    M m = *ctm;
    PObj *mat = dget(s, "Matrix");
    if (mat && mat->t == O_ARR) {
        double v[6];
        size_t k = 0;
        for (size_t i = 0; i < mat->n; i++) {
            double t;
            if (as_num(&mat->items[i], &t)) {
                if (k < 6) v[k] = t;
                k++;
            }
        }
        if (k == 6) {
            M in = {{v[0], v[1], v[2], v[3], v[4], v[5]}};
            m = m_after(ctm, &in);
        }
    }
    PObj *inner = dget_deref(d, s, "Resources");
    if (!inner || inner->t != O_DICT) inner = res;
    Buf b;
    buf_init(&b);
    int r = stream_decode(d, s, CC_REF_MAX_FILE_BYTES, &b);
    if (!r) content(x, (const unsigned char *)b.p, b.len, inner, m, depth + 1);
    else if (!dget(s, "Filter")) content(x, s->data, s->ndata, inner, m, depth + 1);
    else x->skipped++;
    buf_free(&b);
    return x->out->truncated || x->oom;
}

static void content(Ctx *x, const unsigned char *data, size_t n, PObj *res, M base, int depth) {
    /* Content::decode first: a content lopdf fails whole draws nothing */
    P p = {data, n, 0, x->d->a, &x->oom};
    ws_only(&p);
    Op op;
    size_t nops = 0;
    for (;;) {
        size_t before = p.pos;
        int r = c_operation(&p, &op);
        if (r < 0) {
            x->skipped++;
            return;
        }
        if (!r) break;
        if (p.pos == before) {
            x->skipped++;
            return;
        }
        nops++;
    }
    size_t end = p.pos;
    p.pos = 0;
    ws_only(&p);
    M ctm = base, stack[Q_STACK];
    size_t sp = 0;
    Path path = {NULL, 0, 0};
    Line cur = {NULL, 0, 0};
    double last[2] = {0, 0}, start[2] = {0, 0}, q[2];
    for (size_t k = 0; k < nops && p.pos <= end; k++) {
        if (c_operation(&p, &op) <= 0) break;
        const double *a = op.v;
        size_t na = op.nnum;
        if (is(&op, "q")) {
            if (sp < Q_STACK) stack[sp++] = ctm;
        } else if (is(&op, "Q")) ctm = sp ? stack[--sp] : base;
        else if (is(&op, "cm") && na == 6) {
            M in = {{a[0], a[1], a[2], a[3], a[4], a[5]}};
            ctm = m_after(&ctm, &in);
        } else if (is(&op, "m") && na == 2) {
            if (flush(&cur, &path)) goto oom;
            last[0] = a[0], last[1] = a[1];
            start[0] = last[0], start[1] = last[1];
            m_apply(&ctm, last, q);
            if (line_push(&cur, q)) goto oom;
        } else if (is(&op, "l") && na == 2) {
            if (!cur.n) {
                m_apply(&ctm, last, q);
                if (line_push(&cur, q)) goto oom;
            }
            last[0] = a[0], last[1] = a[1];
            m_apply(&ctm, last, q);
            if (line_push(&cur, q)) goto oom;
        } else if ((is(&op, "c") && na == 6) || ((is(&op, "v") || is(&op, "y")) && na == 4)) {
            double p0[2] = {last[0], last[1]}, p1[2], p2[2], p3[2];
            if (is(&op, "c")) p1[0] = a[0], p1[1] = a[1], p2[0] = a[2], p2[1] = a[3], p3[0] = a[4], p3[1] = a[5];
            else if (is(&op, "v")) p1[0] = last[0], p1[1] = last[1], p2[0] = a[0], p2[1] = a[1], p3[0] = a[2], p3[1] = a[3];
            else p1[0] = a[0], p1[1] = a[1], p2[0] = a[2], p2[1] = a[3], p3[0] = a[2], p3[1] = a[3];
            if (!cur.n) {
                m_apply(&ctm, last, q);
                if (line_push(&cur, q)) goto oom;
            }
            for (int i = 1; i <= 12; i++) {
                double t = (double)i / 12.0, u = 1.0 - t;
                double w0 = u * u * u, w1 = 3.0 * u * u * t, w2 = 3.0 * u * t * t, w3 = t * t * t;
                double pt[2] = {w0 * p0[0] + w1 * p1[0] + w2 * p2[0] + w3 * p3[0], w0 * p0[1] + w1 * p1[1] + w2 * p2[1] + w3 * p3[1]};
                m_apply(&ctm, pt, q);
                if (line_push(&cur, q)) goto oom;
            }
            last[0] = p3[0], last[1] = p3[1];
        } else if (is(&op, "h")) {
            if (cur.n) {
                m_apply(&ctm, start, q);
                if (line_push(&cur, q)) goto oom;
                last[0] = start[0], last[1] = start[1];
            }
        } else if (is(&op, "re") && na == 4) {
            if (flush(&cur, &path)) goto oom;
            double X = a[0], Y = a[1], W = a[2], H = a[3];
            double c4[5][2] = {{X, Y}, {X + W, Y}, {X + W, Y + H}, {X, Y + H}, {X, Y}};
            Line r = {NULL, 0, 0};
            for (int i = 0; i < 5; i++) {
                m_apply(&ctm, c4[i], q);
                if (line_push(&r, q)) {
                    free(r.p);
                    goto oom;
                }
            }
            if (flush(&r, &path)) {
                free(r.p);
                goto oom;
            }
            last[0] = X, last[1] = Y;
            start[0] = X, start[1] = Y;
        } else if (is(&op, "S") || is(&op, "s") || is(&op, "f") || is(&op, "F") || is(&op, "f*") || is(&op, "B") ||
                   is(&op, "B*") || is(&op, "b") || is(&op, "b*")) {
            if ((is(&op, "s") || is(&op, "b") || is(&op, "b*")) && cur.n) {
                m_apply(&ctm, start, q);
                if (line_push(&cur, q)) goto oom;
            }
            if (flush(&cur, &path)) goto oom;
            for (size_t i = 0; i < path.n; i++) {
                if (cc_refread_push(x->out, (const double (*)[2])path.v[i].p, path.v[i].n)) goto oom;
                if (x->out->truncated) {
                    path_clear(&path);
                    goto done;
                }
            }
            path_clear(&path);
        } else if (is(&op, "n")) {
            cur.n = 0;
            path_clear(&path);
        } else if (is(&op, "Do")) {
            if (do_form(x, &op, res, &ctm, depth)) goto done;
        }
    }
    goto done;
oom:
    x->oom = 1;
done:
    path_clear(&path);
    free(path.v);
    free(cur.p);
}

/* ---- the read ---- */

static void doc_free(Doc *d) {
    free(d->x), free(d->onum), free(d->oval);
}

int cc_pdf_read(const unsigned char *bytes, size_t n, double scale, cc_refread *out, Buf *err) {
    cc_refread_init(out);
    if (!(scale == scale && scale > 0.0 && scale <= 100000.0)) { /* is_finite: NaN fails, inf > 100000 */
        buf_puts(err, "the PDF's scale must be 1:N with N between 1 and 100000");
        return -1;
    }
    if (n > CC_REF_MAX_FILE_BYTES) {
        buf_puts(err, "not a readable PDF: larger than 256 MB");
        return -1;
    }
    Arena a;
    arena_init(&a, 0);
    Doc d;
    memset(&d, 0, sizeof d);
    d.a = &a, d.depth_left = 4;
    size_t off = find_fwd(bytes, n, 0, "%PDF-");
    if (off == (size_t)-1) off = 0;
    d.b = bytes + off, d.n = n - off;
    int rc = -1;
    const char *bad = "not a readable PDF: couldn't parse input";
    /* header: %PDF- version, the rest of the line, an eol */
    P h = {d.b, d.n, 0, &a, &d.oom};
    if (!starts(&h, "%PDF-")) goto fail;
    while (h.pos < h.n && h.b[h.pos] != '\r' && h.b[h.pos] != '\n') h.pos++;
    if (!eol(&h)) goto fail;
    int rebuilt = 0;
    if (resolve_xref(&d)) {
        int r = reconstruct(&d);
        if (r < 0) goto fail;
        rebuilt = 1;
    }
    if (d.oom) goto oom;
    if (d.has_trailer && dget(&d.trailer, "Encrypt")) {
        buf_puts(err, "the PDF is encrypted: Bimble reads PDFs without a password only (print it again to a PDF without one)");
        goto end;
    }
    if (load_objects(&d)) goto oom;
    if (!d.has_trailer) trailer_from_objects(&d);
    if (!d.has_trailer) goto fail;
    size_t count;
    uint32_t pnum = 0;
    PObj *page = first_page(&d, &count, &pnum);
    if (!page && !rebuilt) {
        /* lopdf: a page tree that does not load is read again from a rebuilt table */
        Doc r;
        memset(&r, 0, sizeof r);
        r.a = &a, r.b = d.b, r.n = d.n, r.depth_left = 4;
        if (reconstruct(&r) >= 0 && !r.oom && !load_objects(&r)) {
            if (!r.has_trailer) trailer_from_objects(&r);
            size_t c2;
            uint32_t p2 = 0;
            PObj *pg = r.has_trailer ? first_page(&r, &c2, &p2) : NULL;
            if (pg) {
                doc_free(&d);
                d = r, page = pg, count = c2, pnum = p2;
                memset(&r, 0, sizeof r);
            }
        }
        doc_free(&r);
    }
    if (!page) {
        buf_puts(err, "the PDF has no page");
        goto end;
    }
    Buf content_b;
    buf_init(&content_b);
    int cr = page->t == O_NULL ? 0 : page_content(&d, page, &content_b);
    if (cr) {
        buf_free(&content_b);
        if (cr == -1) goto oom;
        buf_puts(err, "page 1: the page's content passes 256 MB");
        goto end;
    }
    PObj *res = page->t == O_NULL ? NULL : page_resources(&d, page);
    double k = 25.4 / 72.0 * scale;
    M base = {{k, 0.0, 0.0, k, 0.0, 0.0}};
    Ctx x = {&d, out, 0, 0};
    content(&x, (const unsigned char *)content_b.p, content_b.len, res, base, 0);
    buf_free(&content_b);
    if (x.oom || out->oom) goto oom;
    Buf note;
    buf_init(&note);
    buf_puts(&note, "PDF page 1 of ");
    buf_u64(&note, count);
    buf_puts(&note, ", read at 1:");
    fmt_f64_fixed(&note, scale, -1);
    cc_refread_note(out, buf_cstr(&note));
    buf_free(&note);
    if (!out->nlines) cc_refread_note(out, "no vector lines found (a scanned PDF has none)");
    if (x.skipped) {
        Buf t;
        buf_init(&t);
        buf_u64(&t, x.skipped);
        buf_puts(&t, " drawings not read (too deeply nested or unreadable)");
        cc_refread_note(out, buf_cstr(&t));
        buf_free(&t);
    }
    if (out->truncated) cc_refread_note(out, "stopped at 2000000 points");
    if (out->oom) goto oom;
    rc = 0;
    goto end;
fail:
    if (d.oom) goto oom;
    buf_puts(err, bad);
    goto end;
oom:
    buf_clear(err);
    buf_puts(err, "not a readable PDF: out of memory");
end:
    doc_free(&d);
    arena_free(&a);
    if (rc) cc_refread_free(out), cc_refread_init(out);
    return rc;
}
