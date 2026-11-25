/* The settings file and the recent files (ticket FS1): settings_file.h. */
#include "settings_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena.h"
#include "json_r.h"
#include "json_w.h"
#include "session.h"
#include "settings.h" /* SET1 */

#ifndef CC_VIEW_ONLY /* the viewer-only wasm (build.sh wasm-view) has no files: the stubs at the end */

static char *dup_n(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p) memcpy(p, s, n), p[n] = 0;
    return p;
}

/* settings.rs from_json `recent`: the strings, not empty, <= 4096 bytes, the first 12. */
void cc_settings_recent_of(const JrVal *v, char **paths, size_t *n) {
    const JrVal *r = v && v->type == JR_OBJ ? jr_get(v, "recent") : NULL;
    /* serde_json keeps a repeated key's last value: the last "recent" */
    for (size_t i = 0; v && v->type == JR_OBJ && i < v->n; i++)
        if (jr_key_is(&v->members[i], "recent")) r = &v->members[i].val;
    if (!r || r->type != JR_ARR) return;
    for (size_t i = 0; i < r->n && *n < CC_RECENT_MAX; i++) {
        const JrVal *p = &r->items[i];
        if (p->type != JR_STR || !p->n || p->n > 4096) continue;
        char *c = dup_n(p->s, p->n);
        if (c) paths[(*n)++] = c;
    }
}

int cc_settings_recent_read(const cc_platform *pf, const char *file, char **paths, size_t *n) {
    *n = 0;
    char *text;
    size_t len;
    int st = pf->read_file(pf->ctx, file, CC_SETTINGS_MAX_BYTES, &text, &len);
    if (st == CC_PF_NOT_FOUND) return 0;
    if (st != CC_PF_OK) return -1;
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = jr_parse(&a, text, len, &je);
    /* settings.rs: broken JSON reads as the defaults (an empty list) */
    if (v) cc_settings_recent_of(v, paths, n);
    arena_free(&a);
    free(text);
    return v ? 0 : -1;
}

/* ---- the canonical writer: serde_json's compact form of a Value (a BTreeMap: keys sorted) ---- */

typedef struct {
    const JrMember *m;
    size_t i;
} KeyAt;

static int key_cmp(const void *pa, const void *pb) {
    const KeyAt *a = pa, *b = pb;
    size_t k = a->m->key_n < b->m->key_n ? a->m->key_n : b->m->key_n;
    int c = memcmp(a->m->key, b->m->key, k);
    if (c) return c;
    if (a->m->key_n != b->m->key_n) return a->m->key_n < b->m->key_n ? -1 : 1;
    return a->i < b->i ? -1 : a->i > b->i; /* the document's order: the last one wins below */
}

static int same_key(const JrMember *a, const JrMember *b) {
    return a->key_n == b->key_n && !memcmp(a->key, b->key, a->key_n);
}

/* `m` (at the top level only): the members that replace the file's of the same key (SET1). */
static int put_value(Buf *b, const JrVal *v, const cc_settings_member *m, size_t nm, int top);

static void put_recent(Buf *b, char *const *paths, size_t n) {
    buf_putc(b, '[');
    for (size_t i = 0; i < n; i++) {
        if (i) buf_putc(b, ',');
        json_escape(b, paths[i], strlen(paths[i]));
    }
    buf_putc(b, ']');
}

/* A file member's key against a replacing member's: bytewise, then the shorter first. */
static int key_vs(const JrMember *a, const char *k) {
    size_t kn = strlen(k), n = a->key_n < kn ? a->key_n : kn;
    int c = memcmp(a->key, k, n);
    if (c) return c;
    return a->key_n < kn ? -1 : a->key_n > kn;
}

static void put_member(Buf *b, const cc_settings_member *m, int *first) {
    if (!*first) buf_putc(b, ',');
    *first = 0;
    json_escape(b, m->key, strlen(m->key));
    buf_putc(b, ':');
    buf_append(b, m->json, m->json_n);
}

static int put_object(Buf *b, const JrVal *v, const cc_settings_member *m, size_t nm, int top) {
    KeyAt *k = malloc((v->n ? v->n : 1) * sizeof *k);
    if (!k) return -1;
    for (size_t i = 0; i < v->n; i++) k[i] = (KeyAt){&v->members[i], i};
    qsort(k, v->n, sizeof *k, key_cmp);
    if (!top) nm = 0;
    size_t j = 0;
    int first = 1, rc = 0;
    buf_putc(b, '{');
    for (size_t i = 0; i < v->n && !rc; i++) {
        if (i + 1 < v->n && same_key(k[i].m, k[i + 1].m)) continue; /* a later one of the same key */
        while (j < nm && key_vs(k[i].m, m[j].key) > 0) put_member(b, &m[j++], &first); /* added where it sorts */
        if (j < nm && key_vs(k[i].m, m[j].key) == 0) { /* replaced */
            put_member(b, &m[j++], &first);
            continue;
        }
        if (!first) buf_putc(b, ',');
        first = 0;
        json_escape(b, k[i].m->key, k[i].m->key_n);
        buf_putc(b, ':');
        rc = put_value(b, &k[i].m->val, NULL, 0, 0);
    }
    while (!rc && j < nm) put_member(b, &m[j++], &first);
    buf_putc(b, '}');
    free(k);
    return rc;
}

