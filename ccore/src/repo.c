/* The history store, read side (repo.h; crates/bimble-core/src/repo.rs). */
#include "repo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file.h"
#include "guid.h"
#include "inflate.h"
#include "json_r.h"
#include "json_w.h"

/* ---------------------------------------------------------------- small helpers */

static void err_set(Buf *err, const char *s) {
    if (!err) return;
    buf_clear(err);
    buf_puts(err, s);
}

/* RepoError::Damaged: "damaged history: object {hash} {why}". */
static int damaged(Buf *err, const char *hash, size_t hn, const char *why) {
    if (err) {
        buf_clear(err);
        buf_puts(err, "damaged history: object ");
        buf_append(err, hash, hn);
        buf_putc(err, ' ');
        buf_puts(err, why);
    }
    return -1;
}

/* RepoError::UnknownRev: `unknown revision {rev:?}`. */
static int unknown_rev(Buf *err, const char *rev) {
    if (err) {
        buf_clear(err);
        buf_puts(err, "unknown revision ");
        cc_buf_debug_str(err, rev, strlen(rev));
    }
    return -1;
}

static int is_hex_lower(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
static int is_hex_any(int c) { return is_hex_lower(c) || (c >= 'A' && c <= 'F'); }

/* repo.rs is_hash: 64 lowercase hex digits. */
static int is_hash_n(const char *s, size_t n) {
    if (n != 64) return 0;
    for (size_t i = 0; i < 64; i++)
        if (!is_hex_lower((unsigned char)s[i])) return 0;
    return 1;
}
static int is_hash(const char *s) { return is_hash_n(s, strlen(s)); }

static void hex32(const unsigned char h[32], char out[65]) {
    static const char X[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[2 * i] = X[h[i] >> 4], out[2 * i + 1] = X[h[i] & 15];
    out[64] = 0;
}

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/* `<dir>/<rel>` into a fresh Buf (NUL-terminated through buf_cstr). */
static const char *path_of(const cc_repo *r, Buf *b, const char *rel) {
    buf_clear(b);
    buf_puts(b, r->dir);
    if (rel && *rel) buf_putc(b, '/'), buf_puts(b, rel);
    return buf_cstr(b);
}

/* io::Error's text for a status of read_file (Rust prints the OS error). */
static void io_err(Buf *err, int st) {
    if (!err) return;
    buf_clear(err);
    if (st == CC_PF_NOT_FOUND) buf_puts(err, "io: No such file or directory (os error 2)");
    else if (st == CC_PF_NO_MEMORY) buf_puts(err, "io: out of memory");
    else buf_puts(err, "io: input/output error");
}

/* repo.rs read_small: the file, at most 4096 bytes, trimmed. `out` gets the trimmed text. */
static int read_small(const cc_repo *r, const char *path, Buf *out, Buf *err) {
    char *p = NULL;
    size_t n = 0;
    int st = r->pf->read_file(r->pf->ctx, path, CC_REPO_MAX_SMALL, &p, &n);
    if (st == CC_PF_TOO_LARGE) {
        if (err) buf_clear(err), buf_puts(err, path), buf_puts(err, ": too large");
        return -1;
    }
    if (st != CC_PF_OK) return io_err(err, st), -1;
    if (jr_utf8_check(p, n) >= 0) {
        free(p);
        err_set(err, "io: stream did not contain valid UTF-8");
        return -1;
    }
    size_t a = 0, b = n;
    while (a < b && is_ws((unsigned char)p[a])) a++;
    while (b > a && is_ws((unsigned char)p[b - 1])) b--;
    buf_clear(out);
    buf_append(out, p + a, b - a);
    free(p);
    buf_cstr(out);
    return out->oom ? (err_set(err, "io: out of memory"), -1) : 0;
}

static char *dupz(const char *s, size_t n) {
    char *z = malloc(n + 1);
    if (!z) return NULL;
    memcpy(z, s, n);
    z[n] = 0;
    return z;
}

static char *arena_strz(Arena *a, const char *s, size_t n) {
    char *z = arena_alloc(a, n + 1, 1);
    if (!z) return NULL;
    memcpy(z, s, n);
    z[n] = 0;
    return z;
}

/* arena_dup, 8-aligned (arrays of structs with pointers). */
static void *dup8(Arena *a, const void *src, size_t n) {
    void *p = arena_alloc(a, n, 8);
    if (p) memcpy(p, src, n);
    return p;
}

/* ---------------------------------------------------------------- open */

void cc_repo_history_dir(const char *model_path, Buf *out) {
    buf_puts(out, model_path);
    buf_puts(out, "-history");
}

void cc_repo_close(cc_repo *r) {
    free(r->dir);
    r->dir = NULL;
}

int cc_repo_open_dir(cc_repo *r, const cc_platform *pf, const char *dir, Buf *err) {
    r->pf = pf;
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/') n--;
    r->dir = dupz(dir, n);
    if (!r->dir) return err_set(err, "io: out of memory"), -1;
    Buf p, fmt;
    buf_init(&p), buf_init(&fmt);
    int rc = -1;
    const char *f = path_of(r, &p, "format");
    if (!pf->exists || !pf->read_file || !pf->exists(pf->ctx, f)) {
        if (err) buf_clear(err), buf_puts(err, "no history next to "), buf_puts(err, dir),
            buf_puts(err, " (run `bimble history init`)");
        goto done;
    }
    if (read_small(r, f, &fmt, err)) goto done;
    if (strcmp(buf_cstr(&fmt), CC_REPO_FORMAT) != 0) {
        if (err) {
            buf_clear(err);
            buf_puts(err, r->dir);
            buf_puts(err, ": unsupported history format ");
            cc_buf_debug_str(err, fmt.p, fmt.len);
            buf_puts(err, " (this build reads \"" CC_REPO_FORMAT "\")");
        }
        goto done;
    }
    rc = 0;
done:
    buf_free(&p), buf_free(&fmt);
    return rc;
}

int cc_repo_open(cc_repo *r, const cc_platform *pf, const char *model_path, Buf *err) {
    Buf d;
    buf_init(&d);
    cc_repo_history_dir(model_path, &d);
    r->pf = pf;
    r->dir = NULL;
    int rc;
    Buf f;
    buf_init(&f);
    buf_append(&f, d.p, d.len);
    buf_puts(&f, "/format");
    if (d.oom || f.oom) rc = (err_set(err, "io: out of memory"), -1);
    else if (!pf->exists || !pf->exists(pf->ctx, buf_cstr(&f))) {
        if (err) buf_clear(err), buf_puts(err, "no history next to "), buf_puts(err, model_path),
            buf_puts(err, " (run `bimble history init`)");
        rc = -1;
    } else
        rc = cc_repo_open_dir(r, pf, buf_cstr(&d), err);
    buf_free(&d), buf_free(&f);
    return rc;
}

/* ---------------------------------------------------------------- objects */

int cc_repo_get_bytes(const cc_repo *r, const char *hash, Buf *out, Buf *err) {
    size_t hn = strlen(hash);
    if (!is_hash_n(hash, hn)) return damaged(err, hash, hn, "is not a hash");
    char rel[8 + 2 + 1 + 62 + 1];
    memcpy(rel, "objects/", 8);
    rel[8] = hash[0], rel[9] = hash[1], rel[10] = '/';
    memcpy(rel + 11, hash + 2, 62);
    rel[73] = 0;
    Buf p;
    buf_init(&p);
    const char *path = path_of(r, &p, rel);
    char *data = NULL;
    size_t n = 0;
    int st = p.oom ? CC_PF_NO_MEMORY : r->pf->read_file(r->pf->ctx, path, CC_REPO_MAX_OBJECT, &data, &n);
    buf_free(&p);
    if (st == CC_PF_NOT_FOUND) return damaged(err, hash, hn, "is missing");
    if (st == CC_PF_TOO_LARGE) return damaged(err, hash, hn, "is too large");
    if (st != CC_PF_OK) return io_err(err, st), -1;
    buf_clear(out);
    if (n >= 2 && (unsigned char)data[0] == 0x1f && (unsigned char)data[1] == 0x8b) {
        /* Gzipped (ADR 0042); what unpacks is capped too. One byte over the cap tells "too
         * large" from a damaged stream. */
        const char *why = NULL;
        int g = cc_gunzip((const unsigned char *)data, n, CC_REPO_MAX_OBJECT + 1, out, &why);
        free(data);
        if (g) {
            if (out->oom) return err_set(err, "io: out of memory"), -1;
            if (out->len > CC_REPO_MAX_OBJECT) return damaged(err, hash, hn, "is too large");
            return damaged(err, hash, hn, why ? why : "gzip: damaged");
        }
        if (out->len > CC_REPO_MAX_OBJECT) return damaged(err, hash, hn, "is too large");
    } else {
        buf_append(out, data, n);
        free(data);
    }
    if (out->oom) return err_set(err, "io: out of memory"), -1;
    unsigned char h[32];
    char hex[65];
    cc_blake3(out->p ? out->p : "", out->len, h);
    hex32(h, hex);
    if (memcmp(hex, hash, 64) != 0) return damaged(err, hash, hn, "does not match its hash");
    return 0;
}

/* The object parsed to a DOM in `a` (bytes copied into `a`, so number tokens stay valid). */
static const JrVal *get_dom(const cc_repo *r, const char *hash, Arena *a, Buf *err) {
    Buf b;
    buf_init(&b);
    if (cc_repo_get_bytes(r, hash, &b, err)) return buf_free(&b), NULL;
    char *text = arena_strz(a, b.p ? b.p : "", b.len);
    size_t n = b.len;
    buf_free(&b);
    if (!text) return err_set(err, "io: out of memory"), NULL;
    JrError je;
    const JrVal *v = jr_parse(a, text, n, &je);
    if (!v) {
        damaged(err, hash, strlen(hash), je.msg);
        return NULL;
    }
    return v;
}

/* serde's texts for a struct with deny_unknown_fields: unknown and missing fields. */
static int check_fields(const JrVal *o, const char *const *known, size_t nknown, const char *hash, const char *what,
                        Buf *err) {
    if (!o || o->type != JR_OBJ) {
        char why[96];
        snprintf(why, sizeof why, "invalid type: expected struct %s", what);
        return damaged(err, hash, strlen(hash), why);
    }
    for (size_t i = 0; i < o->n; i++) {
        size_t k = 0;
        while (k < nknown && !jr_key_is(&o->members[i], known[k])) k++;
        if (k == nknown) {
            Buf w;
            buf_init(&w);
            buf_puts(&w, "unknown field `");
            buf_append(&w, o->members[i].key, o->members[i].key_n);
            buf_puts(&w, "`");
            damaged(err, hash, strlen(hash), buf_cstr(&w));
            buf_free(&w);
            return -1;
        }
        for (size_t j = 0; j < i; j++)
            if (o->members[j].key_n == o->members[i].key_n &&
                !memcmp(o->members[j].key, o->members[i].key, o->members[i].key_n)) {
                Buf w;
                buf_init(&w);
                buf_puts(&w, "duplicate field `");
                buf_append(&w, o->members[i].key, o->members[i].key_n);
                buf_puts(&w, "`");
                damaged(err, hash, strlen(hash), buf_cstr(&w));
                buf_free(&w);
                return -1;
            }
    }
    return 0;
}

static int missing_field(const char *hash, const char *field, Buf *err) {
    char why[96];
    snprintf(why, sizeof why, "missing field `%s`", field);
    return damaged(err, hash, strlen(hash), why);
}

static int bad_type(const char *hash, const char *field, const char *expected, Buf *err) {
    char why[128];
    snprintf(why, sizeof why, "invalid type for `%s`: expected %s", field, expected);
    return damaged(err, hash, strlen(hash), why);
}

/* A JSON integer token as i64 (serde: no fraction, no exponent, in range). */
static int token_i64(const JrVal *v, int64_t *out) {
    if (v->type != JR_NUM || !v->num_int) return -1;
    const char *s = v->s;
    size_t n = v->n, i = 0;
    int neg = 0;
    if (i < n && s[i] == '-') neg = 1, i++;
    if (i == n) return -1;
    uint64_t acc = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        uint64_t d = (uint64_t)(s[i] - '0');
        if (acc > (UINT64_MAX - d) / 10) return -1;
        acc = acc * 10 + d;
    }
    if (neg) {
        if (acc > (uint64_t)INT64_MAX + 1) return -1;
        *out = acc == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)acc;
    } else {
        if (acc > (uint64_t)INT64_MAX) return -1;
        *out = (int64_t)acc;
    }
    return 0;
}

