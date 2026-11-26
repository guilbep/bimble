/* The history queries (versions.h; ticket H1). */
#include "versions.h"
#include <string.h>
#include "diff.h"
#include "json_r.h"
#include "json_w.h"
#include "repo.h"
#include "repo_write.h" /* H2a */
#include "repo_merge.h" /* H2b */
#include <stdlib.h>
#include "file.h"
#include "settings.h"

#define NO_BROWSER "no history folder in the browser"

#ifdef CC_VIEW_ONLY
/* The viewer-only wasm (build.sh VIEW_DENY): without repo.c and diff.c, no folders anyway. */
int cc_versions_query(cc_session *s, const char *name, const char *body, size_t n, cc_buf *out) {
    (void)s, (void)body, (void)n;
    if (strcmp(name, "versions") && strcmp(name, "branches") && strcmp(name, "version_changes") &&
        strcmp(name, "pending"))
        return -1;
    buf_clear(out);
    buf_puts(out, "{\"history\":false,\"reason\":\"" NO_BROWSER "\"}");
    return CC_OK;
}
int cc_versions_action(cc_session *s, const char *name, const char *body, size_t n, cc_buf *out) {
    (void)s, (void)body, (void)n;
    if (strcmp(name, "commit") && strcmp(name, "branch_create") && strcmp(name, "branch_switch") &&
        strcmp(name, "merge") /* H2b */)
        return -1;
    buf_clear(out);
    buf_puts(out, NO_BROWSER);
    return CC_ERR;
}
void cc_versions_free(cc_session *s) { (void)s; }
#else

/* {"history": false, "reason": ...}. */
static int none(cc_buf *out, const char *reason, size_t n) {
    buf_clear(out);
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "history");
    jw_bool(&w, 0);
    jw_key(&w, "reason");
    jw_str_n(&w, reason, n);
    jw_obj_end(&w);
    return jw_finish(&w) == JW_OK ? CC_OK : CC_ERR;
}

static int fail(cc_buf *out, const Buf *err) {
    buf_clear(out);
    buf_append(out, err->p, err->len);
    return CC_ERR;
}

/* Opens the history of the session's model. 0 ok; else the answer is written and returned. */
static int open_repo(cc_session *s, cc_repo *r, cc_buf *out, int *answer) {
    r->dir = NULL;
    if (!s->pf->files) return *answer = none(out, NO_BROWSER, strlen(NO_BROWSER)), -1;
    if (!s->open && !s->view) return *answer = none(out, "no model open", 13), -1;
    if (s->untitled || !s->path[0]) return *answer = none(out, "the model has no file yet", 25), -1;
    Buf e;
    buf_init(&e);
    if (cc_repo_open(r, s->pf, s->path, &e)) {
        /* No folder next to the file: no history (a 200 the page shows). A folder that is there
         * but unreadable: 400. */
        Buf d;
        buf_init(&d);
        cc_repo_history_dir(s->path, &d);
        buf_puts(&d, "/format");
        int there = s->pf->exists(s->pf->ctx, buf_cstr(&d));
        buf_free(&d);
        *answer = there ? fail(out, &e) : none(out, e.p, e.len);
        buf_free(&e);
        cc_repo_close(r);
        return -1;
    }
    buf_free(&e);
    return 0;
}

static void head_json(const cc_repo *r, JsonW *w, Buf *err, int *bad) {
    cc_repo_head_t h;
    cc_hash c;
    int found = 0;
    jw_key(w, "head");
    jw_obj_begin(w);
    if (cc_repo_head(r, &h, err) || cc_repo_head_commit(r, &c, &found, err)) {
        *bad = 1;
        jw_obj_end(w);
        return;
    }
    jw_key(w, "branch");
    if (h.detached) jw_null(w);
    else
        jw_str(w, h.branch);
    jw_key(w, "commit");
    if (found) jw_str(w, c.h);
    else
        jw_null(w);
    jw_obj_end(w);
}

/* The body's string member `key` (NUL-terminated in the arena), `dflt` when absent; NULL with
 * *bad set when it is not a string. */
