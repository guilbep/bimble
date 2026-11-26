/* The "BMG1" mesh codec, see meshcodec.h. A port of crates/bimble-geom/src/codec.rs. */
#include "meshcodec.h"
#include <stdlib.h>
#include <string.h>

#define STEP 0.0625 /* 1/16 mm */

void cc_put_varint(Buf *b, uint64_t v) {
    char t[10];
    size_t n = 0;
    while (v >= 0x80) {
        t[n++] = (char)((v & 0x7f) | 0x80);
        v >>= 7;
    }
    t[n++] = (char)v;
    buf_append(b, t, n);
}

static uint64_t zig(int64_t v) {
    return ((uint64_t)v << 1) ^ (uint64_t)(v < 0 ? -1 : 0);
}

static int64_t unzig(uint64_t v) {
    return (int64_t)((v >> 1) ^ (0 - (v & 1)));
}

static int is_finite_f(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return (u & 0x7f800000u) != 0x7f800000u;
}

static float inf_f(void) {
    uint32_t u = 0x7f800000u;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

float cc_f64_to_f32(double d) {
    /* 0x1.ffffffp127: FLT_MAX plus half an ulp, the first value that rounds to infinity. */
    const double lim = 340282356779733661637539395458142568448.0;
    if (d != d) return (float)d;
    if (d >= lim) return inf_f();
    if (d <= -lim) return -inf_f();
    return (float)d;
}

static double fabs_(double x) { return x < 0 ? -x : x; }

/* f64::round (half away from zero) then `as i64` (saturating), for |x| well inside i64. */
static int64_t round_i64(double x) {
    if (x != x) return 0;
    if (x >= 9.2e18) return INT64_MAX;
    if (x <= -9.2e18) return INT64_MIN;
    if (x < 0) return -round_i64(-x);
    int64_t t = (int64_t)x; /* trunc */
    if (x - (double)t >= 0.5) t++;
    return t;
}

static int64_t quant(float lo, float c) {
    return round_i64(((double)c - (double)lo) / STEP);
}

static float dequant(float lo, int64_t k) {
    return cc_f64_to_f32((double)lo + (double)k * STEP);
}

/* Quantizer: 1 with the lower corner in lo, 0 for raw. Points: positions then edge ends. */
static int quantizer(const cc_mesh *m, float lo[3]) {
    float hi[3];
    for (int k = 0; k < 3; k++) lo[k] = inf_f(), hi[k] = -inf_f();
    size_t np = (size_t)m->nv + 2 * (size_t)m->ne;
    for (size_t i = 0; i < np; i++) {
        const float *p = i < m->nv ? m->pos[i] : &m->edges[(i - m->nv) / 2][3 * ((i - m->nv) % 2)];
        for (int k = 0; k < 3; k++) {
            if (!is_finite_f(p[k])) return 0;
            if (p[k] < lo[k]) lo[k] = p[k]; /* f32::min / max (finite here; on a -0.0 / 0.0 tie the first kept) */
            if (p[k] > hi[k]) hi[k] = p[k];
        }
    }
    if (lo[0] > hi[0]) {
        lo[0] = lo[1] = lo[2] = 0.0f;
        return 1;
    }
    for (int k = 0; k < 3; k++)
        if (((double)hi[k] - (double)lo[k]) / STEP >= 4294967295.0) return 0;
    for (size_t i = 0; i < np; i++) {
        const float *p = i < m->nv ? m->pos[i] : &m->edges[(i - m->nv) / 2][3 * ((i - m->nv) % 2)];
        for (int k = 0; k < 3; k++)
            if (fabs_((double)dequant(lo[k], quant(lo[k], p[k])) - (double)p[k]) > CC_MESHCODEC_TOLERANCE_MM)
                return 0;
    }
    return 1;
}

static void put_f32(Buf *b, float f) {
    uint8_t t[4];
    uint32_t u;
    memcpy(&u, &f, 4);
    for (int i = 0; i < 4; i++) t[i] = (uint8_t)(u >> (8 * i));
    buf_append(b, t, 4);
}

static void put_point(Buf *b, int q, const float lo[3], int64_t prev[3], const float *p) {
    for (int k = 0; k < 3; k++) {
        if (q) {
            int64_t v = quant(lo[k], p[k]);
            cc_put_varint(b, zig((int64_t)((uint64_t)v - (uint64_t)prev[k])));
            prev[k] = v;
        } else {
            put_f32(b, p[k]);
        }
    }
}

static uint32_t hash3(const uint32_t k[3]) {
    uint64_t h = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 3; i++) h = (h ^ k[i]) * 0x100000001b3ull, h ^= h >> 29;
    return (uint32_t)h;
}