int cc_repo_commit_info(const cc_repo *r, const char *hash, Arena *a, cc_commit *c, Buf *err) {
    memset(c, 0, sizeof *c);
    const JrVal *o = get_dom(r, hash, a, err);
    if (!o) return -1;
    static const char *const F[] = {"tree", "parents", "author", "time", "message"};
    if (check_fields(o, F, 5, hash, "Commit", err)) return -1;
    const JrVal *tree = jr_get(o, "tree"), *parents = jr_get(o, "parents"), *author = jr_get(o, "author"),
                *time = jr_get(o, "time"), *message = jr_get(o, "message");
    if (!tree) return missing_field(hash, "tree", err);
    if (!parents) return missing_field(hash, "parents", err);
    if (!author) return missing_field(hash, "author", err);
    if (!time) return missing_field(hash, "time", err);
    if (!message) return missing_field(hash, "message", err);
    if (tree->type != JR_STR) return bad_type(hash, "tree", "a string", err);
    if (author->type != JR_STR) return bad_type(hash, "author", "a string", err);
    if (message->type != JR_STR) return bad_type(hash, "message", "a string", err);
    if (parents->type != JR_ARR) return bad_type(hash, "parents", "a sequence", err);
    if (token_i64(time, &c->time)) return bad_type(hash, "time", "i64", err);
    c->tree = tree->s;
    c->author = author->s;
    c->message = message->s;
    c->nparents = parents->n;
    c->parents = parents->n ? arena_alloc(a, parents->n * sizeof *c->parents, 8) : NULL;
    if (parents->n && !c->parents) return err_set(err, "io: out of memory"), -1;
    for (size_t i = 0; i < parents->n; i++) {
        if (parents->items[i].type != JR_STR) return bad_type(hash, "parents", "a string", err);
        c->parents[i] = parents->items[i].s;
    }
    return 0;
}