static const char *str_arg(const JrVal *v, const char *key, const char *dflt, int *bad) {
    const JrVal *x = v && v->type == JR_OBJ ? jr_get(v, key) : NULL;
    if (!x || x->type == JR_NULL) return dflt;
    if (x->type != JR_STR || strlen(x->s) != x->n) return *bad = 1, NULL; /* no NUL inside */
    return x->s;
}

static void changes_json(JsonW *w, const cc_changes *c) {
    jw_key(w, "changes");
    jw_arr_begin(w);
    for (size_t i = 0; i < c->n; i++) cc_version_change_json(&c->v[i], w);
    jw_arr_end(w);
}

static int q_versions(cc_session *s, const JrVal *body, cc_buf *out) {
    int bad = 0;
    const char *rev = str_arg(body, "rev", NULL, &bad);
    size_t limit = 100;
    const JrVal *l = body && body->type == JR_OBJ ? jr_get(body, "limit") : NULL;
    if (l && l->type != JR_NULL) {
        if (l->type != JR_NUM || !l->num_int || l->num_neg || l->num < 0) bad = 1;
        else
            limit = l->num > CC_REPO_MAX_WALK ? CC_REPO_MAX_WALK : (size_t)l->num;
    }
    if (bad) return buf_clear(out), buf_puts(out, "versions: {rev?: a revision, limit?: a count}"), CC_ERR;
    cc_repo r;
    int answer;
    if (open_repo(s, &r, out, &answer)) return answer;
    Buf e;
    buf_init(&e);
    Arena a;
    arena_init(&a, 0);
    cc_log_entry *v = NULL;
    size_t n = 0;
    int rc = CC_OK;
    if (!rev) { /* HEAD, and no versions before the first commit */
        cc_hash h;
        int found;
        if (cc_repo_head_commit(&r, &h, &found, &e)) rc = CC_ERR;
        else if (found && cc_repo_log(&r, h.h, limit, &a, &v, &n, &e))
            rc = CC_ERR;
    } else if (cc_repo_log(&r, rev, limit, &a, &v, &n, &e))
        rc = CC_ERR;
    if (rc == CC_OK) {
        buf_clear(out);
        JsonW w;
        jw_init(&w, out, 0);
        jw_obj_begin(&w);
        jw_key(&w, "history");
        jw_bool(&w, 1);
        int hb = 0;
        head_json(&r, &w, &e, &hb);
        jw_key(&w, "versions");
        jw_arr_begin(&w);
        for (size_t i = 0; i < n; i++) {
            jw_obj_begin(&w);
            jw_key(&w, "hash");
            jw_str(&w, v[i].hash.h);
            jw_key(&w, "parents");
            jw_arr_begin(&w);
            for (size_t k = 0; k < v[i].c.nparents; k++) jw_str(&w, v[i].c.parents[k]);
            jw_arr_end(&w);
            jw_key(&w, "author");
            jw_str(&w, v[i].c.author);
            jw_key(&w, "time");
            jw_i64(&w, v[i].c.time);
            jw_key(&w, "message");
            jw_str(&w, v[i].c.message);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_obj_end(&w);
        if (hb) rc = fail(out, &e);
        else if (jw_finish(&w) != JW_OK)
            rc = CC_ERR;
    } else
        fail(out, &e);
    arena_free(&a);
    buf_free(&e);
    cc_repo_close(&r);
    return rc;
}

static int q_branches(cc_session *s, cc_buf *out) {
    cc_repo r;
    int answer;
    if (open_repo(s, &r, out, &answer)) return answer;
    Buf e;
    buf_init(&e);
    Arena a;
    arena_init(&a, 0);
    cc_branch *b;
    size_t n;
    int rc = CC_OK;
    if (cc_repo_branches(&r, &a, &b, &n, &e)) rc = fail(out, &e);
    else {
        buf_clear(out);
        JsonW w;
        jw_init(&w, out, 0);
        jw_obj_begin(&w);
        jw_key(&w, "history");
        jw_bool(&w, 1);
        int hb = 0;
        head_json(&r, &w, &e, &hb);
        jw_key(&w, "branches");
        jw_arr_begin(&w);
        for (size_t i = 0; i < n; i++) {
            jw_obj_begin(&w);
            jw_key(&w, "name");
            jw_str(&w, b[i].name);
            jw_key(&w, "commit");
            jw_str(&w, b[i].commit.h);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_obj_end(&w);
        if (hb) rc = fail(out, &e);
        else if (jw_finish(&w) != JW_OK)
            rc = CC_ERR;
    }
    arena_free(&a);
    buf_free(&e);
    cc_repo_close(&r);
    return rc;
}

static int q_version_changes(cc_session *s, const JrVal *body, cc_buf *out) {
    int bad = 0;
    const char *ra = str_arg(body, "a", NULL, &bad), *rb = str_arg(body, "b", "HEAD", &bad);
    if (bad || !ra)
        return buf_clear(out), buf_puts(out, "version_changes: {a: a revision, b?: a revision (HEAD)}"), CC_ERR;
    cc_repo r;
    int answer;
    if (open_repo(s, &r, out, &answer)) return answer;
    Buf e;
    buf_init(&e);
    cc_hash ha, hb;
    cc_model ma, mb;
    cc_model_init(&ma), cc_model_init(&mb);
    cc_changes c;
    cc_changes_init(&c);
    int rc = CC_ERR;
    if (cc_repo_resolve(&r, ra, &ha, &e) || cc_repo_resolve(&r, rb, &hb, &e)) goto done;
    cc_model_free(&ma), cc_model_free(&mb);
    if (cc_repo_model_at(&r, ha.h, &ma, &e) || cc_repo_model_at(&r, hb.h, &mb, &e)) goto done;
    if (cc_diff(&ma, &mb, &c, &e)) goto done;
    buf_clear(out);
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "history");
    jw_bool(&w, 1);
    jw_key(&w, "a");
    jw_str(&w, ha.h);
    jw_key(&w, "b");
    jw_str(&w, hb.h);
    changes_json(&w, &c);
    jw_obj_end(&w);
    rc = jw_finish(&w) == JW_OK ? CC_OK : CC_ERR;
done:
    if (rc == CC_ERR && e.len) fail(out, &e);
    cc_changes_free(&c);
    cc_model_free(&ma), cc_model_free(&mb);
    buf_free(&e);
    cc_repo_close(&r);
    return rc;
}

/* ---- H2a: pending, cached per (file, HEAD, rev): HEAD's model is read once per commit, the
 * diff once per rev (H1 read HEAD's whole tree on every call). ---- */
struct cc_versions_cache {
    char *path;      /* malloc'd: the model file the history is next to */
    cc_hash base;    /* HEAD's commit ("" before the first) */
    int has_model;   /* `model` is base's */
    cc_model model;
    uint64_t rev;    /* the answer's rev (and `answered`) */
    int answered;
    Buf answer;
};

void cc_versions_free(cc_session *s) {
    struct cc_versions_cache *c = s->versions_cache;
    if (!c) return;
    free(c->path);
    if (c->has_model) cc_model_free(&c->model);
    buf_free(&c->answer);
    free(c);
    s->versions_cache = NULL;
}

static struct cc_versions_cache *cache_for(cc_session *s, const cc_hash *base) {
    struct cc_versions_cache *c = s->versions_cache;
    if (c && (strcmp(c->path, s->path) || strcmp(c->base.h, base->h))) cc_versions_free(s), c = NULL;
    if (!c) {
        c = calloc(1, sizeof *c);
        if (!c) return NULL;
        size_t n = strlen(s->path);
        c->path = malloc(n + 1);
        if (!c->path) return free(c), NULL;
        memcpy(c->path, s->path, n + 1);
        c->base = *base;
        buf_init(&c->answer);
        s->versions_cache = c;
    }
    return c;
}

static int q_pending(cc_session *s, cc_buf *out) {
    if (s->pf->files && !s->open) return none(out, "no editable model open", 22);
    cc_repo r;
    int answer;
    if (open_repo(s, &r, out, &answer)) return answer;
    Buf e;
    buf_init(&e);
    cc_changes c;
    cc_changes_init(&c);
    cc_hash h;
    memset(&h, 0, sizeof h);
    int found = 0, rc = CC_ERR;
    if (cc_repo_head_commit(&r, &h, &found, &e)) {
        fail(out, &e);
        goto done;
    }
    if (!found) h.h[0] = 0;
    struct cc_versions_cache *k = cache_for(s, &h);
    if (k && k->answered && k->rev == s->rev) {
        buf_clear(out);
        buf_append(out, k->answer.p, k->answer.len);
        rc = out->oom ? CC_ERR : CC_OK;
        goto done;
    }
    if (k && found && !k->has_model) {
        Arena a;
        arena_init(&a, 0);
        cc_commit ci;
        int bad = cc_repo_commit_info(&r, h.h, &a, &ci, &e) || cc_repo_read_tree(&r, ci.tree, &k->model, &e);
        arena_free(&a);
        if (bad) {
            cc_model_free(&k->model);
            fail(out, &e);
            goto done;
        }
        k->has_model = 1;
    }
    int bad;
    if (k && found) bad = cc_diff(&k->model, &s->model, &c, &e);
    else
        bad = cc_repo_status(&r, &s->model, &c, &e);
    if (bad) {
        fail(out, &e);
        goto done;
    }
    buf_clear(out);
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "history");
    jw_bool(&w, 1);
    jw_key(&w, "base");
    if (found) jw_str(&w, h.h);
    else
        jw_null(&w);
    changes_json(&w, &c);
    jw_obj_end(&w);
    rc = jw_finish(&w) == JW_OK ? CC_OK : CC_ERR;
    if (rc == CC_OK && k) {
        buf_clear(&k->answer);
        buf_append(&k->answer, out->p, out->len);
        k->answered = !k->answer.oom, k->rev = s->rev;
    }
done:
    cc_changes_free(&c);
    buf_free(&e);
    cc_repo_close(&r);
    return rc;
}
/* ---- end H2a ---- */

