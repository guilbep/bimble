/* Suite repo_merge (ticket H2b): merging history branches as Rust does.
 * 1. The golden scenario (golden/history: ccore_golden history mode) replayed in C through c5,
 *    then `variant` merged into `main` by bob at T0+18000: Rust's merge commit hash and tree hash,
 *    the merged model byte for byte model_at_6.json, the C folder's objects exactly Rust's, the log
 *    and branches as Rust's. Merging again: up to date; main into variant: a fast-forward.
 * 2. merge_models against Rust's own transcript (golden/history/merge, ccore_golden's H2b block):
 *    26 cases, each Prefer (neither, ours, theirs): the conflicts (Display and id), the merged
 *    model's validation text and the merged model byte for byte. The cases port Rust's tests:
 *      disjoint      repo.rs:1148 merges_take_both_sides_and_report_conflicts (a beam, a column)
 *      same_name     repo.rs:1192-1216 (both rename one column: one conflict; Theirs takes x)
 *      field_level   repo.rs:1219 field_level_merge_keeps_both_changes (width and height kept)
 *      same_field    repo.rs:1247-1251 (one conflict naming generator.width)
 *      sheets        repo.rs:1256 sheets_are_committed_and_merged (name and paper kept)
 *      project_info  repo.rs:1295 project_information_is_committed_and_merged (key by key)
 *      views         repo.rs:1339 views_are_committed_and_merged (name and cut kept)
 *    and the rules of merge_models (repo.rs:620-861): delete vs change both ways, delete both,
 *    delete vs unchanged, the same id added the same and differently, two adds, a generator of
 *    another kind (whole conflict), fields merged with conflicts, the project name, project
 *    information conflicts, schedules (one changed, one added; both changed), the georeference
 *    (ours when both changed; theirs alone), storey and material, view and sheet conflicts, an
 *    invalid union (an element moved to a storey the other side removed), nothing changed.
 * 3. Repo::merge's rules: conflicts with Prefer neither write nothing (HEAD and objects as they
 *    were), Prefer theirs commits; the invalid union is refused ("the merged model is invalid:
 *    …") with HEAD unchanged; nothing committed yet; an unknown revision; a detached HEAD moves.
 * 4. The walk on untrusted histories: a commit naming one parent 1,000,000 times (queued once),
 *    a missing parent (an error, no crash), a parent that is not a hash, a walk limit. (A parent
 *    cycle cannot be written: a commit is named by the hash of its bytes, parents included, and
 *    every object read is checked against its name; the seen set would stop one anyway.)
 * 5. Crash safety: write_file fails on its Nth call for every N of the merge: HEAD stays at ours
 *    and reads; the merge again gives Rust's hash.
 * 6. The C sections (relations, grids; DEVIATIONS 28) merge by the same rules, per object id. */
#define _POSIX_C_SOURCE 200809L /* mkdtemp under -std=c11 */
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../host/host.h"
#include "file.h"
#include "merge.h"
#include "repo.h"
#include "repo_merge.h"
#include "repo_write.h"
#include "command.h" /* WAVE1B (B4) */
#include "diff.h"    /* WAVE1B (B4) */
#include "verbs.h"   /* WAVE1B (B4) */
#include "check.h"

#define GOLD "golden/history"
#define GDIR GOLD "/frame.bimble-history"
#define MDIR GOLD "/merge"
#define T0 1767225600LL
#define MERGE_HASH "9d1b67cbf963c7f84a75f4d315f2c195defc1d748d563256e0f7dd13af6a2f76"

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

static void model_text(const cc_model *m, Buf *b) {
    buf_clear(b);
    if (cc_model_save(m, b)) printf("save failed\n"), exit(2);
}

/* The model of `text` with every `from` replaced by `to` (the fixture's pretty JSON). */
static void edited(cc_model *out, const cc_model *m, const char *from, const char *to) {
    Buf t, r;
    buf_init(&t), buf_init(&r);
    model_text(m, &t);
    const char *p = buf_cstr(&t), *q;
    int hits = 0;
    while ((q = strstr(p, from))) {
        buf_append(&r, p, (size_t)(q - p));
        buf_puts(&r, to);
        p = q + strlen(from);
        hits++;
    }
    buf_puts(&r, p);
    if (!hits) printf("edited: %s not found\n", from), exit(2);
    cc_error e;
    if (cc_model_load(out, r.p, r.len, &e)) printf("edited %s: %s\n", to, e.msg), exit(2);
    buf_free(&t), buf_free(&r);
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
    if (n) qsort(v, n, sizeof *v, cmp_str);
    *out = v;
    return n;
}

static void free_list(char **v, size_t n) {
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

static void tree_of(cc_repo *r, const char *commit, char out[65]) {
    Arena a;
    arena_init(&a, 0);
    cc_commit c;
    out[0] = 0;
    if (!cc_repo_commit_info(r, commit, &a, &c, NULL)) snprintf(out, 65, "%s", c.tree);
    arena_free(&a);
}

static int head_at(cc_repo *r, const char *want) {
    cc_hash h;
    int found = 0;
    return !cc_repo_head_commit(r, &h, &found, NULL) && found && !strcmp(h.h, want);
}

/* The block of log.txt after `log <rev>` up to the next "log " line. */
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
    if (cc_repo_log(r, rev, 100, &a, &v, &n, NULL)) n = 0;
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
    arena_free(&a);
}