/* ---------------------------------------------------------------- trees */

/* Writes a JSON value compactly as it was given (keys in document order): for the parts of a
 * tree copied into the model text (project, schedules, georef). */
static void write_raw(const JrVal *v, Buf *b) {
    switch (v->type) {
    case JR_NULL: buf_puts(b, "null"); break;
    case JR_BOOL: buf_puts(b, v->b ? "true" : "false"); break;
    case JR_NUM: buf_append(b, v->s, v->n); break;
    case JR_STR: json_escape(b, v->s, v->n); break;
    case JR_ARR:
        buf_putc(b, '[');
        for (size_t i = 0; i < v->n; i++) {
            if (i) buf_putc(b, ',');
            write_raw(&v->items[i], b);
        }
        buf_putc(b, ']');
        break;
    case JR_OBJ:
        buf_putc(b, '{');
        for (size_t i = 0; i < v->n; i++) {
            if (i) buf_putc(b, ',');
            json_escape(b, v->members[i].key, v->members[i].key_n);
            buf_putc(b, ':');
            write_raw(&v->members[i].val, b);
        }
        buf_putc(b, '}');
        break;
    }
}

static const char *find_bytes(const char *h, size_t n, const char *needle, size_t m) {
    if (m == 0 || n < m) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (h[i] == needle[0] && !memcmp(h + i, needle, m)) return h + i;
    return NULL;
}

