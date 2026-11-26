/* Suite file2: the binary .bimble (version 2: magic, gzip of CBOR) against Rust's bytes
 * (golden/bin/: Rust's to_bytes and its inflated CBOR), C save -> load, and robustness on
 * broken input: truncations, bit flips, a gzip bomb, deep nesting, huge lengths (run it under
 * ASan/UBSan too). Timing and sizes for frame, and for a large model when CCORE_LARGE names a
 * .bimble (or ccore/out/t16-large-20000.bimble exists). */
#define _POSIX_C_SOURCE 199309L /* clock_gettime under -std=c11 */
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "../src/cbor.h"
#include "../src/file.h"
#include "../src/inflate.h"
#include "../src/model.h"
#include "check.h"

static const char *golden;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static char *find(char *h, size_t n, const char *needle, size_t k) {
    for (size_t i = 0; i + k <= n; i++)
        if (memcmp(h + i, needle, k) == 0) return h + i;
    return NULL;
}

static int contains(const char *s, const char *sub) { return strstr(s, sub) != NULL; }

/* Load (any form), then save as v1 text into `out`. */
static int to_text(const char *p, size_t n, Buf *out, cc_error *err) {
    cc_model m;
    int rc = cc_model_load(&m, p, n, err);
    buf_clear(out);
    if (!rc) rc = cc_model_save(&m, out);
    cc_model_free(&m);
    return rc;
}

static void strip_nl(const char *p, size_t *n) {
    if (*n && p[*n - 1] == '\n') (*n)--;
}

/* A binary file of `cbor`: magic + C gzip. */
static void wrap(const char *cbor, size_t n, Buf *out) {
    buf_clear(out);
    buf_append(out, CC_MAGIC, CC_MAGIC_LEN);
    cc_gzip((const unsigned char *)cbor, n, out);
}

static void expect_load_error(const char *what, const char *p, size_t n, const char *want) {
    cc_model m;
    cc_error err;
    int rc = cc_model_load(&m, p, n, &err);
    cc_model_free(&m);
    if (rc == 0 || !contains(err.msg, want)) printf("  %s: want \"%s\", got rc %d \"%s\"\n", what, want, rc, rc ? err.msg : "");
    check_true(what, rc != 0 && contains(err.msg, want));
}