int cc_versions_query(cc_session *s, const char *name, const char *body, size_t n, cc_buf *out) {
    int which = !strcmp(name, "versions") ? 1 : !strcmp(name, "branches") ? 2 : !strcmp(name, "version_changes") ? 3
                : !strcmp(name, "pending")                                 ? 4
                                                                           : 0;
    if (!which) return -1;
    Arena a;
    arena_init(&a, 0);
    const JrVal *v = NULL;
    if (n) {
        JrError je;
        v = jr_parse(&a, body, n, &je);
        if (!v) {
            arena_free(&a);
            buf_clear(out);
            buf_puts(out, name);
            buf_puts(out, ": the body is not JSON");
            return CC_ERR;
        }
    }
    int rc = which == 1 ? q_versions(s, v, out)
             : which == 2 ? q_branches(s, out)
             : which == 3 ? q_version_changes(s, v, out)
                          : q_pending(s, out);
    arena_free(&a);
    return rc;
}
/* ---- H2a: commit, branch_create, branch_switch (lib.rs:1903-2097, the History panel) ---- */

static int refuse(cc_buf *out, const char *text) {
    buf_clear(out);
    buf_puts(out, text);
    return CC_ERR;
}

/* settings.rs author(): "name <e-mail>", or the name alone. The C core refuses without a name
 * (state.user is null then: the page sends the user to Settings), where Rust falls back to the
 * e-mail alone or $BIMBLE_AUTHOR / $USER (DEVIATIONS 27). 0, or -1 without a name. */
