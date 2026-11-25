/* Suite repo (ticket H1): the C history store reads the history folder Rust wrote
 * (golden/history/frame.bimble-history, ccore_golden's history mode: four commits on main, a
 * branch `variant` with one commit, one merge) and gives Rust's texts byte for byte: log.txt,
 * branches.txt, resolve.txt, each model_at_<n>.json (load + save), the diffs, status.txt and
 * errors.txt (a damaged object, a missing one, unknown revisions, a bad format line). Then the
 * untrusted-input cases Rust has no golden for: a flipped byte in a gzipped object, a too large
 * object, a commit with an unknown field, a deep object, a damaged HEAD, no folders (wasm). */
#include <string.h>
#include "../host/host.h"
#include "diff.h"
#include "file.h"
#include "guid.h"
#include "inflate.h"
#include "repo.h"
#include "check.h"

#define DIR "golden/history/frame.bimble-history"
#define GOLD "golden/history"

/* ---------------------------------------------------------------- a platform with overrides */

typedef struct {
    const char *suffix; /* a path ending in this */
    const char *bytes;
    size_t n;
    int status; /* CC_PF_OK: these bytes; else this status */
} Override;
static Override ov[8];
static int nov;
static const cc_platform *native;
static int no_list;

static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !memcmp(s + a - b, suf, b);
}

static const Override *find_ov(const char *path) {
    for (int i = 0; i < nov; i++)
        if (ends_with(path, ov[i].suffix)) return &ov[i];
    return NULL;
}

static int w_read(void *ctx, const char *path, size_t max, char **p, size_t *n) {
    (void)ctx;
    const Override *o = find_ov(path);
    if (!o) return native->read_file(native->ctx, path, max, p, n);
    *p = NULL, *n = 0;
    if (o->status != CC_PF_OK) return o->status;
    if (o->n > max) return CC_PF_TOO_LARGE;
    *p = malloc(o->n + 1);
    memcpy(*p, o->bytes, o->n);
    (*p)[o->n] = 0;
    *n = o->n;
    return CC_PF_OK;
}
static int w_exists(void *ctx, const char *path) {
    (void)ctx;
    const Override *o = find_ov(path);
    if (o) return o->status != CC_PF_NOT_FOUND;
    return native->exists(native->ctx, path);
}
static int w_list(void *ctx, const char *path, size_t max, char **names, size_t *n) {
    (void)ctx;
    if (no_list) return *names = NULL, *n = 0, CC_PF_NOT_FOUND;
    return native->list_dir(native->ctx, path, max, names, n);
}
static cc_platform WP;

static void set_ov(const char *suffix, const char *bytes, size_t n, int status) {
    ov[nov++] = (Override){suffix, bytes, n, status};
}

/* ---------------------------------------------------------------- helpers */

static char *golden_text(const char *name, size_t *n) { return slurp(GOLD, name, n); }

static void check_text(const char *what, Buf *got, const char *golden_name) {
    size_t n;
    char *want = golden_text(golden_name, &n);
    check_eq(what, got->p ? got->p : "", got->len, want, n);
    free(want);
}

/* The suffix "objects/ab/cdef…" of an object's path. */
static void obj_suffix(const char *hash, char out[80]) {
    snprintf(out, 80, "objects/%.2s/%s", hash, hash + 2);
}

static void resolve_or_die(cc_repo *r, const char *rev, cc_hash *h) {
    Buf e;
    buf_init(&e);
    if (cc_repo_resolve(r, rev, h, &e)) printf("resolve %s: %s\n", rev, buf_cstr(&e)), exit(2);
    buf_free(&e);
}