/* Mesh references (ADR 0042) of one generator object: positions / indices given as a hash. */
typedef struct {
    size_t off; /* of the opening quote in the object's bytes */
    const char *hash;
} MeshRef;

static int collect_refs(const JrVal *gen, MeshRef *refs, size_t *nref, size_t cap) {
    if (!gen || gen->type != JR_OBJ) return 0;
    const JrVal *kind = jr_get(gen, "kind");
    if (!kind || kind->type != JR_STR || kind->n != 4 || memcmp(kind->s, "mesh", 4)) return 0;
    static const char *const F[] = {"positions", "indices"};
    for (int k = 0; k < 2; k++) {
        const JrVal *v = jr_get(gen, F[k]);
        if (!v || v->type != JR_STR) continue;
        if (*nref >= cap) return -1;
        refs[(*nref)++] = (MeshRef){v->off, v->s};
    }
    return 0;
}

/* Cache of the mesh arrays read for one tree, so a shape shared by many elements is read once. */
typedef struct {
    char hash[65];
    char *bytes;
    size_t n;
} MeshObj;
typedef struct {
    MeshObj *v;
    size_t n, cap;
} MeshCache;

static void cache_free(MeshCache *c) {
    for (size_t i = 0; i < c->n; i++) free(c->v[i].bytes);
    free(c->v);
}

static const MeshObj *mesh_get(const cc_repo *r, MeshCache *c, const char *hash, Buf *err) {
    for (size_t i = 0; i < c->n; i++)
        if (!strcmp(c->v[i].hash, hash)) return &c->v[i];
    Buf b;
    buf_init(&b);
    if (!is_hash(hash) || cc_repo_get_bytes(r, hash, &b, NULL)) {
        buf_free(&b);
        if (err) {
            buf_clear(err);
            buf_puts(err, "mesh object ");
            buf_puts(err, hash);
            buf_puts(err, " missing or damaged");
        }
        return NULL;
    }
    if (c->n == c->cap) {
        size_t cap = c->cap ? 2 * c->cap : 16;
        MeshObj *v = realloc(c->v, cap * sizeof *v);
        if (!v) return buf_free(&b), err_set(err, "out of memory"), NULL;
        c->v = v, c->cap = cap;
    }
    MeshObj *m = &c->v[c->n++];
    memcpy(m->hash, hash, 65);
    m->bytes = b.p, m->n = b.len; /* owns the buffer */
    return m;
}

/* Appends one element object to the model text, its mesh references replaced by the arrays. */
static int put_element(const cc_repo *r, MeshCache *mc, const char *hash, const char *text, size_t n, Buf *out,
                       Buf *err) {
    if (!find_bytes(text, n, "\"mesh\"", 6)) {
        buf_append(out, text, n);
        return 0;
    }
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *o = jr_parse(&a, text, n, &je);
    int rc = -1;
    if (!o) {
        damaged(err, hash, strlen(hash), je.msg);
        goto done;
    }
    MeshRef refs[64];
    size_t nref = 0;
    if (o->type == JR_OBJ) {
        if (collect_refs(jr_get(o, "generator"), refs, &nref, 64)) goto too_many;
        const JrVal *ops = jr_get(o, "openings");
        if (ops && ops->type == JR_ARR)
            for (size_t i = 0; i < ops->n; i++)
                if (ops->items[i].type == JR_OBJ && collect_refs(jr_get(&ops->items[i], "generator"), refs, &nref, 64))
                    goto too_many;
    }
    /* In document order (offsets grow). */
    for (size_t i = 1; i < nref; i++)
        for (size_t j = i; j > 0 && refs[j].off < refs[j - 1].off; j--) {
            MeshRef t = refs[j];
            refs[j] = refs[j - 1], refs[j - 1] = t;
        }
    size_t at = 0;
    for (size_t i = 0; i < nref; i++) {
        size_t off = refs[i].off;
        /* A plain reference is the 64 hex digits between quotes, nothing escaped. */
        if (off + 66 > n || text[off] != '"' || text[off + 65] != '"' || memcmp(text + off + 1, refs[i].hash, 64) ||
            strlen(refs[i].hash) != 64) {
            Buf w;
            buf_init(&w);
            buf_puts(&w, "mesh object ");
            buf_puts(&w, refs[i].hash);
            buf_puts(&w, " missing or damaged");
            damaged(err, hash, strlen(hash), buf_cstr(&w));
            buf_free(&w);
            goto done;
        }
        Buf why;
        buf_init(&why);
        const MeshObj *m = mesh_get(r, mc, refs[i].hash, &why);
        if (!m) {
            damaged(err, hash, strlen(hash), buf_cstr(&why));
            buf_free(&why);
            goto done;
        }
        buf_free(&why);
        buf_append(out, text + at, off - at);
        buf_append(out, m->bytes, m->n);
        at = off + 66;
    }
    buf_append(out, text + at, n - at);
    rc = 0;
    goto done;
too_many:
    damaged(err, hash, strlen(hash), "has too many mesh references");
done:
    arena_free(&a);
    return rc;
}