int cc_meshcodec_encode(const cc_mesh *m, Buf *out) {
    float lo[3];
    int q = quantizer(m, lo);
    uint32_t ne = m->ne, nn = m->nn < m->ne ? m->nn : m->ne;
    buf_append(out, "BMG1", 4);
    buf_putc(out, (char)q);
    cc_put_varint(out, m->nv);
    cc_put_varint(out, m->ni);
    cc_put_varint(out, ne);
    cc_put_varint(out, nn);
    if (q)
        for (int k = 0; k < 3; k++) put_f32(out, lo[k]);
    int64_t prev[3] = {0, 0, 0};
    for (uint32_t i = 0; i < m->nv; i++) put_point(out, q, lo, prev, m->pos[i]);
    int64_t last = 0;
    for (uint32_t i = 0; i < m->ni; i++) {
        cc_put_varint(out, zig((int64_t)m->idx[i] - last));
        last = m->idx[i];
    }
    /* Vertex bits -> first index with those bits (open addressing). */
    size_t cap = 16;
    while (cap < 2 * (size_t)m->nv + 2) cap <<= 1;
    uint32_t *slot = malloc(cap * sizeof *slot); /* vertex index + 1, 0 = empty */
    if (!slot) return -1;
    memset(slot, 0, cap * sizeof *slot);
    for (uint32_t i = 0; i < m->nv; i++) {
        uint32_t key[3];
        memcpy(key, m->pos[i], 12);
        size_t h = hash3(key) & (cap - 1);
        while (slot[h] && memcmp(m->pos[slot[h] - 1], key, 12) != 0) h = (h + 1) & (cap - 1);
        if (!slot[h]) slot[h] = i + 1;
    }
    last = 0;
    int64_t eprev[3] = {0, 0, 0};
    for (uint32_t e = 0; e < ne; e++)
        for (int j = 0; j < 2; j++) {
            const float *p = &m->edges[e][3 * j];
            uint32_t key[3];
            memcpy(key, p, 12);
            size_t h = hash3(key) & (cap - 1);
            while (slot[h] && memcmp(m->pos[slot[h] - 1], key, 12) != 0) h = (h + 1) & (cap - 1);
            if (slot[h]) {
                int64_t i = slot[h] - 1;
                cc_put_varint(out, zig(i - last) << 1);
                last = i;
            } else {
                buf_putc(out, 1);
                put_point(out, q, lo, eprev, p);
            }
        }
    free(slot);
    /* Distinct normals in first-use order; key = 3 bytes + 1 (0 = empty). */
    size_t ncap = 16;
    while (ncap < 4 * (size_t)nn + 4) ncap <<= 1;
    uint32_t *nkey = malloc(ncap * sizeof *nkey);
    uint32_t *nval = malloc(ncap * sizeof *nval);
    uint32_t *codes = malloc((2 * (size_t)nn + 1) * sizeof *codes);
    if (!nkey || !nval || !codes) {
        free(nkey), free(nval), free(codes);
        return -1;
    }
    memset(nkey, 0, ncap * sizeof *nkey);
    uint32_t ncodes = 0;
#define NKEY(e, j) \
    (((uint32_t)(uint8_t)m->normals[e][4 * (j)] | (uint32_t)(uint8_t)m->normals[e][4 * (j) + 1] << 8 | \
      (uint32_t)(uint8_t)m->normals[e][4 * (j) + 2] << 16) + 1)
#define NSLOT(key, h)                                                       \
    do {                                                                    \
        uint32_t k_ = (key);                                                \
        h = (size_t)((k_ * 0x9e3779b1u) >> 7) & (ncap - 1);                 \
        while (nkey[h] && nkey[h] != k_) h = (h + 1) & (ncap - 1);         \
    } while (0)
    for (uint32_t e = 0; e < nn; e++)
        for (int j = 0; j < 2; j++) {
            size_t h;
            uint32_t k = NKEY(e, j);
            NSLOT(k, h);
            if (!nkey[h]) {
                nkey[h] = k;
                nval[h] = ncodes;
                codes[ncodes++] = k - 1;
            }
        }
    cc_put_varint(out, ncodes);
    for (uint32_t i = 0; i < ncodes; i++) {
        char t[3] = {(char)(codes[i] & 0xff), (char)(codes[i] >> 8 & 0xff), (char)(codes[i] >> 16 & 0xff)};
        buf_append(out, t, 3);
    }
    for (uint32_t e = 0; e < nn; e++)
        for (int j = 0; j < 2; j++) {
            size_t h;
            NSLOT(NKEY(e, j), h);
            cc_put_varint(out, nval[h]);
        }
#undef NKEY
#undef NSLOT
    free(nkey), free(nval), free(codes);
    return out->oom ? -1 : 0;
}

