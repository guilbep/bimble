/* DXF reference plans (dxf_in.h): reference.rs read_dxf over the dxf crate 0.6.2's text reader.
 * The file and line references in comments are to those sources. */
#pragma STDC FP_CONTRACT OFF
#include "dxf_in.h"
#include <stdlib.h>
#include <string.h>
#include "fmt.h"
#include "ifc_math.h"
#include "math.h"
#include "strtod.h"

#define TAU 6.283185307179586
#define RADS_PER_DEG 0.017453292519943295 /* f64::to_radians: PI / 180.0 */
#define CIRCLE_SEGMENTS 64.0
#define SWEEP_STEPS (1u << 20)     /* `while sweep <= 0 { sweep += TAU }`: Rust has no bound */
#define WORK_BUDGET ((size_t)1 << 24) /* entity visits through blocks (Rust has none) */
/* Points made (pushed or not: a polyline with a non-finite point is dropped after it is made),
 * twice the output's cap: the work of a file is bounded even where the cap is never reached (a
 * million arcs of NaN radius, a block of arcs copied 2^24 times). Rust has none. */
#define POINT_BUDGET (2 * CC_DXF_MAX_POINTS)
/* An arc or ellipse sweeping more than two turns is drawn as one turn (the same curve; Rust makes
 * every turn: 11,200,000 degrees was 1,991,113 points and 8 s). Up to two turns, Rust's points. */
#define MAX_SWEEP (2.0 * TAU)

/* ---------------------------------------------------------------- small maths */

static double fabs_(double x) { return x < 0 ? -x : x == 0 ? 0.0 : x; }

static double ceil_(double x) { /* f64::ceil */
    if (!(fabs_(x) < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x;
    if (t < x) t += 1.0;
    return t == 0.0 && x < 0 ? -0.0 : t;
}

static double max2(double x) { return x != x ? 2.0 : x > 2.0 ? x : 2.0; } /* f64::max(x, 2.0) */

static int finite_(double x) { return x == x && x - x == 0.0; }

/* `x as usize` of a value >= 2 (saturating). */
static size_t as_count(double x) { return x >= 1.8e19 ? (size_t)-1 : (size_t)(uint64_t)x; }

/* ---------------------------------------------------------------- code pairs (code_pair_iter.rs) */

enum { T_STR, T_DOUBLE, T_SHORT, T_INT, T_LONG, T_BOOL, T_BINARY };

static int expected_type(int32_t c) { /* expected_type.rs; -1: none */
    if (c >= 0 && c <= 9) return T_STR;
    if (c >= 10 && c <= 59) return T_DOUBLE;
    if (c >= 60 && c <= 79) return T_SHORT;
    if (c >= 90 && c <= 99) return T_INT;
    if ((c >= 100 && c <= 102) || c == 105) return T_STR;
    if (c >= 110 && c <= 149) return T_DOUBLE;
    if (c >= 160 && c <= 169) return T_LONG;
    if (c >= 170 && c <= 179) return T_SHORT;
    if (c >= 210 && c <= 239) return T_DOUBLE;
    if (c >= 270 && c <= 289) return T_SHORT;
    if (c >= 290 && c <= 299) return T_BOOL;
    if (c >= 300 && c <= 309) return T_STR;
    if (c >= 310 && c <= 319) return T_BINARY;
    if (c >= 320 && c <= 369) return T_STR;
    if (c >= 370 && c <= 389) return T_SHORT;
    if (c >= 390 && c <= 399) return T_STR;
    if (c >= 400 && c <= 409) return T_SHORT;
    if (c >= 410 && c <= 419) return T_STR;
    if (c >= 420 && c <= 429) return T_INT;
    if (c >= 430 && c <= 439) return T_STR;
    if (c >= 440 && c <= 459) return T_INT;
    if (c >= 460 && c <= 469) return T_DOUBLE;
    if (c >= 470 && c <= 481) return T_STR;
    if (c == 999 || (c >= 1000 && c <= 1009)) return T_STR;
    if (c >= 1010 && c <= 1059) return T_DOUBLE;
    if (c >= 1060 && c <= 1070) return T_SHORT;
    if (c == 1071) return T_INT;
    return -1;
}

typedef struct {
    int32_t code;
    int type;
    const char *s; /* T_STR: the value line's bytes (CR stripped) */
    size_t n;
    double d;  /* T_DOUBLE */
    int64_t i; /* T_SHORT, T_INT, T_LONG, T_BOOL */
    size_t offset; /* the code line's number (CodePair.offset) */
} Pair;

typedef struct {
    const uint8_t *p;
    size_t n, pos;
    size_t offset; /* TextCodePairIter.offset */
    int first_done;
    int utf8; /* read_as_utf8 ($ACADVER >= R2007): value lines are UTF-8 */
    Pair back[4];
    int nback;
    Buf *err; /* the last error text (the caller decides whether it counts) */
    Buf scratch;
    int oom;
} Rd;

/* read_line: the bytes up to '\n', one '\r' off. A code line (allow_bom) drops a 0xEF first byte,
 * then a 0xBB second and a 0xBF third byte (helper_functions.rs:294): kept bytes go to `scratch`. */
static void read_line(Rd *r, int allow_bom, const char **s, size_t *n) {
    size_t st = r->pos, e = r->pos;
    while (e < r->n && r->p[e] != '\n') e++;
    r->pos = e < r->n ? e + 1 : e;
    const uint8_t *b = r->p + st;
    size_t len = e - st;
    if (allow_bom && len > 0 && b[0] == 0xEF) {
        buf_clear(&r->scratch);
        for (size_t i = 1; i < len; i++)
            if (!((i == 1 && b[i] == 0xBB) || (i == 2 && b[i] == 0xBF))) buf_putc(&r->scratch, (char)b[i]);
        if (r->scratch.oom) r->oom = 1;
        *s = r->scratch.p ? r->scratch.p : "", *n = r->scratch.len;
    } else {
        *s = (const char *)b, *n = len;
    }
    if (*n && (*s)[*n - 1] == '\r') (*n)--;
}

static int ws_1252(uint8_t c) { return c == ' ' || (c >= 9 && c <= 13) || c == 0xA0; }

/* str::trim of a line decoded as Windows-1252 (code lines always; value lines before UTF-8). */
static void trim_line(const Rd *r, int value, const char **s, size_t *n) {
    const uint8_t *b = (const uint8_t *)*s;
    size_t i = 0, e = *n;
    if (value && r->utf8) {
        for (;;) { /* ASCII white space and the UTF-8 forms of NBSP and U+2000.. spaces */
            if (i < e && (b[i] == ' ' || (b[i] >= 9 && b[i] <= 13))) i++;
            else if (i + 1 < e && b[i] == 0xC2 && (b[i + 1] == 0xA0 || b[i + 1] == 0x85)) i += 2;
            else break;
        }
        for (;;) {
            if (e > i && (b[e - 1] == ' ' || (b[e - 1] >= 9 && b[e - 1] <= 13))) e--;
            else if (e >= i + 2 && b[e - 2] == 0xC2 && (b[e - 1] == 0xA0 || b[e - 1] == 0x85)) e -= 2;
            else break;
        }
    } else {
        while (i < e && ws_1252(b[i])) i++;
        while (e > i && ws_1252(b[e - 1])) e--;
    }
    *s = (const char *)b + i, *n = e - i;
}

static int utf8_ok(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t k;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) k = 1, cp = c & 0x1F;
        else if ((c & 0xF0) == 0xE0) k = 2, cp = c & 0x0F;
        else if ((c & 0xF8) == 0xF0) k = 3, cp = c & 0x07;
        else return 0;
        for (size_t j = 1; j <= k; j++) {
            if (i + j >= n || (s[i + j] & 0xC0) != 0x80) return 0;
            cp = cp << 6 | (s[i + j] & 0x3F);
        }
        if ((k == 1 && cp < 0x80) || (k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        i += k + 1;
    }
    return 1;
}

static void put_off(Buf *b, size_t o) {
    buf_puts(b, " at line/offset ");
    buf_u64(b, o);
}

/* i32 / i64 from_str (Rust's errors). 0 ok, -1 with the text in err. */
static int parse_int(const char *s, size_t n, int64_t lo, int64_t hi, int64_t *out, Buf *err) {
    if (!n) return buf_puts(err, "cannot parse integer from empty string"), -1;
    size_t i = 0;
    int neg = 0;
    if (s[0] == '+' || s[0] == '-') {
        neg = s[0] == '-';
        i = 1;
        if (n == 1) return buf_puts(err, "invalid digit found in string"), -1;
    }
    int64_t v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return buf_puts(err, "invalid digit found in string"), -1;
        int d = s[i] - '0';
        if (!neg) {
            if (v > (hi - d) / 10) return buf_puts(err, "number too large to fit in target type"), -1;
            v = v * 10 + d;
        } else {
            if (v < (lo + d) / 10) return buf_puts(err, "number too small to fit in target type"), -1;
            v = v * 10 - d;
        }
    }
    *out = v;
    return 0;
}

static int ieq(const char *s, size_t n, const char *lit) { /* ASCII case-insensitive */
    if (strlen(lit) != n) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lit[i]) return 0;
    }
    return 1;
}

