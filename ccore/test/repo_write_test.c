/* Suite repo_write (ticket H2a): the C history store writes what Rust writes.
 * 1. Interoperability: golden/history's scenario (ccore_golden history mode: the frame committed
 *    four times on main by fixed authors at fixed times, a branch `variant` at the third commit
 *    with one commit) replayed in C from model_at_<n>.json: every commit hash and every tree hash
 *    equals the golden folder's, every object C writes is one of the golden folder's (same names),
 *    and the H1 reader gives Rust's log texts and models over the C-written folder.
 * 2. Rust's rules: nothing to commit, an empty message (Unicode white space), the message
 *    trimmed, branch names, a branch that exists, init twice, a platform without folders.
 * 3. A commit after an edit shows in log and status is clean; switch gives the other branch's
 *    model; a detached HEAD moves on commit.
 * 4. Crash safety: write_file fails on its Nth call (a half-written temporary file left behind,
 *    as a crash would) for every N of a commit and of a branch creation: the old HEAD stays valid
 *    and reads the old model; the same commit afterwards gives the same hash.
 * 5. The C model's own sections (relations, grids) round trip through a tree. */
#define _POSIX_C_SOURCE 200809L /* mkdtemp under -std=c11 */
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../host/host.h"
#include "diff.h"
#include "file.h"
#include "repo.h"
#include "repo_write.h"
#include "check.h"

#define GOLD "golden/history"
#define GDIR GOLD "/frame.bimble-history"
#define T0 1767225600LL

static const cc_platform *native;
static char tmp[256];

/* ---------------------------------------------------------------- helpers */

static void load(cc_model *m, const char *dir, const char *name) {
    size_t n;
    char *t = slurp(dir, name, &n);
    cc_error e;
    if (cc_model_load(m, t, n, &e)) printf("load %s: %s\n", name, e.msg), exit(2);
    free(t);
}

static void rm_rf(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *de;
    char sub[4096];
    while ((de = readdir(d)))
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
            snprintf(sub, sizeof sub, "%s/%s", path, de->d_name);
            rm_rf(sub);
        }
    closedir(d);
    rmdir(path);
}

/* Object names under <dir>/objects, sorted, "ab<rest>\n" each. */
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static size_t objects(const char *dir, char ***out) {
    char path[4096];
    snprintf(path, sizeof path, "%s/objects", dir);
    char **v = NULL;
    size_t n = 0;
    DIR *d = opendir(path);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (strlen(de->d_name) != 2) continue;
        char sp[4096];
        snprintf(sp, sizeof sp, "%s/%s", path, de->d_name);
        DIR *s = opendir(sp);
        struct dirent *fe;
        while (s && (fe = readdir(s))) {
            if (strlen(fe->d_name) != 62) continue;
            v = realloc(v, (n + 1) * sizeof *v);
            v[n] = malloc(65);
            snprintf(v[n], 65, "%s%s", de->d_name, fe->d_name);
            n++;
        }
        if (s) closedir(s);
    }
    if (d) closedir(d);
    qsort(v, n, sizeof *v, cmp_str);
    *out = v;
    return n;
}

static void model_text(const cc_model *m, Buf *b) {
    buf_clear(b);
    if (cc_model_save(m, b)) printf("save failed\n"), exit(2);
}

/* The golden commit hashes (log.txt "log variant" and "log main~1" blocks), oldest first. */
static const char *const GC[5] = {
    "19e04b09662ba46c4dddf3909ff0b2b6e76246dbf5b853cf31eddf68185cf07e", /* first version */
    "903b5b5fc1b762fe8ed0537a818193b18aaf4ab5663297dfcbe86a8564eb4192", /* rename a beam */
    "c904ef487e159537246bfb3e7d1cbfb4751aa43363b630b993adcd36f40f9342", /* move a column */
    "c1453cc2e32eddc5ff357bbac51950c89225fd97434fa5ef2ee3c16cadea5a8c", /* add a sheet */
    "2245c01674708e2be11f854aa84199d6ac08a807108eb66757519308373f9ab7", /* place on the map (variant) */
};