/* A map of the tree (GlobalId -> object hash) written as the model's map of objects. */
static int put_map(const cc_repo *r, MeshCache *mc, const char *tree, const JrVal *map, const char *key, int elements,
                   Buf *out, Buf *err) {
    if (!map) return 0;
    if (map->type != JR_OBJ) return bad_type(tree, key, "a map", err);
    buf_putc(out, ',');
    json_escape(out, key, strlen(key));
    buf_puts(out, ":{");
    Buf obj;
    buf_init(&obj);
    for (size_t i = 0; i < map->n; i++) {
        const JrMember *m = &map->members[i];
        if (m->val.type != JR_STR) {
            buf_free(&obj);
            return bad_type(tree, key, "a map of hashes", err);
        }
        if (cc_repo_get_bytes(r, m->val.s, &obj, err)) return buf_free(&obj), -1;
        if (i) buf_putc(out, ',');
        json_escape(out, m->key, m->key_n);
        buf_putc(out, ':');
        if (elements) {
            if (put_element(r, mc, m->val.s, obj.p ? obj.p : "", obj.len, out, err)) return buf_free(&obj), -1;
        } else
            buf_append(out, obj.p, obj.len);
        if (out->len > CC_MAX_FILE_BYTES) {
            buf_free(&obj);
            return damaged(err, tree, strlen(tree), "is too large");
        }
    }
    buf_free(&obj);
    buf_putc(out, '}');
    return 0;
}

int cc_repo_read_tree(const cc_repo *r, const char *tree, cc_model *m, Buf *err) {
    cc_model_init(m);
    Arena a;
    arena_init(&a, 0);
    Buf text;
    buf_init(&text);
    MeshCache mc = {0};
    int rc = -1;
    const JrVal *t = get_dom(r, tree, &a, err);
    if (!t) goto done;
    static const char *const F[] = {"project_id", "project_name", "storeys", "materials", "elements", "views",
                                    "sheets",     "project",      "schedules", "georef",   "meshes",
                                    "relations",  "grids" /* H2a: the C model's own sections (DEVIATIONS 26) */};
    if (check_fields(t, F, 13, tree, "Tree", err)) goto done;
    static const char *const REQ[] = {"project_id", "project_name", "storeys", "materials", "elements"};
    for (int i = 0; i < 5; i++)
        if (!jr_get(t, REQ[i])) {
            missing_field(tree, REQ[i], err);
            goto done;
        }
    const JrVal *pid = jr_get(t, "project_id"), *pname = jr_get(t, "project_name");
    if (pid->type != JR_STR || !cc_gid_valid(pid->s, pid->n)) {
        bad_type(tree, "project_id", "an IFC GlobalId", err);
        goto done;
    }
    if (pname->type != JR_STR) {
        bad_type(tree, "project_name", "a string", err);
        goto done;
    }
    const JrVal *meshes = jr_get(t, "meshes");
    if (meshes && meshes->type != JR_ARR) {
        bad_type(tree, "meshes", "a sequence", err);
        goto done;
    }
    buf_puts(&text, "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":");
    write_raw(pid, &text);
    buf_puts(&text, ",\"project_name\":");
    write_raw(pname, &text);
    if (put_map(r, &mc, tree, jr_get(t, "storeys"), "storeys", 0, &text, err) ||
        put_map(r, &mc, tree, jr_get(t, "materials"), "materials", 0, &text, err) ||
        put_map(r, &mc, tree, jr_get(t, "elements"), "elements", 1, &text, err) ||
        put_map(r, &mc, tree, jr_get(t, "views"), "views", 0, &text, err) ||
        put_map(r, &mc, tree, jr_get(t, "sheets"), "sheets", 0, &text, err))
        goto done;
    static const char *const INLINE[] = {"project", "schedules", "georef"};
    for (int i = 0; i < 3; i++) {
        const JrVal *v = jr_get(t, INLINE[i]);
        if (!v) continue;
        buf_putc(&text, ',');
        json_escape(&text, INLINE[i], strlen(INLINE[i]));
        buf_putc(&text, ':');
        write_raw(v, &text);
    }
    /* ---- H2a: relations and grids, objects as the others (DEVIATIONS 26) ---- */
    if (put_map(r, &mc, tree, jr_get(t, "relations"), "relations", 0, &text, err) ||
        put_map(r, &mc, tree, jr_get(t, "grids"), "grids", 0, &text, err))
        goto done;
    /* ---- end H2a ---- */
    buf_puts(&text, "}}");
    if (text.oom) {
        err_set(err, "io: out of memory");
        goto done;
    }
    cc_error e;
    cc_model_free(m);
    if (cc_model_load(m, text.p, text.len, &e)) {
        damaged(err, tree, strlen(tree), e.msg);
        goto done;
    }
    rc = 0;
done:
    cache_free(&mc);
    buf_free(&text);
    arena_free(&a);
    return rc;
}

/* ---------------------------------------------------------------- refs */

int cc_repo_check_branch_name(const char *name, size_t n, Buf *err) {
    int ok = n > 0 && n <= 100 && !(n == 4 && !memcmp(name, "HEAD", 4));
    for (size_t i = 0; ok && i <= n;) {
        size_t j = i;
        while (j < n && name[j] != '/') j++;
        /* part name[i..j) */
        if (j == i || name[i] == '.' || name[i] == '-') ok = 0;
        for (size_t k = i; ok && k < j; k++) {
            unsigned char c = (unsigned char)name[k];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' ||
                  c == '_' || c == '.'))
                ok = 0;
            if (k + 1 < j && c == '.' && name[k + 1] == '.') ok = 0;
        }
        i = j + 1;
    }
    if (ok) return 0;
    if (err) {
        buf_clear(err);
        buf_puts(err, "invalid branch name ");
        cc_buf_debug_str(err, name, n);
        buf_puts(err, ": use letters, digits, '-', '_', '.' and '/'");
    }
    return -1;
}

