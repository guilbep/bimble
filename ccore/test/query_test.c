/* Suite query: replays golden/query.txt (dispatch::query on frame.json, Rust's answers) and
 * compares status and answer byte for byte. Then argument paths the golden does not hold, with
 * texts read from the serde / serde_json sources (marked "inferred": not Rust's output). */
#define _POSIX_C_SOURCE 199309L /* clock_gettime under -std=c11 */
#include <string.h>
#include <time.h>
#include "../src/file.h"
#include "../src/query.h"
#include "check.h"

static const char *golden;

typedef struct {
    char name[64];
    const char *args, *want;
    size_t args_n, want_n;
    int status;
} Rec;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Splits the transcript in place; returns the record count. */
static size_t parse_records(char *s, size_t n, Rec *r, size_t max) {
    size_t k = 0;
    char *end = s + n;
    while (s < end && k < max) {
        char *nl = memchr(s, '\n', (size_t)(end - s));
        if (!nl) break;
        if (nl - s > 10 && memcmp(s, ">>> query ", 10) == 0) {
            char *name = s + 10, *sp = memchr(name, ' ', (size_t)(nl - name));
            if (!sp || sp - name >= 64) break;
            memcpy(r[k].name, name, (size_t)(sp - name));
            r[k].name[sp - name] = 0;
            r[k].args = sp + 1;
            r[k].args_n = (size_t)(nl - sp - 1);
            char *a = nl + 1, *nl2 = memchr(a, '\n', (size_t)(end - a));
            if (!nl2 || nl2 - a < 4) break;
            r[k].status = atoi(a);
            r[k].want = a + 4;
            r[k].want_n = (size_t)(nl2 - a - 4);
            k++;
            nl = nl2;
        }
        s = nl + 1;
    }
    return k;
}

static void expect(const cc_model *m, const char *name, const char *args, int status, const char *want) {
    Buf b;
    buf_init(&b);
    int st = cc_query(m, name, args, strlen(args), &b);
    char what[256];
    snprintf(what, sizeof what, "inferred: %s %s", name, args);
    check_true(what, st == status);
    check_eq(what, b.p ? b.p : "", b.len, want, strlen(want));
    buf_free(&b);
}