static int author_of(cc_session *s, Buf *author) {
    Buf u;
    buf_init(&u);
    JsonW w;
    jw_init(&w, &u, 0);
    cc_settings_user_json(s, &w);
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = jw_finish(&w) == JW_OK ? jr_parse(&a, u.p ? u.p : "", u.len, &je) : NULL;
    const JrVal *name = v && v->type == JR_OBJ ? jr_get(v, "name") : NULL;
    const JrVal *mail = v && v->type == JR_OBJ ? jr_get(v, "email") : NULL;
    int rc = -1;
    if (name && name->type == JR_STR && name->n) {
        buf_append(author, name->s, name->n);
        if (mail && mail->type == JR_STR && mail->n) buf_puts(author, " <"), buf_append(author, mail->s, mail->n), buf_putc(author, '>');
        rc = 0;
    }
    arena_free(&a);
    buf_free(&u);
    return rc;
}

/* The body's members: a string `key` (NUL-terminated in the arena) and an optional bool. */
static const JrVal *parse_body(Arena *a, const char *body, size_t n) {
    if (!n) return NULL;
    JrError je;
    return jr_parse(a, body, n, &je);
}

/* lib.rs:2082-2092: the model a switch gives is written to the working file, which is opened
 * again (fresh undo history, saved, rev + 1; the session file follows). */
