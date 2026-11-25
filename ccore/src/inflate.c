/* gzip + deflate, see inflate.h. Written for this project from RFC 1951 and RFC 1952. */
#include "inflate.h"
#include <stdlib.h>
#include <string.h>

/* ---- CRC-32 ---- */

/* Slicing-by-8 (8 table lookups per 8 bytes, bytes read one by one: any endianness, any
 * alignment). Ticket J1: the byte-at-a-time loop was ~50 ms of a 16 MB gunzip (320 MB/s). */
static uint32_t crc_table[8][256];
static int crc_ready;

uint32_t cc_crc32(uint32_t crc, const void *p, size_t n) {
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; i++)
            for (int t = 1; t < 8; t++) crc_table[t][i] = crc_table[0][crc_table[t - 1][i] & 0xFF] ^ (crc_table[t - 1][i] >> 8);
        crc_ready = 1;
    }
    const unsigned char *s = p;
    crc = ~crc;
    while (n >= 8) {
        uint32_t a = crc ^ ((uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24);
        crc = crc_table[7][a & 0xFF] ^ crc_table[6][(a >> 8) & 0xFF] ^ crc_table[5][(a >> 16) & 0xFF] ^ crc_table[4][a >> 24] ^
              crc_table[3][s[4]] ^ crc_table[2][s[5]] ^ crc_table[1][s[6]] ^ crc_table[0][s[7]];
        s += 8, n -= 8;
    }
    for (size_t i = 0; i < n; i++) crc = crc_table[0][(crc ^ s[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- tables shared by both directions ---- */

static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                       33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                       1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* ---- inflate ---- */

#define FAST_BITS 10

typedef struct {
    uint16_t count[16];          /* codes per length */
    uint16_t sym[288];           /* symbols by code order */
    uint16_t fast[1 << FAST_BITS]; /* low FAST_BITS of the stream -> sym << 4 | len; 0 = longer */
} Huff;

typedef struct {
    const unsigned char *in;
    size_t n, pos;
    uint64_t bits;
    int nbits;
    Buf *out;
    size_t start, max; /* output offset of this member; cap on its length */
    const char *why;
} Inf;

static void refill(Inf *s) {
    while (s->nbits <= 56 && s->pos < s->n) {
        s->bits |= (uint64_t)s->in[s->pos++] << s->nbits;
        s->nbits += 8;
    }
}

static int fail(Inf *s, const char *why) {
    if (!s->why) s->why = why;
    return -1;
}

/* k bits (k <= 32), LSB first. -1 at the end of the data. */
static int getbits(Inf *s, int k, uint32_t *v) {
    if (s->nbits < k) {
        refill(s);
        if (s->nbits < k) return fail(s, "gzip: unexpected end of data");
    }
    *v = (uint32_t)(s->bits & (((uint64_t)1 << k) - 1));
    s->bits >>= k;
    s->nbits -= k;
    return 0;
}

static uint32_t rev_bits(uint32_t c, int len) {
    uint32_t r = 0;
    for (int i = 0; i < len; i++, c >>= 1) r = r << 1 | (c & 1);
    return r;
}

/* Canonical code from code lengths. Over-subscribed sets are errors; incomplete ones are
 * accepted (an unused code is an error when met). */
static int build(Huff *h, const uint8_t *lens, int n) {
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lens[i]) h->sym[offs[lens[i]]++] = (uint16_t)i;
    /* fast table: walk symbols in code order, codes counting up within each length */
    uint32_t code = 0;
    int idx = 0;
    for (int len = 1; len < 16; len++) {
        for (int k = 0; k < h->count[len]; k++, idx++, code++) {
            if (len > FAST_BITS) continue;
            uint32_t r = rev_bits(code, len);
            for (uint32_t j = r; j < (1u << FAST_BITS); j += 1u << len)
                h->fast[j] = (uint16_t)(h->sym[idx] << 4 | len);
        }
        code <<= 1;
    }
    return 0;
}

static int decode(Inf *s, const Huff *h) {
    if (s->nbits < 15) refill(s);
    uint16_t e = h->fast[s->bits & ((1u << FAST_BITS) - 1)];
    if (e && (e & 15) <= s->nbits) {
        s->bits >>= e & 15;
        s->nbits -= e & 15;
        return e >> 4;
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        if (s->nbits < 1) return fail(s, "gzip: unexpected end of data");
        code |= (int)(s->bits & 1);
        s->bits >>= 1;
        s->nbits--;
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return fail(s, "gzip: invalid Huffman code");
}

static int put_room(Inf *s, size_t k) {
    if (s->out->len - s->start > s->max || k > s->max - (s->out->len - s->start))
        return fail(s, "gzip: decompressed data too large");
    if (buf_reserve(s->out, k)) return fail(s, "gzip: out of memory");
    return 0;
}

static int codes(Inf *s, const Huff *lit, const Huff *dist) {
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->out->len == s->out->cap || s->out->len - s->start >= s->max)
                if (put_room(s, 1)) return -1;
            s->out->p[s->out->len++] = (char)sym;
            continue;
        }
        if (sym == 256) return 0;
        sym -= 257;
        if (sym >= 29) return fail(s, "gzip: invalid length code");
        uint32_t x, len, d;
        if (getbits(s, len_extra[sym], &x)) return -1;
        len = len_base[sym] + x;
        int ds = decode(s, dist);
        if (ds < 0) return -1;
        if (ds >= 30) return fail(s, "gzip: invalid distance code");
        if (getbits(s, dist_extra[ds], &x)) return -1;
        d = dist_base[ds] + x;
        if (d > s->out->len - s->start) return fail(s, "gzip: distance too far back");
        if (put_room(s, len)) return -1;
        char *o = s->out->p + s->out->len;
        const char *from = o - d;
        for (uint32_t i = 0; i < len; i++) o[i] = from[i];
        s->out->len += len;
    }
}

static int stored(Inf *s) {
    uint32_t len, nlen, v;
    s->bits >>= s->nbits & 7;
    s->nbits -= s->nbits & 7;
    if (getbits(s, 16, &len) || getbits(s, 16, &nlen)) return -1;
    if ((len ^ 0xFFFF) != nlen) return fail(s, "gzip: stored block length check failed");
    if (put_room(s, len)) return -1;
    while (len && s->nbits >= 8) {
        if (getbits(s, 8, &v)) return -1;
        s->out->p[s->out->len++] = (char)v;
        len--;
    }
    if (len > s->n - s->pos) return fail(s, "gzip: unexpected end of data");
    if (len) memcpy(s->out->p + s->out->len, s->in + s->pos, len); /* an empty stored block: p may be NULL */
    s->out->len += len;
    s->pos += len;
    return 0;
}

static int fixed(Inf *s, Huff *lit, Huff *dist) {
    uint8_t l[288];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    build(lit, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    build(dist, l, 30);
    return codes(s, lit, dist);
}

static int dynamic(Inf *s, Huff *lit, Huff *dist) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t lens[288 + 32];
    uint32_t nlen, ndist, ncode, v;
    if (getbits(s, 5, &nlen) || getbits(s, 5, &ndist) || getbits(s, 4, &ncode)) return -1;
    nlen += 257, ndist += 1, ncode += 4;
    if (nlen > 286 || ndist > 30) return fail(s, "gzip: bad code counts");
    memset(lens, 0, sizeof lens);
    for (uint32_t i = 0; i < ncode; i++) {
        if (getbits(s, 3, &v)) return -1;
        lens[order[i]] = (uint8_t)v;
    }
    if (build(lit, lens, 19)) return fail(s, "gzip: bad code lengths code");
    for (uint32_t i = 0; i < nlen + ndist;) {
        int sym = decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 16) {
            lens[i++] = (uint8_t)sym;
            continue;
        }
        uint8_t rep = 0;
        uint32_t times;
        if (sym == 16) {
            if (i == 0) return fail(s, "gzip: repeat with no first length");
            rep = lens[i - 1];
            if (getbits(s, 2, &times)) return -1;
            times += 3;
        } else if (sym == 17) {
            if (getbits(s, 3, &times)) return -1;
            times += 3;
        } else {
            if (getbits(s, 7, &times)) return -1;
            times += 11;
        }
        if (i + times > nlen + ndist) return fail(s, "gzip: too many code lengths");
        while (times--) lens[i++] = rep;
    }
    if (lens[256] == 0) return fail(s, "gzip: no end-of-block code");
    uint8_t dl[30];
    memcpy(dl, lens + nlen, ndist);
    if (build(lit, lens, (int)nlen)) return fail(s, "gzip: bad literal/length lengths");
    if (build(dist, dl, (int)ndist)) return fail(s, "gzip: bad distance lengths");
    return codes(s, lit, dist);
}

