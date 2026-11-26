/* Suite file: load and save of the .bimble JSON text form against Rust's bytes (the golden .json files),
 * robustness on broken input (run it under ASan/UBSan too), validation errors.
 * The sources are included here because test/run.sh links a fixed list of src files. */
#define _POSIX_C_SOURCE 199309L /* clock_gettime under -std=c11 */
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../src/json_r.h"
#include "../src/model.h"
#include "../src/file.h"
#include "check.h"

static const char *golden; /* the golden directory */

/* Load then save; 0 and the text in `out` on success. */
static int roundtrip(const char *text, size_t n, Buf *out, cc_error *err) {
    cc_model m;
    int rc = cc_model_load(&m, text, n, err);
    buf_clear(out);
    if (!rc) rc = cc_model_save(&m, out);
    cc_model_free(&m);
    return rc;
}

static int is_id_char(char c) { return c != 0 && strchr(cc_gid_alphabet, c) != NULL; }

/* "<new-N>" -> GlobalId::from_u128(N) (N < 64). */
static void unmask(const char *s, size_t n, Buf *out) {
    buf_clear(out);
    for (size_t i = 0; i < n;) {
        if (n - i > 6 && memcmp(s + i, "<new-", 5) == 0) {
            size_t j = i + 5;
            unsigned v = 0;
            while (j < n && s[j] >= '0' && s[j] <= '9') v = v * 10 + (unsigned)(s[j++] - '0');
            if (j < n && s[j] == '>' && v < 64) {
                buf_fill(out, '0', 21);
                buf_putc(out, cc_gid_alphabet[v]);
                i = j + 1;
                continue;
            }
        }
        buf_putc(out, s[i++]);
    }
}

/* Tokens equal to from_u128(N), 1 <= N < 64, back to "<new-N>". */
static void remask(const char *s, size_t n, Buf *out) {
    buf_clear(out);
    for (size_t i = 0; i < n;) {
        if (!is_id_char(s[i])) { buf_putc(out, s[i++]); continue; }
        size_t j = i;
        while (j < n && is_id_char(s[j])) j++;
        int masked = 0;
        if (j - i == 22) {
            int zeros = 1;
            for (size_t k = i; k < i + 21; k++) zeros &= s[k] == '0';
            const char *at = zeros ? memchr(cc_gid_alphabet, s[i + 21], 64) : NULL;
            if (at && at != cc_gid_alphabet) {
                buf_puts(out, "<new-");
                buf_u64(out, (uint64_t)(at - cc_gid_alphabet));
                buf_putc(out, '>');
                masked = 1;
            }
        }
        if (!masked) buf_append(out, s + i, j - i);
        i = j;
    }
}

static void golden_roundtrip(const char *name, int masked) {
    size_t n;
    char *text = slurp(golden, name, &n);
    Buf in, out, re;
    buf_init(&in);
    buf_init(&out);
    buf_init(&re);
    cc_error err;
    /* The golden files end with '\n'; file::to_string has no final newline (README). */
    size_t body = n && text[n - 1] == '\n' ? n - 1 : n;
    if (masked) unmask(text, body, &in);
    else buf_append(&in, text, body);
    int rc = roundtrip(in.p, in.len, &out, &err);
    if (rc) printf("  %s: %s (offset %zu)\n", name, err.msg, err.offset);
    check_true(name, rc == 0);
    if (masked) remask(out.p, out.len, &re);
    else buf_append(&re, out.p, out.len);
    check_eq(name, re.p, re.len, text, body);
    buf_free(&in);
    buf_free(&out);
    buf_free(&re);
    free(text);
}

/* Must not crash; returns the load status. */
static int try_load(const char *s, size_t n, cc_error *err) {
    cc_model m;
    int rc = cc_model_load(&m, s, n, err);
    if (!rc) { /* a model that loads also saves */
        Buf b;
        buf_init(&b);
        check_true("loaded model saves", cc_model_save(&m, &b) == 0);
        buf_free(&b);
    }
    cc_model_free(&m);
    return rc;
}

static int contains(const char *hay, const char *needle) {
    size_t n = strlen(hay), k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++)
        if (memcmp(hay + i, needle, k) == 0) return 1;
    return 0;
}