static void log_lines(cc_repo *r, Buf *out) {
    static const char *const REVS[] = {"HEAD", "variant", "HEAD~2", "main~1"};
    for (int k = 0; k < 4; k++) {
        buf_puts(out, "log "), buf_puts(out, REVS[k]), buf_putc(out, '\n');
        Arena a;
        arena_init(&a, 0);
        cc_log_entry *v;
        size_t n;
        Buf e;
        buf_init(&e);
        if (cc_repo_log(r, REVS[k], 100, &a, &v, &n, &e)) printf("log: %s\n", buf_cstr(&e));
        for (size_t i = 0; i < n; i++) {
            buf_puts(out, v[i].hash.h), buf_putc(out, ' ');
            buf_i64(out, v[i].c.time), buf_putc(out, ' ');
            buf_puts(out, v[i].c.author), buf_putc(out, ' ');
            if (!v[i].c.nparents) buf_putc(out, '-');
            for (size_t j = 0; j < v[i].c.nparents; j++) {
                if (j) buf_putc(out, ',');
                buf_puts(out, v[i].c.parents[j]);
            }
            buf_putc(out, ' '), buf_puts(out, v[i].c.message), buf_putc(out, '\n');
        }
        buf_free(&e);
        arena_free(&a);
    }
    buf_puts(out, "log HEAD limit 2\n");
    Arena a;
    arena_init(&a, 0);
    cc_log_entry *v;
    size_t n;
    if (cc_repo_log(r, "HEAD", 2, &a, &v, &n, NULL)) n = 0;
    for (size_t i = 0; i < n; i++)
        buf_puts(out, v[i].hash.h), buf_putc(out, ' '), buf_puts(out, v[i].c.message), buf_putc(out, '\n');
    arena_free(&a);
}

static void changes_text(cc_changes *c, Buf *out) {
    for (size_t i = 0; i < c->n; i++) cc_change_display(&c->v[i], out), buf_putc(out, '\n');
}

/* Replaces each `from` in `b` with `to`. */
static void replace_all(Buf *b, const char *from, const char *to) {
    Buf o;
    buf_init(&o);
    size_t fl = strlen(from);
    for (size_t i = 0; i < b->len;) {
        if (i + fl <= b->len && !memcmp(b->p + i, from, fl)) buf_puts(&o, to), i += fl;
        else
            buf_putc(&o, b->p[i++]);
    }
    buf_free(b);
    *b = o;
}

/* ---------------------------------------------------------------- the goldens */

static cc_hash commits[6]; /* 1..6 as the dumper numbers them */