static void golden_case(const char *name) {
    char path[256];
    size_t jn, bn, cn;
    snprintf(path, sizeof path, "%s.json", name);
    char *json = slurp(golden, path, &jn);
    strip_nl(json, &jn);
    snprintf(path, sizeof path, "bin/%s.bimble", name);
    char *bin = slurp(golden, path, &bn);
    snprintf(path, sizeof path, "bin/%s.cbor", name);
    char *cbor = slurp(golden, path, &cn);
    Buf a, b;
    buf_init(&a);
    buf_init(&b);
    cc_error err;
    char what[128];

    /* Rust's v2 -> C -> v1 text == Rust's v1 text */
    int rc = to_text(bin, bn, &a, &err);
    if (rc) printf("  %s.bimble: %s\n", name, err.msg);
    snprintf(what, sizeof what, "%s: load Rust v2, save v1 == %s.json", name, name);
    check_eq(what, rc ? "" : a.p, rc ? 0 : a.len, json, jn);

    /* our inflate of Rust's gzip == Rust's CBOR */
    const char *why = NULL;
    buf_clear(&a);
    rc = cc_gunzip((const unsigned char *)bin + CC_MAGIC_LEN, bn - CC_MAGIC_LEN, (size_t)1 << 30, &a, &why);
    snprintf(what, sizeof what, "%s: C inflate of Rust's gzip == Rust's CBOR", name);
    check_true(what, rc == 0 && a.len == cn && memcmp(a.p, cbor, cn) == 0);

    /* C CBOR of the model == Rust's CBOR, byte for byte */
    cc_model m;
    rc = cc_model_load(&m, json, jn, &err);
    check_true("load json", rc == 0);
    buf_clear(&a);
    rc = cc_model_to_cbor(&m, &a);
    snprintf(what, sizeof what, "%s: C CBOR == Rust's CBOR (%zu bytes)", name, cn);
    check_true(what, rc == 0 && a.len == cn && memcmp(a.p, cbor, cn) == 0);
    if (!(a.len == cn && memcmp(a.p, cbor, cn) == 0)) {
        size_t i = 0;
        while (i < a.len && i < cn && a.p[i] == cbor[i]) i++;
        printf("  first difference at byte %zu (C %zu bytes, Rust %zu)\n", i, a.len, cn);
    }

    /* C save_v2 -> C load -> same text; inflate(C output) == Rust's CBOR */
    buf_clear(&b);
    rc = cc_model_save_v2(&m, &b);
    check_true("save_v2", rc == 0 && b.len > CC_MAGIC_LEN && memcmp(b.p, CC_MAGIC, CC_MAGIC_LEN) == 0);
    Buf t;
    buf_init(&t);
    rc = to_text(b.p, b.len, &t, &err);
    snprintf(what, sizeof what, "%s: C save_v2, C load, save v1 == %s.json", name, name);
    check_eq(what, rc ? "" : t.p, rc ? 0 : t.len, json, jn);
    buf_clear(&a);
    rc = cc_gunzip((const unsigned char *)b.p + CC_MAGIC_LEN, b.len - CC_MAGIC_LEN, (size_t)1 << 30, &a, &why);
    snprintf(what, sizeof what, "%s: inflate(C v2) == Rust's CBOR", name);
    check_true(what, rc == 0 && a.len == cn && memcmp(a.p, cbor, cn) == 0);

    /* timing */
    int reps = 200;
    double t0 = now_ms();
    for (int i = 0; i < reps; i++) {
        cc_model m2;
        cc_model_load(&m2, bin, bn, &err);
        cc_model_free(&m2);
    }
    double tl = (now_ms() - t0) / reps;
    t0 = now_ms();
    for (int i = 0; i < reps; i++) {
        buf_clear(&b);
        cc_model_save_v2(&m, &b);
    }
    double ts = (now_ms() - t0) / reps;
    printf("  %s: v1 %zu B, CBOR %zu B, Rust v2 %zu B, C v2 %zu B; load v2 %.3f ms, save v2 %.3f ms\n", name, jn, cn,
           bn, b.len, tl, ts);

    cc_model_free(&m);
    buf_free(&t);
    buf_free(&a);
    buf_free(&b);
    free(json);
    free(bin);
    free(cbor);
}

/* Every truncation and one bit flip per byte of a binary file: errors or a model, never a
 * crash (ASan/UBSan). Returns how many loaded. */
static int mangle(const char *p, size_t n, int truncate_only_errors) {
    char *c = malloc(n);
    int ok = 0, trunc_ok = 0;
    cc_error err;
    for (size_t k = 0; k < n; k++) {
        cc_model m;
        if (cc_model_load(&m, p, k, &err) == 0) trunc_ok++;
        cc_model_free(&m);
    }
    for (size_t k = 0; k < n; k++) {
        memcpy(c, p, n);
        c[k] ^= (char)(1 << (k % 8));
        cc_model m;
        if (cc_model_load(&m, c, n, &err) == 0) ok++;
        cc_model_free(&m);
    }
    free(c);
    if (truncate_only_errors) check_true("every truncation is an error", trunc_ok == 0);
    return ok;
}

/* Bit writer for hand-made deflate streams. */
typedef struct {
    Buf *b;
    uint64_t acc;
    int n;
} W;
static void wput(W *w, uint32_t v, int k) {
    w->acc |= (uint64_t)v << w->n;
    w->n += k;
    while (w->n >= 8) buf_putc(w->b, (char)(w->acc & 0xFF)), w->acc >>= 8, w->n -= 8;
}
static uint32_t rev(uint32_t c, int len) {
    uint32_t r = 0;
    for (int i = 0; i < len; i++, c >>= 1) r = r << 1 | (c & 1);
    return r;
}