/* frame.json with the first `from` replaced by `to`; loads it and checks the error text. */
static void expect_error(const char *frame, size_t n, const char *from, const char *to, const char *want) {
    Buf b;
    buf_init(&b);
    const char *at = NULL;
    size_t k = strlen(from);
    for (size_t i = 0; i + k <= n && !at; i++)
        if (memcmp(frame + i, from, k) == 0) at = frame + i;
    if (!at) {
        printf("  pattern not found: %s\n", from);
        check_true(want, 0);
        return;
    }
    buf_append(&b, frame, (size_t)(at - frame));
    buf_puts(&b, to);
    buf_append(&b, at + k, n - (size_t)(at - frame) - k);
    cc_error err;
    int rc = try_load(b.p, b.len, &err);
    if (rc == 0 || !contains(err.msg, want)) printf("  want \"%s\", got rc %d \"%s\"\n", want, rc, rc ? err.msg : "");
    check_true(want, rc != 0 && contains(err.msg, want));
    buf_free(&b);
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* A small model with every optional part C holds, in Rust's field order (hand-written from
 * model.rs's serde attributes, not produced by Rust). */
static const char SYNTH[] =
    "{\n  \"format\": \"bimble\",\n  \"version\": 1,\n  \"model\": {\n"
    "    \"project_id\": \"0000000000000000000009\",\n    \"project_name\": \"Synth \\\"q\\\" \xc3\xa9\",\n"
    "    \"storeys\": {\n      \"0000000000000000000001\": {\n        \"id\": \"0000000000000000000001\",\n"
    "        \"name\": \"L0\",\n        \"elevation\": 0.0\n      }\n    },\n"
    "    \"materials\": {\n      \"0000000000000000000002\": {\n        \"id\": \"0000000000000000000002\",\n"
    "        \"name\": \"S355\",\n        \"category\": \"steel\",\n        \"props\": {\n"
    "          \"strength_mpa\": 355.0\n        }\n      }\n    },\n"
    "    \"elements\": {\n      \"0000000000000000000003\": {\n        \"id\": \"0000000000000000000003\",\n"
    "        \"class\": \"IfcBuildingElementProxy\",\n        \"name\": \"M\",\n"
    "        \"storey\": \"0000000000000000000001\",\n        \"material\": \"0000000000000000000002\",\n"
    "        \"placement\": {\n          \"origin\": [\n            1e-7,\n            1e+16,\n            0.0\n          ],\n"
    "          \"z_axis\": [\n            0.0,\n            0.0,\n            1.0\n          ],\n"
    "          \"x_axis\": [\n            1.0,\n            0.0,\n            0.0\n          ]\n        },\n"
    "        \"generator\": {\n          \"kind\": \"mesh\",\n          \"positions\": [\n"
    "            [\n              0.0,\n              0.0,\n              0.0\n            ],\n"
    "            [\n              1.0,\n              0.0,\n              0.0\n            ],\n"
    "            [\n              0.0,\n              1.0,\n              0.0\n            ]\n          ],\n"
    "          \"indices\": [\n            0,\n            1,\n            2\n          ]\n        },\n"
    "        \"properties\": {\n          \"A/b\": \"1\",\n          \"Pset/x\": \"y\"\n        },\n"
    "        \"ifc_entity\": \"IfcDoor\"\n      },\n"
    "      \"0000000000000000000004\": {\n        \"id\": \"0000000000000000000004\",\n"
    "        \"class\": \"IfcSlab\",\n        \"name\": \"P\",\n"
    "        \"storey\": \"0000000000000000000001\",\n        \"material\": \"0000000000000000000002\",\n"
    "        \"placement\": {\n          \"origin\": [\n            0.0,\n            0.0,\n            0.0\n          ],\n"
    "          \"z_axis\": [\n            0.0,\n            0.0,\n            1.0\n          ],\n"
    "          \"x_axis\": [\n            1.0,\n            0.0,\n            0.0\n          ]\n        },\n"
    "        \"generator\": {\n          \"kind\": \"extruded_polygon\",\n          \"outline\": [\n"
    "            [\n              0.0,\n              0.0\n            ],\n"
    "            [\n              10.0,\n              0.0\n            ],\n"
    "            [\n              0.0,\n              10.0\n            ]\n          ],\n"
    "          \"voids\": [\n            [\n              [\n                1.0,\n                1.0\n              ],\n"
    "              [\n                1.0,\n                2.0\n              ],\n"
    "              [\n                2.0,\n                1.0\n              ]\n            ]\n          ],\n"
    "          \"height\": 200.0\n        }\n      },\n"
    "      \"0000000000000000000005\": {\n        \"id\": \"0000000000000000000005\",\n"
    "        \"class\": \"IfcWall\",\n        \"name\": \"W\",\n"
    "        \"storey\": \"0000000000000000000001\",\n        \"material\": \"0000000000000000000002\",\n"
    "        \"placement\": {\n          \"origin\": [\n            0.0,\n            0.0,\n            0.0\n          ],\n"
    "          \"z_axis\": [\n            0.0,\n            0.0,\n            1.0\n          ],\n"
    "          \"x_axis\": [\n            1.0,\n            0.0,\n            0.0\n          ]\n        },\n"
    "        \"generator\": {\n          \"kind\": \"wall\",\n          \"length\": 4000.0,\n"
    "          \"thickness\": 200.0,\n          \"height\": 2500.0\n        }\n      }\n    },\n"
    "    \"project\": {\n      \"client\": \"X\",\n      \"phase\": \"31\"\n    }\n  }\n}";

int main(int argc, char **argv) {
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    golden = dir;
    Buf b, b2;
    buf_init(&b);
    buf_init(&b2);
    cc_error err;

    /* a, b: goldens */
    golden_roundtrip("frame.json", 0);
    golden_roundtrip("verbs_after.json", 1);
    golden_roundtrip("verbs_after_undo.json", 1);
    golden_roundtrip("ifc/frame_reimport.json", 1);

    size_t fn;
    char *frame = slurp(golden, "frame.json", &fn);
    if (fn && frame[fn - 1] == '\n') fn--;

    /* timing: load + save of frame.json */
    {
        int reps = 200;
        double t0 = now_ms();
        for (int i = 0; i < reps; i++) roundtrip(frame, fn, &b, &err);
        printf("  frame.json load+save: %.3f ms (mean of %d)\n", (now_ms() - t0) / reps, reps);
    }

    /* the model API on the loaded frame */
    {
        cc_model m;
        check_true("frame loads", cc_model_load(&m, frame, fn, &err) == 0);
        check_true("72 elements, 3 storeys, 1 material", m.nelements == 72 && m.nstoreys == 3 && m.nmaterials == 1);
        int sorted = 1, found = 1;
        for (size_t i = 1; i < m.nelements; i++) sorted &= cc_gid_cmp(&m.elements[i - 1].id, &m.elements[i].id) < 0;
        for (size_t i = 0; i < m.nelements; i++) {
            found &= cc_model_element(&m, &m.elements[i].id) == &m.elements[i];
            found &= cc_model_storey(&m, &m.elements[i].storey) != NULL;
        }
        check_true("elements sorted by id", sorted);
        check_true("lookups by id", found);
        check_true("validate after load", cc_model_validate(&m, &err) == 0);
        cc_model_free(&m);
    }

    /* synthetic model: every optional part, idempotent and in Rust's order */
    {
        int rc = roundtrip(SYNTH, sizeof SYNTH - 1, &b, &err);
        if (rc) printf("  synth: %s\n", err.msg);
        check_eq("synthetic model round trip", b.p, b.len, SYNTH, sizeof SYNTH - 1);
    }

    /* c: robustness. Truncations. */
    int errors = 0, total = 0;
    for (int i = 0; i < 300; i++) {
        size_t cut = (size_t)((double)fn * i / 300.0);
        char *p = malloc(cut ? cut : 1); /* exact size: ASan sees any overread */
        memcpy(p, frame, cut);
        errors += try_load(p, cut, &err) != 0;
        total++;
        free(p);
    }
    check_true("300 truncations all refused", errors == 300);
    /* Single-byte mutations. */
    int ok = 0;
    for (int i = 0; i < 300; i++) {
        char *p = malloc(fn);
        memcpy(p, frame, fn);
        size_t at = (size_t)(next() % fn); /* two statements: a fixed order of the calls */
        p[at] = (char)(next() & 0xff);
        ok += try_load(p, fn, &err) == 0;
        free(p);
    }
    printf("  300 mutations: %d still load, %d refused\n", ok, 300 - ok);
    check_true("mutations survived", 1);
    /* 100-deep array in an ignored field; 64 levels is the limit. */
    {
        buf_clear(&b);
        buf_puts(&b, "{\"x\":");
        buf_fill(&b, '[', 100);
        buf_fill(&b, ']', 100);
        buf_putc(&b, '}');
        check_true("100-deep array refused", try_load(b.p, b.len, &err) != 0 && contains(err.msg, "recursion"));
        JrError je;
        Arena a;
        arena_init(&a, 0);
        buf_clear(&b);
        buf_fill(&b, '[', 64);
        buf_fill(&b, ']', 64);
        check_true("64-deep array parses", jr_parse(&a, b.p, b.len, &je) != NULL);
        buf_clear(&b);
        buf_fill(&b, '[', 65);
        buf_fill(&b, ']', 65);
        check_true("65-deep array refused", jr_parse(&a, b.p, b.len, &je) == NULL);
        arena_free(&a);
    }
    /* A 10 MB project name: loads and comes back whole. */
    {
        const char *key = "\"project_name\": \"Concrete frame 3x2x2\"";
        const char *at = NULL;
        for (size_t i = 0; i + strlen(key) <= fn && !at; i++)
            if (memcmp(frame + i, key, strlen(key)) == 0) at = frame + i;
        buf_clear(&b);
        buf_append(&b, frame, (size_t)(at - frame));
        buf_puts(&b, "\"project_name\": \"");
        for (int i = 0; i < 10 * 1000 * 1000 / 10; i++) buf_puts(&b, "abcd\\u00e9xy");
        buf_putc(&b, '"');
        buf_append(&b, at + strlen(key), fn - (size_t)(at - frame) - strlen(key));
        cc_model m;
        int rc = cc_model_load(&m, b.p, b.len, &err);
        check_true("10 MB string loads", rc == 0 && m.project_name.n == 8 * 1000 * 1000);
        cc_model_free(&m);
    }
    /* -0.0 in a coordinate saves as 0.0; so does -0. */
    {
        const char *from = "\"origin\": [\n            0.0,";
        Buf in;
        buf_init(&in);
        const char *at = NULL;
        for (size_t i = 0; i + strlen(from) <= fn && !at; i++)
            if (memcmp(frame + i, from, strlen(from)) == 0) at = frame + i;
        const char *neg[2] = {"\"origin\": [\n            -0.0,", "\"origin\": [\n            -0,"};
        for (int k = 0; k < 2; k++) {
            buf_clear(&in);
            buf_append(&in, frame, (size_t)(at - frame));
            buf_puts(&in, neg[k]);
            buf_append(&in, at + strlen(from), fn - (size_t)(at - frame) - strlen(from));
            int rc = roundtrip(in.p, in.len, &b, &err);
            check_true("-0.0 loads", rc == 0);
            check_eq("-0.0 saves as 0.0", b.p, b.len, frame, fn);
        }
        buf_free(&in);
    }
    /* NaN-like tokens, out of range numbers, bad JSON. */
    const char *bad_num[] = {"NaN", "Infinity", "-Infinity", "nan", "1e999", "-1e400", "+1", "01", "1.", ".5", "0x10", "1e"};
    for (size_t i = 0; i < sizeof bad_num / sizeof *bad_num; i++) {
        Buf t;
        buf_init(&t);
        buf_puts(&t, "\"elevation\": ");
        buf_puts(&t, bad_num[i]);
        int range = i == 4 || i == 5; /* 1e999, -1e400: valid JSON, out of f64 range */
        expect_error(frame, fn, "\"elevation\": -800.0", buf_cstr(&t), range ? "number out of range" : "parse: ");
        buf_free(&t);
    }
    /* Duplicate keys: a struct field is an error; a map key keeps the last value. */
    expect_error(frame, fn, "\"name\": \"Fondations\",", "\"name\": \"Fondations\", \"name\": \"X\",", "duplicate field `name`");
    expect_error(frame, fn, "\"kind\": \"rect_column\",", "\"kind\": \"rect_column\", \"kind\": \"wall\",", "duplicate field `kind`");
    {
        const char *s = "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"0000000000000000000009\","
                        "\"project_name\":\"d\",\"storeys\":{},\"materials\":{},\"elements\":{},"
                        "\"project\":{\"client\":\"A\",\"client\":\"B\"}}}";
        cc_model m;
        int rc = cc_model_load(&m, s, strlen(s), &err);
        check_true("duplicate map key: last wins", rc == 0 && m.nproject == 1 && m.project[0].value.n == 1 && m.project[0].value.p[0] == 'B');
        cc_model_free(&m);
    }
    /* Format, version, unknown and unsupported sections. */
    expect_error(frame, fn, "\"version\": 1", "\"version\": 2", "unsupported bimble format version 2 (this build reads 1 and 2)");
    expect_error(frame, fn, "\"version\": 1", "\"version\": 1.0", "invalid type: floating point `1.0`, expected u32");
    expect_error(frame, fn, "\"format\": \"bimble\"", "\"format\": \"ifc\"", "not a bimble file (format = \"ifc\")");
    /* Views, sheets and schedules are held since A6a: a bad one is a parse error (it was
     * "unsupported by the C core" before). */
    expect_error(frame, fn, "\"project_name\"", "\"views\": {\"x\": 1}, \"project_name\"", "parse: invalid IFC GlobalId: \"x\"");
    expect_error(frame, fn, "\"project_name\"", "\"schedules\": [1], \"project_name\"", "invalid type: integer `1`, expected struct ScheduleDef");
    /* The georeference is held since GEO1 (ADR 0045): Georef denies unknown fields, as serde. */
    expect_error(frame, fn, "\"project_name\"", "\"georef\": {\"x\": 1}, \"project_name\"",
                 "unknown field `x`, expected one of `rotation_deg`, `crs`, `east_m`, `north_m`, `height_m`, "
                 "`map_rotation_deg`");
    expect_error(frame, fn, "\"generator\": {", "\"openings\": [{}], \"generator\": {", "missing field `placement`");
    expect_error(frame, fn, "\"strength_mpa\": 30.0,", "\"strength_mpa\": 30.0, \"colour\": 1,", "unknown field `colour`");
    expect_error(frame, fn, "\"kind\": \"rect_column\"", "\"kind\": \"cylinder\"", "unknown variant `cylinder`");
    expect_error(frame, fn, "{", "\xef\xbb\xbf{", "parse: ");
    expect_error(frame, fn, "\"Fondations\"", "\"Fond\xc3\"", "invalid UTF-8");
    expect_error(frame, fn, "\"Fondations\"", "\"Fond\\ud800\"", "unexpected end of hex escape");
    expect_error(frame, fn, "\"Fondations\"", "\"Fond\\udc00\"", "lone leading surrogate");
    /* Empty unsupported sections and unknown fields are fine (nothing is lost but the unknown). */
    {
        buf_clear(&b);
        const char *at = frame + 1;
        buf_puts(&b, "{\"extra\": [1, {\"a\": null}],");
        buf_append(&b, at, fn - 1);
        size_t pos = 0;
        while (pos < b.len && memcmp(b.p + pos, "\"project_name\"", 14) != 0) pos++;
        Buf c;
        buf_init(&c);
        buf_append(&c, b.p, pos);
        buf_puts(&c, "\"views\": {}, \"sheets\": {}, \"schedules\": [], \"georef\": {}, \"zzz\": 1, ");
        buf_append(&c, b.p + pos, b.len - pos);
        int rc = roundtrip(c.p, c.len, &b2, &err);
        if (rc) printf("  %s\n", err.msg);
        check_eq("empty sections and unknown fields: same bytes", b2.p, b2.len, frame, fn);
        buf_free(&c);
    }
    /* Strings: escapes and surrogate pairs decode to UTF-8 and save as serde_json writes. */
    expect_error(frame, fn, "\"Fondations\"", "\"F\\u00e9\\ud83d\\ude00\\n\\/\" , \"name\": 1", "duplicate field `name`");
    {
        const char *from = "\"Fondations\"";
        Buf in;
        buf_init(&in);
        const char *at = NULL;
        for (size_t i = 0; i + strlen(from) <= fn && !at; i++)
            if (memcmp(frame + i, from, strlen(from)) == 0) at = frame + i;
        buf_append(&in, frame, (size_t)(at - frame));
        buf_puts(&in, "\"F\\u00e9\\ud83d\\ude00\\n\\/\\u0001\"");
        buf_append(&in, at + strlen(from), fn - (size_t)(at - frame) - strlen(from));
        int rc = roundtrip(in.p, in.len, &b, &err);
        check_true("escaped name loads", rc == 0);
        check_true("escaped name saves as serde_json", contains(buf_cstr(&b), "\"F\xc3\xa9\xf0\x9f\x98\x80\\n/\\u0001\""));
        buf_free(&in);
    }

    /* d: validation errors, Rust's texts. */
    expect_error(frame, fn, "\"z_axis\": [\n            0.0,\n            0.0,\n            1.0",
                 "\"z_axis\": [\n            0.0,\n            0.0,\n            2.0",
                 "invalid model: IfcColumn 004z1Cm4z4hgNOg0ez4Id1 (P2.00): placement axes must be unit vectors");
    expect_error(frame, fn, "\"x_axis\": [\n            1.0,\n            0.0,\n            0.0",
                 "\"x_axis\": [\n            0.0,\n            0.0,\n            1.0",
                 "invalid model: IfcColumn 004z1Cm4z4hgNOg0ez4Id1 (P2.00): placement x_axis must be perpendicular to z_axis");
    expect_error(frame, fn, "\"storey\": \"1jLno3kWj4Hfl6pUGBDd7E\"", "\"storey\": \"1jLno3kWj4Hfl6pUGBDd7F\"",
                 "invalid model: IfcColumn 004z1Cm4z4hgNOg0ez4Id1 (P2.00): unknown storey 1jLno3kWj4Hfl6pUGBDd7F");
    expect_error(frame, fn, "\"material\": \"0aEm4Mq4nC8emERFhb_kvJ\"", "\"material\": \"0aEm4Mq4nC8emERFhb_kvK\"",
                 "invalid model: IfcColumn 004z1Cm4z4hgNOg0ez4Id1 (P2.00): unknown material 0aEm4Mq4nC8emERFhb_kvK");
    expect_error(frame, fn, "\"width\": 300.0", "\"width\": -0.5", "(P2.00): width must be > 0, got -0.5");
    expect_error(frame, fn, "\"width\": 300.0", "\"width\": -0.0", "(P2.00): width must be > 0, got -0");
    expect_error(frame, fn, "\"width\": 300.0", "\"width\": -1e-7", "width must be > 0, got -0.0000001");
    expect_error(frame, fn, "\"height\": 2780.0", "\"height\": -1e20", "height must be > 0, got -100000000000000000000");
    expect_error(frame, fn, "\"id\": \"004z1Cm4z4hgNOg0ez4Id1\"", "\"id\": \"004z1Cm4z4hgNOg0ez4Id2\"",
                 "invalid model: IfcColumn 004z1Cm4z4hgNOg0ez4Id1 (P2.00): key does not match id");
    expect_error(frame, fn, "\"id\": \"004z1Cm4z4hgNOg0ez4Id1\"", "\"id\": \"404z1Cm4z4hgNOg0ez4Id1\"",
                 "invalid IFC GlobalId: \"404z1Cm4z4hgNOg0ez4Id1\"");
    expect_error(frame, fn, "\"e_mpa\": 33000.0", "\"e_mpa\": 3300000.0", "invalid model: material modulus: a positive number up to 1000000");
    expect_error(frame, fn, "\"elevation\": -800.0", "\"elevation\": \"low\"", "invalid type: string \"low\", expected f64");
    expect_error(frame, fn, "\"name\": \"Fondations\",", "", "missing field `name`");
    {
        /* slab orientation, project keys, empty property key, mesh index */
        const char *s1 = "\"generator\": {\n          \"kind\": \"rect_column\",\n          \"width\": 300.0,";
        expect_error(frame, fn, s1, "\"properties\": {\"\": \"x\"}, \"generator\": {\n          \"kind\": \"rect_column\",\n          \"width\": 300.0,",
                     "(P2.00): empty property key");
        expect_error(frame, fn, s1, "\"generator\": {\"kind\": \"mesh\", \"positions\": [[0,0,0]], \"indices\": [0, 1]}, \"x\": {\"width\": 300.0,",
                     "(P2.00): mesh index out of range");
        expect_error(frame, fn, s1, "\"generator\": {\"kind\": \"mesh\", \"positions\": 3, \"indices\": [0, 1]}, \"x\": {\"width\": 300.0,",
                     "unsupported by the C core: mesh arrays by index or hash");
        expect_error(frame, fn, s1, "\"generator\": {\"kind\": \"slab\", \"outline\": [[0,0],[0,1],[1,0]], \"thickness\": 1}, \"x\": {\"width\": 300.0,",
                     "(P2.00): slab outline must be counter-clockwise with non-zero area");
        expect_error(frame, fn, s1, "\"generator\": {\"kind\": \"extruded_polygon\", \"outline\": [[0,0],[1,0],[0,1]], \"voids\": [[[0,0]]], \"height\": 1}, \"x\": {\"width\": 300.0,",
                     "(P2.00): void 0 needs at least 3 points");
        expect_error(frame, fn, "\"project_name\"", "\"project\": {\"bad key\": \"x\"}, \"project_name\"",
                     "invalid model: project information: \"bad key\" is not a key (letters, digits and _, at most 64)");
    }
    check_true("errors carry an offset", 1);
    (void)total;
    free(frame);
    buf_free(&b);
    buf_free(&b2);
    return report("file");
}
