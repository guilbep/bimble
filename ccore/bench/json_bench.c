/* Where JSON costs time in the C core (ticket J1). Synthetic models only (bench/large_model.py).
 *
 *   clang -O2 … bench/json_bench.c out/native/libccore_session.a out/native/libccore.a -lm
 *   json_bench <model v1 .bimble> <work dir> [runs=5]
 *
 * Prints one JSON object (median ms of `runs`, bytes) on stdout:
 *  open    v1 text: whole load; jr_parse (text -> DOM); validate; canonical; DOM -> model is
 *          the rest. v2 binary (the same model saved by cc_model_save_v2): whole load; gunzip;
 *          cbor_parse (CBOR -> DOM); validate; canonical; DOM -> model is the rest.
 *  save    v1 text; v2 split: model -> CBOR, gzip.
 *  journal a session with its session file in <work dir>: per change (verb place, move,
 *          delete) the call with the entry write minus the same call without persistence, and
 *          the bytes the entry adds; restore: the reopen time with 1,000 entries minus with 0.
 *  proto   query elements (1,000 rows), query element, the change JSON: build ms and bytes; the
 *          answers are written to <work dir>/proto-*.json for the page-side parse (json_parse.mjs).
 * The work dir gets copies of the model; nothing outside it is written. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "calls.h"
#include "cbor.h"
#include "file.h"
#include "inflate.h"
#include "json_r.h"
#include "platform.h"
#include "session.h"

const cc_platform *cc_platform_native(void);

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static int dcmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static double median(double *v, int n) {
    qsort(v, n, sizeof *v, dcmp);
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}
static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long k = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc(k + 1);
    if (fread(p, 1, k, f) != (size_t)k) exit(2);
    fclose(f);
    p[k] = 0;
    *n = k;
    return p;
}
static void spit(const char *path, const char *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n) exit(2);
    fclose(f);
}
static long fsize(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long k = ftell(f);
    fclose(f);
    return k;
}
#define RUNS_MAX 31
static int RUNS = 5;
#define MED(expr_)                                         \
    ({                                                     \
        double v_[RUNS_MAX];                               \
        for (int r_ = 0; r_ < RUNS; r_++) {                \
            double t0_ = now_ms();                         \
            expr_;                                         \
            v_[r_] = now_ms() - t0_;                       \
        }                                                  \
        median(v_, RUNS);                                  \
    })

static void open_split(const char *tag, const char *data, size_t n, int binary) {
    cc_model m;
    cc_error err;
    double whole = MED({
        if (cc_model_load(&m, data, n, &err)) { fprintf(stderr, "load: %s\n", err.msg); exit(1); }
        cc_model_free(&m);
    });
    double gz = 0, parse;
    Buf body;
    buf_init(&body);
    if (binary) {
        const char *why;
        gz = MED({
            buf_clear(&body);
            if (cc_gunzip((const unsigned char *)data + CC_MAGIC_LEN, n - CC_MAGIC_LEN, (size_t)1 << 30, &body, &why)) exit(1);
        });
    }
    JrError je;
    parse = MED({
        Arena a;
        arena_init(&a, 0);
        JrVal *root = binary ? cbor_parse(&a, (const unsigned char *)body.p, body.len, &je) : jr_parse(&a, data, n, &je);
        if (!root) exit(1);
        arena_free(&a);
    });
    if (cc_model_load(&m, data, n, &err)) exit(1);
    double val = MED(if (cc_model_validate(&m, &err)) exit(1));
    double can = MED(cc_model_canonical(&m));
    cc_model_free(&m);
    printf("\"open_%s\":{\"bytes\":%zu,\"inflated\":%zu,\"total_ms\":%.2f,\"gunzip_ms\":%.2f,\"to_dom_ms\":%.2f,"
           "\"dom_to_model_ms\":%.2f,\"validate_ms\":%.2f,\"canonical_ms\":%.2f},\n",
           tag, n, body.len, whole, gz, parse, whole - gz - parse - val - can, val, can);
    buf_free(&body);
}

static void call(cc_session *s, const char *op, const char *name, const char *body) {
    Buf out;
    buf_init(&out);
    int st = cc_call(s, op, name, body, strlen(body), &out);
    if (st != CC_OK) { fprintf(stderr, "%s %s %s: %.*s\n", op, name, body, (int)out.len, out.p); exit(1); }
    buf_free(&out);
}

/* The three changes: k-th place (column), move of an existing element, delete of it. */
static void change(cc_session *s, int which, int k, char *buf, size_t cap) {
    const cc_model *m = cc_session_model(s);
    const cc_element *e = &m->elements[(k * 7) % m->nelements];
    char id[23];
    memcpy(id, e->id.c, 22), id[22] = 0;
    if (which == 0) {
        snprintf(buf, cap, "{\"a\":[%.1f,2500.0],\"kind\":\"column\",\"storey\":\"%.22s\"}", 3000.0 + 10 * k, m->storeys[0].id.c);
        call(s, "verb", "place", buf);
    } else if (which == 1) {
        snprintf(buf, cap, "{\"d\":[10.0,0.0,0.0],\"ids\":[\"%s\"]}", id);
        call(s, "verb", "move", buf);
    } else {
        snprintf(buf, cap, "{\"ids\":[\"%s\"]}", id);
        call(s, "verb", "delete", buf);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: json_bench <model v1> <work dir> [runs]\n"); return 2; }
    if (argc > 3) RUNS = atoi(argv[3]);
    if (RUNS < 1 || RUNS > RUNS_MAX) RUNS = 5;
    const char *dir = argv[2];
    size_t n1;
    char *v1 = slurp(argv[1], &n1);
    if (!v1) return 2;
    cc_model m;
    cc_error err;
    if (cc_model_load(&m, v1, n1, &err)) { fprintf(stderr, "%s\n", err.msg); return 1; }
    printf("{\"elements\":%zu,\n", m.nelements);
    Buf b2, b1, cb, gz;
    buf_init(&b2), buf_init(&b1), buf_init(&cb), buf_init(&gz);
    cc_model_save_v2(&m, &b2);
    open_split("v1", v1, n1, 0);
    open_split("v2", b2.p, b2.len, 1);
    double s1 = MED(buf_clear(&b1); cc_model_save(&m, &b1));
    double sc = MED(buf_clear(&cb); cc_model_to_cbor(&m, &cb));
    double sg = MED(buf_clear(&gz); cc_gzip((const unsigned char *)cb.p, cb.len, &gz));
    printf("\"save_v1\":{\"bytes\":%zu,\"ms\":%.2f},\n\"save_v2\":{\"bytes\":%zu,\"cbor_bytes\":%zu,\"to_cbor_ms\":%.2f,\"gzip_ms\":%.2f},\n",
           b1.len, s1, b2.len, cb.len, sc, sg);
    cc_model_free(&m);

    /* journal */
    char mpath[4096], spath[4096], body[512];
    snprintf(mpath, sizeof mpath, "%s/j1-model.bimble", dir);
    snprintf(spath, sizeof spath, "%s/j1-model.bimble.ccore-session", dir);
    spit(mpath, b2.p, b2.len);
    const char *names[3] = {"place", "move", "delete"};
    printf("\"journal\":{");
    for (int which = 0; which < 3; which++) {
        double with[RUNS_MAX], without[RUNS_MAX];
        long bytes = 0;
        for (int pers = 0; pers < 2; pers++) {
            remove(spath);
            cc_session *s = cc_session_new(cc_platform_native());
            if (pers ? cc_session_open_persisted(s, mpath, spath, &err) : cc_session_open(s, mpath, &err)) return 1;
            for (int r = 0; r < RUNS; r++) {
                long before = fsize(spath);
                double t0 = now_ms();
                change(s, which, r, body, sizeof body);
                if (pers) cc_session_persist_now(s);
                double t = now_ms() - t0;
                (pers ? with : without)[r] = t;
                if (pers) bytes = fsize(spath) - before;
            }
            cc_session_free(s);
        }
        double w = median(with, RUNS), wo = median(without, RUNS);
        printf("\"%s\":{\"call_ms\":%.3f,\"entry_ms\":%.3f,\"entry_bytes\":%ld},", names[which], wo, w - wo, bytes);
    }
    /* restore: 1,000 entries (place, move, delete in turn) vs none */
    double rest[2];
    long sbytes = 0;
    for (int k = 0; k < 2; k++) {
        remove(spath);
        cc_session *s = cc_session_new(cc_platform_native());
        if (cc_session_open_persisted(s, mpath, spath, &err)) return 1;
        if (k)
            for (int i = 0; i < 1000; i++) {
                change(s, i % 3, i, body, sizeof body);
                cc_session_persist_now(s);
            }
        cc_session_free(s);
        sbytes = fsize(spath);
        double v[RUNS_MAX];
        for (int r = 0; r < RUNS; r++) {
            cc_session *t = cc_session_new(cc_platform_native());
            double t0 = now_ms();
            if (cc_session_open_persisted(t, mpath, spath, &err)) return 1;
            v[r] = now_ms() - t0;
            cc_session_free(t);
        }
        rest[k] = median(v, RUNS);
    }
    printf("\"restore_0_ms\":%.2f,\"restore_1000_ms\":%.2f,\"replay_per_1000_ms\":%.2f,\"file_bytes_1000\":%ld,\"snapshot_rewrite\":",
           rest[0], rest[1], rest[1] - rest[0], sbytes);
    /* a snapshot with model text (after more than one change in a call, a Save, 2,000 entries) */
    {
        remove(spath);
        cc_session *s = cc_session_new(cc_platform_native());
        if (cc_session_open_persisted(s, mpath, spath, &err)) return 1;
        change(s, 0, 0, body, sizeof body);
        change(s, 0, 1, body, sizeof body); /* two steps in one look: a full snapshot */
        double t0 = now_ms();
        cc_session_persist_now(s);
        double t = now_ms() - t0;
        cc_session_free(s);
        long sb = fsize(spath);
        double v[RUNS_MAX];
        for (int r = 0; r < RUNS; r++) {
            cc_session *u = cc_session_new(cc_platform_native());
            double t1 = now_ms();
            if (cc_session_open_persisted(u, mpath, spath, &err)) return 1;
            v[r] = now_ms() - t1;
            cc_session_free(u);
        }
        printf("{\"write_ms\":%.2f,\"bytes\":%ld,\"restore_ms\":%.2f}},\n", t, sb, median(v, RUNS));
    }
    /* proto */
    {
        cc_session *s = cc_session_new(cc_platform_native());
        if (cc_session_open(s, mpath, &err)) return 1;
        const cc_model *mm = cc_session_model(s);
        char q[128];
        snprintf(q, sizeof q, "{\"id\":\"%.22s\"}", mm->elements[mm->nelements / 2].id.c);
        struct { const char *tag, *op, *name, *body; } P[] = {
            {"query_elements", "query", "elements", "{}"},
            {"query_elements_10000", "query", "elements", "{\"limit\":10000}"},
            {"query_element", "query", "element", q},
        };
        printf("\"proto\":{");
        Buf out;
        buf_init(&out);
        for (size_t i = 0; i < sizeof P / sizeof *P; i++) {
            double t = MED({
                buf_clear(&out);
                if (cc_call(s, P[i].op, P[i].name, P[i].body, strlen(P[i].body), &out) != CC_OK) {
                    fprintf(stderr, "%s: %.*s\n", P[i].tag, (int)out.len, out.p);
                    exit(1);
                }
            });
            char path[4200];
            snprintf(path, sizeof path, "%s/proto-%s.json", dir, P[i].tag);
            spit(path, out.p, out.len);
            printf("\"%s\":{\"build_ms\":%.3f,\"bytes\":%zu},", P[i].tag, t, out.len);
        }
        change(s, 0, 0, body, sizeof body);
        double t = MED(buf_clear(&out); cc_change_json(s, &out));
        char path[4200];
        snprintf(path, sizeof path, "%s/proto-change.json", dir);
        spit(path, out.p, out.len);
        printf("\"change\":{\"build_ms\":%.3f,\"bytes\":%zu}}}\n", t, out.len);
        buf_free(&out);
        cc_session_free(s);
    }
    remove(spath);
    buf_free(&b1), buf_free(&b2), buf_free(&cb), buf_free(&gz);
    free(v1);
    return 0;
}