/* ---- decode ---- */

typedef struct {
    const uint8_t *p;
    size_t n;
} Rd;

static int rd_byte(Rd *r, uint8_t *b) {
    if (!r->n) return CC_MC_TRUNCATED;
    *b = *r->p++;
    r->n--;
    return 0;
}

static int rd_f32(Rd *r, float *f) {
    if (r->n < 4) return CC_MC_TRUNCATED;
    uint32_t u = (uint32_t)r->p[0] | (uint32_t)r->p[1] << 8 | (uint32_t)r->p[2] << 16 | (uint32_t)r->p[3] << 24;
    memcpy(f, &u, 4);
    r->p += 4, r->n -= 4;
    return 0;
}

static int rd_varint(Rd *r, uint64_t *v, const char **why) {
    *v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        uint8_t b;
        if (rd_byte(r, &b)) return CC_MC_TRUNCATED;
        *v |= (uint64_t)(b & 0x7f) << shift;
        if (b < 0x80) return 0;
    }
    *why = "varint too long";
    return CC_MC_INVALID;
}

#define TRY(x)                \
    do {                      \
        int rc_ = (x);        \
        if (rc_) return rc_;  \
    } while (0)

static int rd_point(Rd *r, int q, const float lo[3], int64_t prev[3], float p[3], const char **why) {
    for (int k = 0; k < 3; k++) {
        if (q) {
            uint64_t v;
            TRY(rd_varint(r, &v, why));
            prev[k] = (int64_t)((uint64_t)prev[k] + (uint64_t)unzig(v));
            p[k] = dequant(lo[k], prev[k]);
        } else {
            TRY(rd_f32(r, &p[k]));
        }
    }
    return 0;
}

static uint64_t sat_add(uint64_t a, uint64_t b) { return a + b < a ? UINT64_MAX : a + b; }
static uint64_t sat_mul(uint64_t a, uint64_t b) { return b && a > UINT64_MAX / b ? UINT64_MAX : a * b; }

