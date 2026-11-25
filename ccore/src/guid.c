#include "guid.h"
#include <stdlib.h>
#include <string.h>

/* Vendored BLAKE3 (vendor/blake3, see SOURCE), portable code only, in this translation unit so
 * build.sh needs no change. SIMD is off: tcc and freestanding wasm32 cannot build it. */
#define BLAKE3_NO_SSE2
#define BLAKE3_NO_SSE41
#define BLAKE3_NO_AVX2
#define BLAKE3_NO_AVX512
#define BLAKE3_USE_NEON 0
#if defined(__clang__)
#pragma clang diagnostic push
/* blake3.c keeps a variable only for assert(), which the wasm32 build compiles out. */
#pragma clang diagnostic ignored "-Wunused-variable"
#endif
#include "../vendor/blake3/blake3.c"
#include "../vendor/blake3/blake3_dispatch.c"
#include "../vendor/blake3/blake3_portable.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#ifndef __wasm__
#include <errno.h>
#include <sys/random.h>
#endif

static const char ALPHABET[65] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$";

/* Index of c in ALPHABET, or -1. */
static int alpha_index(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 'a' && c <= 'z') return c - 'a' + 36;
    if (c == '_') return 62;
    if (c == '$') return 63;
    return -1;
}

cc_guid cc_guid_from_u128(uint64_t hi, uint64_t lo) {
    cc_guid g;
    g.c[0] = ALPHABET[(hi >> 62) & 0x3];
    for (int i = 1; i < 22; i++) {
        int shift = 126 - 6 * i; /* bits [shift, shift + 6) of the u128 */
        uint64_t v;
        if (shift >= 64) v = hi >> (shift - 64);
        else if (shift + 6 <= 64) v = lo >> shift;
        else v = (lo >> shift) | (hi << (64 - shift));
        g.c[i] = ALPHABET[v & 0x3f];
    }
    return g;
}

static void u128_to_raw(uint64_t hi, uint64_t lo, cc_guid_raw *out) {
    for (int i = 0; i < 8; i++) {
        out->b[i] = (uint8_t)(hi >> (56 - 8 * i));
        out->b[8 + i] = (uint8_t)(lo >> (56 - 8 * i));
    }
}

static void raw_to_u128(const cc_guid_raw *raw, uint64_t *hi, uint64_t *lo) {
    uint64_t h = 0, l = 0;
    for (int i = 0; i < 8; i++) {
        h = (h << 8) | raw->b[i];
        l = (l << 8) | raw->b[8 + i];
    }
    *hi = h;
    *lo = l;
}

cc_guid cc_guid_from_bytes(const cc_guid_raw *raw) {
    uint64_t hi, lo;
    raw_to_u128(raw, &hi, &lo);
    return cc_guid_from_u128(hi, lo);
}

void cc_guid_to_bytes(const cc_guid *g, cc_guid_raw *out) {
    /* n = (n << 6) | idx(c), as to_u128; the first character contributes its 2 low bits. */
    uint64_t hi = 0, lo = 0;
    for (int i = 0; i < 22; i++) {
        int v = alpha_index((unsigned char)g->c[i]);
        uint64_t d = v < 0 ? 0 : (uint64_t)v;
        hi = (hi << 6) | (lo >> 58);
        lo = (lo << 6) | d;
    }
    u128_to_raw(hi, lo, out);
}

cc_guid cc_guid_derived(const void *data, size_t n) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data, n);
    cc_guid_raw raw;
    blake3_hasher_finalize(&h, raw.b, 16);
    return cc_guid_from_bytes(&raw);
}

int cc_guid_cmp(const cc_guid *a, const cc_guid *b) { return memcmp(a->c, b->c, 22); }

/* ---- parse and Rust's {:?} of the input ---- */

