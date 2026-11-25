/* Suite guid: every line of golden/guid.txt, new() (seeded and random), and cmp against the
 * order of the element ids in golden/frame.json (Rust's BTreeMap order).
 * test/run.sh compiles a fixed list of src files, so this suite includes guid.c itself. */
#include <stdint.h>
#include "../src/guid.c"
#include "check.h"

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Hex text [p, p + n) into out; "-" is empty. Returns the byte count, or -1. */
static long unhex(const char *p, size_t n, unsigned char *out) {
    if (n == 1 && p[0] == '-') return 0;
    if (n % 2) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int a = hexval(p[2 * i]), b = hexval(p[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (unsigned char)(a * 16 + b);
    }
    return (long)(n / 2);
}

static void raw_hex(const cc_guid_raw *r, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[2 * i] = hex[r->b[i] >> 4];
        out[2 * i + 1] = hex[r->b[i] & 15];
    }
}

/* Splits a line into up to 4 space-separated fields; the last one takes the rest of the line. */
static int fields(char *line, char *eol, char **f, size_t *fn, int max) {
    int k = 0;
    char *p = line;
    while (k < max && p <= eol) {
        char *sp = k == max - 1 ? NULL : memchr(p, ' ', (size_t)(eol - p));
        char *end = sp ? sp : eol;
        f[k] = p;
        fn[k] = (size_t)(end - p);
        k++;
        if (!sp) break;
        p = sp + 1;
    }
    return k;
}