/* magic + gzip header + one fixed block: a zero, then (258, distance 1) to `total` bytes. */
static void bomb(Buf *b, uint64_t total) {
    static const unsigned char head[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};
    buf_append(b, CC_MAGIC, CC_MAGIC_LEN);
    buf_append(b, head, 10);
    W w = {b, 0, 0};
    wput(&w, 1, 1);
    wput(&w, 1, 2);
    wput(&w, rev(0x30, 8), 8); /* literal 0 */
    for (uint64_t done = 1; done + 258 <= total; done += 258) {
        wput(&w, rev(0xC5, 8), 8); /* length 258 (symbol 285) */
        wput(&w, 0, 5);            /* distance 1 */
    }
    wput(&w, 0, 7); /* end of block */
    wput(&w, 0, 8);
    buf_fill(b, 0, 8); /* trailer (wrong, never reached) */
}

static void robustness(void) {
    size_t bn, cn;
    char *bin = slurp(golden, "bin/frame2.bimble", &bn);
    char *cbor = slurp(golden, "bin/frame2.cbor", &cn);
    Buf b, c;
    buf_init(&b);
    buf_init(&c);

    /* gzip level: truncations always fail (the trailer is checked); flips mostly fail (CRC) */
    int ok = mangle(bin, bn, 1);
    printf("  frame2.bimble: %zu truncations, %zu bit flips, %d flips still load\n", bn, bn, ok);
    /* CBOR level, re-compressed so the CRC passes: the CBOR decoder meets every damage */
    int tr = 0;
    for (size_t k = 0; k < cn; k++) {
        wrap(cbor, k, &b);
        cc_model m;
        cc_error err;
        if (cc_model_load(&m, b.p, b.len, &err) == 0) tr++;
        cc_model_free(&m);
    }
    check_true("every CBOR truncation is an error", tr == 0);
    int fl = 0;
    char *x = malloc(cn);
    for (size_t k = 0; k < cn; k++) {
        memcpy(x, cbor, cn);
        x[k] ^= (char)(1 << (k % 8));
        wrap(x, cn, &b);
        cc_model m;
        cc_error err;
        if (cc_model_load(&m, b.p, b.len, &err) == 0) fl++;
        cc_model_free(&m);
    }
    printf("  frame2.cbor: %zu truncations, %zu bit flips (re-gzipped), %d flips still load\n", cn, cn, fl);
    check_true("CBOR flips mostly refused", fl < (int)cn / 2);

    /* gzip bomb: 2 GiB of zeros in ~13 MB, stopped at the cap */
    buf_clear(&b);
    bomb(&b, (uint64_t)2 << 30);
    double t0 = now_ms();
    expect_load_error("gzip bomb (2 GiB)", b.p, b.len, "decompressed data too large");
    printf("  gzip bomb: %zu B file for 2 GiB, refused in %.0f ms (cap %zu MiB)\n", b.len, now_ms() - t0,
           CC_MAX_INFLATED_BYTES >> 20);

    /* deep nesting */
    buf_clear(&c);
    buf_fill(&c, (char)0x81, 100000);
    buf_putc(&c, 0);
    wrap(c.p, c.len, &b);
    expect_load_error("CBOR 100000 nested arrays", b.p, b.len, "recursion limit exceeded");
    buf_clear(&c);
    for (int i = 0; i < 100; i++) buf_putc(&c, (char)0xA1), buf_putc(&c, 0x61), buf_putc(&c, 'a');
    buf_putc(&c, 0);
    wrap(c.p, c.len, &b);
    expect_load_error("CBOR 100 nested maps", b.p, b.len, "recursion limit exceeded");

    /* huge lengths */
    static const unsigned char huge[][10] = {
        {0x9B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0},
        {0xBB, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0},
        {0x7B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0},
        {0x9A, 0x10, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0},
    };
    for (int i = 0; i < 4; i++) {
        wrap((const char *)huge[i], 10, &b);
        expect_load_error("CBOR huge length", b.p, b.len, "length runs past the data");
    }
    /* FIXLIB: the DOM budget (json_r.h), charged at the container's head, before the memory is
     * taken. An array of 16 M + 1 zeros (one byte each, 16 MB inflated, 16 KB of gzip) asked for
     * 1 GB at once and decoded it; a map of 14 M entries passes the byte budget first (80 bytes a
     * member). Both refused now before any allocation (old code: "invalid type: sequence…"
     * after a 1 GB DOM, or "out of memory" under an address-space cap). */
    {
        size_t cnt = JR_MAX_NODES + 1;
        buf_clear(&c);
        buf_fill(&c, 0, 5 + cnt);
        c.p[0] = (char)0x9A;
        for (int k = 0; k < 4; k++) c.p[1 + k] = (char)(cnt >> (24 - 8 * k));
        wrap(c.p, c.len, &b);
        double t1 = now_ms();
        expect_load_error("CBOR array of 16 M + 1 one-byte items: the value budget", b.p, b.len, JR_ERR_NODES);
        printf("  array bomb: %zu B file, %zu items, refused in %.0f ms\n", b.len, cnt, now_ms() - t1);
        size_t mc = 14000000;
        buf_clear(&c);
        buf_fill(&c, 0, 5 + 2 * mc);
        c.p[0] = (char)0xBA;
        for (int k = 0; k < 4; k++) c.p[1 + k] = (char)(mc >> (24 - 8 * k));
        wrap(c.p, c.len, &b);
        expect_load_error("CBOR map of 14 M entries: the byte budget", b.p, b.len, JR_ERR_BYTES);
    }
    /* other refused CBOR */
    static const struct {
        const char *what, *bytes;
        size_t n;
        const char *want;
    } bad[] = {
        {"indefinite array", "\x9F\xFF", 2, "indefinite-length"},
        {"byte string", "\x41\x00", 2, "byte strings"},
        {"tag", "\xC1\x00", 2, "tags"},
        {"non-text key", "\xA1\x00\x00", 3, "map key is not a text"},
        {"bad UTF-8", "\x62\xC3\x28", 3, "invalid UTF-8"},
        {"trailing bytes", "\xA0\x00", 2, "trailing bytes"},
        {"empty", "", 0, "unexpected end of CBOR data"},
        {"empty map", "\xA0", 1, "missing field `format`"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        wrap(bad[i].bytes, bad[i].n, &b);
        expect_load_error(bad[i].what, b.p, b.len, bad[i].want);
    }
    /* FIXLIB: a back-reference before the start of the output ("distance too far back"; without
     * the check a copy from before the buffer, seen only by ASan): fixed block, literal 'a', then
     * length 3 at distance 2 (one byte written); distance 1 is fine. */
    for (int dist = 1; dist <= 2; dist++) {
        static const unsigned char head[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};
        Buf g, o;
        buf_init(&g), buf_init(&o);
        buf_append(&g, head, 10);
        W w = {&g, 0, 0};
        wput(&w, 1, 1), wput(&w, 1, 2);    /* last block, fixed codes */
        wput(&w, rev(0x30 + 'a', 8), 8);   /* literal 'a' */
        wput(&w, rev(1, 7), 7);            /* length 3 (symbol 257) */
        wput(&w, rev((uint32_t)dist - 1, 5), 5); /* distance code 0 (1) or 1 (2) */
        wput(&w, 0, 7), wput(&w, 0, 8);    /* end of block, flush */
        unsigned char tr[8] = {0}; /* CRC32 of "aaaa" and ISIZE 4 */
        uint32_t crc = 0xFFFFFFFFu;
        for (int i = 0; i < 4; i++) {
            crc ^= 'a';
            for (int k = 0; k < 8; k++) crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1)));
        }
        crc ^= 0xFFFFFFFFu;
        for (int i = 0; i < 4; i++) tr[i] = (unsigned char)(crc >> 8 * i);
        tr[4] = 4;
        buf_append(&g, tr, 8);
        const char *why = "";
        int rc = cc_gunzip((const unsigned char *)g.p, g.len, 1 << 20, &o, &why);
        if (dist == 1) check_true("gzip: distance 1 after one byte: \"aaaa\"", rc == 0 && o.len == 4 && !memcmp(o.p, "aaaa", 4));
        else check_true("gzip: distance 2 after one byte: \"distance too far back\"", rc != 0 && !strcmp(why, "gzip: distance too far back"));
        buf_free(&g), buf_free(&o);
    }
    /* gzip-level damage */
    expect_load_error("magic only", CC_MAGIC, CC_MAGIC_LEN, "gzip: not gzip data");
    buf_clear(&b);
    buf_append(&b, bin, bn);
    buf_putc(&b, 0);
    expect_load_error("bytes after the gzip member", b.p, b.len, "gzip: trailing bytes");
    buf_clear(&b);
    buf_append(&b, bin, bn);
    b.p[bn - 1] ^= 1; /* ISIZE */
    expect_load_error("gzip length mismatch", b.p, b.len, "gzip: length mismatch");
    buf_clear(&b);
    buf_append(&b, bin, bn);
    b.p[bn - 8] ^= 1; /* CRC */
    expect_load_error("gzip CRC mismatch", b.p, b.len, "gzip: data checksum mismatch");

    /* semantic: version 1 inside a binary file; a mesh index outside the table */
    {
        buf_clear(&c);
        buf_append(&c, cbor, cn);
        char *v = find(c.p, c.len, "gversion\x02", 9);
        check_true("version in CBOR", v != NULL);
        if (v) v[8] = 1;
        wrap(c.p, c.len, &b);
        expect_load_error("binary with version 1", b.p, b.len, "unsupported bimble format version 1");
        buf_clear(&c);
        buf_append(&c, cbor, cn);
        char *pi = find(c.p, c.len, "ipositions\x00", 11);
        check_true("mesh index in CBOR", pi != NULL);
        if (pi) pi[10] = 5;
        wrap(c.p, c.len, &b);
        expect_load_error("mesh index outside the table", b.p, b.len, "mesh 5 not in the file's table");
    }
    free(x);
    free(bin);
    free(cbor);
    buf_free(&b);
    buf_free(&c);
}