int cc_meshcodec_decode(const uint8_t *p, size_t n, Arena *a, cc_mesh *out, const char **why) {
    Rd r = {p, n};
    memset(out, 0, sizeof *out);
    *why = "";
    if (n < 4 || memcmp(p, "BMG1", 4) != 0) return CC_MC_BAD_MAGIC;
    r.p += 4, r.n -= 4;
    uint8_t mode;
    TRY(rd_byte(&r, &mode));
    if (mode > 1) {
        *why = "unknown mode";
        return CC_MC_INVALID;
    }
    int q = mode;
    uint64_t nv, ni, ne, nn;
    TRY(rd_varint(&r, &nv, why));
    TRY(rd_varint(&r, &ni, why));
    TRY(rd_varint(&r, &ne, why));
    TRY(rd_varint(&r, &nn, why));
    uint64_t need = sat_add(sat_add(sat_add(sat_mul(nv, q ? 3 : 12), ni), sat_mul(ne, 2)), sat_mul(nn, 2));
    if (need > r.n) return CC_MC_TRUNCATED;
    if (nn > ne || ni % 3 != 0) {
        *why = "counts";
        return CC_MC_INVALID;
    }
    float lo[3] = {0, 0, 0};
    if (q)
        for (int k = 0; k < 3; k++) TRY(rd_f32(&r, &lo[k]));
    /* Counts are below n (checked above), so they fit u32 and size_t. */
    out->pos = arena_alloc_n(a, (size_t)nv + 1, sizeof out->pos[0], 4);
    out->idx = arena_alloc_n(a, (size_t)ni + 1, sizeof(uint32_t), 4);
    out->edges = arena_alloc_n(a, (size_t)ne + 1, sizeof out->edges[0], 4);
    out->normals = arena_alloc_n(a, (size_t)nn + 1, sizeof out->normals[0], 1);
    if (!out->pos || !out->idx || !out->edges || !out->normals) return CC_MC_OOM;
    int64_t prev[3] = {0, 0, 0};
    for (uint64_t i = 0; i < nv; i++) TRY(rd_point(&r, q, lo, prev, out->pos[i], why));
    out->nv = (uint32_t)nv;
    int64_t last = 0;
    for (uint64_t i = 0; i < ni; i++) {
        uint64_t v;
        TRY(rd_varint(&r, &v, why));
        last = (int64_t)((uint64_t)last + (uint64_t)unzig(v));
        if (last < 0 || (uint64_t)last >= nv) {
            *why = "index out of range";
            return CC_MC_INVALID;
        }
        out->idx[i] = (uint32_t)last;
    }
    out->ni = (uint32_t)ni;
    int64_t elast = 0, eprev[3] = {0, 0, 0};
    for (uint64_t e = 0; e < ne; e++)
        for (int j = 0; j < 2; j++) {
            uint64_t c;
            float *pt = &out->edges[e][3 * j];
            TRY(rd_varint(&r, &c, why));
            if (c == 1) {
                TRY(rd_point(&r, q, lo, eprev, pt, why));
                continue;
            }
            if (c & 1) {
                *why = "edge code";
                return CC_MC_INVALID;
            }
            elast = (int64_t)((uint64_t)elast + (uint64_t)unzig(c >> 1));
            if (elast < 0 || (uint64_t)elast >= nv) {
                *why = "edge vertex";
                return CC_MC_INVALID;
            }
            memcpy(pt, out->pos[elast], 12);
        }
    out->ne = (uint32_t)ne;
    uint64_t nt;
    TRY(rd_varint(&r, &nt, why));
    if (nt > r.n / 3) return CC_MC_TRUNCATED;
    const uint8_t *table = r.p;
    r.p += nt * 3, r.n -= nt * 3;
    for (uint64_t e = 0; e < nn; e++)
        for (int j = 0; j < 2; j++) {
            uint64_t i;
            TRY(rd_varint(&r, &i, why));
            if (i >= nt) {
                *why = "normal";
                return CC_MC_INVALID;
            }
            for (int k = 0; k < 3; k++) out->normals[e][4 * j + k] = (int8_t)table[i * 3 + k];
            out->normals[e][4 * j + 3] = 0;
        }
    out->nn = (uint32_t)nn;
    if (r.n) {
        *why = "trailing bytes";
        return CC_MC_INVALID;
    }
    return 0;
}