/* f64 from_str (Rust's grammar and errors). 0 ok, -1 with the text in err. */
static int parse_f64(const char *s, size_t n, double *out, Buf *err) {
    if (!n) return buf_puts(err, "cannot parse float from empty string"), -1;
    size_t i = 0;
    int neg = 0;
    if (s[0] == '+' || s[0] == '-') neg = s[0] == '-', i = 1;
    const char *b = s + i;
    size_t m = n - i;
    if (ieq(b, m, "inf") || ieq(b, m, "infinity")) {
        *out = neg ? -1.0 / 0.0 : 1.0 / 0.0;
        return 0;
    }
    if (ieq(b, m, "nan")) {
        *out = 0.0 / 0.0;
        return 0;
    }
    size_t j = 0, digits = 0;
    while (j < m && b[j] >= '0' && b[j] <= '9') j++, digits++;
    if (j < m && b[j] == '.') {
        j++;
        while (j < m && b[j] >= '0' && b[j] <= '9') j++, digits++;
    }
    if (!digits) return buf_puts(err, "invalid float literal"), -1;
    if (j < m && (b[j] == 'e' || b[j] == 'E')) {
        j++;
        if (j < m && (b[j] == '+' || b[j] == '-')) j++;
        size_t ed = 0;
        while (j < m && b[j] >= '0' && b[j] <= '9') j++, ed++;
        if (!ed) return buf_puts(err, "invalid float literal"), -1;
    }
    if (j != m) return buf_puts(err, "invalid float literal"), -1;
    *out = cc_strtod(s, n);
    return 0;
}

static int16_t as_i16(double d) { /* `f64 as i16` */
    if (d != d) return 0;
    if (d >= 32767.0) return 32767;
    if (d <= -32768.0) return -32768;
    return (int16_t)(int32_t)d;
}