/* f64 -> CBOR float width, as ciborium picks it. */
static void floats(void) {
    static const struct {
        double x;
        int len;
    } cases[] = {{0.0, 3},      {-0.0, 3},     {1.0, 3},       {65504.0, 3},     {65505.0, 5},  {0.5, 3},
                 {5.960464477539063e-8, 3},   {2.98023223876953125e-8, 5},     {1.0 / 3.0, 9}, {300.0, 3},
                 {3.4028234663852886e38, 5},  {1e300, 9},    {1.401298464324817e-45, 5},      {0.1, 9},
                 {16777216.0, 5}, {16777217.0, 9}, {-2.5, 3}};
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        buf_clear(&b);
        cb_f64(&b, cases[i].x);
        Arena a;
        arena_init(&a, 0);
        JrError e;
        JrVal *v = cbor_parse(&a, (const unsigned char *)b.p, b.len, &e);
        char what[96];
        snprintf(what, sizeof what, "cb_f64(%.17g): %d bytes, reads back exactly", cases[i].x, cases[i].len);
        check_true(what, (int)b.len == cases[i].len && v && v->type == JR_NUM &&
                             memcmp(&v->num, &cases[i].x, 8) == 0);
        arena_free(&a);
    }
    buf_free(&b);
}

static void large(void) {
    const char *path = getenv("CCORE_LARGE");
    char def[] = "out/t16-large-20000.bimble";
    FILE *f = fopen(path ? path : def, "rb");
    if (!f) {
        printf("  large: no file (set CCORE_LARGE or make %s)\n", def);
        return;
    }
    fseek(f, 0, SEEK_END);
    size_t n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc(n);
    if (!p || fread(p, 1, n, f) != n) exit(2);
    fclose(f);
    cc_model m;
    cc_error err;
    double t0 = now_ms();
    int rc = cc_model_load(&m, p, n, &err);
    double tl = now_ms() - t0;
    check_true("large: loads", rc == 0);
    if (rc) {
        printf("  large: %s\n", err.msg);
        free(p);
        return;
    }
    Buf v1, v2, cb, t;
    buf_init(&v1), buf_init(&v2), buf_init(&cb), buf_init(&t);
    t0 = now_ms();
    cc_model_save(&m, &v1);
    double ts1 = now_ms() - t0;
    t0 = now_ms();
    cc_model_to_cbor(&m, &cb);
    double tc = now_ms() - t0;
    t0 = now_ms();
    cc_model_save_v2(&m, &v2);
    double ts2 = now_ms() - t0;
    t0 = now_ms();
    rc = to_text(v2.p, v2.len, &t, &err);
    double tl2 = now_ms() - t0;
    check_true("large: v2 round trip gives the same v1 text", rc == 0 && t.len == v1.len && memcmp(t.p, v1.p, t.len) == 0);
    /* to_text = load v2 + save v1; take save v1 off to get the load */
    printf("  large (%zu elements): input %s %zu B loaded in %.0f ms\n", m.nelements,
           n >= CC_MAGIC_LEN && memcmp(p, CC_MAGIC, CC_MAGIC_LEN) == 0 ? "v2" : "v1", n, tl);
    printf("  large: v1 %zu B (save %.0f ms); CBOR %zu B (%.0f ms); C v2 %zu B (save %.0f ms); load v2 %.0f ms\n",
           v1.len, ts1, cb.len, tc, v2.len, ts2, tl2 - ts1);
    cc_model_free(&m);
    buf_free(&v1), buf_free(&v2), buf_free(&cb), buf_free(&t);
    free(p);
}