int main(int argc, char **argv) {
    /* run.sh passes golden/floats; the goldens used here sit one level up. */
    char golden[4096];
    snprintf(golden, sizeof golden, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    size_t n;
    char *text = slurp(golden, "guid.txt", &n);
    unsigned char *seed = malloc(n + 1);
    if (!seed) return 2;
    int counts[4] = {0, 0, 0, 0};
    char what[128];
    for (char *line = text; *line;) {
        char *eol = strchr(line, '\n');
        if (!eol) eol = line + strlen(line);
        char *f[4];
        size_t fn[4];
        if ((size_t)(eol - line) >= 8 && memcmp(line, "invalid ", 8) == 0) {
            /* invalid <input-hex> <error text with spaces> */
            int k = fields(line, eol, f, fn, 3);
            long m = k == 3 ? unhex(f[1], fn[1], seed) : -1;
            if (m < 0) {
                printf("bad line: %.*s\n", (int)(eol - line), line);
                return 2;
            }
            cc_guid g;
            const char *err = NULL;
            snprintf(what, sizeof what, "invalid %.*s", (int)(fn[1] > 60 ? 60 : fn[1]), f[1]);
            check_true(what, cc_guid_parse((const char *)seed, (size_t)m, &g, &err) == -1);
            check_eq(what, err ? err : "", err ? strlen(err) : 0, f[2], fn[2]);
            counts[3]++;
        } else {
            int k = fields(line, eol, f, fn, 4);
            if (k >= 3 && fn[0] == 7 && memcmp(f[0], "derived", 7) == 0 && k == 4) {
                long m = unhex(f[1], fn[1], seed);
                if (m < 0) return 2;
                cc_guid g = cc_guid_derived(seed, (size_t)m);
                cc_guid_raw r;
                char hx[32];
                snprintf(what, sizeof what, "derived %ld bytes %.*s", m, (int)(fn[1] > 40 ? 40 : fn[1]), f[1]);
                check_eq(what, g.c, 22, f[2], fn[2]);
                cc_guid_to_bytes(&g, &r);
                raw_hex(&r, hx);
                check_eq(what, hx, 32, f[3], fn[3]);
                /* derived is the raw blake3 prefix: from_bytes(hash[..16]) gives the same id. */
                unsigned char h[16];
                if (unhex(f[3], fn[3], h) == 16) {
                    memcpy(r.b, h, 16);
                    cc_guid g2 = cc_guid_from_bytes(&r);
                    check_eq(what, g2.c, 22, f[2], fn[2]);
                }
                counts[0]++;
            } else if (k == 3 && fn[0] == 9 && memcmp(f[0], "from_u128", 9) == 0) {
                unsigned char h[16];
                if (unhex(f[1], fn[1], h) != 16) return 2;
                uint64_t hi = 0, lo = 0;
                for (int i = 0; i < 8; i++) {
                    hi = (hi << 8) | h[i];
                    lo = (lo << 8) | h[8 + i];
                }
                cc_guid g = cc_guid_from_u128(hi, lo);
                snprintf(what, sizeof what, "from_u128 %.*s", (int)fn[1], f[1]);
                check_eq(what, g.c, 22, f[2], fn[2]);
                cc_guid_raw r;
                char hx[32];
                cc_guid_to_bytes(&g, &r);
                raw_hex(&r, hx);
                check_eq(what, hx, 32, f[1], fn[1]);
                counts[1]++;
            } else if (k == 3 && fn[0] == 5 && memcmp(f[0], "parse", 5) == 0) {
                cc_guid g;
                const char *err = NULL;
                snprintf(what, sizeof what, "parse %.*s", (int)fn[1], f[1]);
                int ok = cc_guid_parse(f[1], fn[1], &g, &err) == 0;
                check_true(what, ok);
                if (ok) {
                    check_eq(what, g.c, 22, f[1], fn[1]);
                    cc_guid_raw r;
                    char hx[32];
                    cc_guid_to_bytes(&g, &r);
                    raw_hex(&r, hx);
                    check_eq(what, hx, 32, f[2], fn[2]);
                    cc_guid g2 = cc_guid_from_bytes(&r);
                    check_eq(what, g2.c, 22, f[1], fn[1]);
                }
                counts[2]++;
            } else {
                printf("bad line: %.*s\n", (int)(eol - line > 80 ? 80 : eol - line), line);
                return 2;
            }
        }
        line = *eol ? eol + 1 : eol;
    }
    check_true("24 derived lines", counts[0] == 24);
    check_true("9 from_u128 lines", counts[1] == 9);
    check_true("at least 70 parse lines", counts[2] >= 70);
    check_true("at least 9 invalid lines", counts[3] >= 9);

    /* Error text escapes as Rust's {:?} (expected output printed by stable rustc for the same input). */
    {
        static const char in1[] = "a\"b\\c\td\n\r\0\x01\x7f'";
        static const char in2[] = "\xc2\x80\xc2\x9f\xc2\xad\xc3\xa9\xcc\x81\xe2\x80\x8b\xe2\x80\xa8\xef\xb8\x8f"
                                  "\xef\xbb\xbf\xee\x80\x80\xf3\xa0\x80\x81\xf0\x9f\x8f\xbb\xe6\x97\xa5\xf0\x9f\x8f\x97"
                                  "\xef\xbf\xb0\xc2\xa0";
        static const char want1[] = "invalid IFC GlobalId: \"a\\\"b\\\\c\\td\\n\\r\\0\\u{1}\\u{7f}'\"";
        static const char want2[] = "invalid IFC GlobalId: \"\\u{80}\\u{9f}\\u{ad}\xc3\xa9\\u{301}\\u{200b}\\u{2028}"
                                    "\\u{fe0f}\\u{feff}\\u{e000}\\u{e0001}\xf0\x9f\x8f\xbb\xe6\x97\xa5\xf0\x9f\x8f\x97"
                                    "\\u{fff0}\\u{a0}\"";
        cc_guid g;
        const char *err = NULL;
        cc_guid_parse(in1, sizeof in1 - 1, &g, &err);
        check_eq("debug escape ascii", err, strlen(err), want1, sizeof want1 - 1);
        cc_guid_parse(in2, sizeof in2 - 1, &g, &err);
        check_eq("debug escape unicode", err, strlen(err), want2, sizeof want2 - 1);
    }

    /* Seeded new(): from_u128(1), (2), (3) in creation order. */
    cc_guid_seed(1);
    for (uint64_t i = 1; i <= 3; i++) {
        cc_guid g = cc_guid_new(), want = cc_guid_from_u128(0, i);
        check_eq("seeded new", g.c, 22, want.c, 22);
    }
    /* Random new(): a valid id, UUID v4 version and variant bits, two calls differ. */
    cc_guid_seed(0);
    cc_guid a = cc_guid_new(), b = cc_guid_new();
    cc_guid tmp;
    check_true("random new parses", cc_guid_parse(a.c, 22, &tmp, NULL) == 0);
    check_true("two random ids differ", memcmp(a.c, b.c, 22) != 0);
    for (int i = 0; i < 1000; i++) {
        cc_guid g = cc_guid_new();
        cc_guid_raw r;
        cc_guid_to_bytes(&g, &r);
        check_true("v4 version nibble", (r.b[6] >> 4) == 4);
        check_true("v4 variant bits", (r.b[8] >> 6) == 2);
        cc_guid back = cc_guid_from_bytes(&r);
        check_true("random roundtrip", memcmp(back.c, g.c, 22) == 0);
    }

    /* cmp: Rust's Ord is on the 22 characters, not on the u128. */
    cc_guid lo_text, hi_text;
    lo_text = cc_guid_from_u128(0, 63); /* "000...0$", numerically 63 */
    hi_text = cc_guid_from_u128(0, 10); /* "000...0A", numerically 10 */
    check_true("cmp is text order ($ < A)", cc_guid_cmp(&lo_text, &hi_text) < 0);
    check_true("cmp equal", cc_guid_cmp(&lo_text, &lo_text) == 0);
    check_true("cmp antisymmetric", cc_guid_cmp(&hi_text, &lo_text) > 0);

    /* cmp agrees with the element order of frame.json (sorted by Rust). */
    size_t fn_len;
    char *frame = slurp(golden, "frame.json", &fn_len);
    char *p = strstr(frame, "\"elements\": {");
    check_true("frame.json has elements", p != NULL);
    int elems = 0;
    cc_guid prev;
    while (p && (p = strstr(p, "\"id\": \"")) != NULL) {
        p += 7;
        cc_guid g;
        const char *err = NULL;
        check_true("frame id parses", cc_guid_parse(p, 22, &g, &err) == 0 && p[22] == '"');
        if (elems > 0) {
            snprintf(what, sizeof what, "frame order %.22s < %.22s", prev.c, g.c);
            check_true(what, cc_guid_cmp(&prev, &g) < 0);
        }
        prev = g;
        elems++;
    }
    check_true("72 frame elements", elems == 72);

    printf("guid: %d derived, %d from_u128, %d parse, %d invalid, %d frame ids\n", counts[0], counts[1],
           counts[2], counts[3], elems);
    free(frame);
    free(seed);
    free(text);
    return report("guid");
}