static void goldens(void) {
    cc_repo r;
    Buf e, out;
    buf_init(&e), buf_init(&out);
    if (cc_repo_open_dir(&r, &WP, DIR, &e)) printf("open: %s\n", buf_cstr(&e)), exit(2);

    /* branches.txt */
    cc_repo_head_t h;
    check_true("head", !cc_repo_head(&r, &h, &e) && !h.detached);
    buf_puts(&out, "HEAD ref: "), buf_puts(&out, h.branch), buf_putc(&out, '\n');
    Arena a;
    arena_init(&a, 0);
    cc_branch *br;
    size_t nb;
    check_true("branches", !cc_repo_branches(&r, &a, &br, &nb, &e));
    for (size_t i = 0; i < nb; i++) buf_puts(&out, br[i].name), buf_putc(&out, ' '), buf_puts(&out, br[i].commit.h), buf_putc(&out, '\n');
    check_text("branches.txt", &out, "branches.txt");
    arena_free(&a);

    /* log.txt */
    buf_clear(&out);
    log_lines(&r, &out);
    check_text("log.txt", &out, "log.txt");

    /* resolve.txt: the revisions are the golden's, each line's text before " -> " / " error ". */
    size_t gn;
    char *g = golden_text("resolve.txt", &gn);
    buf_clear(&out);
    for (char *line = g; *line;) {
        char *nl = strchr(line, '\n');
        if (!nl) break;
        *nl = 0;
        char *arrow = strstr(line, " -> "), *er = strstr(line, " error ");
        char *cut = arrow && (!er || arrow < er) ? arrow : er;
        if (cut) {
            char rev[256];
            snprintf(rev, sizeof rev, "%.*s", (int)(cut - line), line);
            cc_hash x;
            buf_puts(&out, rev);
            if (!cc_repo_resolve(&r, rev, &x, &e)) buf_puts(&out, " -> "), buf_puts(&out, x.h);
            else
                buf_puts(&out, " error "), buf_append(&out, e.p, e.len);
            buf_putc(&out, '\n');
        }
        *nl = '\n';
        line = nl + 1;
    }
    check_eq("resolve.txt", out.p, out.len, g, gn);
    free(g);

    /* model_at_<n>.json */
    static const char *const REVS[6] = {"HEAD~4", "HEAD~3", "HEAD~2", "HEAD~1", "variant", "HEAD"};
    cc_model models[6];
    for (int i = 0; i < 6; i++) {
        resolve_or_die(&r, REVS[i], &commits[i]);
        int rc = cc_repo_model_at(&r, commits[i].h, &models[i], &e);
        if (rc) printf("model_at %s: %s\n", REVS[i], buf_cstr(&e));
        buf_clear(&out);
        cc_model_save(&models[i], &out);
        char name[32], what[48];
        snprintf(name, sizeof name, "model_at_%d.json", i + 1);
        snprintf(what, sizeof what, "%s (%s)", name, REVS[i]);
        check_text(what, &out, name);
    }
    /* GEO1: the variant placed the model on the map; it survives the history (repo.rs:1080). */
    check_true("georef survives", models[4].georef.rotation_deg == 30.0 && models[4].georef.has_crs &&
                                      models[5].georef.rotation_deg == 30.0 && models[5].georef.has_crs &&
                                      !models[0].georef.has_crs);

    /* diff_<a>_<b>.txt */
    static const int PAIRS[][2] = {{1, 2}, {2, 3}, {3, 4}, {4, 6}, {3, 5}, {5, 6}, {1, 6}, {6, 1}, {2, 2}};
    for (size_t k = 0; k < sizeof PAIRS / sizeof *PAIRS; k++) {
        cc_changes c;
        cc_changes_init(&c);
        check_true("diff runs", !cc_diff(&models[PAIRS[k][0] - 1], &models[PAIRS[k][1] - 1], &c, &e));
        buf_clear(&out);
        changes_text(&c, &out);
        char name[32];
        snprintf(name, sizeof name, "diff_%d_%d.txt", PAIRS[k][0], PAIRS[k][1]);
        check_text(name, &out, name);
        cc_changes_free(&c);
    }
    /* diff_6_extra.txt: objects and arrays as values, the schedules, a sheet. */
    size_t xn;
    char *xt = golden_text("extra.json", &xn);
    cc_model extra;
    cc_error me;
    check_true("extra.json loads", !cc_model_load(&extra, xt, xn, &me));
    free(xt);
    cc_changes c;
    cc_changes_init(&c);
    check_true("diff extra", !cc_diff(&models[5], &extra, &c, &e));
    buf_clear(&out);
    changes_text(&c, &out);
    check_text("diff_6_extra.txt", &out, "diff_6_extra.txt");
    /* Its serde form: the first change of the list, values sorted as serde_json writes them. */
    Buf js;
    buf_init(&js);
    JsonW w;
    jw_init(&w, &js, 0);
    cc_version_change_json(&c.v[1], &w);
    check_true("change json", jw_finish(&w) == JW_OK);
    const char *want = "{\"change\":\"modified\",\"kind\":\"IfcRailing\",\"id\":\"3mj8$HiJ5q_3XkRMdd9B6r\","
                       "\"name\":\"Garde-corps 1\",\"fields\":[{\"path\":\"generator\",\"before\":{\"indices\":[0,2,1,0,1,3,"
                       "0,3,2,1,2,3],\"kind\":\"mesh\",\"positions\":[[0.0,0.0,0.0],[1000.0,0.0,0.0],[0.0,1000.0,0.0],"
                       "[0.0,0.0,1100.0]]},\"after\":{\"depth\":300.0,\"height\":2780.0,\"kind\":\"rect_column\","
                       "\"width\":300.0}}]}";
    check_eq("change json", js.p, js.len, want, strlen(want));
    buf_free(&js);
    cc_changes_free(&c);
    cc_model_free(&extra);

    /* status.txt: working.json against HEAD. */
    char *wt = golden_text("working.json", &xn);
    cc_model working;
    check_true("working.json loads", !cc_model_load(&working, wt, xn, &me));
    free(wt);
    cc_changes_init(&c);
    check_true("status", !cc_repo_status(&r, &working, &c, &e));
    buf_clear(&out);
    changes_text(&c, &out);
    check_text("status.txt", &out, "status.txt");
    cc_changes_free(&c);
    cc_model_free(&working);

    for (int i = 0; i < 6; i++) cc_model_free(&models[i]);
    cc_repo_close(&r);
    buf_free(&e), buf_free(&out);
}