/* CRC-32 (ticket J1: slicing-by-8): the check value of RFC 1952's CRC, and every length and
 * offset up to 64 against the byte-at-a-time definition, also chained in two calls. */
static uint32_t crc_ref(uint32_t crc, const unsigned char *s, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= s[i];
        for (int k = 0; k < 8; k++) crc = crc & 1 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}
static void crc32_cases(void) {
    check_true("crc32 check value", cc_crc32(0, "123456789", 9) == 0xCBF43926u);
    check_true("crc32 empty", cc_crc32(0, "", 0) == 0);
    unsigned char buf[80];
    for (int i = 0; i < 80; i++) buf[i] = (unsigned char)(i * 37 + 11);
    int bad = 0;
    for (size_t off = 0; off < 8; off++)
        for (size_t n = 0; n <= 64; n++) {
            uint32_t want = crc_ref(0, buf + off, n);
            if (cc_crc32(0, buf + off, n) != want) bad++;
            if (cc_crc32(cc_crc32(0, buf + off, n / 3), buf + off + n / 3, n - n / 3) != want) bad++;
        }
    check_true("crc32 = byte-at-a-time, every length and offset", !bad);
}

/* Ticket J1: the session's side file form (cc_model_save_v2_stored): the magic, a gzip of stored
 * blocks whose content is Rust's CBOR byte for byte; it loads back to the same v1 text. Also a
 * model bigger than one stored block (65,535 bytes) and the empty input. */
