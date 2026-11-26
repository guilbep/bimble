/* Merging history branches (repo_merge.h; crates/bimble-core/src/repo.rs:864-959). */
#include "repo_merge.h"
#include <stdlib.h>
#include <string.h>
#include "guid.h"
#include "repo_write.h"

static void err_set(Buf *err, const char *s) {
    if (!err) return;
    buf_clear(err);
    buf_puts(err, s);
}

static int oom(Buf *err) { return err_set(err, "io: out of memory"), -1; }

static int is_hash(const char *s) {
    size_t i = 0;
    for (; s[i]; i++)
        if (i >= 64 || !((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return i == 64;
}

/* ---------------------------------------------------------------- a set of hashes */

/* Open addressing; the slot from BLAKE3 of the text, so names a peer chose cannot pile up in one
 * chain. */
typedef struct {
    cc_hash *v;
    size_t cap, n;
} HSet;

static size_t slot_of(const char *h, size_t cap) {
    unsigned char d[32];
    cc_blake3(h, 64, d);
    size_t x = 0;
    for (int i = 0; i < 8; i++) x = x << 8 | d[i];
    return x & (cap - 1);
}

/* 1 added, 0 already there, -1 out of memory. */
static int hset_add(HSet *s, const char *h) {
    if (2 * (s->n + 1) > s->cap) {
        size_t cap = s->cap ? 2 * s->cap : 64;
        cc_hash *v = calloc(cap, sizeof *v);
        if (!v) return -1;
        for (size_t i = 0; i < s->cap; i++)
            if (s->v[i].h[0]) {
                size_t k = slot_of(s->v[i].h, cap);
                while (v[k].h[0]) k = (k + 1) & (cap - 1);
                v[k] = s->v[i];
            }
        free(s->v);
        s->v = v, s->cap = cap;
    }
    size_t k = slot_of(h, s->cap);
    while (s->v[k].h[0]) {
        if (!memcmp(s->v[k].h, h, 64)) return 0;
        k = (k + 1) & (s->cap - 1);
    }
    memcpy(s->v[k].h, h, 64);
    s->v[k].h[64] = 0;
    s->n++;
    return 1;
}

static int hset_has(const HSet *s, const char *h) {
    if (!s->cap) return 0;
    size_t k = slot_of(h, s->cap);
    while (s->v[k].h[0]) {
        if (!memcmp(s->v[k].h, h, 64)) return 1;
        k = (k + 1) & (s->cap - 1);
    }
    return 0;
}

/* ---------------------------------------------------------------- the walk */

int cc_repo_ancestors(const cc_repo *r, const char *h, size_t limit, cc_hash **out, size_t *n, Buf *err) {
    *out = NULL, *n = 0;
    HSet seen = {0};
    cc_hash *q = NULL; /* the queue; [0, *n) is also the answer: each hash queued once */
    size_t qn = 0, qcap = 0, head = 0;
    int rc = -1;
    if (!is_hash(h)) {
        /* commit_info's own error for it */
        Arena a;
        arena_init(&a, 0);
        cc_commit c;
        if (!cc_repo_commit_info(r, h, &a, &c, err)) err_set(err, "a commit hash is needed");
        arena_free(&a);
        return -1;
    }
    q = malloc(16 * sizeof *q);
    if (!q || hset_add(&seen, h) < 0) goto nomem;
    qcap = 16;
    memcpy(q[0].h, h, 65);
    qn = 1;
    while (head < qn && head < limit) {
        Arena a;
        arena_init(&a, 0);
        cc_commit c;
        if (cc_repo_commit_info(r, q[head].h, &a, &c, err)) {
            arena_free(&a);
            goto done;
        }
        head++;
        for (size_t i = 0; i < c.nparents; i++) {
            const char *p = c.parents[i];
            if (!is_hash(p)) {
                cc_commit pc;
                if (!cc_repo_commit_info(r, p, &a, &pc, err)) err_set(err, "a parent is not a commit hash");
                arena_free(&a);
                goto done;
            }
            int added = hset_add(&seen, p);
            if (added < 0) {
                arena_free(&a);
                goto nomem;
            }
            if (!added) continue;
            if (qn == qcap) {
                cc_hash *v = realloc(q, 2 * qcap * sizeof *v);
                if (!v) {
                    arena_free(&a);
                    goto nomem;
                }
                q = v, qcap *= 2;
            }
            memcpy(q[qn].h, p, 64);
            q[qn++].h[64] = 0;
        }
        arena_free(&a);
    }
    *out = q, *n = head; /* the commits visited, in order */
    q = NULL;
    rc = 0;
    goto done;
nomem:
    oom(err);
done:
    free(q);
    free(seen.v);
    return rc;
}

int cc_repo_merge_base(const cc_repo *r, const char *a, const char *b, cc_hash *out, int *found, Buf *err) {
    *found = 0;
    cc_hash *va = NULL, *vb = NULL;
    size_t na = 0, nb = 0;
    HSet of_a = {0};
    int rc = -1;
    if (cc_repo_ancestors(r, a, CC_REPO_MAX_WALK, &va, &na, err) ||
        cc_repo_ancestors(r, b, CC_REPO_MAX_WALK, &vb, &nb, err))
        goto done;
    for (size_t i = 0; i < na; i++)
        if (hset_add(&of_a, va[i].h) < 0) {
            oom(err);
            goto done;
        }
    for (size_t i = 0; i < nb && !*found; i++)
        if (hset_has(&of_a, vb[i].h)) *out = vb[i], *found = 1;
    rc = 0;
done:
    free(va), free(vb), free(of_a.v);
    return rc;
}

/* ---------------------------------------------------------------- merge */

int cc_repo_merge(const cc_repo *r, const char *rev, cc_prefer prefer, const char *author, int64_t time,
                  const char *message, int *how, cc_hash *commit, cc_model *m, cc_conflicts *conflicts, Buf *err) {
    cc_model_init(m);
    *how = 0;
    cc_hash theirs, ours, base;
    int found = 0, has_base = 0;
    if (cc_repo_resolve(r, rev, &theirs, err) || cc_repo_head_commit(r, &ours, &found, err)) return -1;
    if (!found) return err_set(err, "nothing committed yet"), -1;
    if (cc_repo_merge_base(r, ours.h, theirs.h, &base, &has_base, err)) return -1;
    if (has_base && !strcmp(base.h, theirs.h)) {
        *how = CC_MERGED_UP_TO_DATE;
        *commit = ours;
        return cc_repo_model_at(r, ours.h, m, err);
    }
    if (has_base && !strcmp(base.h, ours.h)) {
        /* The model first: a damaged target leaves the ref where it was. */
        if (cc_repo_model_at(r, theirs.h, m, err) || cc_repo_move_head(r, theirs.h, err)) return -1;
        *how = CC_MERGED_FAST_FORWARD;
        *commit = theirs;
        return 0;
    }
    cc_model mo, mt, mb;
    cc_model_init(&mo), cc_model_init(&mt), cc_model_init(&mb);
    int rc = -1;
    if (cc_repo_model_at(r, ours.h, &mo, err) || cc_repo_model_at(r, theirs.h, &mt, err)) goto free_all;
    if (has_base) {
        if (cc_repo_model_at(r, base.h, &mb, err)) goto free_all;
    } else
        mb.project_id = mo.project_id; /* Model { project_id: ours', ..Default } */
    if (cc_merge_models(&mb, &mo, &mt, prefer, m, conflicts, err)) goto free_all;
    if (conflicts->n && prefer == CC_PREFER_NEITHER) {
        *how = CC_MERGED_CONFLICTS;
        cc_model_free(m);
        cc_model_init(m);
        rc = 0;
        goto free_all;
    }
    /* Each side valid does not make the union valid (an element on a storey the other removed). */
    cc_error ve;
    if (cc_model_validate(m, &ve)) {
        if (err) buf_clear(err), buf_puts(err, "the merged model is invalid: "), buf_puts(err, ve.msg);
        goto free_all;
    }
    cc_model_canonical(m);
    {
        cc_hash tree, parents[2] = {ours, theirs};
        Buf msg;
        buf_init(&msg);
        if (message) buf_puts(&msg, message);
        else
            buf_puts(&msg, "Merge "), buf_puts(&msg, rev);
        if (msg.oom) {
            buf_free(&msg);
            oom(err);
            goto free_all;
        }
        int w = cc_repo_write_tree(r, m, &tree, err) ||
                cc_repo_put_commit(r, tree.h, parents, 2, author, time, buf_cstr(&msg), commit, err) ||
                cc_repo_move_head(r, commit->h, err); /* the ref last */
        buf_free(&msg);
        if (w) goto free_all;
    }
    *how = CC_MERGED_COMMIT;
    rc = 0;
free_all:
    cc_model_free(&mo), cc_model_free(&mt), cc_model_free(&mb);
    if (rc) {
        cc_model_free(m);
        cc_model_init(m);
    }
    return rc;
}