int cc_gunzip(const unsigned char *in, size_t n, size_t max_out, Buf *out, const char **why) {
    *why = NULL;
    size_t p = 10;
    if (n < 18 || in[0] != 0x1F || in[1] != 0x8B) return *why = "gzip: not gzip data", -1;
    if (in[2] != 8) return *why = "gzip: unknown compression method", -1;
    int flg = in[3];
    if (flg & 0xE0) return *why = "gzip: reserved header flags set", -1;
    if (flg & 4) { /* FEXTRA */
        if (n - p < 2) return *why = "gzip: unexpected end of data", -1;
        size_t xlen = in[p] | (size_t)in[p + 1] << 8;
        p += 2;
        if (n - p < xlen) return *why = "gzip: unexpected end of data", -1;
        p += xlen;
    }
    for (int f = 8; f <= 16; f += 8)
        if (flg & f) { /* FNAME, FCOMMENT: zero-terminated */
            while (p < n && in[p]) p++;
            if (p >= n) return *why = "gzip: unexpected end of data", -1;
            p++;
        }
    if (flg & 2) { /* FHCRC */
        if (n - p < 2) return *why = "gzip: unexpected end of data", -1;
        if ((cc_crc32(0, in, p) & 0xFFFF) != (uint32_t)(in[p] | in[p + 1] << 8))
            return *why = "gzip: header checksum mismatch", -1;
        p += 2;
    }
    Inf s = {in, n, p, 0, 0, out, out->len, max_out, NULL};
    Huff *h = malloc(2 * sizeof(Huff));
    if (!h) return *why = "gzip: out of memory", -1;
    int rc = 0;
    uint32_t last = 0, type;
    while (!rc && !last) {
        if (getbits(&s, 1, &last) || getbits(&s, 2, &type)) rc = -1;
        else if (type == 0) rc = stored(&s);
        else if (type == 1) rc = fixed(&s, &h[0], &h[1]);
        else if (type == 2) rc = dynamic(&s, &h[0], &h[1]);
        else rc = fail(&s, "gzip: invalid block type");
    }
    free(h);
    if (!rc) {
        uint32_t crc, size, b0 = 0, b1 = 0;
        s.bits >>= s.nbits & 7;
        s.nbits -= s.nbits & 7;
        if (getbits(&s, 16, &b0) || getbits(&s, 16, &b1)) rc = -1;
        crc = b0 | b1 << 16;
        if (!rc && (getbits(&s, 16, &b0) || getbits(&s, 16, &b1))) rc = -1;
        size = b0 | b1 << 16;
        if (!rc && crc != cc_crc32(0, out->p + s.start, out->len - s.start))
            rc = fail(&s, "gzip: data checksum mismatch");
        if (!rc && size != (uint32_t)(out->len - s.start)) rc = fail(&s, "gzip: length mismatch");
        if (!rc && (s.nbits || s.pos != n)) rc = fail(&s, "gzip: trailing bytes after the data");
    }
    if (rc) {
        out->len = s.start;
        *why = s.why ? s.why : "gzip: damaged data";
    }
    return rc;
}