/* Code points that Rust's char::escape_debug (escape_grapheme_extended: true) writes as
 * \u{..}. Exact for ASCII; outside ASCII an approximation of Rust's is_printable and
 * Grapheme_Extend tables: C1 controls, no-break space, soft hyphen, common combining marks, format characters,
 * variation selectors, BOM, tags and private use. Unassigned code points are not detected. */
static int needs_unicode_escape(uint32_t c) {
    static const uint32_t ranges[][2] = {
        {0x0000, 0x001f}, {0x007f, 0x00a0}, {0x00ad, 0x00ad}, {0x0300, 0x036f},
        {0x0483, 0x0489}, {0x0591, 0x05bd}, {0x0610, 0x061a}, {0x061c, 0x061c},
        {0x064b, 0x065f}, {0x180b, 0x180f}, {0x200b, 0x200f}, {0x2028, 0x202e},
        {0x2060, 0x206f}, {0x20d0, 0x20ff}, {0xd800, 0xdfff}, {0xe000, 0xf8ff},
        {0xfe00, 0xfe0f}, {0xfe20, 0xfe2f}, {0xfeff, 0xfeff}, {0xfff0, 0xfffb},
        {0xe0000, 0xe0fff}, {0xf0000, 0x10ffff},
    };
    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; i++)
        if (c >= ranges[i][0] && c <= ranges[i][1]) return 1;
    return 0;
}

/* Decodes one UTF-8 scalar from s[0..n) (n > 0). Invalid bytes give U+FFFD and length 1. */
static uint32_t utf8_next(const unsigned char *s, size_t n, size_t *len) {
    unsigned char c = s[0];
    *len = 1;
    if (c < 0x80) return c;
    int k;
    uint32_t v, min;
    if (c >= 0xc2 && c <= 0xdf) k = 1, v = c & 0x1f, min = 0x80;
    else if (c >= 0xe0 && c <= 0xef) k = 2, v = c & 0x0f, min = 0x800;
    else if (c >= 0xf0 && c <= 0xf4) k = 3, v = c & 0x07, min = 0x10000;
    else return 0xfffd;
    if ((size_t)k >= n) return 0xfffd;
    for (int i = 1; i <= k; i++) {
        if ((s[i] & 0xc0) != 0x80) return 0xfffd;
        v = (v << 6) | (s[i] & 0x3f);
    }
    if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return 0xfffd;
    *len = (size_t)k + 1;
    return v;
}

static size_t put_utf8(char *o, uint32_t v) {
    if (v < 0x80) {
        o[0] = (char)v;
        return 1;
    }
    if (v < 0x800) {
        o[0] = (char)(0xc0 | (v >> 6));
        o[1] = (char)(0x80 | (v & 0x3f));
        return 2;
    }
    if (v < 0x10000) {
        o[0] = (char)(0xe0 | (v >> 12));
        o[1] = (char)(0x80 | ((v >> 6) & 0x3f));
        o[2] = (char)(0x80 | (v & 0x3f));
        return 3;
    }
    o[0] = (char)(0xf0 | (v >> 18));
    o[1] = (char)(0x80 | ((v >> 12) & 0x3f));
    o[2] = (char)(0x80 | ((v >> 6) & 0x3f));
    o[3] = (char)(0x80 | (v & 0x3f));
    return 4;
}

/* Writes Rust's format!("{:?}", s) into o (room for 2 + 10 * n bytes); returns the length. */
static size_t rust_debug_str(char *o, const unsigned char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    size_t w = 0;
    o[w++] = '"';
    for (size_t i = 0; i < n;) {
        size_t len;
        uint32_t c = utf8_next(s + i, n - i, &len);
        i += len;
        const char *e = NULL;
        switch (c) {
        case 0: e = "\\0"; break;
        case '\t': e = "\\t"; break;
        case '\r': e = "\\r"; break;
        case '\n': e = "\\n"; break;
        case '\\': e = "\\\\"; break;
        case '"': e = "\\\""; break;
        default: break;
        }
        if (e) {
            o[w++] = e[0];
            o[w++] = e[1];
        } else if (needs_unicode_escape(c)) {
            int digits = 1;
            while (digits < 6 && (c >> (4 * digits))) digits++;
            o[w++] = '\\';
            o[w++] = 'u';
            o[w++] = '{';
            for (int d = digits - 1; d >= 0; d--) o[w++] = hex[(c >> (4 * d)) & 0xf];
            o[w++] = '}';
        } else {
            w += put_utf8(o + w, c);
        }
    }
    o[w++] = '"';
    return w;
}