static int follow(cc_session *s, cc_model *m, cc_buf *out) {
    Buf b;
    buf_init(&b);
    size_t pn = strlen(s->path);
    int json = pn >= 5 && !strcmp(s->path + pn - 5, ".json"); /* file.rs:145, as cc_session_save */
    int ok = (json ? cc_model_save(m, &b) : cc_model_save_v2(m, &b)) == 0 && !b.oom &&
             s->pf->write_file(s->pf->ctx, s->path, b.p, b.len) == 0;
    buf_free(&b);
    if (!ok) {
        buf_clear(out);
        buf_puts(out, s->path);
        buf_puts(out, ": cannot write");
        return CC_ERR;
    }
    char path[sizeof s->path];
    memcpy(path, s->path, sizeof path);
    cc_error err;
    if (cc_session_open(s, path, &err)) return refuse(out, err.msg);
    return CC_OK;
}

static int a_commit(cc_session *s, const JrVal *v, cc_buf *out) {
    int bad = 0;
    const char *msg = str_arg(v, "message", NULL, &bad);
    if (bad || !msg) return refuse(out, "commit: {message: the version's message}");
    Buf author, e;
    buf_init(&author), buf_init(&e);
    cc_repo r;
    r.dir = NULL;
    int rc = CC_ERR;
    if (author_of(s, &author)) {
        refuse(out, "commit: set your name in Settings first (the author of each version)");
        goto done;
    }
    /* lib.rs:1976: save first, so the file and the history agree. */
    if (cc_session_unsaved(s)) {
        cc_error err;
        if (cc_session_save(s, NULL, &err)) {
            refuse(out, err.msg);
            goto done;
        }
    }
    /* lib.rs:1932: a model without a history starts one ("Start history and commit"). */
    Buf d;
    buf_init(&d);
    cc_repo_history_dir(s->path, &d);
    buf_puts(&d, "/format");
    int there = s->pf->exists(s->pf->ctx, buf_cstr(&d));
    buf_free(&d);
    if (there ? cc_repo_open(&r, s->pf, s->path, &e) : cc_repo_init(&r, s->pf, s->path, &e)) {
        fail(out, &e);
        goto done;
    }
    cc_hash h;
    int64_t now = s->pf->unix_s ? s->pf->unix_s(s->pf->ctx) : 0;
    if (cc_repo_commit(&r, &s->model, msg, buf_cstr(&author), now, &h, &e)) {
        fail(out, &e);
        goto done;
    }
    buf_clear(out);
    buf_puts(out, "{\"commit\":\"");
    buf_append(out, h.h, 64);
    buf_puts(out, "\"}");
    rc = CC_OK;
done:
    cc_repo_close(&r);
    buf_free(&author), buf_free(&e);
    return rc;
}