/* zlib (RFC 1950) for PDF FlateDecode streams (pdf_in.c, ticket L1): the 2-byte header (a raw
 * deflate stream without one is read too), the blocks, then anything after (the Adler-32 sum,
 * padding) ignored, as PDF readers do. */
int cc_zinflate(const unsigned char *in, size_t n, size_t max_out, Buf *out, const char **why) {
    *why = NULL;
    size_t p = 0;
    if (n >= 2 && (in[0] & 15) == 8 && (((unsigned)in[0] << 8) | in[1]) % 31 == 0) p = (in[1] & 0x20) ? 6 : 2;
    if (p > n) return *why = "zlib: unexpected end of data", -1;
    Inf s = {in, n, p, 0, 0, out, out->len, max_out, NULL};
    Huff *h = malloc(2 * sizeof(Huff));
    if (!h) return *why = "zlib: out of memory", -1;
    int rc = 0;
    uint32_t last = 0, type;
    while (!rc && !last) {
        if (getbits(&s, 1, &last) || getbits(&s, 2, &type)) rc = -1;
        else if (type == 0) rc = stored(&s);
        else if (type == 1) rc = fixed(&s, &h[0], &h[1]);
        else if (type == 2) rc = dynamic(&s, &h[0], &h[1]);
        else rc = fail(&s, "zlib: invalid block type");
    }
    free(h);
    if (rc) {
        out->len = s.start;
        *why = s.why ? s.why : "zlib: damaged data";
    }
    return rc;
}

/* ---- deflate: greedy LZ77, fixed Huffman codes, one block ---- */

typedef struct {
    Buf *out;
    uint64_t acc;
    int n;
} BitW;

static void put(BitW *w, uint32_t v, int k) {
    w->acc |= (uint64_t)v << w->n;
    w->n += k;
    while (w->n >= 8) {
        buf_putc(w->out, (char)(w->acc & 0xFF));
        w->acc >>= 8;
        w->n -= 8;
    }
}

