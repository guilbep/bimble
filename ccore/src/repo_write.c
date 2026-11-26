/* The history store, write side (repo_write.h; crates/bimble-core/src/repo.rs). */
#include "repo_write.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file.h"
#include "georef.h"
#include "guid.h"
#include "inflate.h"
#include "json_w.h"

/* ---------------------------------------------------------------- helpers */

static void err_set(Buf *err, const char *s) {
    if (!err) return;
    buf_clear(err);
    buf_puts(err, s);
}

static int oom(Buf *err) { return err_set(err, "io: out of memory"), -1; }

static void hex32(const unsigned char h[32], char out[65]) {
    static const char X[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[2 * i] = X[h[i] >> 4], out[2 * i + 1] = X[h[i] & 15];
    out[64] = 0;
}

static void hash_of(const char *p, size_t n, cc_hash *out) {
    unsigned char h[32];
    cc_blake3(p ? p : "", n, h);
    hex32(h, out->h);
}

/* `<dir>/<rel>` into `b` (NUL-terminated). */
static const char *path_of(const cc_repo *r, Buf *b, const char *rel) {
    buf_clear(b);
    buf_puts(b, r->dir);
    if (rel && *rel) buf_putc(b, '/'), buf_puts(b, rel);
    return buf_cstr(b);
}

/* create_dir_all through the platform: each level of `path` (made, or already a folder). */
static int make_dirs(const cc_platform *pf, const char *path, Buf *err) {
    if (!pf->make_dir) return err_set(err, "io: no history folder here"), -1;
    size_t n = strlen(path);
    char *p = malloc(n + 1);
    if (!p) return oom(err);
    memcpy(p, path, n + 1);
    int rc = 0;
    for (size_t i = 1; i <= n && !rc; i++) {
        if (i < n && p[i] != '/') continue;
        if (p[i - 1] == '/') continue; /* "//" or a final '/' */
        char c = p[i];
        p[i] = 0;
        if (pf->make_dir(pf->ctx, p)) {
            if (err) buf_clear(err), buf_puts(err, "io: cannot create the folder "), buf_puts(err, p);
            rc = -1;
        }
        p[i] = c;
    }
    free(p);
    return rc;
}

/* write_atomic through the platform. */
static int write_file(const cc_repo *r, const char *path, const char *p, size_t n, Buf *err) {
    if (!r->pf->write_file || r->pf->write_file(r->pf->ctx, path, p, n)) {
        if (err) buf_clear(err), buf_puts(err, "io: cannot write "), buf_puts(err, path);
        return -1;
    }
    return 0;
}

/* A code point of Unicode's White_Space (char::is_whitespace, what str::trim drops). */
static int uni_space(uint32_t c) {
    return (c >= 9 && c <= 13) || c == 32 || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

/* The code point at s[i] (valid UTF-8 assumed: a stray byte reads as itself), its length. */
static uint32_t cp_at(const unsigned char *s, size_t n, size_t i, size_t *len) {
    unsigned c = s[i];
    size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + k > n) k = 1;
    uint32_t v = k == 1 ? c : k == 2 ? (c & 31) : k == 3 ? (c & 15) : (c & 7);
    for (size_t j = 1; j < k; j++) v = (v << 6) | (s[i + j] & 63);
    *len = k;
    return v;
}

/* str::trim: [*a, *b) of s without white space at either end. */
static void trim(const char *s, size_t *a, size_t *b) {
    const unsigned char *u = (const unsigned char *)s;
    size_t n = strlen(s), i = 0, len;
    while (i < n && uni_space(cp_at(u, n, i, &len))) i += len;
    size_t j = n;
    while (j > i) {
        size_t k = j - 1;
        while (k > i && (u[k] & 0xC0) == 0x80 && j - k < 4) k--;
        if (!uni_space(cp_at(u, n, k, &len)) || k + len != j) break;
        j = k;
    }
    *a = i, *b = j;
}

/* ---------------------------------------------------------------- init */

int cc_repo_init_dir(cc_repo *r, const cc_platform *pf, const char *dir, Buf *err) {
    r->pf = pf;
    r->dir = NULL;
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/') n--;
    r->dir = malloc(n + 1);
    if (!r->dir) return oom(err);
    memcpy(r->dir, dir, n);
    r->dir[n] = 0;
    Buf p;
    buf_init(&p);
    int rc = -1;
    if (pf->exists && pf->exists(pf->ctx, path_of(r, &p, "format"))) {
        if (err) buf_clear(err), buf_puts(err, r->dir), buf_puts(err, " already has a history");
        goto done;
    }
    /* format last: a folder without it is no history yet, and init runs again. */
    if (make_dirs(pf, path_of(r, &p, "objects"), err) || make_dirs(pf, path_of(r, &p, "refs/heads"), err) ||
        write_file(r, path_of(r, &p, "HEAD"), "ref: main\n", 10, err) ||
        write_file(r, path_of(r, &p, "format"), CC_REPO_FORMAT "\n", sizeof CC_REPO_FORMAT, err))
        goto done;
    if (p.oom) {
        oom(err);
        goto done;
    }
    rc = 0;
done:
    buf_free(&p);
    return rc;
}

int cc_repo_init(cc_repo *r, const cc_platform *pf, const char *model_path, Buf *err) {
    Buf d;
    buf_init(&d);
    cc_repo_history_dir(model_path, &d);
    int rc = d.oom ? (r->pf = pf, r->dir = NULL, oom(err)) : cc_repo_init_dir(r, pf, buf_cstr(&d), err);
    buf_free(&d);
    return rc;
}

/* ---------------------------------------------------------------- objects */

int cc_repo_put_bytes(const cc_repo *r, const char *p, size_t n, cc_hash *out, Buf *err) {
    hash_of(p, n, out);
    char rel[8 + 2 + 1 + 62 + 1];
    memcpy(rel, "objects/", 8);
    rel[8] = out->h[0], rel[9] = out->h[1], rel[10] = 0;
    Buf path;
    buf_init(&path);
    int rc = -1;
    path_of(r, &path, rel);
    buf_putc(&path, '/');
    buf_append(&path, out->h + 2, 62);
    const char *full = buf_cstr(&path);
    if (path.oom) {
        oom(err);
        goto done;
    }
    if (r->pf->exists && r->pf->exists(r->pf->ctx, full)) { /* never rewritten */
        rc = 0;
        goto done;
    }
    {
        Buf dir, gz;
        buf_init(&dir), buf_init(&gz);
        path_of(r, &dir, rel);
        if (dir.oom || cc_gzip((const unsigned char *)(p ? p : ""), n, &gz) || gz.oom) oom(err);
        else if (!make_dirs(r->pf, buf_cstr(&dir), err) && !write_file(r, full, gz.p, gz.len, err))
            rc = 0;
        buf_free(&dir), buf_free(&gz);
    }
done:
    buf_free(&path);
    return rc;
}

/* ---------------------------------------------------------------- trees */

/* The mesh arrays of one write_tree (Rust's Refs scope): every hash referred to, for the tree's
 * `meshes`; each array stored once. */
typedef struct {
    const cc_repo *r;
    Buf *err;
    cc_hash *v;
    size_t n, cap;
    int failed; /* err holds why */
} Meshes;

static int mesh_ref(void *ctx, const char *json, size_t n, char hash[65]) {
    Meshes *ms = ctx;
    cc_hash h;
    hash_of(json, n, &h);
    for (size_t i = 0; i < ms->n; i++)
        if (!memcmp(ms->v[i].h, h.h, 64)) return memcpy(hash, h.h, 65), 0;
    if (cc_repo_put_bytes(ms->r, json, n, &h, ms->err)) return ms->failed = 1, -1;
    if (ms->n == ms->cap) {
        size_t cap = ms->cap ? 2 * ms->cap : 16;
        cc_hash *v = realloc(ms->v, cap * sizeof *v);
        if (!v) return oom(ms->err), ms->failed = 1, -1;
        ms->v = v, ms->cap = cap;
    }
    ms->v[ms->n++] = h;
    memcpy(hash, h.h, 65);
    return 0;
}

static int cmp_hash(const void *a, const void *b) { return memcmp(a, b, 64); }

/* `,"key":{"<id>":"<hash>",…}`: each object stored. */
static int put_map(const cc_repo *r, const cc_model *m, int kind, size_t count, const char *key, Meshes *ms,
                   Buf *tree, Buf *err) {
    buf_puts(tree, ",\"");
    buf_puts(tree, key);
    buf_puts(tree, "\":{");
    Buf obj;
    buf_init(&obj);
    for (size_t i = 0; i < count; i++) {
        const cc_gid *id = kind == CC_OBJ_STOREY      ? &m->storeys[i].id
                           : kind == CC_OBJ_MATERIAL  ? &m->materials[i].id
                           : kind == CC_OBJ_ELEMENT   ? &m->elements[i].id
                           : kind == CC_OBJ_VIEW      ? &m->views[i].id
                           : kind == CC_OBJ_SHEET     ? &m->sheets[i].id
                           : kind == CC_OBJ_RELATION  ? &m->relations[i].id
                                                      : &m->grids[i].id;
        buf_clear(&obj);
        if (cc_model_object_json(m, kind, i, kind == CC_OBJ_ELEMENT ? &(cc_mesh_refs){mesh_ref, ms} : NULL, &obj)) {
            if (!ms->failed) err_set(err, "write_tree: an object does not serialize");
            buf_free(&obj);
            return -1;
        }
        cc_hash h;
        if (cc_repo_put_bytes(r, obj.p, obj.len, &h, err)) return buf_free(&obj), -1;
        if (i) buf_putc(tree, ',');
        buf_putc(tree, '"');
        buf_append(tree, id->c, 22);
        buf_puts(tree, "\":\"");
        buf_append(tree, h.h, 64);
        buf_putc(tree, '"');
    }
    buf_free(&obj);
    buf_putc(tree, '}');
    return 0;
}

/* `,"key":<the object's JSON>`. */
static int put_inline(const cc_model *m, int kind, const char *key, Buf *tree, Buf *err) {
    buf_puts(tree, ",\"");
    buf_puts(tree, key);
    buf_puts(tree, "\":");
    if (cc_model_object_json(m, kind, 0, NULL, tree)) return err_set(err, "write_tree: an object does not serialize"), -1;
    return 0;
}

int cc_repo_write_tree(const cc_repo *r, const cc_model *m, cc_hash *out, Buf *err) {
    Meshes ms = {r, err, NULL, 0, 0, 0};
    Buf t;
    buf_init(&t);
    int rc = -1;
    /* repo.rs Tree, its fields in order; the optional ones left out when empty. */
    buf_puts(&t, "{\"project_id\":\"");
    buf_append(&t, m->project_id.c, 22);
    buf_puts(&t, "\",\"project_name\":");
    json_escape(&t, m->project_name.n ? m->project_name.p : "", m->project_name.n);
    if (put_map(r, m, CC_OBJ_STOREY, m->nstoreys, "storeys", &ms, &t, err) ||
        put_map(r, m, CC_OBJ_MATERIAL, m->nmaterials, "materials", &ms, &t, err) ||
        put_map(r, m, CC_OBJ_ELEMENT, m->nelements, "elements", &ms, &t, err) ||
        (m->nviews && put_map(r, m, CC_OBJ_VIEW, m->nviews, "views", &ms, &t, err)) ||
        (m->nsheets && put_map(r, m, CC_OBJ_SHEET, m->nsheets, "sheets", &ms, &t, err)) ||
        (m->nproject && put_inline(m, CC_OBJ_PROJECT, "project", &t, err)) ||
        (m->nschedules && put_inline(m, CC_OBJ_SCHEDULES, "schedules", &t, err)) ||
        (!cc_georef_is_default(&m->georef) && put_inline(m, CC_OBJ_GEOREF, "georef", &t, err)))
        goto done;
    if (ms.n) { /* a BTreeSet: sorted, once each */
        qsort(ms.v, ms.n, sizeof *ms.v, cmp_hash);
        buf_puts(&t, ",\"meshes\":[");
        for (size_t i = 0; i < ms.n; i++) {
            if (i) buf_putc(&t, ',');
            buf_putc(&t, '"');
            buf_append(&t, ms.v[i].h, 64);
            buf_putc(&t, '"');
        }
        buf_putc(&t, ']');
    }
    /* The C model's own sections (DEVIATIONS 27): only when there are some. */
    if ((m->nrelations && put_map(r, m, CC_OBJ_RELATION, m->nrelations, "relations", &ms, &t, err)) ||
        (m->ngrids && put_map(r, m, CC_OBJ_GRID, m->ngrids, "grids", &ms, &t, err)))
        goto done;
    buf_putc(&t, '}');
    if (t.oom) {
        oom(err);
        goto done;
    }
    rc = cc_repo_put_bytes(r, t.p, t.len, out, err);
done:
    free(ms.v);
    buf_free(&t);
    return rc;
}

/* ---------------------------------------------------------------- refs */

int cc_repo_set_head(const cc_repo *r, const cc_repo_head_t *h, Buf *err) {
    Buf text, p;
    buf_init(&text), buf_init(&p);
    if (h->detached) buf_append(&text, h->commit.h, 64);
    else
        buf_puts(&text, "ref: "), buf_puts(&text, h->branch);
    buf_putc(&text, '\n');
    path_of(r, &p, "HEAD");
    int rc = text.oom || p.oom ? oom(err) : write_file(r, p.p, text.p, text.len, err);
    buf_free(&text), buf_free(&p);
    return rc;
}

int cc_repo_set_branch(const cc_repo *r, const char *name, const char *hash, Buf *err) {
    if (cc_repo_check_branch_name(name, strlen(name), err)) return -1;
    Buf p, text;
    buf_init(&p), buf_init(&text);
    int rc = -1;
    path_of(r, &p, "refs/heads/");
    buf_puts(&p, name);
    /* The folders of a name with '/' (repo.rs: create_dir_all of the parent). */
    char *slash = strrchr(buf_cstr(&p), '/');
    if (p.oom) {
        oom(err);
        goto done;
    }
    *slash = 0;
    int md = make_dirs(r->pf, p.p, err);
    *slash = '/';
    if (md) goto done;
    buf_puts(&text, hash);
    buf_putc(&text, '\n');
    rc = text.oom ? oom(err) : write_file(r, p.p, text.p, text.len, err);
done:
    buf_free(&p), buf_free(&text);
    return rc;
}

/* ---------------------------------------------------------------- operations */

int cc_repo_commit(const cc_repo *r, const cc_model *m, const char *message, const char *author, int64_t time,
                   cc_hash *out, Buf *err) {
    size_t ma, mb;
    trim(message, &ma, &mb);
    if (ma == mb) return err_set(err, "a commit needs a message"), -1;
    cc_hash tree, parent;
    int found = 0;
    if (cc_repo_write_tree(r, m, &tree, err) || cc_repo_head_commit(r, &parent, &found, err)) return -1;
    if (found) {
        Arena a;
        arena_init(&a, 0);
        cc_commit pc;
        int same = -1;
        if (!cc_repo_commit_info(r, parent.h, &a, &pc, err)) same = !strcmp(pc.tree, tree.h);
        arena_free(&a);
        if (same < 0) return -1;
        if (same) {
            if (err) buf_clear(err), buf_puts(err, "nothing to commit: the model is the same as "),
                buf_append(err, parent.h, 12);
            return CC_REPO_NOTHING;
        }
    }
    /* repo.rs Commit: tree, parents, author, time, message. */
    Buf c;
    buf_init(&c);
    buf_puts(&c, "{\"tree\":\"");
    buf_append(&c, tree.h, 64);
    buf_puts(&c, "\",\"parents\":[");
    if (found) buf_putc(&c, '"'), buf_append(&c, parent.h, 64), buf_putc(&c, '"');
    buf_puts(&c, "],\"author\":");
    json_escape(&c, author, strlen(author));
    char num[32];
    snprintf(num, sizeof num, ",\"time\":%lld,\"message\":", (long long)time);
    buf_puts(&c, num);
    json_escape(&c, message + ma, mb - ma);
    buf_putc(&c, '}');
    int rc = c.oom ? oom(err) : cc_repo_put_bytes(r, c.p, c.len, out, err);
    buf_free(&c);
    if (rc) return -1;
    /* The ref last: until here the old HEAD stands. */
    cc_repo_head_t h;
    if (cc_repo_head(r, &h, err)) return -1;
    if (h.detached) {
        h.commit = *out;
        return cc_repo_set_head(r, &h, err);
    }
    return cc_repo_set_branch(r, h.branch, out->h, err);
}

int cc_repo_create_branch(const cc_repo *r, const char *name, const char *at, cc_hash *out, Buf *err) {
    if (cc_repo_check_branch_name(name, strlen(name), err)) return -1;
    cc_hash h;
    int found = 0;
    if (cc_repo_branch_commit(r, name, &h, &found, err)) return -1;
    if (found) {
        if (err) buf_clear(err), buf_puts(err, "branch "), buf_puts(err, name), buf_puts(err, " already exists");
        return -1;
    }
    if (cc_repo_resolve(r, at ? at : "HEAD", out, err)) return -1;
    return cc_repo_set_branch(r, name, out->h, err);
}

static int model_of(const cc_repo *r, const char *commit, cc_model *m, Buf *err) {
    Arena a;
    arena_init(&a, 0);
    cc_commit c;
    int rc = cc_repo_commit_info(r, commit, &a, &c, err);
    if (!rc) {
        cc_model_free(m);
        rc = cc_repo_read_tree(r, c.tree, m, err);
    }
    arena_free(&a);
    return rc;
}

int cc_repo_switch(const cc_repo *r, const char *rev, cc_model *m, Buf *err) {
    cc_model_init(m);
    cc_repo_head_t h;
    memset(&h, 0, sizeof h);
    cc_hash c;
    int found = 0;
    size_t n = strlen(rev);
    if (!cc_repo_check_branch_name(rev, n, NULL)) {
        if (cc_repo_branch_commit(r, rev, &c, &found, err)) return -1;
    }
    if (found) {
        memcpy(h.branch, rev, n + 1);
    } else {
        if (cc_repo_resolve(r, rev, &c, err)) return -1;
        h.detached = 1;
        h.commit = c;
    }
    if (model_of(r, c.h, m, err)) return -1;
    return cc_repo_set_head(r, &h, err);
}