int main(int argc, char **argv) {
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    golden = dir;

    size_t fn, qn;
    char *frame = slurp(golden, "frame.json", &fn);
    if (fn && frame[fn - 1] == '\n') fn--;
    cc_model m;
    cc_error err;
    if (cc_model_load(&m, frame, fn, &err)) {
        printf("frame.json does not load: %s\n", err.msg);
        return 1;
    }
    char *text = slurp(golden, "query.txt", &qn);
    static Rec rec[256];
    size_t nrec = parse_records(text, qn, rec, 256);
    check_true("query.txt has records", nrec >= 18);

    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < nrec; i++) {
        buf_clear(&b);
        int st = cc_query(&m, rec[i].name, rec[i].args, rec[i].args_n, &b);
        char what[256];
        snprintf(what, sizeof what, "query %s %.*s", rec[i].name, (int)rec[i].args_n, rec[i].args);
        check_true(what, st == rec[i].status);
        check_eq(what, b.p ? b.p : "", b.len, rec[i].want, rec[i].want_n);
    }
    printf("  golden records: %zu\n", nrec);

    /* frame2: walls, extruded polygons with voids, a mesh, properties, project info (T2b). */
    {
        size_t f2n, q2n;
        char *f2 = slurp(golden, "frame2.json", &f2n);
        if (f2n && f2[f2n - 1] == '\n') f2n--;
        cc_model m2;
        cc_error e2;
        int loaded = cc_model_load(&m2, f2, f2n, &e2) == 0;
        check_true("frame2.json loads", loaded);
        if (loaded) {
            char *t2 = slurp(golden, "frame2/query.txt", &q2n);
            static Rec rec2[256];
            size_t n2 = parse_records(t2, q2n, rec2, 256);
            check_true("frame2/query.txt has records", n2 >= 25);
            for (size_t i = 0; i < n2; i++) {
                buf_clear(&b);
                int st = cc_query(&m2, rec2[i].name, rec2[i].args, rec2[i].args_n, &b);
                char what[256];
                snprintf(what, sizeof what, "frame2 query %s %.*s", rec2[i].name, (int)rec2[i].args_n,
                         rec2[i].args);
                check_true(what, st == rec2[i].status);
                check_eq(what, b.p ? b.p : "", b.len, rec2[i].want, rec2[i].want_n);
            }
            printf("  frame2 golden records: %zu\n", n2);
            free(t2);
        }
        cc_model_free(&m2);
        free(f2);
    }

    {
        int reps = 100;
        double t0 = now_ms();
        for (int r = 0; r < reps; r++)
            for (size_t i = 0; i < nrec; i++) {
                buf_clear(&b);
                cc_query(&m, rec[i].name, rec[i].args, rec[i].args_n, &b);
            }
        printf("  all %zu records: %.3f ms per pass (mean of %d)\n", nrec, (now_ms() - t0) / reps, reps);
    }

    /* Inferred paths (serde_core 1.0.229, serde_json 1.0.154 sources), not in the golden. */
    {
        Buf x;
        buf_init(&x);
        int st = cc_query(&m, "storeys", "", 0, &x);
        check_true("empty body is null, reads as {}", st == 200 && x.len > 2 && x.p[0] == '[');
        buf_clear(&x);
        st = cc_query(&m, "materials", " null ", 6, &x);
        check_true("null args", st == 200);
        buf_clear(&x);
        st = cc_query(&m, "elements", "[]", 2, &x);
        check_true("elements [] is the default filter", st == 200 && x.len > 1000);
        buf_free(&x);
    }
    expect(&m, "views", "{}", 200, "[]");
    expect(&m, "sheets", "null", 200, "[]");
    expect(&m, "schedules", "{}", 200, "[]");
    expect(&m, "project", "[]", 200,
           "{\"building_id\":\"3S50QIFi3RTlrcI6MgJC7G\",\"id\":\"1ECFGZP4n20ADaVDzRQc6J\",\"info\":{},"
           "\"name\":\"Concrete frame 3x2x2\",\"site_id\":\"24iRmPJEuGZKmhVgNqicyR\"}");
    expect(&m, "elements", "{\"limit\":0}", 200, "[]");
    expect(&m, "elements", "{\"class\":\"IfcWall\"}", 200, "[]");
    {
        Buf x, y;
        buf_init(&x);
        buf_init(&y);
        const char *a1 = "{\"class\":null,\"limit\":null,\"storey\":null}";
        int s1 = cc_query(&m, "elements", a1, strlen(a1), &x), s2 = cc_query(&m, "elements", "{}", 2, &y);
        check_true("null fields are None", s1 == 200 && s2 == 200);
        check_eq("null fields are None", x.p, x.len, y.p, y.len);
        buf_free(&x);
        buf_free(&y);
    }
    expect(&m, "storeys", "[1]", 400, "bad arguments: invalid length 1, expected fewer elements in array");
    expect(&m, "storeys", "1", 400, "bad arguments: invalid type: integer `1`, expected struct NoArgs");
    expect(&m, "storeys", "\"a\\n\"", 400, "bad arguments: invalid type: string \"a\\n\", expected struct NoArgs");
    expect(&m, "storeys", "{\"b\":1,\"a\":2}", 400, "bad arguments: unknown field `a`, there are no fields");
    expect(&m, "element", "{}", 400, "bad arguments: missing field `id`");
    expect(&m, "element", "{\"id\":1}", 400, "bad arguments: invalid type: integer `1`, expected a string");
    expect(&m, "element", "{\"id\":null}", 400, "bad arguments: invalid type: null, expected a string");
    expect(&m, "element", "{\"x\":1,\"id\":\"bad\"}", 400, "bad arguments: invalid IFC GlobalId: \"bad\"");
    expect(&m, "element", "{\"id\":\"03dpTFfU58oAzh8$WYDhNY\",\"x\":1}", 400,
           "bad arguments: unknown field `x`, expected `id`");
    expect(&m, "element", "[]", 400, "bad arguments: invalid length 0, expected struct IdArgs with 1 element");
    expect(&m, "element", "true", 400, "bad arguments: invalid type: boolean `true`, expected struct IdArgs");
    expect(&m, "elements", "{\"limit\":-1}", 400, "bad arguments: invalid value: integer `-1`, expected usize");
    expect(&m, "elements", "{\"limit\":1.5}", 400, "bad arguments: invalid type: floating point `1.5`, expected usize");
    expect(&m, "elements", "{\"limit\":-0}", 400, "bad arguments: invalid type: floating point `-0.0`, expected usize");
    expect(&m, "elements", "{\"limit\":\"3\"}", 400, "bad arguments: invalid type: string \"3\", expected usize");
    expect(&m, "elements", "{\"class\":1}", 400, "bad arguments: invalid type: integer `1`, expected string or map");
    expect(&m, "elements", "{\"class\":{}}", 400, "bad arguments: invalid value: map, expected map with a single key");
    expect(&m, "elements", "{\"class\":{\"IfcBeam\":1}}", 400, "bad arguments: invalid type: integer `1`, expected unit");
    expect(&m, "elements", "{\"z\":1,\"limit\":-1}", 400, "bad arguments: invalid value: integer `-1`, expected usize");
    expect(&m, "elements", "{\"y\":1}", 400, "bad arguments: unknown field `y`, expected one of `class`, `storey`, `limit`");
    expect(&m, "elements", "{\"limit\":5,\"limit\":-1}", 400, "bad arguments: invalid value: integer `-1`, expected usize");
    {
        /* elements [class, storey, limit] by position, and a class given as {"IfcBeam": null} */
        Buf x, y;
        buf_init(&x);
        buf_init(&y);
        const char *a1 = "[\"IfcColumn\",\"1uAZA0Yfn05f$xRd1k4uPR\",5]";
        const char *a2 = "{\"class\":{\"IfcColumn\":null},\"limit\":5,\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\"}";
        int s1 = cc_query(&m, "elements", a1, strlen(a1), &x), s2 = cc_query(&m, "elements", a2, strlen(a2), &y);
        check_true("array filter = map filter", s1 == 200 && s2 == 200);
        check_eq("array filter = map filter", x.p, x.len, y.p, y.len);
        buf_free(&x);
        buf_free(&y);
    }
    expect(&m, "elements", "[null,null,null,1]", 400, "bad arguments: invalid length 4, expected fewer elements in array");
    {
        /* dispatch::parse refuses more than MAX_IDS ids before anything else */
        Buf big;
        buf_init(&big);
        buf_puts(&big, "{\"ids\":[");
        for (int i = 0; i <= CC_QUERY_MAX_IDS; i++) buf_puts(&big, i ? ",0" : "0");
        buf_puts(&big, "]}");
        buf_clear(&b);
        int st = cc_query(&m, "storeys", big.p, big.len, &b);
        check_true("too many ids", st == 400);
        check_eq("too many ids", b.p, b.len, "10001 ids: at most 10000 per call", 33);
        buf_free(&big);
    }
    {
        Buf x;
        buf_init(&x);
        int st = cc_query(&m, "storeys", "{", 1, &x);
        check_true("bad JSON is 400", st == 400 && x.len > 0);
        buf_free(&x);
    }

    buf_free(&b);
    cc_model_free(&m);
    free(text);
    free(frame);
    return report("query");
}