/* The block of log.txt after the line `log <rev>` up to the next "log " line. */
static void golden_log_block(const char *rev, Buf *out) {
    size_t n;
    char *g = slurp(GOLD, "log.txt", &n);
    char head[64];
    snprintf(head, sizeof head, "log %s\n", rev);
    char *p = strstr(g, head);
    buf_clear(out);
    if (p) {
        p += strlen(head);
        char *end = strstr(p, "\nlog ");
        buf_append(out, p, end ? (size_t)(end + 1 - p) : strlen(p));
    }
    free(g);
}

static void log_block(cc_repo *r, const char *rev, Buf *out) {
    buf_clear(out);
    Arena a;
    arena_init(&a, 0);
    cc_log_entry *v;
    size_t n = 0;
    Buf e;
    buf_init(&e);
    if (cc_repo_log(r, rev, 100, &a, &v, &n, &e)) printf("log %s: %s\n", rev, buf_cstr(&e)), n = 0;
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

static void tree_of(cc_repo *r, const char *commit, char out[65]) {
    Arena a;
    arena_init(&a, 0);
    cc_commit c;
    out[0] = 0;
    if (!cc_repo_commit_info(r, commit, &a, &c, NULL)) snprintf(out, 65, "%s", c.tree);
    arena_free(&a);
}

static void head_is(cc_repo *r, const char *what, const char *want) {
    cc_hash h;
    int found = 0;
    Buf e;
    buf_init(&e);
    int rc = cc_repo_head_commit(r, &h, &found, &e);
    check_true(what, !rc && found && !strcmp(h.h, want));
    if (rc || !found || strcmp(h.h, want)) printf("  head: %s %s\n", found ? h.h : "-", buf_cstr(&e));
    buf_free(&e);
}

/* H2A_KEEP set: the folders are kept, and C's answers printed in the form of the Rust reader
 * (oracle/h2a crates/bimble-core/examples/h2a_read.rs) to compare with it. */
static void dump(const char *dir) {
    if (!getenv("H2A_KEEP")) return;
    cc_repo r;
    Buf e, o;
    buf_init(&e), buf_init(&o);
    if (cc_repo_open_dir(&r, native, dir, &e)) return;
    printf("H2A-DUMP %s\n", dir);
    cc_repo_head_t h;
    if (!cc_repo_head(&r, &h, &e)) printf(h.detached ? "HEAD %s\n" : "HEAD ref: %s\n", h.detached ? h.commit.h : h.branch);
    Arena a;
    arena_init(&a, 0);
    cc_branch *br;
    size_t nb = 0;
    cc_repo_branches(&r, &a, &br, &nb, &e);
    for (size_t i = 0; i < nb; i++) printf("%s %s\n", br[i].name, br[i].commit.h);
    for (size_t i = 0; i <= nb; i++) {
        const char *rev = i < nb ? br[i].name : h.detached ? h.commit.h : NULL;
        if (!rev) break;
        printf("log %s\n", rev);
        Arena la;
        arena_init(&la, 0);
        cc_log_entry *v;
        size_t n = 0;
        cc_repo_log(&r, rev, 100, &la, &v, &n, &e);
        for (size_t k = 0; k < n; k++) {
            log_block(&r, v[k].hash.h, &o);
            char *nl = strchr(o.p, '\n');
            printf("%.*s\n", nl ? (int)(nl - o.p) : (int)o.len, o.p);
            cc_model m;
            if (!cc_repo_model_at(&r, v[k].hash.h, &m, &e)) printf("  model_at ok: %zu elements\n", m.nelements);
            else
                printf("  model_at error %s\n", buf_cstr(&e));
            cc_model_free(&m);
        }
        arena_free(&la);
    }
    arena_free(&a);
    cc_repo_close(&r);
    buf_free(&e), buf_free(&o);
}

/* ---------------------------------------------------------------- 1. interoperability */

static const char *const AUTHOR[5] = {"alice@example.org", "bob@example.org", "alice@example.org", "bob@example.org",
                                      "alice@example.org"};
static const char *const MESSAGE[5] = {"first version", "rename a beam", "move a column", "add a sheet",
                                       "place on the map"};

static void interop(void) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/frame.bimble-history", tmp);
    cc_repo r, g;
    Buf e, a, b;
    buf_init(&e), buf_init(&a), buf_init(&b);
    if (cc_repo_init_dir(&r, native, dir, &e)) printf("init: %s\n", buf_cstr(&e)), exit(2);
    if (cc_repo_open_dir(&g, native, GDIR, &e)) printf("open golden: %s\n", buf_cstr(&e)), exit(2);
    cc_model m[5];
    char name[32];
    for (int i = 0; i < 5; i++) snprintf(name, sizeof name, "model_at_%d.json", i + 1), load(&m[i], GOLD, name);
    cc_hash c[5];
    printf("commit  tree (C)                                                          tree (Rust)  commit equal\n");
    for (int i = 0; i < 5; i++) {
        if (i == 4) { /* the variant, from the moved column */
            cc_hash vb;
            check_true("create_branch variant", !cc_repo_create_branch(&r, "variant", c[2].h, &vb, &e));
            check_true("variant at c3", !strcmp(vb.h, c[2].h));
            cc_model sw;
            check_true("switch variant", !cc_repo_switch(&r, "variant", &sw, &e));
            model_text(&sw, &a), model_text(&m[2], &b);
            check_eq("switch variant: the model of c3", a.p, a.len, b.p, b.len);
            cc_model_free(&sw);
        }
        int rc = cc_repo_commit(&r, &m[i], MESSAGE[i], AUTHOR[i], T0 + 3600 * i, &c[i], &e);
        if (rc) printf("commit %d: %s\n", i + 1, buf_cstr(&e)), exit(2);
        char tc[65], tg[65];
        tree_of(&r, c[i].h, tc);
        tree_of(&g, GC[i], tg);
        char what[64];
        snprintf(what, sizeof what, "commit %d hash", i + 1);
        check_eq(what, c[i].h, 64, GC[i], 64);
        snprintf(what, sizeof what, "commit %d tree", i + 1);
        check_eq(what, tc, strlen(tc), tg, strlen(tg));
        printf("H2A-HASH c%d commit C %s Rust %s %s | tree C %s Rust %s %s\n", i + 1, c[i].h, GC[i],
               strcmp(c[i].h, GC[i]) ? "DIFFER" : "equal", tc, tg, strcmp(tc, tg) ? "DIFFER" : "equal");
    }
    cc_model sw;
    check_true("switch main", !cc_repo_switch(&r, "main", &sw, &e));
    model_text(&sw, &a), model_text(&m[3], &b);
    check_eq("switch main: the model of c4", a.p, a.len, b.p, b.len);
    cc_model_free(&sw);

    /* Every object C wrote is one of Rust's, by name (gzip bytes may differ, names may not). */
    char **co, **go;
    size_t nc = objects(dir, &co), ng = objects(GDIR, &go);
    size_t missing = 0;
    for (size_t i = 0, j = 0; i < nc; i++) {
        while (j < ng && strcmp(go[j], co[i]) < 0) j++;
        if (j >= ng || strcmp(go[j], co[i])) {
            if (missing++ < 5) printf("  C object not in the golden folder: %s\n", co[i]);
        }
    }
    check_true("every C object is a golden object", missing == 0 && nc > 0);
    /* Rust's folder has the merge on top: its commit and its tree only. */
    check_true("golden has 2 objects more (the merge commit and tree)", ng == nc + 2);
    printf("H2A-OBJECTS C %zu Rust %zu (Rust also holds the merge commit and its tree)\n", nc, ng);
    for (size_t i = 0; i < nc; i++) free(co[i]);
    for (size_t i = 0; i < ng; i++) free(go[i]);
    free(co), free(go);

    /* The H1 reader over the C folder gives Rust's texts: main is c4 here (Rust's main~1). */
    log_block(&r, "HEAD", &a), golden_log_block("main~1", &b);
    check_eq("log HEAD (C folder) = Rust's log main~1", a.p, a.len, b.p, b.len);
    log_block(&r, "variant", &a), golden_log_block("variant", &b);
    check_eq("log variant (C folder)", a.p, a.len, b.p, b.len);
    log_block(&r, "HEAD~1", &a), golden_log_block("HEAD~2", &b);
    check_eq("log HEAD~1 (C folder) = Rust's HEAD~2", a.p, a.len, b.p, b.len);
    Arena ar;
    arena_init(&ar, 0);
    cc_branch *br;
    size_t nb = 0;
    check_true("branches", !cc_repo_branches(&r, &ar, &br, &nb, &e) && nb == 2 && !strcmp(br[0].name, "main") &&
                               !strcmp(br[0].commit.h, GC[3]) && !strcmp(br[1].name, "variant") &&
                               !strcmp(br[1].commit.h, GC[4]));
    arena_free(&ar);
    for (int i = 0; i < 5; i++) {
        cc_model x;
        snprintf(name, sizeof name, "model_at_%d.json", i + 1);
        int rc = cc_repo_model_at(&r, c[i].h, &x, &e);
        model_text(&x, &a);
        size_t n;
        char *want = slurp(GOLD, name, &n);
        char what[64];
        snprintf(what, sizeof what, "model_at %d (C folder)", i + 1);
        check_true(what, !rc);
        check_eq(what, a.p, a.len, want, n);
        free(want);
        cc_model_free(&x);
    }
    cc_changes ch;
    cc_changes_init(&ch);
    check_true("status clean after the commits", !cc_repo_status(&r, &m[3], &ch, &e) && ch.n == 0);
    cc_changes_free(&ch);

    /* 2. nothing to commit, as Rust */
    cc_hash x;
    int rc = cc_repo_commit(&r, &m[3], "again", "bob@example.org", T0, &x, &e);
    check_true("nothing to commit: CC_REPO_NOTHING", rc == CC_REPO_NOTHING);
    check_eq("nothing to commit: the text", e.p, e.len, "nothing to commit: the model is the same as c1453cc2e32e",
             strlen("nothing to commit: the model is the same as c1453cc2e32e"));
    head_is(&r, "nothing to commit: HEAD unchanged", GC[3]);

    for (int i = 0; i < 5; i++) cc_model_free(&m[i]);
    cc_repo_close(&r), cc_repo_close(&g);
    dump(dir);
    buf_free(&e), buf_free(&a), buf_free(&b);
}