static int put_value(Buf *b, const JrVal *v, const cc_settings_member *m, size_t nm, int top) {
    switch (v->type) {
    case JR_NULL: buf_puts(b, "null"); return 0;
    case JR_BOOL: buf_puts(b, v->b ? "true" : "false"); return 0;
    case JR_NUM: buf_append(b, v->s, v->n); return 0; /* the file's own text: exact */
    case JR_STR: json_escape(b, v->s, v->n); return 0;
    case JR_ARR:
        buf_putc(b, '[');
        for (size_t i = 0; i < v->n; i++) {
            if (i) buf_putc(b, ',');
            if (put_value(b, &v->items[i], NULL, 0, 0)) return -1;
        }
        buf_putc(b, ']');
        return 0;
    case JR_OBJ: return put_object(b, v, m, nm, top); /* depth: jr_parse's bound */
    }
    return -1;
}

int cc_settings_merge(const char *text, size_t n, const cc_settings_member *m, size_t nm, Buf *out, char *why) {
    if (!text || !n) {
        int first = 1;
        buf_putc(out, '{');
        for (size_t j = 0; j < nm; j++) put_member(out, &m[j], &first);
        buf_putc(out, '}');
        return out->oom ? -1 : 0;
    }
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = jr_parse(&a, text, n, &je);
    int rc = -1;
    if (!v || v->type != JR_OBJ) snprintf(why, 128, "%s", v ? "not a JSON object" : je.msg);
    else rc = put_value(out, v, m, nm, 1);
    arena_free(&a);
    return rc || out->oom ? -1 : 0;
}

/* The folders of `file`, made one by one (settings.rs write: create_dir_all). */
static void make_parents(const cc_platform *pf, const char *file) {
    if (!pf->make_dir) return;
    size_t n = strlen(file);
    char *p = dup_n(file, n);
    if (!p) return;
    for (size_t i = 1; i < n; i++)
        if (p[i] == '/' || p[i] == '\\') {
            char c = p[i];
            p[i] = 0;
            if (!(i == 2 && p[1] == ':')) pf->make_dir(pf->ctx, p); /* not "C:" */
            p[i] = c;
        }
    free(p);
}

int cc_settings_write(const cc_platform *pf, const char *file, const cc_settings_member *m, size_t nm) {
    char *text = NULL;
    size_t len = 0;
    int st = pf->read_file(pf->ctx, file, CC_SETTINGS_MAX_BYTES, &text, &len);
    Buf b;
    buf_init(&b);
    int rc = 0;
    char why[128] = "";
    if (st == CC_PF_NOT_FOUND) {
        make_parents(pf, file);
        rc = cc_settings_merge(NULL, 0, m, nm, &b, why);
    } else if (st != CC_PF_OK) {
        cc_log(pf, "bimble-ccore: %s: cannot read the settings: the change is kept in memory", file);
        rc = -1;
    } else if (cc_settings_merge(text, len, m, nm, &b, why)) {
        cc_log(pf, "bimble-ccore: %s: not a JSON object (%s): left alone, the change is kept in memory", file, why);
        rc = -1;
    }
    free(text);
    if (!rc && b.oom) rc = -1;
    if (!rc && pf->write_file(pf->ctx, file, b.p, b.len)) {
        cc_log(pf, "bimble-ccore: %s: cannot write the settings: the change is kept in memory", file);
        rc = -1;
    }
    buf_free(&b);
    return rc;
}

int cc_settings_recent_write(const cc_platform *pf, const char *file, char *const *paths, size_t n) {
    Buf r;
    buf_init(&r);
    put_recent(&r, paths, n);
    cc_settings_member m = {"recent", r.p ? r.p : "[]", r.p ? r.len : 2};
    int rc = r.oom ? -1 : cc_settings_write(pf, file, &m, 1);
    buf_free(&r);
    return rc;
}

/* ---- the session's list ---- */

static void list_clear(cc_session *s) {
    for (size_t i = 0; i < s->nrecent_files; i++) free(s->recent_files[i]);
    s->nrecent_files = 0;
}

void cc_recent_free(cc_session *s) {
    cc_settings_free(s); /* SET1: the settings follow the file */
    list_clear(s);
    free(s->settings_file);
    s->settings_file = NULL;
}

void cc_recent_setup(cc_session *s, const char *file) {
    cc_recent_free(s);
    if (!file || !s->pf->files) return;
    s->settings_file = dup_n(file, strlen(file));
    if (cc_settings_recent_read(s->pf, file, s->recent_files, &s->nrecent_files))
        cc_log(s->pf, "bimble-ccore: %s: the settings do not read: no recent files from it", file);
}

static int absolute(const char *p) {
    return p[0] == '/' || p[0] == '\\' || (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':' && (p[2] == '/' || p[2] == '\\'));
}