static int hexc(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* The next code pair from the text: 1 one, 0 the end (None), -1 an error (text in r->err).
 * The lines it took stay taken after an error, as in Rust. */
static int raw_next(Rd *r, Pair *p) {
    const char *s;
    size_t n;
    if (r->first_done) {
        r->offset++;
        read_line(r, 1, &s, &n);
    } else {
        r->first_done = 1;
        read_line(r, 1, &s, &n);
    }
    trim_line(r, 0, &s, &n);
    if (!n) return 0;
    size_t code_offset = r->offset;
    int64_t code;
    buf_clear(r->err);
    if (parse_int(s, n, INT32_MIN, INT32_MAX, &code, r->err)) return put_off(r->err, code_offset), -1;
    r->offset++;
    const char *v;
    size_t vn;
    read_line(r, 0, &v, &vn);
    if (r->utf8 && !utf8_ok((const uint8_t *)v, vn)) return buf_puts(r->err, "the string is malformed"), -1;
    int t = expected_type((int32_t)code);
    if (t < 0) {
        buf_puts(r->err, "the specified enum value does not fall into the expected range");
        return put_off(r->err, r->offset), -1;
    }
    memset(p, 0, sizeof *p);
    p->code = (int32_t)code, p->type = t, p->offset = code_offset;
    const char *tv = v;
    size_t tn = vn;
    if (t != T_STR && t != T_BINARY) trim_line(r, 1, &tv, &tn);
    double d;
    switch (t) {
    case T_STR:
        p->s = v, p->n = vn;
        break;
    case T_BINARY:
        for (size_t i = 0; i < vn; i++)
            if (!hexc(v[i])) {
                buf_puts(r->err, "there was a general parsing error");
                return put_off(r->err, r->offset), -1;
            }
        p->s = v, p->n = vn;
        break;
    case T_DOUBLE:
        if (parse_f64(tv, tn, &p->d, r->err)) return put_off(r->err, r->offset), -1;
        break;
    case T_SHORT:
    case T_BOOL:
        if (parse_f64(tv, tn, &d, r->err)) return put_off(r->err, r->offset), -1;
        p->i = as_i16(d);
        break;
    case T_INT:
        if (parse_int(tv, tn, INT32_MIN, INT32_MAX, &p->i, r->err)) return put_off(r->err, r->offset), -1;
        break;
    case T_LONG:
        if (parse_int(tv, tn, INT64_MIN, INT64_MAX, &p->i, r->err)) return put_off(r->err, r->offset), -1;
        break;
    }
    return 1;
}

/* CodePairPutBack::next: put-back pairs first, else the text without its 999 comments. */
static int next(Rd *r, Pair *p) {
    if (r->nback) {
        *p = r->back[--r->nback];
        return 1;
    }
    for (;;) {
        int k = raw_next(r, p);
        if (k != 1 || p->code != 999) return k;
    }
}

static void put_back(Rd *r, const Pair *p) {
    if (r->nback < 4) r->back[r->nback++] = *p;
}

static int is(const Pair *p, const char *lit) {
    size_t n = strlen(lit);
    return p->type == T_STR && p->n == n && !memcmp(p->s, lit, n);
}

static int err_eof(Rd *r) {
    buf_clear(r->err);
    buf_puts(r->err, "the input unexpectedly ended before the drawing was completely loaded");
    return -1;
}

static void pair_debug(Buf *b, const Pair *p) { /* CodePair's Debug: [@offset]code/value */
    buf_puts(b, "[@"), buf_u64(b, p->offset), buf_putc(b, ']');
    buf_i64(b, p->code), buf_putc(b, '/');
    switch (p->type) {
    case T_STR:
    case T_BINARY:
        buf_append(b, p->s, p->n);
        break;
    case T_SHORT:
    case T_INT: { /* {: >6} and {: >9} */
        Buf t;
        buf_init(&t);
        buf_i64(&t, p->i);
        size_t w = p->type == T_SHORT ? 6 : 9;
        if (t.len < w) buf_fill(b, ' ', w - t.len);
        buf_append(b, t.p, t.len);
        buf_free(&t);
        break;
    }
    case T_LONG:
    case T_BOOL:
        buf_i64(b, p->i);
        break;
    default: /* format_f64 is {:.12} with zeros trimmed: {:?} here (DEVIATIONS 14) */
        fmt_f64_debug(b, p->d);
        break;
    }
}

static int err_pair(Rd *r, const Pair *p, const char *what) { /* DxfError::UnexpectedCodePair */
    buf_clear(r->err);
    buf_puts(r->err, "the code pair '");
    pair_debug(r->err, p);
    buf_puts(r->err, "' was not expected at this time: ");
    buf_puts(r->err, what);
    put_off(r->err, p->offset);
    return -1;
}

/* ---------------------------------------------------------------- entities */

enum {
    E_LINE = 1, E_CIRCLE, E_ARC, E_ELLIPSE, E_LWPOLY, E_POLY, E_SPLINE, E_SOLID, E_INSERT,
    E_TEXT, E_MTEXT, E_ATTRIB, E_ATTDEF, E_VERTEX, E_SEQEND, E_DIM, E_OTHER
};

typedef struct { /* a growable array of doubles */
    double *v;
    size_t n, cap;
} Dv;

static int dv_push(Dv *a, double x) {
    if (a->n == a->cap) {
        size_t c = a->cap ? a->cap * 2 : 16;
        double *nv = realloc(a->v, c * sizeof *nv);
        if (!nv) return -1;
        a->v = nv, a->cap = c;
    }
    a->v[a->n++] = x;
    return 0;
}

typedef struct Ent Ent;
struct Ent {
    int type;
    int paper;
    uint32_t layer;
    double p[4][2]; /* LINE p1 p2; CIRCLE, ARC, ELLIPSE centre; ELLIPSE axis p[1]; SOLID; INSERT, TEXT at */
    double r, a0, a1; /* radius / ratio, start, end */
    double height, rot;
    double sx, sy, csp, rsp;
    int cols, rows, has_attr, flags;
    char *name; /* INSERT block name / TEXT text (malloc'd, NUL-terminated) */
    size_t namen;
    Dv x, y, z, b;  /* LWPOLYLINE: x y bulge; POLYLINE: its vertices; SPLINE control x y z */
    Dv fx, fy, fz;  /* SPLINE fit points */
};

static void ent_free(Ent *e) {
    free(e->name);
    free(e->x.v), free(e->y.v), free(e->z.v), free(e->b.v);
    free(e->fx.v), free(e->fy.v), free(e->fz.v);
    memset(e, 0, sizeof *e);
}

typedef struct {
    char *name;
    size_t n;
    double base[2];
    Ent *e;
    size_t ne, cap;
} Block;

typedef struct {
    Rd rd;
    cc_dxf *d;
    Block *blocks;
    size_t nblocks, capb;
    Ent *top;
    size_t ntop, captop;
    uint32_t *lhash; /* open addressing over d->layers, size LHASH */
    size_t work;
    size_t made; /* points made, against POINT_BUDGET */
    int oom;
} Ctx;

#define LHASH 8192

static uint32_t hash_bytes(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

/* The layer named s[0..n) (added when new, within CC_DXF_MAX_LAYERS). */
static uint32_t layer_of(Ctx *c, const char *s, size_t n) {
    cc_dxf *d = c->d;
    uint32_t h = hash_bytes(s, n) & (LHASH - 1);
    for (;;) {
        uint32_t k = c->lhash[h];
        if (k == CC_DXF_NO_LAYER) break;
        if (d->layers[k].n == n && !memcmp(d->layers[k].name, s, n)) return k;
        h = (h + 1) & (LHASH - 1);
    }
    if (d->nlayers >= CC_DXF_MAX_LAYERS) return CC_DXF_NO_LAYER;
    if (d->nlayers == d->caps[2]) {
        size_t cap = d->caps[2] ? d->caps[2] * 2 : 16;
        cc_dxf_layer *nl = realloc(d->layers, cap * sizeof *nl);
        if (!nl) return c->oom = 1, CC_DXF_NO_LAYER;
        d->layers = nl, d->caps[2] = cap;
    }
    char *nm = malloc(n + 1);
    if (!nm) return c->oom = 1, CC_DXF_NO_LAYER;
    memcpy(nm, s, n), nm[n] = 0;
    cc_dxf_layer *l = &d->layers[d->nlayers];
    l->name = nm, l->n = n, l->on = 1, l->frozen = 0, l->polylines = 0;
    c->lhash[h] = (uint32_t)d->nlayers;
    return (uint32_t)d->nlayers++;
}

static char *dupz(const char *s, size_t n) {
    char *o = malloc(n + 1);
    if (o) memcpy(o, s, n), o[n] = 0;
    return o;
}

/* ExtensionGroup::read_group (extension_data.rs:24), iteratively. 0 ok, -1 error. */
static int read_group(Rd *r, const Pair *start) {
    if (!(start->n && start->s[0] == '{')) {
        buf_clear(r->err);
        buf_puts(r->err, "there was a general parsing error");
        return put_off(r->err, start->offset), -1;
    }
    size_t depth = 1;
    while (depth) {
        Pair p;
        int k = next(r, &p);
        if (k < 0) return -1;
        if (!k) return err_eof(r);
        if (p.code == 102) {
            if (p.n == 1 && p.s[0] == '}') depth--;
            else if (p.n && p.s[0] == '{') depth++;
            else return err_pair(r, &p, "expected an extension start or end pair");
        }
    }
    return 0;
}

/* XData::read_item (x_data.rs:52) after a 1001 pair: the items (codes >= 1000) are taken. */
static int read_xdata(Rd *r) {
    for (;;) {
        Pair p;
        int k = next(r, &p);
        if (k < 0) return -1;
        if (!k) return 0;
        if (p.code == 0 || p.code == 1001 || p.code < 1000) {
            put_back(r, &p);
            return 0;
        }
        if (p.code == 1002) { /* a control group, to its "}" (nested ones too) */
            size_t depth = 1;
            while (depth) {
                int q = next(r, &p);
                if (q < 0) return -1;
                if (!q) return err_eof(r);
                if (p.code < 1000) return err_pair(r, &p, "expected XDATA item");
                if (p.code == 1002) {
                    if (p.n == 1 && p.s[0] == '}') depth--;
                    else depth++;
                }
            }
        }
    }
}

/* EntityCommon::apply_individual_pair: the fields the reader needs (layer, paper space) and the
 * pairs groups take (102, 1001). 0 ok, -1 error. */
static int common_pair(Ctx *c, Ent *e, const Pair *p) {
    if (p->code == 8) {
        e->layer = layer_of(c, p->s, p->n);
        if (c->oom) return -1;
    } else if (p->code == 67) e->paper = p->i == 1;
    else if (p->code == 102) return read_group(&c->rd, p);
    else if (p->code == 1001) return read_xdata(&c->rd);
    return 0;
}

static const char *const KNOWN_OTHER[] = {"3DFACE", "3DSOLID", "ACAD_PROXY_ENTITY", "ARCALIGNEDTEXT", "BODY",
                                          "HELIX", "IMAGE", "LEADER", "LIGHT", "MLINE", "OLE2FRAME", "OLEFRAME",
                                          "PDFUNDERLAY", "DGNUNDERLAY", "DWFUNDERLAY", "POINT", "RAY", "REGION",
                                          "RTEXT", "SECTION", "SHAPE", "TOLERANCE", "TRACE", "WIPEOUT", "XLINE"};

static int type_of(const Pair *p) {
    static const struct { const char *s; int t; } T[] = {
        {"LINE", E_LINE}, {"3DLINE", E_LINE}, {"CIRCLE", E_CIRCLE}, {"ARC", E_ARC}, {"ELLIPSE", E_ELLIPSE},
        {"LWPOLYLINE", E_LWPOLY}, {"POLYLINE", E_POLY}, {"SPLINE", E_SPLINE}, {"SOLID", E_SOLID},
        {"INSERT", E_INSERT}, {"TEXT", E_TEXT}, {"MTEXT", E_MTEXT}, {"ATTRIB", E_ATTRIB}, {"ATTDEF", E_ATTDEF},
        {"VERTEX", E_VERTEX}, {"SEQEND", E_SEQEND}};
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (is(p, T[i].s)) return T[i].t;
    for (size_t i = 0; i < sizeof KNOWN_OTHER / sizeof KNOWN_OTHER[0]; i++)
        if (is(p, KNOWN_OTHER[i])) return E_OTHER;
    return 0;
}

static void ent_init(Ent *e, int type) {
    memset(e, 0, sizeof *e);
    e->type = type;
    e->layer = CC_DXF_NO_LAYER;
    e->sx = e->sy = 1.0;
    e->cols = e->rows = 1;
    if (type == E_ARC) e->a1 = 360.0;
    if (type == E_ELLIPSE) e->p[1][0] = 1.0, e->r = 1.0, e->a1 = 3.141592653589793 * 2.0;
}

/* The fields of one pair (try_apply_code_pair of the entity types drawn); others to common. */
static int apply_pair(Ctx *c, Ent *e, const Pair *p) {
    int k = p->code;
    double v = p->d;
    switch (e->type) {
    case E_LINE:
        if (k == 10) return e->p[0][0] = v, 0;
        if (k == 20) return e->p[0][1] = v, 0;
        if (k == 11) return e->p[1][0] = v, 0;
        if (k == 21) return e->p[1][1] = v, 0;
        if (k == 30 || k == 31 || k == 39 || k == 210 || k == 220 || k == 230) return 0;
        break;
    case E_CIRCLE:
    case E_ARC:
        if (k == 10) return e->p[0][0] = v, 0;
        if (k == 20) return e->p[0][1] = v, 0;
        if (k == 40) return e->r = v, 0;
        if (e->type == E_ARC && k == 50) return e->a0 = v, 0;
        if (e->type == E_ARC && k == 51) return e->a1 = v, 0;
        if (k == 30 || k == 39 || k == 210 || k == 220 || k == 230) return 0;
        break;
    case E_ELLIPSE:
        if (k == 10) return e->p[0][0] = v, 0;
        if (k == 20) return e->p[0][1] = v, 0;
        if (k == 11) return e->p[1][0] = v, 0;
        if (k == 21) return e->p[1][1] = v, 0;
        if (k == 40) return e->r = v, 0;
        if (k == 41) return e->a0 = v, 0;
        if (k == 42) return e->a1 = v, 0;
        if (k == 30 || k == 31 || k == 210 || k == 220 || k == 230) return 0;
        break;
    case E_SOLID:
        if (k >= 10 && k <= 13) return e->p[k - 10][0] = v, 0;
        if (k >= 20 && k <= 23) return e->p[k - 20][1] = v, 0;
        if ((k >= 30 && k <= 33) || k == 39 || k == 210 || k == 220 || k == 230) return 0;
        break;
    case E_VERTEX:
        if (k == 10) return e->p[0][0] = v, 0;
        if (k == 20) return e->p[0][1] = v, 0;
        if (k == 42) return e->r = v, 0; /* bulge */
        if (k == 70) return e->flags = (int)p->i, 0;
        break;
    case E_POLY:
        if (k == 70) return e->flags = (int)p->i, 0;
        if (k == 66 || k == 10 || k == 20 || k == 30 || k == 39 || k == 40 || k == 41 || (k >= 71 && k <= 75) ||
            k == 210 || k == 220 || k == 230)
            return 0;
        break;
    case E_LWPOLY: /* apply_custom_reader_lwpolyline (entity.rs:1104) */
        if (k == 10) {
            if (dv_push(&e->x, v) || dv_push(&e->y, 0.0) || dv_push(&e->b, 0.0)) return c->oom = 1, -1;
            return 0;
        }
        if (k == 20 || k == 40 || k == 41 || k == 42 || k == 91) {
            if (!e->x.n) {
                buf_clear(c->rd.err);
                buf_puts(c->rd.err, "the set was not expected to be empty");
                return -1;
            }
            if (k == 20) e->y.v[e->y.n - 1] = v;
            if (k == 42) e->b.v[e->b.n - 1] = v;
            return 0;
        }
        if (k == 70) return e->flags = (int)p->i, 0;
        if (k == 39 || k == 43 || k == 210 || k == 220 || k == 230) return 0;
        break;
    case E_SPLINE: {
        Dv *a = k == 10 ? &e->x : k == 20 ? &e->y : k == 30 ? &e->z : k == 11 ? &e->fx : k == 21 ? &e->fy : k == 31 ? &e->fz : NULL;
        if (a) {
            if (dv_push(a, v)) return c->oom = 1, -1;
            return 0;
        }
        if (k == 70 || (k >= 71 && k <= 74) || (k >= 40 && k <= 44) || k == 12 || k == 22 || k == 32 || k == 13 ||
            k == 23 || k == 33 || k == 210 || k == 220 || k == 230)
            return 0;
        break;
    }
    case E_INSERT:
        if (k == 66) return e->has_attr = p->i == 1, 0;
        if (k == 2) {
            free(e->name);
            if (!(e->name = dupz(p->s, p->n))) return c->oom = 1, -1;
            e->namen = p->n;
            return 0;
        }
        if (k == 10) return e->p[0][0] = v, 0;
        if (k == 20) return e->p[0][1] = v, 0;
        if (k == 41) return e->sx = v, 0;
        if (k == 42) return e->sy = v, 0;
        if (k == 50) return e->rot = v, 0;
        if (k == 70) return e->cols = (int)p->i, 0;
        if (k == 71) return e->rows = (int)p->i, 0;
        if (k == 44) return e->csp = v, 0;
        if (k == 45) return e->rsp = v, 0;
        if (k == 30 || k == 43 || k == 210 || k == 220 || k == 230) return 0;
        break;
    case E_TEXT:
    case E_MTEXT:
        if (k == 10) return e->p[0][0] = v, 0;
        if (k == 20) return e->p[0][1] = v, 0;
        if (k == 40) return e->height = v, 0;
        if (k == 50 && e->type == E_TEXT) return e->rot = v, 0;
        if (k == 1 || (k == 3 && e->type == E_MTEXT)) { /* TEXT: its 1; MTEXT: the 3 chunks, then 1 */
            size_t on = e->type == E_MTEXT && e->name ? e->namen : 0;
            char *t = realloc(e->name, on + p->n + 1);
            if (!t) return c->oom = 1, -1;
            memcpy(t + on, p->s, p->n), t[on + p->n] = 0;
            e->name = t, e->namen = on + p->n;
            return 0;
        }
        break;
    default:
        break;
    }
    if (k == 102 || k == 1001 || k == 8 || k == 67) return common_pair(c, e, p);
    return 0;
}

/* Entity::read (entity.rs:408). 1 an entity in `e`, 0 none (0/ENDSEC or 0/ENDBLK put back), -1
 * an error (the EntityIter then ends: the caller treats it as 0). */
static int read_entity(Ctx *c, Ent *e) {
    Rd *r = &c->rd;
    ent_init(e, 0);
    for (;;) {
        Pair p;
        int k = next(r, &p);
        if (k < 0) return -1;
        if (!k) return err_eof(r);
        if (p.code != 0) return err_pair(r, &p, "expected 0/entity-type or 0/ENDSEC");
        if (is(&p, "ENDSEC") || is(&p, "ENDBLK")) {
            put_back(r, &p);
            return 0;
        }
        if (is(&p, "DIMENSION")) { /* known only with one of its subclass markers */
            ent_init(e, E_DIM);
            int known = 0;
            for (;;) {
                Pair q;
                int s = next(r, &q);
                if (s < 0) return -1;
                if (!s) return err_eof(r);
                if (q.code == 0) {
                    put_back(r, &q);
                    break;
                }
                if (!known && q.code == 100 &&
                    (is(&q, "AcDbAlignedDimension") || is(&q, "AcDbRadialDimension") || is(&q, "AcDbDiametricDimension") ||
                     is(&q, "AcDb3PointAngularDimension") || is(&q, "AcDbOrdinateDimension")))
                    known = 1;
                else if (q.code == 102 || q.code == 1001 || q.code == 8 || q.code == 67) {
                    if (common_pair(c, e, &q)) return -1;
                }
            }
            if (known) return 1;
            continue; /* an unsuccessful dimension match: the next entity */
        }
        int t = type_of(&p);
        if (!t) { /* swallow an unsupported entity */
            for (;;) {
                Pair q;
                int s = next(r, &q);
                if (s < 0) return -1;
                if (!s) return err_eof(r);
                if (q.code == 0) {
                    put_back(r, &q);
                    break;
                }
            }
            continue;
        }
        ent_init(e, t);
        int custom = t == E_LWPOLY || t == E_MTEXT || t == E_ATTRIB || t == E_ATTDEF;
        for (;;) {
            Pair q;
            int s = next(r, &q);
            if (s < 0) return -1;
            if (!s) {
                if (custom) return 1; /* next_pair!: None ends a custom reader well */
                return err_eof(r);
            }
            if (q.code == 0) {
                put_back(r, &q);
                break;
            }
            if (apply_pair(c, e, &q)) return -1;
        }
        return 1;
    }
}

/* EntityIter over read_entity with itertools' one-entity put-back. */
typedef struct {
    Ctx *c;
    Ent held;
    int has_held;
    int done;
} EIt;

static int eit_next(EIt *it, Ent *e) { /* 1 an entity, 0 none */
    if (it->has_held) {
        *e = it->held;
        it->has_held = 0;
        return 1;
    }
    if (it->done) return 0;
    Ent t;
    int k = read_entity(it->c, &t);
    if (k == 1) {
        *e = t;
        return 1;
    }
    if (k < 0) ent_free(&t);
    it->done = 1;
    return 0;
}

static void eit_back(EIt *it, Ent *e) { it->held = *e, it->has_held = 1; }

static int ents_push(Ent **v, size_t *n, size_t *cap, Ent *e) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        Ent *nv = realloc(*v, nc * sizeof *nv);
        if (!nv) return -1;
        *v = nv, *cap = nc;
    }
    (*v)[(*n)++] = *e;
    return 0;
}