int cc_repo_head(const cc_repo *r, cc_repo_head_t *h, Buf *err) {
    memset(h, 0, sizeof *h);
    Buf p, s;
    buf_init(&p), buf_init(&s);
    int rc = -1;
    if (read_small(r, path_of(r, &p, "HEAD"), &s, err)) goto done;
    if (s.len >= 5 && !memcmp(s.p, "ref: ", 5)) {
        if (cc_repo_check_branch_name(s.p + 5, s.len - 5, err)) goto done;
        memcpy(h->branch, s.p + 5, s.len - 5);
        h->branch[s.len - 5] = 0;
        rc = 0;
    } else if (is_hash_n(s.p, s.len)) {
        h->detached = 1;
        memcpy(h->commit.h, s.p, 64);
        h->commit.h[64] = 0;
        rc = 0;
    } else
        err_set(err, "HEAD is damaged");
done:
    buf_free(&p), buf_free(&s);
    return rc;
}

int cc_repo_branch_commit(const cc_repo *r, const char *name, cc_hash *out, int *found, Buf *err) {
    *found = 0;
    size_t n = strlen(name);
    if (cc_repo_check_branch_name(name, n, err)) return -1;
    Buf rel, p, s;
    buf_init(&rel), buf_init(&p), buf_init(&s);
    buf_puts(&rel, "refs/heads/");
    buf_puts(&rel, name);
    const char *path = path_of(r, &p, buf_cstr(&rel));
    int rc = 0;
    if (!r->pf->exists(r->pf->ctx, path)) goto done;
    if (read_small(r, path, &s, err)) {
        rc = -1;
        goto done;
    }
    if (!is_hash_n(s.p, s.len)) {
        if (err) buf_clear(err), buf_puts(err, "branch "), buf_puts(err, name), buf_puts(err, " is damaged");
        rc = -1;
        goto done;
    }
    memcpy(out->h, s.p, 64);
    out->h[64] = 0;
    *found = 1;
done:
    buf_free(&rel), buf_free(&p), buf_free(&s);
    return rc;
}

int cc_repo_head_commit(const cc_repo *r, cc_hash *out, int *found, Buf *err) {
    cc_repo_head_t h;
    if (cc_repo_head(r, &h, err)) return -1;
    if (h.detached) {
        *out = h.commit;
        *found = 1;
        return 0;
    }
    return cc_repo_branch_commit(r, h.branch, out, found, err);
}

/* list_dir through the platform: entries as "d<name>\0" / "f<name>\0". NULL list_dir (the wasm
 * module): no folders. */
#define LIST_MAX ((size_t)16 << 20)
static int list_dir(const cc_repo *r, const char *path, char **names, size_t *n) {
    *names = NULL, *n = 0;
    if (!r->pf->list_dir) return CC_PF_NOT_FOUND;
    return r->pf->list_dir(r->pf->ctx, path, LIST_MAX, names, n);
}

static int cmp_branch(const void *a, const void *b) {
    return strcmp(((const cc_branch *)a)->name, ((const cc_branch *)b)->name);
}