/* ---------------------------------------------------------------- 2-3. rules and flows */

static void rename_first_element(cc_model *m, const char *name) {
    cc_element *el = &m->elements[0];
    el->name.p = name, el->name.n = strlen(name);
}

static void rules(void) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/rules.bimble-history", tmp);
    cc_repo r, r2;
    Buf e, a, b;
    buf_init(&e), buf_init(&a), buf_init(&b);
    check_true("init", !cc_repo_init_dir(&r, native, dir, &e));
    check_true("init twice refused", cc_repo_init_dir(&r2, native, dir, &e) != 0);
    char want[600];
    snprintf(want, sizeof want, "%s already has a history", dir);
    check_eq("init twice: the text", e.p, e.len, want, strlen(want));
    cc_repo_close(&r2);
    {
        cc_platform nofolders = *native;
        nofolders.make_dir = NULL;
        char d2[512];
        snprintf(d2, sizeof d2, "%s/none.bimble-history", tmp);
        check_true("no folders: init refused", cc_repo_init_dir(&r2, &nofolders, d2, &e) != 0);
        check_eq("no folders: the text", e.p, e.len, "io: no history folder here", 26);
        cc_repo_close(&r2);
    }
    /* HEAD before the first commit: no commit, the branch main */
    cc_hash h;
    int found = 1;
    check_true("new history: no head commit", !cc_repo_head_commit(&r, &h, &found, &e) && !found);
    cc_model m;
    load(&m, GOLD, "model_at_1.json");
    /* messages */
    check_true("empty message refused", cc_repo_commit(&r, &m, "", "a", 0, &h, &e) == -1);
    check_eq("empty message: the text", e.p, e.len, "a commit needs a message", 24);
    check_true("white space only refused", cc_repo_commit(&r, &m, " \t\n\xe3\x80\x80\xc2\xa0 ", "a", 0, &h, &e) == -1);
    check_true("no commit after refusals", !cc_repo_head_commit(&r, &h, &found, &e) && !found);
    cc_hash c1;
    check_true("first commit", !cc_repo_commit(&r, &m, " \xe2\x80\x83 first\n ", "Ann <ann@example.org>", 7, &c1, &e));
    Arena ar;
    arena_init(&ar, 0);
    cc_commit ci;
    check_true("commit info", !cc_repo_commit_info(&r, c1.h, &ar, &ci, &e));
    check_true("message trimmed (Unicode white space)", !strcmp(ci.message, "first"));
    check_true("author kept", !strcmp(ci.author, "Ann <ann@example.org>") && ci.time == 7 && ci.nparents == 0);
    arena_free(&ar);
    /* an edit, a commit: first in log, status clean */
    cc_model m2;
    load(&m2, GOLD, "model_at_1.json");
    rename_first_element(&m2, "Edited \"name\" \xc3\xa9");
    cc_changes ch;
    cc_changes_init(&ch);
    check_true("status: one change before the commit", !cc_repo_status(&r, &m2, &ch, &e) && ch.n == 1);
    cc_changes_free(&ch);
    cc_hash c2;
    check_true("commit after an edit", !cc_repo_commit(&r, &m2, "edit", "Ann", 8, &c2, &e));
    arena_init(&ar, 0);
    cc_log_entry *v;
    size_t n = 0;
    check_true("log: the new commit first", !cc_repo_log(&r, "HEAD", 10, &ar, &v, &n, &e) && n == 2 &&
                                                 !strcmp(v[0].hash.h, c2.h) && !strcmp(v[0].c.message, "edit") &&
                                                 v[0].c.nparents == 1 && !strcmp(v[0].c.parents[0], c1.h));
    arena_free(&ar);
    cc_changes_init(&ch);
    check_true("status clean after the commit", !cc_repo_status(&r, &m2, &ch, &e) && ch.n == 0);
    cc_changes_free(&ch);
    /* branch names, a branch that exists */
    static const char *const BAD[] = {"", "HEAD", "-x", ".x", "a..b", "a//b", "a/", "sp ace", "\xc3\xa9t\xc3\xa9"};
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        char what[64];
        snprintf(what, sizeof what, "bad branch name %zu refused", i);
        check_true(what, cc_repo_create_branch(&r, BAD[i], NULL, &h, &e) == -1 &&
                             !strncmp(e.p, "invalid branch name ", 20));
    }
    check_true("branch main exists", cc_repo_create_branch(&r, "main", NULL, &h, &e) == -1);
    check_eq("branch exists: the text", e.p, e.len, "branch main already exists", 26);
    check_true("unknown revision", cc_repo_create_branch(&r, "x", "nope", &h, &e) == -1 &&
                                       !strcmp(buf_cstr(&e), "unknown revision \"nope\""));
    cc_hash fb;
    check_true("branch with a folder", !cc_repo_create_branch(&r, "team/idea-1.b", c1.h, &fb, &e) && !strcmp(fb.h, c1.h));
    /* switch: the other branch's model; back */
    cc_model sw;
    check_true("switch team/idea-1.b", !cc_repo_switch(&r, "team/idea-1.b", &sw, &e));
    model_text(&sw, &a), model_text(&m, &b);
    check_eq("switch: the branch's model", a.p, a.len, b.p, b.len);
    cc_model_free(&sw);
    cc_repo_head_t hd;
    check_true("HEAD on the branch", !cc_repo_head(&r, &hd, &e) && !hd.detached && !strcmp(hd.branch, "team/idea-1.b"));
    cc_hash c3;
    cc_model m3;
    load(&m3, GOLD, "model_at_1.json");
    rename_first_element(&m3, "On the branch");
    check_true("commit on the branch", !cc_repo_commit(&r, &m3, "branch edit", "Ann", 9, &c3, &e));
    check_true("switch main", !cc_repo_switch(&r, "main", &sw, &e));
    model_text(&sw, &a), model_text(&m2, &b);
    check_eq("switch back: main's model", a.p, a.len, b.p, b.len);
    cc_model_free(&sw);
    head_is(&r, "main's head", c2.h);
    /* switch to a commit: detached; a commit moves the detached HEAD */
    check_true("switch to a commit", !cc_repo_switch(&r, c1.h, &sw, &e));
    cc_model_free(&sw);
    check_true("detached", !cc_repo_head(&r, &hd, &e) && hd.detached && !strcmp(hd.commit.h, c1.h));
    cc_hash c4;
    cc_model m4;
    load(&m4, GOLD, "model_at_1.json");
    rename_first_element(&m4, "Detached");
    check_true("commit detached", !cc_repo_commit(&r, &m4, "detached", "Ann", 10, &c4, &e));
    check_true("still detached, moved", !cc_repo_head(&r, &hd, &e) && hd.detached && !strcmp(hd.commit.h, c4.h));
    cc_hash mb;
    int f2;
    check_true("main unmoved", !cc_repo_branch_commit(&r, "main", &mb, &f2, &e) && f2 && !strcmp(mb.h, c2.h));
    /* a damaged target: switch refused, HEAD where it was */
    check_true("switch to an unknown revision refused", cc_repo_switch(&r, "zzzz", &sw, &e) == -1);
    cc_model_free(&sw);
    check_true("HEAD unchanged after a refused switch", !cc_repo_head(&r, &hd, &e) && hd.detached && !strcmp(hd.commit.h, c4.h));
    cc_model_free(&m), cc_model_free(&m2), cc_model_free(&m3), cc_model_free(&m4);
    cc_repo_close(&r);
    dump(dir);
    buf_free(&e), buf_free(&a), buf_free(&b);
}