/* Fixed literal/length code of `sym`, bits in stream order. */
static void put_lit(BitW *w, int sym) {
    if (sym < 144) put(w, rev_bits(0x30 + (uint32_t)sym, 8), 8);
    else if (sym < 256) put(w, rev_bits(0x190 + (uint32_t)(sym - 144), 9), 9);
    else if (sym < 280) put(w, rev_bits((uint32_t)(sym - 256), 7), 7);
    else put(w, rev_bits(0xC0 + (uint32_t)(sym - 280), 8), 8);
}

static void put_match(BitW *w, uint32_t len, uint32_t d) {
    int c = 28;
    while (len_base[c] > len) c--;
    put_lit(w, 257 + c);
    if (len_extra[c]) put(w, len - len_base[c], len_extra[c]);
    c = 29;
    while (dist_base[c] > d) c--;
    put(w, rev_bits((uint32_t)c, 5), 5);
    if (dist_extra[c]) put(w, d - dist_base[c], dist_extra[c]);
}

#define HASH_BITS 15
#define WINDOW 32768
#define CHAIN 8

static uint32_t hash3(const unsigned char *p) {
    return ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 2654435761u >> (32 - HASH_BITS);
}

int cc_gzip(const unsigned char *in, size_t n, Buf *out) {
    static const unsigned char head[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 4, 0xFF};
    buf_append(out, head, sizeof head);
    /* head/prev hold position + 1 (0 = none), positions mod 2^32 kept apart by the window check */
    uint32_t *tab = calloc((size_t)(1 << HASH_BITS) + WINDOW, sizeof *tab);
    if (!tab) return -1;
    uint32_t *hh = tab, *prev = tab + (1 << HASH_BITS);
    BitW w = {out, 0, 0};
    put(&w, 1, 1); /* final */
    put(&w, 1, 2); /* fixed codes */
    size_t i = 0;
    while (i < n) {
        uint32_t best = 0, bestd = 0;
        if (n - i >= 3) {
            uint32_t h = hash3(in + i);
            size_t maxlen = n - i < 258 ? n - i : 258;
            uint32_t cand = hh[h];
            for (int c = 0; c < CHAIN && cand; c++) {
                size_t p = cand - 1;
                if (p >= i || i - p > WINDOW) break;
                if (in[p + best] == in[i + best]) {
                    size_t l = 0;
                    while (l < maxlen && in[p + l] == in[i + l]) l++;
                    if (l > best) {
                        best = (uint32_t)l;
                        bestd = (uint32_t)(i - p);
                        if (l == maxlen) break;
                    }
                }
                uint32_t nx = prev[p & (WINDOW - 1)];
                if (nx >= cand) break; /* chains only go back */
                cand = nx;
            }
            prev[i & (WINDOW - 1)] = hh[h];
            hh[h] = (uint32_t)(i + 1);
        }
        if (best >= 3) {
            put_match(&w, best, bestd);
            size_t end = i + best;
            for (i++; i < end; i++)
                if (n - i >= 3) {
                    uint32_t h = hash3(in + i);
                    prev[i & (WINDOW - 1)] = hh[h];
                    hh[h] = (uint32_t)(i + 1);
                }
        } else {
            put_lit(&w, in[i]);
            i++;
        }
    }
    put_lit(&w, 256);
    if (w.n) put(&w, 0, 8 - w.n);
    free(tab);
    uint32_t crc = cc_crc32(0, in, n), size = (uint32_t)n;
    unsigned char tail[8];
    for (int k = 0; k < 4; k++) tail[k] = (unsigned char)(crc >> 8 * k), tail[4 + k] = (unsigned char)(size >> 8 * k);
    buf_append(out, tail, 8);
    return out->oom ? -1 : 0;
}

/* Stored blocks (no compression, ticket J1): any gzip reader inflates it; a memcpy and the CRC. */
int cc_gzip_stored(const unsigned char *in, size_t n, Buf *out) {
    static const unsigned char head[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};
    buf_append(out, head, sizeof head);
    size_t i = 0;
    do {
        size_t k = n - i < 65535 ? n - i : 65535;
        unsigned char h[5] = {(unsigned char)(i + k == n), (unsigned char)k, (unsigned char)(k >> 8),
                              (unsigned char)~k, (unsigned char)(~k >> 8)};
        buf_append(out, h, 5);
        if (k) buf_append(out, in + i, k);
        i += k;
    } while (i < n);
    uint32_t crc = cc_crc32(0, in, n), size = (uint32_t)n;
    unsigned char tail[8];
    for (int k = 0; k < 4; k++) tail[k] = (unsigned char)(crc >> 8 * k), tail[4 + k] = (unsigned char)(size >> 8 * k);
    buf_append(out, tail, 8);
    return out->oom ? -1 : 0;
}