static const char *const AUTHOR[5] = {"alice@example.org", "bob@example.org", "alice@example.org", "bob@example.org",
                                      "alice@example.org"};
static const char *const MESSAGE[5] = {"first version", "rename a beam", "move a column", "add a sheet",
                                       "place on the map"};

/* golden/history's c1..c5 in C into `dir` (HEAD left on main at c4). */
static void replay(const cc_platform *pf, const char *dir, cc_repo *r, cc_hash c[5]) {
    Buf e;
    buf_init(&e);
    if (cc_repo_init_dir(r, pf, dir, &e)) printf("init: %s\n", buf_cstr(&e)), exit(2);
    cc_model m[5];
    char name[32];
    for (int i = 0; i < 5; i++) snprintf(name, sizeof name, "model_at_%d.json", i + 1), load(&m[i], GOLD, name);
    for (int i = 0; i < 5; i++) {
        cc_model sw;
        if (i == 4 && (cc_repo_create_branch(r, "variant", c[2].h, &c[4], &e) || cc_repo_switch(r, "variant", &sw, &e)))
            printf("variant: %s\n", buf_cstr(&e)), exit(2);
        if (i == 4) cc_model_free(&sw);
        if (cc_repo_commit(r, &m[i], MESSAGE[i], AUTHOR[i], T0 + 3600 * i, &c[i], &e))
            printf("commit %d: %s\n", i + 1, buf_cstr(&e)), exit(2);
    }
    cc_model sw;
    if (cc_repo_switch(r, "main", &sw, &e)) printf("switch main: %s\n", buf_cstr(&e)), exit(2);
    cc_model_free(&sw);
    for (int i = 0; i < 5; i++) cc_model_free(&m[i]);
    buf_free(&e);
}

/* ---------------------------------------------------------------- 1. the golden merge */

static void golden_merge(void) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/frame.bimble-history", tmp);
    cc_repo r, g;
    cc_hash c[5];
    replay(native, dir, &r, c);
    Buf e, a, b;
    buf_init(&e), buf_init(&a), buf_init(&b);
    if (cc_repo_open_dir(&g, native, GDIR, &e)) printf("open golden: %s\n", buf_cstr(&e)), exit(2);

    /* merge_base of main (c4) and variant (c5): c3 */
    cc_hash base;
    int found = 0;
    check_true("merge_base(c4, c5) = c3", !cc_repo_merge_base(&r, c[3].h, c[4].h, &base, &found, &e) && found &&
                                             !strcmp(base.h, c[2].h));
    check_true("merge_base(c5, c1) = c1", !cc_repo_merge_base(&r, c[4].h, c[0].h, &base, &found, &e) && found &&
                                             !strcmp(base.h, c[0].h));

    int how = 0;
    cc_hash mc;
    cc_model m;
    cc_conflicts cf;
    cc_conflicts_init(&cf);
    int rc = cc_repo_merge(&r, "variant", CC_PREFER_NEITHER, "bob@example.org", T0 + 18000, NULL, &how, &mc, &m, &cf, &e);
    if (rc) printf("merge: %s\n", buf_cstr(&e));
    check_true("merge variant into main: a commit", !rc && how == CC_MERGED_COMMIT && cf.n == 0);
    char tc[65], tg[65];
    tree_of(&r, mc.h, tc);
    tree_of(&g, MERGE_HASH, tg);
    check_eq("merge commit hash = Rust's", mc.h, strlen(mc.h), MERGE_HASH, 64);
    check_eq("merge tree hash = Rust's", tc, strlen(tc), tg, strlen(tg));
    printf("H2B-HASH merge commit C %s Rust %s %s | tree C %s Rust %s %s\n", mc.h, MERGE_HASH,
           strcmp(mc.h, MERGE_HASH) ? "DIFFER" : "equal", tc, tg, strcmp(tc, tg) ? "DIFFER" : "equal");
    model_text(&m, &a);
    size_t n;
    char *want = slurp(GOLD, "model_at_6.json", &n);
    check_eq("merged model = model_at_6.json", a.p, a.len, want, n);
    cc_model x;
    check_true("model_at HEAD", !cc_repo_model_at(&r, "HEAD", &x, &e));
    model_text(&x, &b);
    check_eq("model_at HEAD = model_at_6.json", b.p, b.len, want, n);
    cc_model_free(&x), cc_model_free(&m);
    free(want);
    check_true("HEAD at the merge", head_at(&r, MERGE_HASH));
    log_block(&r, "HEAD", &a), golden_log_block("HEAD", &b);
    check_eq("log HEAD (C folder) = Rust's", a.p, a.len, b.p, b.len);
    char **co, **go;
    size_t nc = objects(dir, &co), ng = objects(GDIR, &go);
    int same = nc == ng && nc > 0;
    for (size_t i = 0; same && i < nc; i++) same = !strcmp(co[i], go[i]);
    check_true("the C folder's objects are Rust's, all of them", same);
    printf("H2B-OBJECTS C %zu Rust %zu %s\n", nc, ng, same ? "same names" : "DIFFER");
    free_list(co, nc), free_list(go, ng);

    /* again: up to date; main into variant: fast-forward */
    rc = cc_repo_merge(&r, "variant", CC_PREFER_NEITHER, "bob@example.org", T0 + 18001, NULL, &how, &mc, &m, &cf, &e);
    check_true("merge again: up to date", !rc && how == CC_MERGED_UP_TO_DATE && !strcmp(mc.h, MERGE_HASH));
    cc_model_free(&m);
    cc_model sw;
    check_true("switch variant", !cc_repo_switch(&r, "variant", &sw, &e));
    cc_model_free(&sw);
    rc = cc_repo_merge(&r, "main", CC_PREFER_NEITHER, "alice@example.org", T0 + 18002, NULL, &how, &mc, &m, &cf, &e);
    check_true("main into variant: fast-forward", !rc && how == CC_MERGED_FAST_FORWARD && !strcmp(mc.h, MERGE_HASH));
    model_text(&m, &a);
    want = slurp(GOLD, "model_at_6.json", &n);
    check_eq("fast-forward: the merged model", a.p, a.len, want, n);
    free(want);
    cc_model_free(&m);
    cc_hash vb;
    check_true("variant at the merge", !cc_repo_branch_commit(&r, "variant", &vb, &found, &e) && found &&
                                          !strcmp(vb.h, MERGE_HASH));
    cc_repo_head_t hd;
    check_true("HEAD still on variant", !cc_repo_head(&r, &hd, &e) && !hd.detached && !strcmp(hd.branch, "variant"));
    cc_conflicts_free(&cf);
    cc_repo_close(&r), cc_repo_close(&g);
    buf_free(&e), buf_free(&a), buf_free(&b);
}