int cc_repo_branches(const cc_repo *r, Arena *a, cc_branch **out, size_t *nout, Buf *err) {
    *out = NULL, *nout = 0;
    /* Folders to visit: relative paths under refs/heads ("" the root), with their depth. */
    typedef struct {
        char *rel;
        int depth;
    } Dir;
    Dir *stack = NULL;
    size_t ns = 0, caps = 0, cap = 0, visited = 0;
    cc_branch *v = NULL;
    int rc = -1;
    Buf p, rel;
    buf_init(&p), buf_init(&rel);
    stack = malloc(16 * sizeof *stack);
    if (!stack) goto oom;
    caps = 16;
    stack[ns++] = (Dir){dupz("", 0), 0};
    if (!stack[0].rel) goto oom;
    while (ns) {
        Dir d = stack[--ns];
        if (d.depth > 16 || ++visited > 4096) {
            free(d.rel);
            continue;
        }
        buf_clear(&rel);
        buf_puts(&rel, "refs/heads");
        if (*d.rel) buf_putc(&rel, '/'), buf_puts(&rel, d.rel);
        char *names = NULL;
        size_t nn = 0;
        int st = list_dir(r, path_of(r, &p, buf_cstr(&rel)), &names, &nn);
        if (st != CC_PF_OK) {
            free(d.rel);
            /* No refs/heads (or no folders at all, the wasm module): no branches. Rust's
             * read_dir fails there instead, on a folder its own init always makes. */
            if (d.depth == 0 && st == CC_PF_NOT_FOUND) {
                rc = 0;
                goto done;
            }
            io_err(err, st);
            goto done;
        }
        for (size_t i = 0; i < nn;) {
            const char *e = names + i;
            size_t el = strlen(e);
            i += el + 1;
            if (el < 2) continue;
            Buf name;
            buf_init(&name);
            if (*d.rel) buf_puts(&name, d.rel), buf_putc(&name, '/');
            buf_puts(&name, e + 1);
            const char *nm = buf_cstr(&name);
            if (name.oom) {
                buf_free(&name), free(names), free(d.rel);
                goto oom;
            }
            if (e[0] == 'd') {
                if (ns == caps) {
                    Dir *s2 = realloc(stack, 2 * caps * sizeof *s2);
                    if (!s2) {
                        buf_free(&name), free(names), free(d.rel);
                        goto oom;
                    }
                    stack = s2, caps *= 2;
                }
                stack[ns++] = (Dir){dupz(nm, name.len), d.depth + 1};
                buf_free(&name);
                continue;
            }
            if ((name.len >= 4 && !memcmp(nm + name.len - 4, ".tmp", 4)) ||
                cc_repo_check_branch_name(nm, name.len, NULL)) {
                buf_free(&name);
                continue;
            }
            cc_hash h;
            int found;
            if (cc_repo_branch_commit(r, nm, &h, &found, err)) {
                buf_free(&name), free(names), free(d.rel);
                goto done;
            }
            if (found) {
                if (*nout == cap) {
                    size_t c2 = cap ? 2 * cap : 8;
                    cc_branch *v2 = realloc(v, c2 * sizeof *v2);
                    if (!v2) {
                        buf_free(&name), free(names), free(d.rel);
                        goto oom;
                    }
                    v = v2, cap = c2;
                }
                v[*nout].name = arena_strz(a, nm, name.len);
                v[*nout].commit = h;
                if (!v[*nout].name) {
                    buf_free(&name), free(names), free(d.rel);
                    goto oom;
                }
                (*nout)++;
            }
            buf_free(&name);
        }
        free(names);
        free(d.rel);
    }
    rc = 0;
    goto done;
oom:
    err_set(err, "io: out of memory");
done:
    while (ns) free(stack[--ns].rel);
    free(stack);
    buf_free(&p), buf_free(&rel);
    if (rc == 0 && *nout) {
        qsort(v, *nout, sizeof *v, cmp_branch);
        *out = dup8(a, v, *nout * sizeof *v);
        if (!*out) rc = -1, err_set(err, "io: out of memory");
    }
    if (rc) *nout = 0;
    free(v);
    return rc;
}

/* ---------------------------------------------------------------- revisions */

/* The commit whose hash starts with `prefix` (lower case, at least 4 hex digits); only commits
 * count. *found 0: none. */
static int find_prefix(const cc_repo *r, const char *prefix, cc_hash *out, int *found, Buf *err) {
    *found = 0;
    char rel[16] = "objects/";
    rel[8] = prefix[0], rel[9] = prefix[1], rel[10] = 0;
    Buf p;
    buf_init(&p);
    char *names = NULL;
    size_t nn = 0;
    int st = list_dir(r, path_of(r, &p, rel), &names, &nn);
    buf_free(&p);
    if (st == CC_PF_NOT_FOUND) return 0;
    if (st != CC_PF_OK) return io_err(err, st), -1;
    size_t plen = strlen(prefix);
    int rc = 0;
    Arena a;
    arena_init(&a, 0);
    for (size_t i = 0; i < nn;) {
        const char *e = names + i;
        size_t el = strlen(e);
        i += el + 1;
        if (el != 63) continue; /* the type letter and 62 digits */
        char name[65];
        name[0] = prefix[0], name[1] = prefix[1];
        memcpy(name + 2, e + 1, 62);
        name[64] = 0;
        if (memcmp(name, prefix, plen) || !is_hash(name)) continue;
        cc_commit c;
        arena_reset(&a);
        if (cc_repo_commit_info(r, name, &a, &c, NULL)) continue;
        if (*found) {
            if (err) buf_clear(err), buf_puts(err, prefix), buf_puts(err, " is ambiguous");
            rc = -1;
            break;
        }
        memcpy(out->h, name, 65);
        *found = 1;
    }
    arena_free(&a);
    free(names);
    return rc;
}

/* The first parent of a commit; *has 0 for a root commit. A parent that is not 64 bytes long is
 * no hash: Rust reports it on the next read ("is not a hash"), here at once. */
static int first_parent(const cc_repo *r, const char *h, cc_hash *out, int *has, Buf *err) {
    Arena a;
    arena_init(&a, 0);
    cc_commit c;
    int rc = cc_repo_commit_info(r, h, &a, &c, err);
    *has = 0;
    if (!rc && c.nparents) {
        size_t n = strlen(c.parents[0]);
        if (n != 64) rc = damaged(err, c.parents[0], n, "is not a hash");
        else
            memcpy(out->h, c.parents[0], 65), *has = 1;
    }
    arena_free(&a);
    return rc;
}

