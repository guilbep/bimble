/* The history queries (versions.h; ticket H1). */
#include "versions.h"
#include <string.h>
#include "diff.h"
#include "json_r.h"
#include "json_w.h"
#include "repo.h"

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
    int found = 0, rc = CC_ERR;
    if (cc_repo_head_commit(&r, &h, &found, &e) || cc_repo_status(&r, &s->model, &c, &e)) {
        fail(out, &e);
    } else {
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
    }
    cc_changes_free(&c);
    buf_free(&e);
    cc_repo_close(&r);
    return rc;
}

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
#endif