/* ---------------------------------------------------------------- 2. Rust's transcript */

static const char *const CASES[] = {"disjoint",
                                    "same_name",
                                    "field_level",
                                    "same_field",
                                    "sheets",
                                    "project_info",
                                    "views",
                                    "delete_vs_change",
                                    "change_vs_delete",
                                    "delete_both",
                                    "delete_vs_unchanged",
                                    "add_same_id_same",
                                    "add_same_id_differ",
                                    "add_both",
                                    "generator_kind",
                                    "fields_and_conflict",
                                    "project_name",
                                    "project_info_conflict",
                                    "schedules",
                                    "schedule_conflict",
                                    "georef",
                                    "georef_theirs",
                                    "storey_material",
                                    "view_sheet_conflict",
                                    "invalid_union",
                                    "unchanged"};

static void transcript(void) {
    cc_model base;
    load(&base, MDIR, "base.json");
    int ncases = 0, nconf = 0, nsame = 0;
    for (size_t k = 0; k < sizeof CASES / sizeof CASES[0]; k++) {
        const char *name = CASES[k];
        char f[128];
        cc_model ours, theirs;
        snprintf(f, sizeof f, "%s.ours.json", name), load(&ours, MDIR, f);
        snprintf(f, sizeof f, "%s.theirs.json", name), load(&theirs, MDIR, f);
        /* the texts a merged model may equal, as the dumper names them */
        Buf seen_text[6];
        char seen_name[6][128];
        int nseen = 3;
        for (int i = 0; i < 6; i++) buf_init(&seen_text[i]);
        model_text(&base, &seen_text[0]), snprintf(seen_name[0], 128, "base.json");
        model_text(&ours, &seen_text[1]), snprintf(seen_name[1], 128, "%s.ours.json", name);
        model_text(&theirs, &seen_text[2]), snprintf(seen_name[2], 128, "%s.theirs.json", name);
        Buf t, mt;
        buf_init(&t), buf_init(&mt);
        static const char *const PN[] = {"neither", "ours", "theirs"};
        for (int p = 0; p < 3; p++) {
            cc_model m;
            cc_conflicts cf;
            cc_conflicts_init(&cf);
            Buf e;
            buf_init(&e);
            if (cc_merge_models(&base, &ours, &theirs, (cc_prefer)p, &m, &cf, &e))
                printf("merge %s %s: %s\n", name, PN[p], buf_cstr(&e));
            char line[128];
            snprintf(line, sizeof line, "prefer %s: %zu conflict(s)\n", PN[p], cf.n);
            buf_puts(&t, line);
            for (size_t i = 0; i < cf.n; i++) {
                buf_puts(&t, "conflict ");
                cc_conflict_display(&cf.v[i], &t);
                buf_puts(&t, " | ");
                buf_puts(&t, cf.v[i].has_id ? cf.v[i].id : "-");
                buf_putc(&t, '\n');
            }
            nconf += (int)cf.n;
            cc_error ve;
            if (cc_model_validate(&m, &ve)) buf_puts(&t, "invalid "), buf_puts(&t, ve.msg), buf_putc(&t, '\n');
            else
                buf_puts(&t, "valid\n");
            model_text(&m, &mt);
            int s = -1;
            for (int i = 0; i < nseen && s < 0; i++)
                if (seen_text[i].len == mt.len && !memcmp(seen_text[i].p, mt.p, mt.len)) s = i;
            if (s < 0) {
                s = nseen++;
                buf_clear(&seen_text[s]);
                buf_append(&seen_text[s], mt.p, mt.len);
                snprintf(seen_name[s], 128, "%s.%s.json", name, PN[p]);
                /* a model Rust wrote: the same bytes */
                size_t n;
                char *want = slurp(MDIR, seen_name[s], &n);
                char what[192];
                snprintf(what, sizeof what, "merge %s prefer %s: the merged model = Rust's", name, PN[p]);
                check_eq(what, mt.p, mt.len, want, n);
                free(want);
            }
            buf_puts(&t, "model "), buf_puts(&t, seen_name[s]), buf_putc(&t, '\n');
            cc_model_free(&m);
            cc_conflicts_free(&cf);
            buf_free(&e);
        }
        snprintf(f, sizeof f, "%s.txt", name);
        size_t n;
        char *want = slurp(MDIR, f, &n);
        char what[192];
        snprintf(what, sizeof what, "merge %s: conflicts, validation, model (Rust's transcript)", name);
        check_eq(what, t.p, t.len, want, n);
        if (t.len == n && !memcmp(t.p, want, n)) nsame++;
        free(want);
        ncases++;
        for (int i = 0; i < 6; i++) buf_free(&seen_text[i]);
        buf_free(&t), buf_free(&mt);
        cc_model_free(&ours), cc_model_free(&theirs);
    }
    printf("H2B-CASES %d cases x 3 prefers, %d conflicts listed, %d transcripts equal to Rust's\n", ncases, nconf, nsame);
    cc_model_free(&base);
}