/* The file's list when it reads, else the one in memory. */
static void reread(cc_session *s) {
    if (!s->settings_file) return;
    char *paths[CC_RECENT_MAX];
    size_t n;
    if (cc_settings_recent_read(s->pf, s->settings_file, paths, &n)) return;
    list_clear(s);
    memcpy(s->recent_files, paths, n * sizeof *paths);
    s->nrecent_files = n;
}

static int find(const cc_session *s, const char *path) {
    for (size_t i = 0; i < s->nrecent_files; i++)
        if (!strcmp(s->recent_files[i], path)) return (int)i;
    return -1;
}

void cc_recent_opened(cc_session *s, const char *path) {
    if (!s->pf->files || !path || !*path || strlen(path) > 4096 || !absolute(path)) return;
    reread(s);
    char *c = dup_n(path, strlen(path));
    if (!c) return;
    int at = find(s, path);
    if (at >= 0) { /* retain(|p| p != path) */
        free(s->recent_files[at]);
        memmove(s->recent_files + at, s->recent_files + at + 1, (s->nrecent_files - (size_t)at - 1) * sizeof *s->recent_files);
        s->nrecent_files--;
    }
    if (s->nrecent_files == CC_RECENT_MAX) free(s->recent_files[--s->nrecent_files]); /* truncate(RECENT) */
    memmove(s->recent_files + 1, s->recent_files, s->nrecent_files * sizeof *s->recent_files);
    s->recent_files[0] = c, s->nrecent_files++;
    if (s->settings_file) cc_settings_recent_write(s->pf, s->settings_file, s->recent_files, s->nrecent_files);
}

int cc_recent_remove(cc_session *s, const char *path) {
    reread(s);
    int at = find(s, path);
    if (at < 0) return -1;
    free(s->recent_files[at]);
    memmove(s->recent_files + at, s->recent_files + at + 1, (s->nrecent_files - (size_t)at - 1) * sizeof *s->recent_files);
    s->nrecent_files--;
    if (s->settings_file) cc_settings_recent_write(s->pf, s->settings_file, s->recent_files, s->nrecent_files);
    return 0;
}

static const char *ext_of(const char *name) {
    const char *d = strrchr(name, '.');
    return d && d != name ? d + 1 : "";
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if ((*a | 32) != (*b | 32)) return 0;
    return !*a && !*b;
}

void cc_recent_json(cc_session *s, Buf *out) {
    reread(s);
    JsonW w;
    jw_init(&w, out, 0);
    jw_arr_begin(&w);
    for (size_t i = 0; i < s->nrecent_files; i++) {
        const char *p = s->recent_files[i], *slash = strrchr(p, '/'), *bs = strrchr(p, '\\');
        if (bs && (!slash || bs > slash)) slash = bs;
        const char *name = slash ? slash + 1 : p;
        const char *e = ext_of(name);
        static const char *const KINDS[] = {"bimble", "ifc", "bimbleview", "json", "pdf", "dxf"};
        const char *kind = "other";
        for (size_t k = 0; k < sizeof KINDS / sizeof KINDS[0]; k++)
            if (ieq(e, KINDS[k])) kind = KINDS[k];
        cc_pf_stat st;
        int has = s->pf->stat_path && s->pf->stat_path(s->pf->ctx, p, 1, &st) == CC_PF_OK;
        jw_obj_begin(&w);
        jw_key(&w, "exists"), jw_bool(&w, has || s->pf->exists(s->pf->ctx, p));
        jw_key(&w, "folder"), jw_str_n(&w, p, slash ? (size_t)(slash - p) : 0);
        jw_key(&w, "kind"), jw_str(&w, kind);
        jw_key(&w, "mtime");
        if (has) jw_i64(&w, st.mtime_s);
        else jw_null(&w);
        jw_key(&w, "name"), jw_str(&w, name);
        jw_key(&w, "opened_unix"), jw_null(&w); /* calls.rs:262: the settings keep paths only */
        jw_key(&w, "path"), jw_str(&w, p);
        jw_key(&w, "schema");
        if (ieq(e, "bimble")) jw_str(&w, ".bimble");
        else jw_null(&w);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_finish(&w);
}
#else
/* The viewer-only wasm: no folders, no settings file, no recent files (the 400 KB budget). */
int cc_settings_recent_read(const cc_platform *pf, const char *file, char **paths, size_t *n) {
    (void)pf, (void)file, (void)paths;
    *n = 0;
    return 0;
}
int cc_settings_recent_write(const cc_platform *pf, const char *file, char *const *paths, size_t n) {
    (void)pf, (void)file, (void)paths, (void)n;
    return -1;
}
void cc_recent_setup(cc_session *s, const char *file) { (void)s, (void)file; }
void cc_recent_opened(cc_session *s, const char *path) { (void)s, (void)path; }
int cc_recent_remove(cc_session *s, const char *path) { return (void)s, (void)path, -1; }
void cc_recent_json(cc_session *s, Buf *out) { (void)s, buf_puts(out, "[]"); }
void cc_recent_free(cc_session *s) { (void)s; }
#endif