/* collect_entities (entity_iter.rs:31): ATTRIB/ATTDEF take a following MTEXT, an INSERT with
 * attributes its ATTRIBs and SEQEND, a POLYLINE its VERTEXes and SEQEND. 0 ok, -1 out of memory. */
static int collect(Ctx *c, Ent **v, size_t *n, size_t *cap) {
    EIt it = {c, {0}, 0, 0};
    Ent e;
    while (eit_next(&it, &e)) {
        if (c->oom) return ent_free(&e), -1;
        if (e.type == E_ATTRIB || e.type == E_ATTDEF) {
            Ent m;
            if (eit_next(&it, &m)) {
                if (m.type == E_MTEXT) ent_free(&m);
                else eit_back(&it, &m);
            }
        } else if (e.type == E_INSERT && e.has_attr) {
            Ent a;
            while (eit_next(&it, &a)) {
                if (a.type == E_ATTRIB) {
                    ent_free(&a);
                    continue;
                }
                eit_back(&it, &a);
                break;
            }
            if (eit_next(&it, &a)) {
                if (a.type == E_SEQEND) ent_free(&a);
                else eit_back(&it, &a);
            }
        } else if (e.type == E_POLY) {
            Ent a;
            while (eit_next(&it, &a)) {
                if (a.type == E_VERTEX) {
                    int bad = dv_push(&e.x, a.p[0][0]) || dv_push(&e.y, a.p[0][1]) || dv_push(&e.b, a.r);
                    ent_free(&a);
                    if (bad) return ent_free(&e), -1;
                    continue;
                }
                eit_back(&it, &a);
                break;
            }
            if (eit_next(&it, &a)) {
                if (a.type == E_SEQEND) ent_free(&a);
                else eit_back(&it, &a);
            }
        }
        if (ents_push(v, n, cap, &e)) return ent_free(&e), -1;
    }
    if (it.has_held) ent_free(&it.held);
    return c->oom ? -1 : 0;
}