/* ---------------------------------------------------------------- 3. Repo::merge's rules */

static void rules(void) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/rules.bimble-history", tmp);
    cc_repo r;
    Buf e;
    buf_init(&e);
    check_true("init", !cc_repo_init_dir(&r, native, dir, &e));
    cc_model base, x, y, m;
    cc_conflicts cf;
    cc_conflicts_init(&cf);
    int how;
    cc_hash h, c1, cx, cy;
    load(&base, MDIR, "base.json");
    check_true("nothing committed yet", cc_repo_merge(&r, "main", CC_PREFER_NEITHER, "a", 1, NULL, &how, &h, &m, &cf, &e) == -1 &&
                                            !strcmp(buf_cstr(&e), "unknown revision \"main\""));
    cc_model_free(&m);
    check_true("commit base", !cc_repo_commit(&r, &base, "base", "a", 1, &c1, &e));
    check_true("an unknown revision", cc_repo_merge(&r, "nope", CC_PREFER_NEITHER, "a", 1, NULL, &how, &h, &m, &cf, &e) == -1 &&
                                          !strcmp(buf_cstr(&e), "unknown revision \"nope\""));
    cc_model_free(&m);
    /* both rename one column (repo.rs:1192-1216) */
    check_true("branch b", !cc_repo_create_branch(&r, "b", NULL, &h, &e));
    load(&x, MDIR, "same_name.theirs.json"); /* "x" */
    load(&y, MDIR, "same_name.ours.json");   /* "y" */
    cc_model sw;
    check_true("switch b", !cc_repo_switch(&r, "b", &sw, &e));
    cc_model_free(&sw);
    check_true("commit x on b", !cc_repo_commit(&r, &x, "x", "a", 7, &cx, &e));
    check_true("switch main", !cc_repo_switch(&r, "main", &sw, &e));
    cc_model_free(&sw);
    check_true("commit y on main", !cc_repo_commit(&r, &y, "y", "a", 8, &cy, &e));
    char **o1, **o2;
    size_t n1 = objects(dir, &o1);
    int rc = cc_repo_merge(&r, "b", CC_PREFER_NEITHER, "a", 9, NULL, &how, &h, &m, &cf, &e);
    check_true("conflict: reported", !rc && how == CC_MERGED_CONFLICTS && cf.n == 1 && cf.v[0].has_id);
    size_t n2 = objects(dir, &o2);
    check_true("conflict: nothing written, HEAD unchanged", n1 == n2 && head_at(&r, cy.h));
    free_list(o1, n1), free_list(o2, n2);
    cc_model_free(&m);
    cc_conflicts_free(&cf);
    cc_conflicts_init(&cf);
    rc = cc_repo_merge(&r, "b", CC_PREFER_THEIRS, "a", 9, NULL, &how, &h, &m, &cf, &e);
    check_true("Prefer theirs: a commit", !rc && how == CC_MERGED_COMMIT && cf.n == 1);
    Buf a, b;
    buf_init(&a), buf_init(&b);
    model_text(&m, &a), model_text(&x, &b);
    check_eq("Prefer theirs: the column is x", a.p, a.len, b.p, b.len);
    cc_model_free(&m);
    Arena ar;
    arena_init(&ar, 0);
    cc_commit ci;
    check_true("the merge commit: two parents, the default message",
               !cc_repo_commit_info(&r, h.h, &ar, &ci, &e) && ci.nparents == 2 && !strcmp(ci.parents[0], cy.h) &&
                   !strcmp(ci.parents[1], cx.h) && !strcmp(ci.message, "Merge b") && head_at(&r, h.h));
    arena_free(&ar);
    cc_conflicts_free(&cf);
    /* the invalid union, refused: HEAD where it was */
    cc_model io, it;
    load(&io, MDIR, "invalid_union.ours.json");
    load(&it, MDIR, "invalid_union.theirs.json");
    cc_hash ci2, ct2, before;
    check_true("branch u", !cc_repo_create_branch(&r, "u", c1.h, &ct2, &e));
    check_true("branch v", !cc_repo_create_branch(&r, "v", c1.h, &ct2, &e));
    check_true("switch u", !cc_repo_switch(&r, "u", &sw, &e));
    cc_model_free(&sw);
    check_true("commit ours on u", !cc_repo_commit(&r, &io, "drop a storey", "a", 10, &ci2, &e));
    check_true("switch v", !cc_repo_switch(&r, "v", &sw, &e));
    cc_model_free(&sw);
    check_true("commit theirs on v", !cc_repo_commit(&r, &it, "move a column", "a", 11, &ct2, &e));
    check_true("switch u", !cc_repo_switch(&r, "u", &sw, &e));
    cc_model_free(&sw);
    before = ci2;
    cc_conflicts_init(&cf);
    rc = cc_repo_merge(&r, "v", CC_PREFER_NEITHER, "a", 12, NULL, &how, &h, &m, &cf, &e);
    size_t ntxt;
    char *tx = slurp(MDIR, "invalid_union.txt", &ntxt);
    char *inv = strstr(tx, "invalid "), *nl = inv ? strchr(inv, '\n') : NULL;
    Buf want;
    buf_init(&want);
    buf_puts(&want, "the merged model is invalid: ");
    if (inv && nl) buf_append(&want, inv + 8, (size_t)(nl - inv - 8));
    check_true("the invalid union: refused", rc == -1 && head_at(&r, before.h));
    check_eq("the invalid union: Rust's text", e.p, e.len, want.p, want.len);
    free(tx);
    buf_free(&want);
    cc_model_free(&m);
    cc_conflicts_free(&cf);
    /* a detached HEAD moves to the merge */
    check_true("detach at u", !cc_repo_switch(&r, ci2.h, &sw, &e));
    cc_model_free(&sw);
    cc_conflicts_init(&cf);
    rc = cc_repo_merge(&r, "b", CC_PREFER_OURS, "a", 13, "a message as given ", &how, &h, &m, &cf, &e);
    if (rc) printf("detached merge: %s\n", buf_cstr(&e));
    cc_repo_head_t hd;
    check_true("detached: merged, still detached at the merge",
               !rc && how == CC_MERGED_COMMIT && !cc_repo_head(&r, &hd, &e) && hd.detached && !strcmp(hd.commit.h, h.h));
    arena_init(&ar, 0);
    check_true("the message kept as given (Repo::merge does not trim)",
               !cc_repo_commit_info(&r, h.h, &ar, &ci, &e) && !strcmp(ci.message, "a message as given "));
    arena_free(&ar);
    cc_model_free(&m);
    cc_conflicts_free(&cf);
    cc_model_free(&io), cc_model_free(&it);
    cc_model_free(&base), cc_model_free(&x), cc_model_free(&y);
    buf_free(&a), buf_free(&b), buf_free(&e);
    cc_repo_close(&r);
}