int cc_repo_resolve(const cc_repo *r, const char *rev, cc_hash *out, Buf *err) {
    const char *tilde = NULL;
    for (const char *q = rev; *q; q++)
        if (*q == '~') {
            tilde = q;
            break;
        }
    size_t blen = tilde ? (size_t)(tilde - rev) : strlen(rev);
    uint64_t back = 0;
    if (tilde) {
        const char *s = tilde + 1;
        if (!*s) back = 1;
        else {
            /* usize::from_str: an optional '+', then digits, no overflow. */
            if (*s == '+') s++;
            if (!*s) return unknown_rev(err, rev);
            for (; *s; s++) {
                if (*s < '0' || *s > '9') return unknown_rev(err, rev);
                uint64_t d = (uint64_t)(*s - '0');
                if (back > (UINT64_MAX - d) / 10) return unknown_rev(err, rev);
                back = back * 10 + d;
            }
        }
    }
    char *base = dupz(rev, blen);
    if (!base) return err_set(err, "io: out of memory"), -1;
    int found = 0, rc = -1;
    if (blen == 0 || !strcmp(base, "HEAD")) {
        if (cc_repo_head_commit(r, out, &found, err)) goto done;
        if (!found) {
            unknown_rev(err, rev);
            goto done;
        }
    } else {
        if (!cc_repo_check_branch_name(base, blen, NULL)) {
            if (cc_repo_branch_commit(r, base, out, &found, err)) goto done;
        }
        if (!found) {
            int hex = blen >= 4;
            for (size_t i = 0; hex && i < blen; i++) hex = is_hex_any((unsigned char)base[i]);
            if (!hex || blen > 64) {
                unknown_rev(err, rev);
                goto done;
            }
            for (size_t i = 0; i < blen; i++)
                if (base[i] >= 'A' && base[i] <= 'F') base[i] = (char)(base[i] - 'A' + 'a');
            if (find_prefix(r, base, out, &found, err)) goto done;
            if (!found) {
                unknown_rev(err, rev);
                goto done;
            }
        }
    }
    if (back > CC_REPO_MAX_WALK) back = CC_REPO_MAX_WALK;
    for (uint64_t i = 0; i < back; i++) {
        int has;
        cc_hash p;
        if (first_parent(r, out->h, &p, &has, err)) goto done;
        if (!has) {
            unknown_rev(err, rev);
            goto done;
        }
        *out = p;
    }
    rc = 0;
done:
    free(base);
    return rc;
}

/* The model of a commit (its tree, read). */
static int model_of_commit(const cc_repo *r, const char *h, cc_model *m, Buf *err) {
    Arena a;
    arena_init(&a, 0);
    cc_commit c;
    int rc;
    if (cc_repo_commit_info(r, h, &a, &c, err)) rc = (cc_model_init(m), -1);
    else
        rc = cc_repo_read_tree(r, c.tree, m, err);
    arena_free(&a);
    return rc;
}

int cc_repo_model_at(const cc_repo *r, const char *rev, cc_model *m, Buf *err) {
    cc_hash h;
    if (cc_repo_resolve(r, rev, &h, err)) return cc_model_init(m), -1;
    return model_of_commit(r, h.h, m, err);
}

int cc_repo_log(const cc_repo *r, const char *rev, size_t limit, Arena *a, cc_log_entry **out, size_t *n, Buf *err) {
    *out = NULL, *n = 0;
    cc_hash h;
    if (cc_repo_resolve(r, rev, &h, err)) return -1;
    if (limit > CC_REPO_MAX_WALK) limit = CC_REPO_MAX_WALK;
    cc_log_entry *v = NULL;
    size_t cap = 0;
    int has = 1, rc = 0;
    while (has && *n < limit) {
        if (*n == cap) {
            size_t c2 = cap ? 2 * cap : 16;
            cc_log_entry *v2 = realloc(v, c2 * sizeof *v2);
            if (!v2) {
                rc = (err_set(err, "io: out of memory"), -1);
                break;
            }
            v = v2, cap = c2;
        }
        cc_log_entry *e = &v[*n];
        e->hash = h;
        if (cc_repo_commit_info(r, h.h, a, &e->c, err)) {
            rc = -1;
            break;
        }
        (*n)++;
        has = e->c.nparents > 0;
        if (has && *n < limit) {
            size_t pl = strlen(e->c.parents[0]);
            if (pl != 64) {
                rc = damaged(err, e->c.parents[0], pl, "is not a hash");
                break;
            }
            memcpy(h.h, e->c.parents[0], 65);
        }
    }
    if (!rc && *n) {
        *out = dup8(a, v, *n * sizeof *v);
        if (!*out) rc = (err_set(err, "io: out of memory"), -1);
    }
    free(v);
    if (rc) *out = NULL, *n = 0;
    return rc;
}

int cc_repo_status(const cc_repo *r, const cc_model *working, cc_changes *out, Buf *err) {
    cc_hash h;
    int found;
    if (cc_repo_head_commit(r, &h, &found, err)) return -1;
    cc_model base;
    if (found) {
        if (model_of_commit(r, h.h, &base, err)) return cc_model_free(&base), -1;
    } else {
        /* Before the first commit: an empty model with the working one's project. */
        cc_model_init(&base);
        base.project_id = working->project_id;
        base.project_name = working->project_name; /* borrowed: base is only read, its arena frees its own */
    }
    int rc = cc_diff(&base, working, out, err);
    cc_model_free(&base);
    return rc;
}