static void stored_case(const char *name) {
    char path[256];
    size_t jn, cn;
    snprintf(path, sizeof path, "%s.json", name);
    char *json = slurp(golden, path, &jn);
    strip_nl(json, &jn);
    snprintf(path, sizeof path, "bin/%s.cbor", name);
    char *cbor = slurp(golden, path, &cn);
    cc_model m;
    cc_error err;
    Buf st, in, txt;
    buf_init(&st), buf_init(&in), buf_init(&txt);
    char what[128];
    int rc = cc_model_load(&m, json, jn, &err) || cc_model_save_v2_stored(&m, &st);
    const char *why = NULL;
    rc = rc || st.len < CC_MAGIC_LEN || memcmp(st.p, CC_MAGIC, CC_MAGIC_LEN) ||
         cc_gunzip((const unsigned char *)st.p + CC_MAGIC_LEN, st.len - CC_MAGIC_LEN, (size_t)1 << 30, &in, &why);
    snprintf(what, sizeof what, "%s: stored v2 inflates to Rust's CBOR", name);
    check_eq(what, rc ? "" : in.p, rc ? 0 : in.len, cbor, cn);
    cc_model back;
    rc = rc || cc_model_load(&back, st.p, st.len, &err) || cc_model_save(&back, &txt);
    snprintf(what, sizeof what, "%s: stored v2 loads back to the same v1 text", name);
    check_eq(what, rc ? "" : txt.p, rc ? 0 : txt.len, json, jn);
    cc_model_free(&m), cc_model_free(&back);
    buf_free(&st), buf_free(&in), buf_free(&txt);
    free(json), free(cbor);
}
static void stored_blocks(void) {
    static unsigned char big[200000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (unsigned char)(i * 131 + (i >> 9));
    size_t sizes[] = {0, 1, 65535, 65536, sizeof big};
    int bad = 0;
    for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        Buf z, o;
        buf_init(&z), buf_init(&o);
        const char *why = NULL;
        if (cc_gzip_stored(big, sizes[k], &z) || cc_gunzip((const unsigned char *)z.p, z.len, sizes[k] + 1, &o, &why) ||
            o.len != sizes[k] || (sizes[k] && memcmp(o.p, big, sizes[k])))
            bad++;
        buf_free(&z), buf_free(&o);
    }
    check_true("gzip stored blocks: 0, 1, 65535, 65536, 200000 bytes round trip", !bad);
}