/* ---------------------------------------------------------------- 4. untrusted histories */

/* A commit object written by hand (plain bytes through put_bytes). */
static void put_commit_text(cc_repo *r, const char *tree, const char *parents_json, cc_hash *out) {
    Buf c;
    buf_init(&c);
    buf_puts(&c, "{\"tree\":\""), buf_puts(&c, tree), buf_puts(&c, "\",\"parents\":["), buf_puts(&c, parents_json);
    buf_puts(&c, "],\"author\":\"m\",\"time\":1,\"message\":\"m\"}");
    if (c.oom || cc_repo_put_bytes(r, c.p, c.len, out, NULL)) printf("put commit failed\n"), exit(2);
    buf_free(&c);
}

static void walks(void) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/walk.bimble-history", tmp);
    cc_repo r;
    Buf e;
    buf_init(&e);
    check_true("init", !cc_repo_init_dir(&r, native, dir, &e));
    cc_model base;
    load(&base, MDIR, "base.json");
    cc_hash c1, tree;
    check_true("commit", !cc_repo_commit(&r, &base, "base", "a", 1, &c1, &e));
    char t[65];
    tree_of(&r, c1.h, t);
    snprintf(tree.h, 65, "%s", t);
    /* one parent named 1,000,000 times */
    Buf ps;
    buf_init(&ps);
    for (int i = 0; i < 1000000; i++) {
        if (i) buf_putc(&ps, ',');
        buf_putc(&ps, '"'), buf_puts(&ps, c1.h), buf_putc(&ps, '"');
    }
    cc_hash big;
    put_commit_text(&r, tree.h, buf_cstr(&ps), &big);
    buf_free(&ps);
    cc_hash *v;
    size_t n;
    int rc = cc_repo_ancestors(&r, big.h, CC_REPO_MAX_WALK, &v, &n, &e);
    check_true("1,000,000 parents: two ancestors", !rc && n == 2 && !strcmp(v[0].h, big.h) && !strcmp(v[1].h, c1.h));
    if (!rc) free(v);
    cc_hash mb;
    int found;
    check_true("1,000,000 parents: merge_base", !cc_repo_merge_base(&r, big.h, c1.h, &mb, &found, &e) && found &&
                                                     !strcmp(mb.h, c1.h));
    /* a missing parent: an error */
    cc_hash miss;
    put_commit_text(&r, tree.h, "\"0000000000000000000000000000000000000000000000000000000000000000\"", &miss);
    rc = cc_repo_ancestors(&r, miss.h, CC_REPO_MAX_WALK, &v, &n, &e);
    check_true("a missing parent: an error", rc == -1 && e.len > 0);
    printf("H2B-WALK missing parent: %s\n", buf_cstr(&e));
    check_true("a missing parent: merge_base fails", cc_repo_merge_base(&r, miss.h, c1.h, &mb, &found, &e) == -1);
    /* a parent that is not a hash */
    cc_hash bad;
    put_commit_text(&r, tree.h, "\"../../etc/passwd\"", &bad);
    rc = cc_repo_ancestors(&r, bad.h, CC_REPO_MAX_WALK, &v, &n, &e);
    check_true("a parent that is not a hash: an error", rc == -1 && e.len > 0);
    printf("H2B-WALK bad parent: %s\n", buf_cstr(&e));
    /* a chain of 6, a walk limited to 3; merge on such a history refuses nothing it reads */
    cc_hash chain[6];
    chain[0] = c1;
    for (int i = 1; i < 6; i++) {
        char pj[80];
        snprintf(pj, sizeof pj, "\"%s\"", chain[i - 1].h);
        put_commit_text(&r, tree.h, pj, &chain[i]);
    }
    rc = cc_repo_ancestors(&r, chain[5].h, 3, &v, &n, &e);
    check_true("a walk limit: 3 commits, newest first", !rc && n == 3 && !strcmp(v[0].h, chain[5].h) &&
                                                         !strcmp(v[2].h, chain[3].h));
    if (!rc) free(v);
    rc = cc_repo_ancestors(&r, chain[5].h, CC_REPO_MAX_WALK, &v, &n, &e);
    check_true("the whole chain", !rc && n == 6);
    if (!rc) free(v);
    cc_model_free(&base);
    cc_repo_close(&r);
    buf_free(&e);
}