/* ---------------------------------------------------------------- sections (drawing.rs) */

static int swallow_section(Rd *r) {
    for (;;) {
        Pair p;
        int k = next(r, &p);
        if (k < 0) return -1;
        if (!k) return 0;
        if (p.code == 0 && is(&p, "ENDSEC")) {
            put_back(r, &p);
            return 0;
        }
    }
}

static int version_utf8(const Pair *p) { /* AcadVersion::from_safe(..) >= R2007 */
    static const char *const V[] = {"AC1021", "17.0", "17.1", "17.2", "AC1024", "18.0", "18.1", "18.2",
                                    "AC1027", "19.0", "19.1", "19.2", "19.3", "AC1032"};
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++)
        if (is(p, V[i])) return 1;
    return 0;
}

static int err_code(Rd *r, const Pair *p) { /* DxfError::UnexpectedCode */
    buf_clear(r->err);
    buf_puts(r->err, "an unexpected code '");
    buf_i64(r->err, p->code);
    buf_puts(r->err, "' was encountered");
    return put_off(r->err, p->offset), -1;
}

static int read_header(Ctx *c, int *units) { /* Header::read (header.rs:21): $INSUNITS, $ACADVER */
    Rd *r = &c->rd;
    for (;;) {
        Pair p;
        int k = next(r, &p);
        if (k < 0) return -1;
        if (!k) return 0;
        if (p.code == 0) {
            put_back(r, &p);
            return 0;
        }
        if (p.code != 9) return err_pair(r, &p, "");
        int var = is(&p, "$INSUNITS") ? 1 : is(&p, "$ACADVER") ? 2 : 0;
        for (;;) {
            Pair q;
            int s = next(r, &q);
            if (s < 0) return -1;
            if (!s) break;
            if (q.code == 0 || q.code == 9) {
                put_back(r, &q);
                break;
            }
            if (var == 1) {
                if (q.code != 70) return err_code(r, &q);
                *units = (int)q.i;
            } else if (var == 2) {
                if (q.code != 1) return err_code(r, &q);
                if (version_utf8(&q)) r->utf8 = 1;
            }
        }
    }
}

static int read_tables(Ctx *c) { /* the LAYER entries (name 2, colour 62, flags 70); the rest skipped */
    Rd *r = &c->rd;
    int in_layer = 0;
    uint32_t cur = CC_DXF_NO_LAYER;
    int on = 1, frozen = 0;
    for (;;) {
        Pair p;
        int k = next(r, &p);
        if (k < 0) return -1;
        if (!k) return err_eof(r);
        if (p.code == 0) {
            if (cur != CC_DXF_NO_LAYER) c->d->layers[cur].on = on, c->d->layers[cur].frozen = frozen;
            cur = CC_DXF_NO_LAYER, on = 1, frozen = 0;
            if (is(&p, "ENDSEC")) {
                put_back(r, &p);
                return 0;
            }
            in_layer = is(&p, "LAYER");
            continue;
        }
        if (!in_layer) continue;
        if (p.code == 2) {
            cur = layer_of(c, p.s, p.n);
            if (c->oom) return -1;
        } else if (p.code == 62) on = p.i >= 0;
        else if (p.code == 70) frozen = (p.i & 1) != 0;
    }
}