/* ---- LNK1: the "links" section (model.h cc_model_link, DEVIATIONS 29): left out when empty (frame
 * saves without it, its bytes as before: golden_case), kept through text -> binary -> text, and
 * refused when broken (not an array, a missing path, too many entries). Fails on the code before
 * LNK1: the key was ignored and lost on save. ---- */
static void links_section(void) {
    size_t n = 0;
    char *frame = slurp(golden, "frame.json", &n);
    check_true("links: frame.json read", n > 0);
    if (!n) return free(frame);
    cc_model m;
    cc_error err;
    Buf t, b, t2;
    buf_init(&t), buf_init(&b), buf_init(&t2);
    check_true("links: frame loads", cc_model_load(&m, frame, n, &err) == 0);
    cc_model_save(&m, &t);
    check_true("links: no \"links\" key without links", !contains(buf_cstr(&t), "\"links\""));
    static cc_model_link L[2];
    L[0].name = (cc_str){"a.ifc", 5}, L[0].path = (cc_str){"/x/models/a.ifc", 15}, L[0].relative = (cc_str){"../models/a.ifc", 15};
    L[1].name = (cc_str){"b.bimble", 8}, L[1].path = (cc_str){"/y/b.bimble", 11}, L[1].hidden = 1;
    m.links = L, m.nlinks = 2;
    buf_clear(&t);
    cc_model_save(&m, &t);
    check_true("links: the text names both, hidden only when hidden",
               contains(buf_cstr(&t), "\"links\": [") && contains(buf_cstr(&t), "\"relative\": \"../models/a.ifc\"") &&
                   contains(buf_cstr(&t), "\"hidden\": true"));
    check_true("links: binary save", cc_model_save_v2(&m, &b) == 0);
    cc_model_free(&m);
    check_true("links: text -> binary -> text, the same text", to_text(b.p, b.len, &t2, &err) == 0 && t2.len == t.len &&
                                                                    !memcmp(t2.p, t.p, t.len));
    char *last = strrchr(frame, '}');
    static const char *const bad[] = {"\"links\": 1", "\"links\": [{\"name\": \"a.ifc\"}]",
                                      "\"links\": [{\"name\": \"a.ifc\", \"path\": \"p\", \"x\": 1}]"};
    for (int i = 0; i < 3 && last; i++) { /* into the model object: before the model's closing brace */
        Buf e;
        buf_init(&e);
        char *mo = last - 1;
        while (mo > frame && *mo != '}') mo--;
        buf_append(&e, frame, (size_t)(mo - frame));
        buf_puts(&e, ", "), buf_puts(&e, bad[i]);
        buf_append(&e, mo, n - (size_t)(mo - frame));
        cc_model x;
        int bad_rc = cc_model_load(&x, e.p, e.len, &err);
        check_true("links: a broken section is refused (its own error)", bad_rc != 0 &&
                   (contains(err.msg, "sequence") || contains(err.msg, "path") || contains(err.msg, "unknown field")));
        if (!bad_rc) cc_model_free(&x);
        buf_free(&e);
    }
    buf_free(&t), buf_free(&b), buf_free(&t2);
    free(frame);
}
/* ---- end LNK1 ---- */

int main(int argc, char **argv) {
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    golden = dir;
    floats();
    golden_case("frame");
    golden_case("frame2");
    robustness();
    large();
    crc32_cases();
    stored_case("frame");
    stored_case("frame2");
    stored_blocks();
    links_section(); /* LNK1 */
    return report("file2");
}