/* ---------------------------------------------------------------- 5. crash safety */

static int calls, fail_at;

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
    cc_model m4;
    load(&m4, GOLD, "model_at_4.json");
    int tested = 0, writes = 0, good_all = 1, again_ok = 1;
    for (int k = 1; k < 200; k++) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s/crash%d.bimble-history", tmp, k);
        cc_repo r;
        cc_hash c[5];
        fail_at = 0, calls = 0;
        replay(&fp, dir, &r, c);
        calls = 0, fail_at = k;
        int how;
        cc_hash h;
        cc_model m;
        cc_conflicts cf;
        cc_conflicts_init(&cf);
        int rc = cc_repo_merge(&r, "variant", CC_PREFER_NEITHER, "bob@example.org", T0 + 18000, NULL, &how, &h, &m, &cf, &e);
        int nw = calls;
        fail_at = 0;
        cc_model_free(&m);
        cc_conflicts_free(&cf);
        if (!rc) {
            writes = nw;
            cc_repo_close(&r);
            rm_rf(dir);
            break;
        }
        tested++;
        cc_repo o;
        cc_model x;
        cc_model_init(&x);
        int good = !cc_repo_open_dir(&o, native, dir, &e) && head_at(&o, c[3].h) && !cc_repo_model_at(&o, "HEAD", &x, &e);
        if (good) {
            model_text(&x, &a), model_text(&m4, &b);
            good = a.len == b.len && !memcmp(a.p, b.p, a.len);
        }
        cc_model_free(&x);
        cc_repo_close(&o);
        if (!good) good_all = 0, printf("  crash at write %d: HEAD not valid (%s)\n", k, buf_cstr(&e));
        cc_conflicts_init(&cf);
        rc = cc_repo_merge(&r, "variant", CC_PREFER_NEITHER, "bob@example.org", T0 + 18000, NULL, &how, &h, &m, &cf, &e);
        if (rc || strcmp(h.h, MERGE_HASH)) again_ok = 0;
        cc_model_free(&m);
        cc_conflicts_free(&cf);
        cc_repo_close(&r);
        rm_rf(dir);
    }
    check_true("crash: every write of the merge tried", tested >= 3 && writes == tested);
    check_true("crash: HEAD stays at main's c4, which reads", good_all);
    check_true("crash: the merge again gives Rust's hash", again_ok);
    printf("H2B-CRASH the golden merge makes %d writes; a failure at each one leaves HEAD valid\n", writes);
    cc_model_free(&m4);
    buf_free(&e), buf_free(&a), buf_free(&b);
}

/* ---------------------------------------------------------------- 6. relations and grids */