/* errors.txt: each case as the dumper made it. */
static void errors(void) {
    Buf e, out, plain;
    buf_init(&e), buf_init(&out), buf_init(&plain);
    size_t gn;
    char *g = golden_text("errors.txt", &gn);
    /* The beam's object, from the golden's own "missing" line. */
    char beam[65] = {0};
    const char *m = strstr(g, "missing damaged history: object ");
    if (m) memcpy(beam, m + 32, 64);

    /* c2 (commits[1]) as plain JSON, byte 10 flipped. */
    char c2suf[80], beamsuf[80];
    obj_suffix(commits[1].h, c2suf);
    obj_suffix(beam, beamsuf);
    size_t zn;
    char path[256];
    snprintf(path, sizeof path, "%s/%s", DIR, c2suf);
    char *gz = slurp(".", path, &zn);
    const char *why;
    check_true("gunzip c2", !cc_gunzip((const unsigned char *)gz, zn, 1 << 20, &plain, &why));
    char *flipped = malloc(plain.len);
    memcpy(flipped, plain.p, plain.len);
    flipped[10] ^= 1;

    cc_repo r;
    Arena a;
    arena_init(&a, 0);
    cc_commit c;
    nov = 0;
    set_ov(c2suf, flipped, plain.len, CC_PF_OK);
    check_true("open", !cc_repo_open_dir(&r, &WP, DIR, &e));
    buf_puts(&out, "damaged_commit ");
    if (cc_repo_commit_info(&r, commits[1].h, &a, &c, &e)) buf_append(&out, e.p, e.len);
    buf_putc(&out, '\n');
    buf_puts(&out, "damaged_log ");
    cc_log_entry *lv;
    size_t ln;
    if (cc_repo_log(&r, "HEAD", 100, &a, &lv, &ln, &e)) buf_append(&out, e.p, e.len);
    buf_putc(&out, '\n');
    nov = 0;
    set_ov(c2suf, plain.p, plain.len, CC_PF_OK); /* plain and intact: an older history */
    buf_puts(&out, "plain_commit ");
    if (!cc_repo_commit_info(&r, commits[1].h, &a, &c, &e)) buf_puts(&out, c.message);
    buf_putc(&out, '\n');
    set_ov(beamsuf, NULL, 0, CC_PF_NOT_FOUND);
    buf_puts(&out, "missing ");
    cc_model mm;
    if (cc_repo_model_at(&r, "HEAD", &mm, &e)) buf_append(&out, e.p, e.len);
    cc_model_free(&mm);
    buf_putc(&out, '\n');
    nov = 0;
    static const char *const UNK[] = {"nope", "HEAD~9"};
    for (int i = 0; i < 2; i++) {
        cc_hash h;
        buf_puts(&out, "unknown ");
        if (cc_repo_resolve(&r, UNK[i], &h, &e)) buf_append(&out, e.p, e.len);
        buf_putc(&out, '\n');
    }
    buf_puts(&out, "not_a_hash ");
    if (cc_repo_commit_info(&r, "abc", &a, &c, &e)) buf_append(&out, e.p, e.len);
    buf_putc(&out, '\n');
    cc_repo_close(&r);
    set_ov("history/frame.bimble-history/format", "bimble-history 2\n", 17, CC_PF_OK);
    buf_puts(&out, "format ");
    if (cc_repo_open_dir(&r, &WP, DIR, &e)) {
        replace_all(&e, DIR, "<dir>");
        buf_append(&out, e.p, e.len);
    }
    cc_repo_close(&r);
    buf_putc(&out, '\n');
    nov = 0;
    buf_puts(&out, "no_repo ");
    if (cc_repo_open_dir(&r, &WP, GOLD "/none.bimble-history", &e)) {
        replace_all(&e, GOLD "/none.bimble-history", "<dir>");
        buf_append(&out, e.p, e.len);
    }
    cc_repo_close(&r);
    buf_putc(&out, '\n');
    check_eq("errors.txt", out.p, out.len, g, gn);

    /* No golden from here: the C side's own checks of untrusted input. */
    check_true("open again", !cc_repo_open_dir(&r, &WP, DIR, &e));
    /* A flipped byte inside the gzip stream: damaged (the CRC or the hash), never used. */
    gz[zn / 2] ^= 0x10;
    nov = 0;
    set_ov(c2suf, gz, zn, CC_PF_OK);
    char prefix[160];
    snprintf(prefix, sizeof prefix, "damaged history: object %s ", commits[1].h);
    int rc = cc_repo_commit_info(&r, commits[1].h, &a, &c, &e);
    check_true("gzip flip is damaged", rc && !strncmp(buf_cstr(&e), prefix, strlen(prefix)));
    /* Too large (the platform's cap). */
    nov = 0;
    set_ov(c2suf, NULL, 0, CC_PF_TOO_LARGE);
    rc = cc_repo_commit_info(&r, commits[1].h, &a, &c, &e);
    snprintf(prefix, sizeof prefix, "damaged history: object %s is too large", commits[1].h);
    check_true("too large", rc && !strcmp(buf_cstr(&e), prefix));
    /* A commit with an unknown field, stored under its true hash: deny_unknown_fields. */
    static const char odd[] = "{\"tree\":\"x\",\"parents\":[],\"author\":\"a\",\"time\":1,\"message\":\"m\",\"extra\":1}";
    unsigned char hb[32];
    cc_blake3(odd, sizeof odd - 1, hb);
    char oh[65], osuf[80];
    for (int i = 0; i < 32; i++) snprintf(oh + 2 * i, 3, "%02x", hb[i]);
    obj_suffix(oh, osuf);
    nov = 0;
    set_ov(osuf, odd, sizeof odd - 1, CC_PF_OK);
    rc = cc_repo_commit_info(&r, oh, &a, &c, &e);
    check_true("unknown field", rc && strstr(buf_cstr(&e), "unknown field `extra`"));
    /* A deep object: the reader's depth limit, an error, no crash. */
    size_t dn = 200000;
    char *deep = malloc(dn);
    for (size_t i = 0; i < dn / 2; i++) deep[i] = '[', deep[dn - 1 - i] = ']';
    cc_blake3(deep, dn, hb);
    for (int i = 0; i < 32; i++) snprintf(oh + 2 * i, 3, "%02x", hb[i]);
    obj_suffix(oh, osuf);
    nov = 0;
    set_ov(osuf, deep, dn, CC_PF_OK);
    rc = cc_repo_commit_info(&r, oh, &a, &c, &e);
    check_true("deep object", rc && strstr(buf_cstr(&e), "damaged history: object "));
    free(deep);
    /* A damaged HEAD, a damaged branch. */
    nov = 0;
    set_ov("frame.bimble-history/HEAD", "garbage\n", 8, CC_PF_OK);
    cc_hash h;
    rc = cc_repo_resolve(&r, "HEAD", &h, &e);
    check_true("HEAD damaged", rc && !strcmp(buf_cstr(&e), "HEAD is damaged"));
    nov = 0;
    set_ov("refs/heads/variant", "1234\n", 5, CC_PF_OK);
    rc = cc_repo_resolve(&r, "variant", &h, &e);
    check_true("branch damaged", rc && !strcmp(buf_cstr(&e), "branch variant is damaged"));
    nov = 0;
    set_ov("frame.bimble-history/HEAD", "ref: ../x\n", 10, CC_PF_OK);
    rc = cc_repo_resolve(&r, "HEAD", &h, &e);
    check_true("HEAD bad branch name", rc && strstr(buf_cstr(&e), "invalid branch name \"../x\""));
    nov = 0;
    /* No folders (the wasm module): branches empty, a hash prefix unknown, a branch still read. */
    no_list = 1;
    Arena b;
    arena_init(&b, 0);
    cc_branch *br;
    size_t nb = 9;
    check_true("no folders: no branches", !cc_repo_branches(&r, &b, &br, &nb, &e) && nb == 0);
    check_true("no folders: prefix unknown", cc_repo_resolve(&r, "903b", &h, &e) != 0);
    check_true("no folders: branch", !cc_repo_resolve(&r, "variant~1", &h, &e) && !strcmp(h.h, commits[2].h));
    no_list = 0;
    arena_free(&b);
    cc_repo_close(&r);

    free(g), free(gz), free(flipped);
    arena_free(&a);
    buf_free(&e), buf_free(&out), buf_free(&plain);
}

static void branch_names(void) {
    static const char *const OK[] = {"main", "variant", "a/b", "v1.2", "x_y-z"};
    static const char *const BAD[] = {"", "HEAD", ".x", "-x", "a//b", "a/", "a..b", "a b", "é", "a/.b"};
    for (size_t i = 0; i < sizeof OK / sizeof *OK; i++)
        check_true(OK[i], !cc_repo_check_branch_name(OK[i], strlen(OK[i]), NULL));
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++)
        check_true(BAD[i], cc_repo_check_branch_name(BAD[i], strlen(BAD[i]), NULL) != 0);
    char long_name[102];
    memset(long_name, 'a', 101);
    long_name[101] = 0;
    check_true("101 bytes", cc_repo_check_branch_name(long_name, 101, NULL) != 0);
    check_true("100 bytes", !cc_repo_check_branch_name(long_name, 100, NULL));
}

int main(void) {
    native = cc_platform_native();
    WP = *native;
    WP.read_file = w_read;
    WP.exists = w_exists;
    WP.list_dir = w_list;
    goldens();
    errors();
    branch_names();
    return report("repo");
}