/* ---------------------------------------------------------------- 4. crash safety */

static int calls, fail_at; /* write_file's calls; fails on call number fail_at (0: never) */

static int w_fail(void *ctx, const char *path, const char *p, size_t n) {
    (void)ctx;
    if (++calls == fail_at) { /* a crash during the write: half the bytes in the temporary file */
        char t[4096];
        snprintf(t, sizeof t, "%s.tmp", path);
        FILE *f = fopen(t, "wb");
        if (f) fwrite(p, 1, n / 2, f), fclose(f);
        return -1;
    }
    return native->write_file(native->ctx, path, p, n);
}

static void crash(void) {
    cc_platform fp = *native;
    fp.write_file = w_fail;
    Buf e, a, b;
    buf_init(&e), buf_init(&a), buf_init(&b);
    cc_model m1, m2;
    load(&m1, GOLD, "model_at_4.json");
    load(&m2, GOLD, "model_at_5.json"); /* new elements, two mesh arrays: many objects */
    int ncalls = 0, tested = 0, ok_after = 1;
    for (int k = 1;; k++) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s/crash%d.bimble-history", tmp, k);
        cc_repo r;
        fail_at = 0, calls = 0;
        if (cc_repo_init_dir(&r, &fp, dir, &e)) printf("crash init: %s\n", buf_cstr(&e)), exit(2);
        cc_hash c1, c2, again;
        if (cc_repo_commit(&r, &m1, "add a sheet", "bob@example.org", T0, &c1, &e)) printf("crash c1: %s\n", buf_cstr(&e)), exit(2);
        calls = 0, fail_at = k;
        int rc = cc_repo_commit(&r, &m2, "place on the map", "alice@example.org", T0 + 1, &c2, &e);
        int n = calls;
        fail_at = 0;
        if (!rc) { /* k is past the last write: done */
            ncalls = n;
            cc_repo_close(&r);
            rm_rf(dir);
            break;
        }
        tested++;
        /* A fresh reader: HEAD is the old commit, its model reads, the log is whole. */
        cc_repo o;
        int open_ok = !cc_repo_open_dir(&o, native, dir, &e);
        cc_hash h;
        int found = 0;
        cc_model x;
        cc_model_init(&x);
        int good = open_ok && !cc_repo_head_commit(&o, &h, &found, &e) && found && !strcmp(h.h, c1.h) &&
                   !cc_repo_model_at(&o, "HEAD", &x, &e);
        if (good) {
            model_text(&x, &a), model_text(&m1, &b);
            good = a.len == b.len && !memcmp(a.p, b.p, a.len);
        }
        cc_model_free(&x);
        if (!good) printf("  crash at write %d: old HEAD not valid (%s)\n", k, buf_cstr(&e));
        check_true("crash: the old HEAD stays valid", good);
        /* The same commit again, nothing failing: Rust's hash for it. */
        if (cc_repo_commit(&r, &m2, "place on the map", "alice@example.org", T0 + 1, &again, &e)) ok_after = 0;
        else {
            cc_repo r3;
            char d3[512];
            snprintf(d3, sizeof d3, "%s/clean.bimble-history", tmp);
            static cc_hash clean;
            static int have;
            if (!have) {
                cc_hash t1;
                if (cc_repo_init_dir(&r3, native, d3, &e) ||
                    cc_repo_commit(&r3, &m1, "add a sheet", "bob@example.org", T0, &t1, &e) ||
                    cc_repo_commit(&r3, &m2, "place on the map", "alice@example.org", T0 + 1, &clean, &e))
                    printf("clean: %s\n", buf_cstr(&e)), exit(2);
                cc_repo_close(&r3);
                have = 1;
            }
            if (strcmp(again.h, clean.h)) ok_after = 0;
        }
        if (open_ok) cc_repo_close(&o);
        cc_repo_close(&r);
        rm_rf(dir);
        if (k > 500) break;
    }
    check_true("crash: every write of a commit tried", tested >= 5 && ncalls == tested);
    check_true("crash: the commit again gives the same hash", ok_after);
    printf("H2A-CRASH a commit of model_at_5 makes %d writes; a failure at each one leaves HEAD valid\n", ncalls);

    /* create_branch and switch: one write each (the ref, HEAD), so failing it leaves the old ref. */
    char dir[512];
    snprintf(dir, sizeof dir, "%s/crashb.bimble-history", tmp);
    cc_repo r;
    cc_hash c1, hb;
    fail_at = 0;
    if (cc_repo_init_dir(&r, &fp, dir, &e) || cc_repo_commit(&r, &m1, "x", "a", 1, &c1, &e)) exit(2);
    calls = 0, fail_at = 1;
    check_true("crash: create_branch fails", cc_repo_create_branch(&r, "b", NULL, &hb, &e) == -1);
    fail_at = 0;
    int found = 1;
    check_true("crash: no branch b", !cc_repo_branch_commit(&r, "b", &hb, &found, &e) && !found);
    check_true("create_branch", !cc_repo_create_branch(&r, "b", NULL, &hb, &e));
    cc_model sw;
    calls = 0, fail_at = 1;
    check_true("crash: switch fails", cc_repo_switch(&r, "b", &sw, &e) == -1);
    cc_model_free(&sw);
    fail_at = 0;
    cc_repo_head_t hd;
    check_true("crash: HEAD still main", !cc_repo_head(&r, &hd, &e) && !hd.detached && !strcmp(hd.branch, "main"));
    cc_repo_close(&r);
    cc_model_free(&m1), cc_model_free(&m2);
    buf_free(&e), buf_free(&a), buf_free(&b);
}