static void expect_merge(const char *what, const cc_model *b, const cc_model *o, const cc_model *t, cc_prefer p,
                         const char *want_conflicts, const cc_model *want) {
    cc_model m;
    cc_conflicts cf;
    cc_conflicts_init(&cf);
    Buf e, got, x, y;
    buf_init(&e), buf_init(&got), buf_init(&x), buf_init(&y);
    int rc = cc_merge_models(b, o, t, p, &m, &cf, &e);
    if (rc) printf("%s: %s\n", what, buf_cstr(&e));
    for (size_t i = 0; i < cf.n; i++) cc_conflict_display(&cf.v[i], &got), buf_putc(&got, '\n');
    char w[256];
    snprintf(w, sizeof w, "%s: conflicts", what);
    check_eq(w, got.p ? got.p : "", got.len, want_conflicts, strlen(want_conflicts));
    model_text(&m, &x), model_text(want, &y);
    snprintf(w, sizeof w, "%s: the merged model", what);
    check_eq(w, x.p, x.len, y.p, y.len);
    cc_model_free(&m);
    cc_conflicts_free(&cf);
    buf_free(&e), buf_free(&got), buf_free(&x), buf_free(&y);
}

static void own_sections(void) {
    cc_model g, go, gt, gw, gc;
    load(&g, "golden/grids", "model.json");
    edited(&go, &g, "\"Main grid\"", "\"Grid ours\"");
    edited(&gt, &g, "\"tag\": \"2a\"", "\"tag\": \"2b\"");
    cc_model tmp1;
    edited(&tmp1, &g, "\"tag\": \"2a\"", "\"tag\": \"2b\"");
    edited(&gw, &tmp1, "\"Main grid\"", "\"Grid ours\"");
    expect_merge("grids: a name and an axis tag, both kept", &g, &go, &gt, CC_PREFER_NEITHER, "", &gw);
    edited(&gc, &g, "\"Main grid\"", "\"Grid theirs\"");
    expect_merge("grids: both renamed", &g, &go, &gc, CC_PREFER_NEITHER,
                 "IfcGrid Grid ours: name [0000000000000000000004]\n", &go);
    expect_merge("grids: both renamed, theirs", &g, &go, &gc, CC_PREFER_THEIRS,
                 "IfcGrid Grid ours: name [0000000000000000000004]\n", &gc);
    cc_model_free(&tmp1), cc_model_free(&go), cc_model_free(&gt), cc_model_free(&gw), cc_model_free(&gc);
    cc_model_free(&g);

    cc_model r, ro, rt, rw, rc2;
    load(&r, "golden/relations", "assembly.json");
    edited(&ro, &r, "4 x M16 8.8", "4 x M20 8.8");
    edited(&rt, &r, "\"Bimble_Connection/Reversible\": \"true\"", "\"Bimble_Connection/Reversible\": \"false\"");
    cc_model tmp2;
    edited(&tmp2, &r, "4 x M16 8.8", "4 x M20 8.8");
    edited(&rw, &tmp2, "\"Bimble_Connection/Reversible\": \"true\"", "\"Bimble_Connection/Reversible\": \"false\"");
    expect_merge("relations: two properties, both kept", &r, &ro, &rt, CC_PREFER_NEITHER, "", &rw);
    edited(&rc2, &r, "4 x M16 8.8", "4 x M24 8.8");
    expect_merge("relations: one property changed both sides", &r, &ro, &rc2, CC_PREFER_THEIRS,
                 "relation IfcRelConnectsWithRealizingElements: properties.Bimble_Connection/Fastener "
                 "[2RELCONNECTS0000000001]\n",
                 &rc2);
    cc_model_free(&tmp2), cc_model_free(&ro), cc_model_free(&rt), cc_model_free(&rw), cc_model_free(&rc2);
    cc_model_free(&r);
}

/* ---- WAVE1B (B4): openings in merge and diff (ADR 0077, DEVIATIONS 28) ---- */
/* A copy of `m` with the verb run on it (its Commands applied). The new element's or opening's
 * GlobalId in *made when made is not NULL. */