static int a_branch(cc_session *s, int create, const JrVal *v, cc_buf *out) {
    int bad = 0;
    const char *name = str_arg(v, "name", NULL, &bad);
    const JrVal *dv = v && v->type == JR_OBJ ? jr_get(v, "discard") : NULL;
    if (dv && dv->type != JR_BOOL && dv->type != JR_NULL) bad = 1;
    int discard = dv && dv->type == JR_BOOL && dv->b;
    if (bad || !name)
        return refuse(out, create ? "branch_create: {name: the new branch}"
                                  : "branch_switch: {name: a branch or a revision, discard?: bool}");
    cc_repo r;
    int answer;
    if (open_repo(s, &r, out, &answer)) {
        /* No history yet: nothing to branch from (Rust's panel shows only the first commit). */
        return answer == CC_OK ? refuse(out, "no history yet: commit a first version") : answer;
    }
    Buf e;
    buf_init(&e);
    int rc = CC_ERR;
    cc_model m;
    cc_model_init(&m);
    if (create) { /* lib.rs:2047-2050: create at HEAD, then switch to it (the model stays) */
        cc_hash h;
        if (cc_repo_create_branch(&r, name, NULL, &h, &e) || cc_repo_switch(&r, name, &m, &e)) fail(out, &e);
        else
            buf_clear(out), buf_puts(out, "{\"branch\":"), json_escape(out, name, strlen(name)), buf_putc(out, '}'), rc = CC_OK;
        goto done;
    }
    /* lib.rs:2006: Switch is refused while changes are not committed ("Commit or undo the changes
     * first"); {"discard": true} drops them. */
    if (!discard) {
        cc_changes c;
        cc_changes_init(&c);
        int st = cc_repo_status(&r, &s->model, &c, &e);
        size_t n = c.n;
        cc_changes_free(&c);
        if (st) {
            fail(out, &e);
            goto done;
        }
        if (n) {
            refuse(out, "uncommitted changes: commit them first, or send {\"discard\": true}");
            goto done;
        }
    }
    cc_model_free(&m);
    if (cc_repo_switch(&r, name, &m, &e)) {
        fail(out, &e);
        goto done;
    }
    rc = follow(s, &m, out);
    if (rc == CC_OK) buf_clear(out), buf_puts(out, "{\"branch\":"), json_escape(out, name, strlen(name)), buf_putc(out, '}');
done:
    cc_model_free(&m);
    buf_free(&e);
    cc_repo_close(&r);
    return rc;
}

/* ---- H2b: merge {name, prefer?} (lib.rs:2015-2033, the panel's Merge) ---- */

/* The conflicts as the page lists them: [{"kind", "id" (or null), "name", "text"}]. */
static void conflicts_json(JsonW *w, const cc_conflicts *cf) {
    jw_key(w, "conflicts");
    jw_arr_begin(w);
    Buf t;
    buf_init(&t);
    for (size_t i = 0; i < cf->n; i++) {
        const cc_conflict *c = &cf->v[i];
        jw_obj_begin(w);
        jw_key(w, "kind");
        jw_str(w, c->kind);
        jw_key(w, "id");
        if (c->has_id) jw_str(w, c->id);
        else
            jw_null(w);
        jw_key(w, "name");
        jw_str(w, c->name);
        jw_key(w, "text");
        buf_clear(&t);
        cc_conflict_display(c, &t);
        jw_str_n(w, t.p ? t.p : "", t.len);
        jw_obj_end(w);
    }
    buf_free(&t);
    jw_arr_end(w);
}

