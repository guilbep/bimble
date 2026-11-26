/* CBOR reader and writer, see cbor.h. */
#include "cbor.h"
#include <string.h>

typedef struct {
    Arena *a;
    const unsigned char *p;
    size_t n, pos;
    JrError *err;
    JrBudget bud; /* json_r.h: the same budget as the text form */
} R;

static int rfail(R *r, size_t off, const char *msg) {
    r->err->offset = off;
    size_t k = strlen(msg);
    if (k >= sizeof r->err->msg) k = sizeof r->err->msg - 1;
    memcpy(r->err->msg, msg, k);
    r->err->msg[k] = 0;
    return -1;
}

static double pow2(int e) { /* 2^e for -1022 <= e <= 1023 */
    uint64_t bits = (uint64_t)(e + 1023) << 52;
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static double half_to_f64(uint16_t h) {
    int exp = h >> 10 & 31;
    double m = h & 1023, v;
    if (exp == 0) v = m * pow2(-24);
    else if (exp == 31) {
        uint64_t bits = m == 0 ? 0x7FF0000000000000ull : 0x7FF8000000000000ull;
        memcpy(&v, &bits, 8);
    } else v = (m + 1024) * pow2(exp - 25);
    return h & 0x8000 ? -v : v;
}

/* Head of the item at r->pos: major type, argument. -1 on a reserved or indefinite one. */
static int head(R *r, int *major, uint64_t *arg, int *ai) {
    size_t at = r->pos;
    if (at >= r->n) return rfail(r, at, "unexpected end of CBOR data");
    unsigned b = r->p[r->pos++];
    *major = (int)(b >> 5);
    *ai = (int)(b & 31);
    if (*ai < 24) {
        *arg = (uint64_t)*ai;
        return 0;
    }
    if (*ai > 27) return rfail(r, at, *ai == 31 ? "indefinite-length CBOR items are not used in .bimble" : "reserved CBOR head");
    int k = 1 << (*ai - 24);
    if (r->n - r->pos < (size_t)k) return rfail(r, at, "unexpected end of CBOR data");
    uint64_t v = 0;
    for (int i = 0; i < k; i++) v = v << 8 | r->p[r->pos++];
    *arg = v;
    return 0;
}

/* Charges the DOM budget before the memory is taken. */
static int take(R *r, size_t at, size_t nodes, size_t each, size_t extra) {
    const char *why;
    return jr_budget_take(&r->bud, nodes, each, extra, &why) ? rfail(r, at, why) : 0;
}

static int text(R *r, size_t at, uint64_t len, const char **s, size_t *n) {
    if (len > r->n - r->pos) return rfail(r, at, "CBOR length runs past the data");
    if (take(r, at, 0, 0, (size_t)len + 1)) return -1;
    const char *src = (const char *)r->p + r->pos;
    if (jr_utf8_check(src, (size_t)len) >= 0) return rfail(r, at, "invalid UTF-8 in a CBOR text");
    char *d = arena_alloc(r->a, (size_t)len + 1, 1);
    if (!d) return rfail(r, at, "out of memory");
    memcpy(d, src, (size_t)len);
    d[len] = 0;
    r->pos += (size_t)len;
    *s = d;
    *n = (size_t)len;
    return 0;
}

static int item(R *r, JrVal *v, int depth) {
    int major, ai;
    uint64_t arg;
    size_t at = r->pos;
    memset(v, 0, sizeof *v);
    v->off = at;
    if (head(r, &major, &arg, &ai)) return -1;
    switch (major) {
    case 0:
        v->type = JR_NUM;
        v->num_int = 1;
        v->num = (double)arg;
        return 0;
    case 1:
        v->type = JR_NUM;
        v->num_int = 1;
        v->num_neg = 1;
        if (arg < ((uint64_t)1 << 63)) v->num = (double)(-1 - (int64_t)arg);
        else v->num = arg == UINT64_MAX ? -18446744073709551616.0 : -(double)(arg + 1);
        return 0;
    case 2: return rfail(r, at, "CBOR byte strings are not used in .bimble");
    case 3:
        v->type = JR_STR;
        return text(r, at, arg, &v->s, &v->n);
    case 4:
    case 5: {
        if (depth >= JR_MAX_DEPTH) return rfail(r, at, "recursion limit exceeded");
        /* each item takes one byte at least: a longer count is a lie, refused before allocating */
        size_t left = r->n - r->pos;
        if (arg > (major == 4 ? left : left / 2)) return rfail(r, at, "CBOR length runs past the data");
        size_t cnt = (size_t)arg;
        v->n = cnt;
        /* the count is honest only as far as the bytes go: the budget bounds what it asks for
         * (a 19 KB gzip of 20 M one-byte items asked for 1.2 GB at once) */
        if (take(r, at, cnt, major == 4 ? sizeof(JrVal) : sizeof(JrMember), 0)) return -1;
        if (major == 4) {
            v->type = JR_ARR;
            if (!cnt) return 0;
            v->items = arena_alloc_n(r->a, cnt, sizeof(JrVal), 8);
            if (!v->items) return rfail(r, at, "out of memory");
            for (size_t i = 0; i < cnt; i++)
                if (item(r, &v->items[i], depth + 1)) return -1;
            return 0;
        }
        v->type = JR_OBJ;
        if (!cnt) return 0;
        v->members = arena_alloc_n(r->a, cnt, sizeof(JrMember), 8);
        if (!v->members) return rfail(r, at, "out of memory");
        for (size_t i = 0; i < cnt; i++) {
            JrMember *m = &v->members[i];
            size_t kat = r->pos;
            int kmaj, kai;
            uint64_t klen;
            if (head(r, &kmaj, &klen, &kai)) return -1;
            if (kmaj != 3) return rfail(r, kat, "CBOR map key is not a text");
            if (text(r, kat, klen, &m->key, &m->key_n)) return -1;
            if (item(r, &m->val, depth + 1)) return -1;
        }
        return 0;
    }
    case 6: return rfail(r, at, "CBOR tags are not used in .bimble");
    default:
        v->type = JR_NUM;
        if (ai == 20 || ai == 21) {
            v->type = JR_BOOL;
            v->b = ai == 21;
        } else if (ai == 22) {
            v->type = JR_NULL;
        } else if (ai == 25) {
            v->num = half_to_f64((uint16_t)arg);
        } else if (ai == 26) {
            uint32_t bits = (uint32_t)arg;
            float f;
            memcpy(&f, &bits, 4);
            v->num = f;
        } else if (ai == 27) {
            memcpy(&v->num, &arg, 8);
        } else {
            return rfail(r, at, "CBOR simple value not used in .bimble");
        }
        if (v->type == JR_NUM) v->num_neg = (arg >> (ai == 25 ? 15 : ai == 26 ? 31 : 63)) & 1;
        return 0;
    }
}

JrVal *cbor_parse(Arena *a, const unsigned char *p, size_t n, JrError *err) {
    R r = {a, p, n, 0, err, {0, 0}};
    err->offset = 0;
    err->msg[0] = 0;
    if (take(&r, 0, 1, sizeof(JrVal), 0)) return NULL;
    JrVal *root = arena_alloc(a, sizeof *root, 8);
    if (!root) {
        rfail(&r, 0, "out of memory");
        return NULL;
    }
    if (item(&r, root, 0)) return NULL;
    if (r.pos != n) {
        rfail(&r, r.pos, "trailing bytes after the CBOR data");
        return NULL;
    }
    return root;
}

/* ---- writer ---- */

void cb_head(Buf *b, int major, uint64_t v) {
    unsigned char t[9];
    int k;
    unsigned m = (unsigned)major << 5;
    if (v < 24) t[0] = (unsigned char)(m | v), k = 0;
    else if (v <= 0xFF) t[0] = (unsigned char)(m | 24), k = 1;
    else if (v <= 0xFFFF) t[0] = (unsigned char)(m | 25), k = 2;
    else if (v <= 0xFFFFFFFFu) t[0] = (unsigned char)(m | 26), k = 4;
    else t[0] = (unsigned char)(m | 27), k = 8;
    for (int i = 0; i < k; i++) t[1 + i] = (unsigned char)(v >> 8 * (k - 1 - i));
    buf_append(b, t, (size_t)(1 + k));
}

void cb_text(Buf *b, const char *s, size_t n) {
    cb_head(b, CB_TEXT, n);
    buf_append(b, s, n);
}

static int put_be(Buf *b, unsigned char ib, uint64_t v, int k) {
    unsigned char t[9];
    t[0] = ib;
    for (int i = 0; i < k; i++) t[1 + i] = (unsigned char)(v >> 8 * (k - 1 - i));
    buf_append(b, t, (size_t)(1 + k));
    return 0;
}

/* ciborium: f16 if f64(f16(x)) has x's bits, else f32 if f64(f32(x)) does, else f64. */
static int f64_enc(Buf *b, double x) {
    uint64_t bits;
    memcpy(&bits, &x, 8);
    uint16_t sign = (uint16_t)(bits >> 48 & 0x8000);
    int e = (int)(bits >> 52 & 0x7FF);
    uint64_t mant = bits & 0xFFFFFFFFFFFFFull;
    if (e == 0 && mant == 0) return put_be(b, 0xF9, sign, 2);
    if (e == 0x7FF && mant == 0) return put_be(b, 0xF9, sign | 0x7C00u, 2);
    if (e != 0 && e != 0x7FF) {
        int ue = e - 1023;
        if (ue >= -14 && ue <= 15 && (mant & (((uint64_t)1 << 42) - 1)) == 0)
            return put_be(b, 0xF9, sign | (uint16_t)((ue + 15) << 10) | (uint16_t)(mant >> 42), 2);
        if (ue >= -24 && ue < -14) {
            int shift = 52 - (ue + 24); /* value = full * 2^(ue-52) = m * 2^-24 */
            uint64_t full = mant | (uint64_t)1 << 52;
            if ((full & (((uint64_t)1 << shift) - 1)) == 0) return put_be(b, 0xF9, sign | (uint16_t)(full >> shift), 2);
        }
        /* f32: normal exponents -126..127 with 23 mantissa bits, subnormal down to 2^-149 */
        if (ue >= -126 && ue <= 127 && (mant & (((uint64_t)1 << 29) - 1)) == 0) {
            float f = (float)x;
            uint32_t fb;
            memcpy(&fb, &f, 4);
            return put_be(b, 0xFA, fb, 4);
        }
        if (ue >= -149 && ue < -126) {
            int shift = 52 - (ue + 149);
            uint64_t full = mant | (uint64_t)1 << 52;
            if ((full & (((uint64_t)1 << shift) - 1)) == 0) {
                float f = (float)x;
                uint32_t fb;
                memcpy(&fb, &f, 4);
                return put_be(b, 0xFA, fb, 4);
            }
        }
    }
    return put_be(b, 0xFB, bits, 8);
}

void cb_f64(Buf *b, double x) { (void)f64_enc(b, x); }