static void with_verb(cc_model *out, const cc_model *m, const char *verb, const char *args, cc_gid *made) {
    Buf t;
    buf_init(&t);
    model_text(m, &t);
    cc_error e;
    if (cc_model_load(out, t.p, t.len, &e)) printf("with_verb: %s\n", e.msg), exit(2);
    buf_free(&t);
    cc_cmds l;
    cc_cmds_init(&l);
    Buf err;
    buf_init(&err);
    if (cc_verb_run_ex(out, verb, args, strlen(args), 0, -1, 0, &l, &err)) printf("%s: %s\n", verb, buf_cstr(&err)), exit(2);
    for (size_t i = 0; i < l.n; i++) {
        cc_command inv;
        if (cc_command_apply(out, &l.v[i], &inv, &err)) printf("%s apply: %s\n", verb, buf_cstr(&err)), exit(2);
        if (made && l.v[i].op == CC_CMD_ADD_ELEMENT) *made = l.v[i].element.id;
        if (made && l.v[i].op == CC_CMD_SET_OPENINGS && l.v[i].element.nopenings)
            *made = l.v[i].element.openings[l.v[i].element.nopenings - 1].id;
    }
    cc_cmds_free(&l);
    buf_free(&err);
    cc_model_canonical(out);
}
static void openings_merge(void) {
    cc_model f, b, o, t, w, x, y;
    load(&f, "golden", "frame.json");
    cc_gid wall, op1, op2;
    with_verb(&b, &f, "place", "{\"kind\":\"wall\",\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\",\"a\":[0,-3000],\"b\":[6000,-3000]}", &wall);
    char a1[200], a2[200], mv[120];
    snprintf(a1, sizeof a1, "{\"host\":\"%.22s\",\"a\":[1500,-3000]}", wall.c);
    snprintf(a2, sizeof a2, "{\"host\":\"%.22s\",\"a\":[4500,-3000],\"shape\":\"round\",\"diameter\":200}", wall.c);
    snprintf(mv, sizeof mv, "{\"ids\":[\"%.22s\"],\"d\":[0,-500,0]}", wall.c);
    /* ours adds an opening, theirs moves the wall: both kept */
    with_verb(&o, &b, "opening_add", a1, &op1);
    with_verb(&t, &b, "move", mv, NULL);
    with_verb(&w, &o, "move", mv, NULL);
    expect_merge("B4 an opening added (ours), the host moved (theirs): both kept", &b, &o, &t, CC_PREFER_NEITHER, "", &w);
    cc_model_free(&t), cc_model_free(&w);
    /* ours adds an opening, theirs another one: both openings kept (by GlobalId; Rust picked the list whole) */
    with_verb(&t, &b, "opening_add", a2, &op2);
    with_verb(&w, &o, "opening_add", a2, NULL);
    /* the expected: ours' list then theirs' new one, theirs' GlobalId */
    {
        cc_element *we = (cc_element *)cc_model_element(&w, &wall);
        if (we && we->nopenings == 2) we->openings[1].id = op2;
    }
    expect_merge("B4 two openings added, one each side: both kept", &b, &o, &t, CC_PREFER_NEITHER, "", &w);
    cc_model_free(&t), cc_model_free(&w);
    /* the same opening changed on both sides: a conflict shown like the others */
    char s1[200], s2[200];
    snprintf(s1, sizeof s1, "{\"host\":\"%.22s\",\"id\":\"%.22s\",\"width\":800}", wall.c, op1.c);
    snprintf(s2, sizeof s2, "{\"host\":\"%.22s\",\"id\":\"%.22s\",\"width\":600}", wall.c, op1.c);
    with_verb(&x, &o, "opening_set", s1, NULL);
    with_verb(&y, &o, "opening_set", s2, NULL);
    char want[256];
    snprintf(want, sizeof want, "element IfcWall M1: openings.%.22s.generator.width [%.22s]\n", op1.c, wall.c);
    expect_merge("B4 the same opening resized on both sides: a conflict, ours kept", &o, &x, &y, CC_PREFER_NEITHER, want, &x);
    expect_merge("B4 the same opening resized on both sides: a conflict, theirs taken", &o, &x, &y, CC_PREFER_THEIRS, want, &y);
    /* the diff names the opening by its GlobalId */
    cc_changes ch;
    cc_changes_init(&ch);
    Buf e, d;
    buf_init(&e), buf_init(&d);
    if (!cc_diff(&b, &o, &ch, &e))
        for (size_t i = 0; i < ch.n; i++) cc_change_display(&ch.v[i], &d), buf_putc(&d, '\n');
    char key[64];
    snprintf(key, sizeof key, "openings.%.22s", op1.c);
    check_true("B4 diff: the opening added, named by its GlobalId", strstr(buf_cstr(&d), key) != NULL);
    if (!strstr(buf_cstr(&d), key)) printf("  diff: %s\n", buf_cstr(&d));
    cc_changes_free(&ch);
    buf_clear(&d);
    cc_changes_init(&ch);
    if (!cc_diff(&o, &x, &ch, &e))
        for (size_t i = 0; i < ch.n; i++) cc_change_display(&ch.v[i], &d), buf_putc(&d, '\n');
    snprintf(key, sizeof key, "openings.%.22s.generator.width", op1.c);
    check_true("B4 diff: the opening's width changed, by its GlobalId", strstr(buf_cstr(&d), key) != NULL);
    if (!strstr(buf_cstr(&d), key)) printf("  diff: %s\n", buf_cstr(&d));
    cc_changes_free(&ch);
    buf_free(&e), buf_free(&d);
    cc_model_free(&x), cc_model_free(&y), cc_model_free(&o), cc_model_free(&b), cc_model_free(&f);
}

int main(void) {
    native = cc_platform_native();
    const char *t = getenv("TMPDIR");
    snprintf(tmp, sizeof tmp, "%s/h2b-merge-XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(tmp)) printf("mkdtemp failed\n"), exit(2);
    golden_merge();
    transcript();
    rules();
    walks();
    crash();
    own_sections();
    openings_merge(); /* WAVE1B (B4) */
    if (getenv("H2B_KEEP")) printf("H2B-KEEP %s\n", tmp);
    else
        rm_rf(tmp);
    return report("repo_merge");
}