static int a_merge(cc_session *s, const JrVal *v, cc_buf *out) {
    int bad = 0;
    const char *name = str_arg(v, "name", NULL, &bad);
    const char *pv = str_arg(v, "prefer", NULL, &bad);
    cc_prefer prefer = CC_PREFER_NEITHER;
    if (pv && !strcmp(pv, "ours")) prefer = CC_PREFER_OURS;
    else if (pv && !strcmp(pv, "theirs"))
        prefer = CC_PREFER_THEIRS;
    else if (pv && strcmp(pv, "neither"))
        bad = 1;
    if (bad || !name || !*name)
        return refuse(out, "merge: {name: the branch (or revision) to merge into the current one, prefer?: "
                           "\"ours\" | \"theirs\"}");
    cc_repo r;
    int answer;
    if (open_repo(s, &r, out, &answer)) return answer == CC_OK ? refuse(out, "no history yet: commit a first version") : answer;
    Buf e, author;
    buf_init(&e), buf_init(&author);
    cc_model m;
    cc_model_init(&m);
    cc_conflicts cf;
    cc_conflicts_init(&cf);
    int rc = CC_ERR;
    cc_repo_head_t hd;
    if (cc_repo_head(&r, &hd, &e)) {
        fail(out, &e);
        goto done;
    }
    /* Rust offers Merge on the other branches only. */
    if (!hd.detached && !strcmp(hd.branch, name)) {
        buf_clear(out), buf_puts(out, "merge: "), buf_puts(out, name), buf_puts(out, " is the current branch");
        goto done;
    }
    if (author_of(s, &author)) {
        refuse(out, "merge: set your name in Settings first (the author of the merge)");
        goto done;
    }
    /* lib.rs:2016: Merge is disabled while changes are not committed. */
    {
        cc_changes c;
        cc_changes_init(&c);
        int st = cc_repo_status(&r, &s->model, &c, &e);
        size_t n = c.n;
        cc_changes_free(&c);
        if (st) {
            fail(out, &e);
            goto done;
        }
        if (n) {
            refuse(out, "uncommitted changes: commit them first");
            goto done;
        }
    }
    int how = 0;
    cc_hash h;
    int64_t now = s->pf->unix_s ? s->pf->unix_s(s->pf->ctx) : 0;
    if (cc_repo_merge(&r, name, prefer, buf_cstr(&author), now, NULL, &how, &h, &m, &cf, &e)) {
        fail(out, &e);
        goto done;
    }
    /* lib.rs:2077-2092: the merged model (or the one fast-forwarded to) becomes the working file. */
    if (how == CC_MERGED_FAST_FORWARD || how == CC_MERGED_COMMIT) {
        if (follow(s, &m, out) != CC_OK) goto done;
    }
    buf_clear(out);
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "merged");
    jw_str(&w, how == CC_MERGED_UP_TO_DATE    ? "up_to_date"
               : how == CC_MERGED_FAST_FORWARD ? "fast_forward"
               : how == CC_MERGED_COMMIT       ? "commit"
                                               : "conflicts");
    if (how != CC_MERGED_CONFLICTS) {
        jw_key(&w, "commit");
        jw_str(&w, h.h);
    }
    conflicts_json(&w, &cf);
    jw_obj_end(&w);
    rc = jw_finish(&w) == JW_OK ? CC_OK : CC_ERR;
done:
    cc_conflicts_free(&cf);
    cc_model_free(&m);
    buf_free(&e), buf_free(&author);
    cc_repo_close(&r);
    return rc;
}
/* ---- end H2b ---- */

int cc_versions_action(cc_session *s, const char *name, const char *body, size_t n, cc_buf *out) {
    int which = !strcmp(name, "commit") ? 1 : !strcmp(name, "branch_create") ? 2 : !strcmp(name, "branch_switch") ? 3
                : !strcmp(name, "merge")                                                                          ? 4 /* H2b */
                                                                                                                  : 0;
    if (!which) return -1;
    if (!s->pf->files) return refuse(out, NO_BROWSER);
    if (!s->open) return refuse(out, s->view ? "this file is read only: import it to edit" : "no model open");
    if (s->untitled || !s->path[0])
        return refuse(out, "the model has no file yet: action save_as with {\"path\": \"<file>\"} first");
    Arena a;
    arena_init(&a, 0);
    const JrVal *v = parse_body(&a, body, n);
    int rc;
    if (n && !v) rc = refuse(out, "the body is not JSON");
    else
        rc = which == 1 ? a_commit(s, v, out) : which == 4 ? a_merge(s, v, out) : a_branch(s, which == 2, v, out);
    arena_free(&a);
    return rc;
}
/* ---- end H2a ---- */
#endif