static int read_block(Ctx *c) { /* Block::read_block (block.rs:121) */
    Rd *r = &c->rd;
    Block b;
    memset(&b, 0, sizeof b);
    int rc = 0;
    for (;;) {
        Pair p;
        int k = next(r, &p);
        if (k < 0) {
            rc = -1;
            break;
        }
        if (!k) {
            rc = err_eof(r);
            break;
        }
        if (p.code == 0 && is(&p, "ENDBLK")) {
            for (;;) {
                Pair q;
                int s = next(r, &q);
                if (s < 0) {
                    rc = -1;
                    break;
                }
                if (!s) {
                    rc = err_eof(r);
                    break;
                }
                if (q.code == 0) {
                    put_back(r, &q);
                    break;
                }
            }
            break;
        }
        if (p.code == 0 && (is(&p, "ENDSEC") || is(&p, "EOF"))) {
            put_back(r, &p);
            break;
        }
        if (p.code == 0) {
            put_back(r, &p);
            size_t cap = b.cap;
            if (collect(c, &b.e, &b.ne, &cap)) {
                rc = -1;
                break;
            }
            b.cap = cap;
            continue;
        }
        if (p.code == 2) {
            free(b.name);
            if (!(b.name = dupz(p.s, p.n))) {
                c->oom = 1, rc = -1;
                break;
            }
            b.n = p.n;
        } else if (p.code == 10) b.base[0] = p.d;
        else if (p.code == 20) b.base[1] = p.d;
        else if (p.code == 102) {
            if (read_group(r, &p)) {
                rc = -1;
                break;
            }
        } else if (p.code == 1001) {
            if (read_xdata(r)) {
                rc = -1;
                break;
            }
        }
    }
    if (rc == 0) { /* drawing.add_block: kept, even when it never reached ENDBLK */
        if (c->nblocks == c->capb) {
            size_t nc = c->capb ? c->capb * 2 : 8;
            Block *nb = realloc(c->blocks, nc * sizeof *nb);
            if (!nb) c->oom = 1, rc = -1;
            else c->blocks = nb, c->capb = nc;
        }
        if (rc == 0) {
            if (!b.name && !(b.name = dupz("", 0))) c->oom = 1, rc = -1;
            else {
                c->blocks[c->nblocks++] = b;
                return 0;
            }
        }
    }
    for (size_t i = 0; i < b.ne; i++) ent_free(&b.e[i]);
    free(b.e), free(b.name);
    return rc;
}

static int read_items(Ctx *c, const char *item) { /* read_section_item("BLOCK", read_block) */
    Rd *r = &c->rd;
    for (;;) {
        Pair p;
        int k = next(r, &p);
        if (k < 0) return -1;
        if (!k) return err_eof(r);
        if (p.code != 0) return err_pair(r, &p, "");
        if (is(&p, "ENDSEC")) {
            put_back(r, &p);
            return 0;
        }
        if (!is(&p, item)) return err_pair(r, &p, "");
        if (read_block(c)) return -1;
    }
}

static int read_sections(Ctx *c, int *units) { /* drawing.rs:974 */
    Rd *r = &c->rd;
    for (;;) {
        Pair p;
        int k = next(r, &p);
        if (k < 0) return -1;
        if (!k) return 0;
        if (p.code != 0) return err_pair(r, &p, "expected 0/SECTION or 0/EOF");
        if (p.type != T_STR) return err_pair(r, &p, "expected 0/SECTION"); /* code 0 is always a string */
        if (is(&p, "EOF")) {
            put_back(r, &p);
            return 0;
        }
        if (!is(&p, "SECTION")) return err_pair(r, &p, "expected 0/SECTION");
        Pair s;
        k = next(r, &s);
        if (k < 0) return -1;
        if (!k) return err_eof(r);
        if (s.code != 2) return err_pair(r, &s, "expected 2/<section-name>");
        int rc;
        if (is(&s, "HEADER")) rc = read_header(c, units);
        else if (is(&s, "TABLES")) rc = read_tables(c);
        else if (is(&s, "BLOCKS")) rc = read_items(c, "BLOCK");
        else if (is(&s, "ENTITIES")) rc = collect(c, &c->top, &c->ntop, &c->captop);
        else rc = swallow_section(r); /* CLASSES, OBJECTS, THUMBNAILIMAGE: not read (DEVIATIONS 14) */
        if (rc) return -1;
        Pair e;
        k = next(r, &e);
        if (k < 0) return -1;
        if (!k) return err_eof(r);
        if (!(e.code == 0 && is(&e, "ENDSEC"))) return err_pair(r, &e, "expected 0/ENDSEC");
    }
}

/* ---------------------------------------------------------------- geometry (reference.rs) */

typedef struct {
    double m[6];
} M;

static void apply_m(const M *m, double x, double y, double out[2]) {
    out[0] = m->m[0] * x + m->m[2] * y + m->m[4];
    out[1] = m->m[1] * x + m->m[3] * y + m->m[5];
}

static M after(const M *self, const M *inner) {
    const double *a = inner->m, *b = self->m;
    M o = {{a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2], a[2] * b[1] + a[3] * b[3],
            a[4] * b[0] + a[5] * b[2] + b[4], a[4] * b[1] + a[5] * b[3] + b[5]}};
    return o;
}

typedef struct {
    Ctx *c;
    Dv pl; /* the polyline being made (x y pairs), before mapping */
    int too_long; /* it already holds more points than may ever be pushed */
} G;

static size_t room(const cc_dxf *d) { return CC_DXF_MAX_POINTS - d->npts; }

/* Takes k points of the point budget: 0, or -1 (the budget spent: its note, nothing taken). */
static int spend(G *g, size_t k) {
    Ctx *c = g->c;
    if (k > POINT_BUDGET - c->made) {
        c->d->over_points = 1;
        return -1;
    }
    c->made += k;
    return 0;
}

static int pl_add(G *g, double x, double y) {
    if (g->too_long || g->c->d->over_points) return 0;
    if (spend(g, 1)) return 0;
    if (g->pl.n / 2 > CC_DXF_MAX_POINTS) { /* can never fit: Out.push would truncate */
        g->too_long = 1;
        return 0;
    }
    if (dv_push(&g->pl, x) || dv_push(&g->pl, y)) return g->c->oom = 1, -1;
    return 0;
}

/* Out::push of the polyline in g->pl, mapped by m. */
static int out_push(G *g, const M *m, uint32_t layer) {
    cc_dxf *d = g->c->d;
    size_t n = g->pl.n / 2;
    if (d->over_points) { /* the budget ran out while it was made: not drawn */
        g->pl.n = 0, g->too_long = 0;
        return 0;
    }
    if (g->too_long) {
        g->pl.n = 0, g->too_long = 0;
        d->truncated = 1;
        return 0;
    }
    double *v = g->pl.v;
    for (size_t i = 0; i < n; i++) {
        double q[2];
        apply_m(m, v[2 * i], v[2 * i + 1], q);
        v[2 * i] = q[0], v[2 * i + 1] = q[1];
    }
    g->pl.n = 0;
    if (n < 2) return 0;
    for (size_t i = 0; i < 2 * n; i++)
        if (!finite_(v[i])) return 0;
    if (n > room(d)) {
        d->truncated = 1;
        return 0;
    }
    if (d->npts + n > d->caps[0]) {
        size_t cap = d->caps[0] ? d->caps[0] : 1024;
        while (cap < d->npts + n) cap *= 2;
        double *nx = realloc(d->xy, cap * 2 * sizeof *nx);
        if (!nx) return g->c->oom = 1, -1;
        d->xy = nx, d->caps[0] = cap;
    }
    if (d->npolylines + 2 > d->caps[1]) {
        size_t cap = d->caps[1] ? d->caps[1] * 2 : 64;
        size_t *ns = realloc(d->start, cap * sizeof *ns);
        if (!ns) return g->c->oom = 1, -1;
        d->start = ns;
        uint32_t *nl = realloc(d->layer, cap * sizeof *nl);
        if (!nl) return g->c->oom = 1, -1;
        d->layer = nl, d->caps[1] = cap;
    }
    memcpy(d->xy + 2 * d->npts, v, 2 * n * sizeof *v);
    d->start[d->npolylines] = d->npts;
    d->layer[d->npolylines] = layer;
    if (layer != CC_DXF_NO_LAYER) d->layers[layer].polylines++;
    d->npolylines++;
    d->npts += n;
    d->start[d->npolylines] = d->npts;
    return 0;
}