/* ---------------------------------------------------------------- 5. relations and grids */

static void own_sections(void) {
    static const char *const F[][2] = {{"golden/grids", "model.json"}, {"golden/relations", "assembly.json"}};
    for (int k = 0; k < 2; k++) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s/own%d.bimble-history", tmp, k);
        cc_repo r;
        Buf e, a, b;
        buf_init(&e), buf_init(&a), buf_init(&b);
        cc_model m, x;
        load(&m, F[k][0], F[k][1]);
        check_true(k ? "the fixture has relations" : "the fixture has grids", k ? m.nrelations > 0 : m.ngrids > 0);
        cc_hash c;
        check_true("init", !cc_repo_init_dir(&r, native, dir, &e));
        int rc = cc_repo_commit(&r, &m, "own", "a", 1, &c, &e);
        if (rc) printf("own commit: %s\n", buf_cstr(&e));
        check_true("commit", !rc);
        rc = cc_repo_model_at(&r, "HEAD", &x, &e);
        if (rc) printf("own read: %s\n", buf_cstr(&e));
        model_text(&x, &a), model_text(&m, &b);
        check_eq(k ? "relations round trip" : "grids round trip", a.p, a.len, b.p, b.len);
        cc_model_free(&m), cc_model_free(&x);
        cc_repo_close(&r);
        buf_free(&e), buf_free(&a), buf_free(&b);
    }
}

int main(void) {
    native = cc_platform_native();
    const char *t = getenv("TMPDIR");
#ifdef _WIN32
    /* A native Windows program: /tmp is MSYS2's path, not the system's. %TEMP%, else the suite's folder. */
    if (!t || !*t) t = getenv("TEMP");
    if (!t || !*t) t = ".";
#endif
    snprintf(tmp, sizeof tmp, "%s/h2a-repo-XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(tmp)) printf("mkdtemp failed\n"), exit(2);
    interop();
    rules();
    crash();
    own_sections();
    if (getenv("H2A_KEEP")) printf("H2A-KEEP %s\n", tmp);
    else
        rm_rf(tmp);
    return report("repo_write");
}