static char *err_buf;
static size_t err_cap;

static const char *make_error(const char *s, size_t n) {
    static const char prefix[] = "invalid IFC GlobalId: ";
    static const char fallback[] = "invalid IFC GlobalId";
    size_t plen = sizeof prefix - 1;
    if (n > ((size_t)-1 - plen - 3) / 10) return fallback;
    size_t need = plen + 2 + 10 * n + 1;
    if (need > err_cap) {
        char *p = realloc(err_buf, need);
        if (!p) return fallback;
        err_buf = p;
        err_cap = need;
    }
    memcpy(err_buf, prefix, plen);
    size_t w = plen + rust_debug_str(err_buf + plen, (const unsigned char *)s, n);
    err_buf[w] = 0;
    return err_buf;
}

int cc_guid_parse(const char *s, size_t n, cc_guid *out, const char **err) {
    int ok = n == 22 && s[0] >= '0' && s[0] <= '3';
    for (size_t i = 0; ok && i < 22; i++) ok = alpha_index((unsigned char)s[i]) >= 0;
    if (!ok) {
        if (err) *err = make_error(s ? s : "", s ? n : 0);
        return -1;
    }
    memcpy(out->c, s, 22);
    return 0;
}

/* ---- new() ---- */

static uint64_t seed_next;   /* > 0: seeded mode, next id is from_u128(seed_next) */
#ifndef __wasm__
static int seed_env_checked; /* CCORE_SEED looked up */
#endif

void cc_guid_seed(uint64_t k) {
    seed_next = k;
#ifndef __wasm__
    seed_env_checked = 1;
#endif
}

#ifdef __wasm__
static uint64_t rng_state = 0x6a09e667f3bcc908ull;

void cc_guid_entropy(const void *data, size_t n) {
    cc_guid_raw h;
    blake3_hasher b;
    blake3_hasher_init(&b);
    blake3_hasher_update(&b, &rng_state, sizeof rng_state);
    blake3_hasher_update(&b, data, n);
    blake3_hasher_finalize(&b, h.b, 8);
    uint64_t s = 0;
    for (int i = 0; i < 8; i++) s = (s << 8) | h.b[i];
    rng_state = s;
}

static uint64_t splitmix64(void) {
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static void random_bytes(uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i += 8) {
        uint64_t r = splitmix64();
        for (size_t j = 0; j < 8 && i + j < n; j++) b[i + j] = (uint8_t)(r >> (8 * j));
    }
}
#else
void cc_guid_entropy(const void *data, size_t n) {
    (void)data;
    (void)n;
}

static void random_bytes(uint8_t *b, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(b + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            abort(); /* no entropy: refuse to make ids that could collide */
        }
        got += (size_t)r;
    }
}
#endif

cc_guid cc_guid_new(void) {
#ifndef __wasm__
    if (!seed_env_checked) {
        seed_env_checked = 1;
        if (getenv("CCORE_SEED")) seed_next = 1;
    }
#endif
    if (seed_next) return cc_guid_from_u128(0, seed_next++);
    /* uuid::Uuid::new_v4: 16 random bytes, version 4, RFC 4122 variant. */
    cc_guid_raw raw;
    random_bytes(raw.b, 16);
    raw.b[6] = (uint8_t)((raw.b[6] & 0x0f) | 0x40);
    raw.b[8] = (uint8_t)((raw.b[8] & 0x3f) | 0x80);
    return cc_guid_from_bytes(&raw);
}