/* arc (reference.rs:92) into g->pl. 0 ok, -1 out of memory; 1: Rust would not end (skipped). */
static int arc(G *g, double cx, double cy, double r, double a0, double a1) {
    double sweep = a1 - a0;
    unsigned steps = 0;
    while (sweep <= 0.0) {
        if (++steps > SWEEP_STEPS) return 1;
        sweep += TAU;
    }
    if (sweep > MAX_SWEEP) sweep = TAU; /* past two turns: one (DEVIATIONS 14) */
    /* a non-finite centre or radius makes a non-finite point, which Out::push drops: the same
     * answer without the sines (a file of a million such arcs) */
    if (!finite_(cx) || !finite_(cy) || !finite_(r)) return 1;
    size_t n = as_count(max2(ceil_((sweep / TAU) * CIRCLE_SEGMENTS)));
    if (n + 1 > POINT_BUDGET - g->c->made) return g->c->d->over_points = 1, 1; /* before the sines */
    if (g->c->d->truncated && n + 1 > room(g->c->d)) return 1; /* refused or dropped: the same answer */
    for (size_t i = 0; i <= n; i++) {
        double a = a0 + sweep * (double)i / (double)n, s, co;
        cc_sincos(a, &s, &co);
        if (pl_add(g, cx + r * co, cy + r * s)) return -1;
    }
    return 0;
}

/* bulged (reference.rs:283) into g->pl. */
static int bulged(G *g, const double *x, const double *y, const double *bu, size_t n, int closed) {
    if (!n) return 0;
    size_t segs = closed ? n : n - 1;
    if (pl_add(g, x[0], y[0])) return -1;
    for (size_t i = 0; i < segs; i++) {
        double ax = x[i], ay = y[i], bulge = bu[i];
        double bx = x[(i + 1) % n], by = y[(i + 1) % n];
        if (fabs_(bulge) < 1e-9 || !finite_(bulge)) {
            if (pl_add(g, bx, by)) return -1;
            continue;
        }
        double theta = 4.0 * cc_atan2(bulge, 1.0);
        double dx = bx - ax, dy = by - ay;
        double chord = cc_sqrt(dx * dx + dy * dy);
        if (chord < 1e-12) continue;
        double r = chord / (2.0 * cc_sin(theta / 2.0));
        double mid0 = (ax + bx) / 2.0, mid1 = (ay + by) / 2.0;
        double h = r * cc_cos(theta / 2.0);
        double n0 = -(by - ay) / chord, n1 = (bx - ax) / chord;
        double c0 = mid0 + n0 * h, c1 = mid1 + n1 * h;
        double a0 = cc_atan2(ay - c1, ax - c0);
        size_t steps = as_count(max2(ceil_((fabs_(theta) / TAU) * CIRCLE_SEGMENTS)));
        double rr = fabs_(r);
        if (steps > POINT_BUDGET - g->c->made) return g->c->d->over_points = 1, 0; /* before the sines */
        for (size_t k = 1; k <= steps; k++) {
            double t = a0 + theta * (double)k / (double)steps, s, co;
            cc_sincos(t, &s, &co);
            if (pl_add(g, c0 + rr * co, c1 + rr * s)) return -1;
        }
    }
    return 0;
}

static const Block *find_block(const Ctx *c, const char *name, size_t n) { /* HashMap: the last wins */
    for (size_t i = c->nblocks; i-- > 0;)
        if (c->blocks[i].n == n && !memcmp(c->blocks[i].name, name, n)) return &c->blocks[i];
    return NULL;
}

static int add_text(Ctx *c, const Ent *e, const M *m, uint32_t layer, double k) {
    cc_dxf *d = c->d;
    if (d->ntexts >= CC_DXF_MAX_TEXTS) return 0;
    double at[2];
    apply_m(m, e->p[0][0], e->p[0][1], at);
    if (!finite_(at[0]) || !finite_(at[1])) return 0;
    if (d->ntexts == d->caps[3]) {
        size_t cap = d->caps[3] ? d->caps[3] * 2 : 16;
        cc_dxf_text *nt = realloc(d->texts, cap * sizeof *nt);
        if (!nt) return c->oom = 1, -1;
        d->texts = nt, d->caps[3] = cap;
    }
    char *t = dupz(e->name ? e->name : "", e->name ? e->namen : 0);
    if (!t) return c->oom = 1, -1;
    cc_dxf_text *x = &d->texts[d->ntexts++];
    x->at[0] = at[0], x->at[1] = at[1];
    x->height = e->height * k;
    x->angle_deg = e->type == E_TEXT ? e->rot : 0.0;
    x->layer = layer;
    x->text = t, x->n = e->name ? e->namen : 0;
    return 0;
}

static int d_spent(const Ctx *c) { return c->d->over_budget || c->d->over_points; }

/* dxf_entity (reference.rs:158). 0 ok, -1 out of memory. */
static int entity(G *g, const Ent *e, const M *m, size_t depth, uint32_t parent_layer, double k) {
    Ctx *c = g->c;
    cc_dxf *d = c->d;
    if (d->over_points) return 0;
    if (++c->work > WORK_BUDGET) {
        d->over_budget = 1;
        return 0;
    }
    uint32_t layer = e->layer;
    if (depth > 0 && parent_layer != CC_DXF_NO_LAYER &&
        (layer == CC_DXF_NO_LAYER || (d->layers[layer].n == 1 && d->layers[layer].name[0] == '0')))
        layer = parent_layer; /* an entity of layer 0 in a block takes the INSERT's (AutoCAD) */
    int rc = 0;
    switch (e->type) {
    case E_LINE:
        if (pl_add(g, e->p[0][0], e->p[0][1]) || pl_add(g, e->p[1][0], e->p[1][1])) return -1;
        return out_push(g, m, layer);
    case E_CIRCLE:
        rc = arc(g, e->p[0][0], e->p[0][1], e->r, 0.0, TAU);
        break;
    case E_ARC:
        rc = arc(g, e->p[0][0], e->p[0][1], e->r, e->a0 * RADS_PER_DEG, e->a1 * RADS_PER_DEG);
        break;
    case E_ELLIPSE: {
        double t0 = e->a0, t1 = e->a1, ax = e->p[1][0], ay = e->p[1][1], r = e->r;
        unsigned steps = 0;
        while (t1 <= t0) {
            if (++steps > SWEEP_STEPS) return g->pl.n = 0, 0;
            t1 += TAU;
        }
        if (t1 - t0 > MAX_SWEEP) t1 = t0 + TAU; /* past two turns: one (DEVIATIONS 14) */
        size_t n = as_count(max2(ceil_(((t1 - t0) / TAU) * CIRCLE_SEGMENTS)));
        if (n + 1 > POINT_BUDGET - c->made) d->over_points = 1; /* before the sines */
        else if (d->truncated && n + 1 > room(d)) return g->pl.n = 0, 0; /* refused or dropped: the same answer */
        else
            for (size_t i = 0; i <= n; i++) {
                double t = t0 + (t1 - t0) * (double)i / (double)n, s, co;
                cc_sincos(t, &s, &co);
                if (pl_add(g, e->p[0][0] + ax * co - ay * r * s, e->p[0][1] + ay * co + ax * r * s)) return -1;
            }
        break;
    }
    case E_LWPOLY:
    case E_POLY:
        rc = bulged(g, e->x.v, e->y.v, e->b.v, e->x.n, e->flags & 1);
        break;
    case E_SPLINE: {
        size_t nf = e->fx.n < e->fy.n ? e->fx.n : e->fy.n;
        if (e->fz.n < nf) nf = e->fz.n;
        size_t ncp = e->x.n < e->y.n ? e->x.n : e->y.n;
        if (e->z.n < ncp) ncp = e->z.n;
        int fit = nf >= 2;
        const double *px = fit ? e->fx.v : e->x.v, *py = fit ? e->fy.v : e->y.v;
        size_t n = fit ? nf : ncp;
        for (size_t i = 0; i < n; i++)
            if (pl_add(g, px[i], py[i])) return -1;
        break;
    }
    case E_SOLID: {
        static const int ORDER[5] = {0, 1, 3, 2, 0};
        for (int i = 0; i < 5; i++)
            if (pl_add(g, e->p[ORDER[i]][0], e->p[ORDER[i]][1])) return -1;
        break;
    }
    case E_INSERT: {
        const Block *b = find_block(c, e->name ? e->name : "", e->name ? e->namen : 0);
        if (!b || depth >= CC_DXF_MAX_DEPTH) {
            d->skipped++;
            return 0;
        }
        double rad = e->rot * RADS_PER_DEG;
        double s = cc_sin(rad), co = cc_cos(rad);
        double sx = e->sx, sy = e->sy;
        int cols = e->cols < 1 ? 1 : e->cols > 1000 ? 1000 : e->cols;
        int rows = e->rows < 1 ? 1 : e->rows > 1000 ? 1000 : e->rows;
        for (int row = 0; row < rows; row++)
            for (int col = 0; col < cols; col++) {
                double dx = (double)col * e->csp, dy = (double)row * e->rsp;
                M local = {{sx * co, sx * s, -sy * s, sy * co,
                            e->p[0][0] + dx * co - dy * s - (b->base[0] * sx * co - b->base[1] * sy * s),
                            e->p[0][1] + dx * s + dy * co - (b->base[0] * sx * s + b->base[1] * sy * co)}};
                M mm = after(m, &local);
                for (size_t i = 0; i < b->ne; i++) {
                    if (entity(g, &b->e[i], &mm, depth + 1, layer, k)) return -1;
                    if (d->truncated || d->over_budget || d->over_points) return 0;
                }
            }
        return 0;
    }
    case E_TEXT:
    case E_MTEXT:
        d->skipped++;
        return add_text(c, e, m, layer, k);
    default:
        d->skipped++;
        return 0;
    }
    if (rc < 0) return -1;
    if (rc > 0) {
        g->pl.n = 0, g->too_long = 0;
        return 0;
    }
    return out_push(g, m, layer);
}

/* ---------------------------------------------------------------- the reader */

void cc_dxf_free(cc_dxf *d) {
    if (!d) return;
    free(d->xy), free(d->start), free(d->layer);
    for (size_t i = 0; i < d->nlayers; i++) free(d->layers[i].name);
    free(d->layers);
    for (size_t i = 0; i < d->ntexts; i++) free(d->texts[i].text);
    free(d->texts);
    memset(d, 0, sizeof *d);
}

static void ctx_free(Ctx *c) {
    for (size_t i = 0; i < c->nblocks; i++) {
        for (size_t j = 0; j < c->blocks[i].ne; j++) ent_free(&c->blocks[i].e[j]);
        free(c->blocks[i].e), free(c->blocks[i].name);
    }
    free(c->blocks);
    for (size_t i = 0; i < c->ntop; i++) ent_free(&c->top[i]);
    free(c->top);
    free(c->lhash);
}

int cc_dxf_read(const uint8_t *bytes, size_t n, cc_dxf *out, Buf *err) {
    memset(out, 0, sizeof *out);
    if (n > CC_DXF_MAX_BYTES) { /* reference.rs:26 (the name is the caller's) */
        buf_puts(err, "larger than 256 MB");
        return -1;
    }
    /* Drawing::load_with_encoding: the first line says DXB or binary DXF. */
    size_t e = 0;
    while (e < n && bytes[e] != '\n') e++;
    size_t l = e;
    if (l && bytes[l - 1] == '\r') l--;
    if ((l == 15 && !memcmp(bytes, "AutoCAD DXB 1.0", 15)) || (l >= 18 && !memcmp(bytes, "AutoCAD Binary DXF", 18))) {
        buf_puts(err, "not a readable DXF: binary DXF is not read by the C core; save it as a text (ASCII) DXF");
        return -1;
    }
    Ctx c;
    memset(&c, 0, sizeof c);
    Buf ebuf;
    buf_init(&ebuf);
    c.rd.p = bytes, c.rd.n = n, c.rd.offset = 1, c.rd.err = &ebuf;
    buf_init(&c.rd.scratch);
    c.d = out;
    c.lhash = malloc(LHASH * sizeof *c.lhash);
    int rc = -1, units = 0;
    if (!c.lhash) {
        buf_puts(err, "out of memory");
        goto done;
    }
    for (size_t i = 0; i < LHASH; i++) c.lhash[i] = CC_DXF_NO_LAYER;
    if (read_sections(&c, &units)) {
        if (c.oom) buf_puts(err, "out of memory");
        else buf_puts(err, "not a readable DXF: "), buf_append(err, ebuf.p, ebuf.len);
        goto done;
    }
    { /* load_from_iter: then 0/EOF, or nothing */
        Pair p;
        int k = next(&c.rd, &p);
        if (k < 0 || (k == 1 && !(p.code == 0 && is(&p, "EOF")))) {
            if (k == 1) err_pair(&c.rd, &p, "expected 0/EOF");
            buf_puts(err, "not a readable DXF: "), buf_append(err, ebuf.p, ebuf.len);
            goto done;
        }
    }
    static const struct { int u; double k; const char *t; } U[] = {
        {4, 1.0, "mm"}, {5, 10.0, "cm"}, {14, 100.0, "dm"}, {6, 1000.0, "m"}, {1, 25.4, "inches"}, {2, 304.8, "feet"},
        {0, 1.0, "none (read as mm)"}};
    out->unit_mm = 1.0, out->unit = "unusual (read as mm)";
    for (size_t i = 0; i < sizeof U / sizeof U[0]; i++)
        if (U[i].u == units) out->unit_mm = U[i].k, out->unit = U[i].t;
    {
        double k = out->unit_mm;
        M scale = {{k, 0.0, 0.0, k, 0.0, 0.0}};
        G g = {&c, {0}, 0};
        for (size_t i = 0; i < c.ntop; i++) {
            if (c.top[i].paper) continue;
            if (entity(&g, &c.top[i], &scale, 0, CC_DXF_NO_LAYER, k)) break;
            /* the budgets are spent: nothing more is drawn (a truncated output goes on, as Rust's
             * Out::push: a later polyline that fits is still pushed, its work under POINT_BUDGET) */
            if (d_spent(&c)) break;
        }
        free(g.pl.v);
        if (c.oom) {
            buf_puts(err, "out of memory");
            goto done;
        }
    }
    rc = 0;
done:
    ctx_free(&c);
    buf_free(&ebuf);
    buf_free(&c.rd.scratch);
    if (rc) cc_dxf_free(out);
    return rc;
}

size_t cc_dxf_notes(const cc_dxf *d, Buf *out) {
    size_t k = 1;
    buf_puts(out, "DXF units: "), buf_puts(out, d->unit ? d->unit : "mm"), buf_putc(out, '\n');
    if (d->skipped) {
        buf_u64(out, d->skipped);
        buf_puts(out, " entities not drawn (text, hatches, 3D, nesting)\n");
        k++;
    }
    if (d->truncated) {
        buf_puts(out, "stopped at "), buf_u64(out, CC_DXF_MAX_POINTS), buf_puts(out, " points\n");
        k++;
    }
    if (d->over_budget) {
        buf_puts(out, "stopped expanding blocks (too many copies)\n");
        k++;
    }
    if (d->over_points) {
        buf_puts(out, "stopped drawing (more than 4000000 points made)\n");
        k++;
    }
    return k;
}

static int ends_with(const char *s, size_t n, const char *ext) {
    size_t k = strlen(ext);
    if (n < k) return 0;
    for (size_t i = 0; i < k; i++) {
        char c = s[n - k + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != ext[i]) return 0;
    }
    return 1;
}

int cc_dxf_name_kind(const char *name, size_t n, Buf *err) {
    if (ends_with(name, n, ".dxf")) return 1;
    if (ends_with(name, n, ".dwg")) {
        buf_puts(err, "DWG can't be read directly (its libraries are closed or GPL). Convert it to DXF first, for "
                      "example with the free ODA File Converter or LibreDWG's dwg2dxf, then add the DXF.");
        return -1;
    }
    return 0;
}
