/* The native host's session: see session.h. */
#define _POSIX_C_SOURCE 200809L
#ifdef __wasm__
#include "wasm_api.h" /* snprintf, strcmp…, freestanding; no files, no clock */
#else
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "command.h"
#include "file.h"
#include "ifc_in.h"
#include "json_r.h"
#include "json_w.h"
#include "session.h"

static cc_session S;

static void persist_if_changed(void);
static int persist_on, persist_busy;

cc_session *cc_session_get(void) {
    if (persist_on && !persist_busy) persist_if_changed();
    return &S;
}

const cc_model *cc_host_model(void) { return S.open ? &S.model : NULL; }
uint32_t cc_host_rev(void) { return (uint32_t)S.rev; }

uint64_t cc_session_step(const cc_session *s) { return s->nundo_ids ? s->undo_ids[s->nundo_ids - 1] : 0; }
int cc_session_unsaved(const cc_session *s) { return s->open && cc_session_step(s) != s->saved_step; }

#ifdef __wasm__
static double now_ms(void) { return 0; } /* no clock in the module: load_ms reads 0 */

/* No files in the module: cc_session_open_text hands the text, read_all copies it. */
static const char *wasm_text;
static size_t wasm_n;

static char *read_all(const char *path, size_t *n, cc_error *err) {
    if (!wasm_text) {
        snprintf(err->msg, sizeof err->msg, "%s: cannot open", path);
        return NULL;
    }
    if (wasm_n > CC_MAX_FILE_BYTES) {
        snprintf(err->msg, sizeof err->msg, "%s: larger than %zu bytes", path, (size_t)CC_MAX_FILE_BYTES);
        return NULL;
    }
    char *p = malloc(wasm_n + 1);
    if (!p) {
        snprintf(err->msg, sizeof err->msg, "%s: %s", path, "out of memory");
        return NULL;
    }
    memcpy(p, wasm_text, wasm_n);
    p[wasm_n] = 0;
    *n = wasm_n;
    return p;
}

int cc_session_open_text(const char *name, const char *text, size_t n, cc_error *err) {
    err->offset = CC_NO_OFFSET;
    if (S.open) { /* mesh.c and calls.c cache per (model, rev): one model per instance */
        snprintf(err->msg, sizeof err->msg, "a model is already open");
        return -1;
    }
    wasm_text = text, wasm_n = n;
    int rc = cc_session_open(name, err);
    wasm_text = NULL, wasm_n = 0;
    return rc;
}
#else
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* The whole file, NUL-terminated, at most CC_MAX_FILE_BYTES. NULL with `err`. */
static char *read_all(const char *path, size_t *n, cc_error *err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err->msg, sizeof err->msg, "%s: cannot open", path);
        return NULL;
    }
    size_t cap = 1 << 16, len = 0;
    char *p = malloc(cap + 1);
    while (p) {
        if (len == cap) {
            if (cap >= CC_MAX_FILE_BYTES) {
                snprintf(err->msg, sizeof err->msg, "%s: larger than %zu bytes", path, (size_t)CC_MAX_FILE_BYTES);
                free(p);
                fclose(f);
                return NULL;
            }
            char *q = realloc(p, 2 * cap + 1);
            if (!q) break;
            p = q, cap *= 2;
        }
        size_t k = fread(p + len, 1, cap - len, f);
        len += k;
        if (k == 0) break;
    }
    int bad = ferror(f);
    fclose(f);
    if (!p || bad) {
        free(p);
        snprintf(err->msg, sizeof err->msg, "%s: %s", path, bad ? "read error" : "out of memory");
        return NULL;
    }
    p[len] = 0;
    *n = len;
    return p;
}
#endif

static int ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    if (n < k) return 0;
    for (size_t i = 0; i < k; i++) {
        char c = s[n - k + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suffix[i]) return 0;
    }
    return 1;
}

/* lib.rs:503-521 editable_copy_path: "<stem>.bimble", "<stem> (imported).bimble",
 * "<stem> (imported 2).bimble"… next to the IFC, the first that does not exist. */
static int editable_copy_path(const char *ifc, char *out, size_t cap) {
    const char *slash = strrchr(ifc, '/');
    size_t dir = slash ? (size_t)(slash - ifc + 1) : 0;
    const char *name = ifc + dir;
    const char *dot = strrchr(name, '.');
    size_t stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    for (unsigned k = 0; k < 100000; k++) {
        int w;
        if (k == 0) w = snprintf(out, cap, "%.*s%.*s.bimble", (int)dir, ifc, (int)stem, name);
        else if (k == 1) w = snprintf(out, cap, "%.*s%.*s (imported).bimble", (int)dir, ifc, (int)stem, name);
        else w = snprintf(out, cap, "%.*s%.*s (imported %u).bimble", (int)dir, ifc, (int)stem, name, k);
        if (w < 0 || (size_t)w >= cap) return -1;
#ifdef __wasm__
        return 0; /* no files: the first name is free */
#else
        if (access(out, F_OK) != 0) return 0;
#endif
    }
    return -1;
}

int cc_session_open(const char *path, cc_error *err) {
    err->offset = CC_NO_OFFSET;
    err->msg[0] = 0;
    double t0 = now_ms();
    size_t n = 0;
    char *text = read_all(path, &n, err);
    if (!text) return -1;
    int ifc = ends_with_ci(path, ".ifc");
    cc_model_init(&S.model);
    int rc;
    if (ifc) {
        cc_ifc_report rep;
        cc_model_free(&S.model);
        rc = cc_ifc_import(&S.model, &rep, text, n, err);
        cc_ifc_report_free(&rep);
        if (rc == 0) { /* lib.rs:529-536: the project takes the file stem, then validate */
            const char *name = strrchr(path, '/');
            name = name ? name + 1 : path;
            const char *dot = strrchr(name, '.');
            size_t k = dot && dot != name ? (size_t)(dot - name) : strlen(name);
            char *p = arena_dup(&S.model.arena, name, k ? k : 1);
            if (p && k) S.model.project_name = (cc_str){p, k};
            cc_error ve;
            if (cc_model_validate(&S.model, &ve)) {
                snprintf(err->msg, sizeof err->msg, "imported model invalid: %s", ve.msg);
                rc = -1;
            }
        }
        if (rc == 0 && editable_copy_path(path, S.path, sizeof S.path)) {
            snprintf(err->msg, sizeof err->msg, "%s: no name for the editable copy", path);
            rc = -1;
        }
    } else {
        cc_model_free(&S.model);
        rc = cc_model_load(&S.model, text, n, err);
        if (rc == 0) {
            if (strlen(path) >= sizeof S.path) {
                snprintf(err->msg, sizeof err->msg, "path too long");
                rc = -1;
            } else strcpy(S.path, path);
        }
    }
    free(text);
    if (rc) {
        cc_model_free(&S.model);
        return -1;
    }
    cc_history_init(&S.hist);
    const char *slash = strrchr(S.path, '/');
    S.file = slash ? slash + 1 : S.path;
    S.schema_bimble = strlen(S.file) > 7 && !strcmp(S.file + strlen(S.file) - 7, ".bimble");
    S.open = 1;
    S.rev = 1;            /* lib.rs:1679 */
    S.next_step = 1;
    S.saved_step = ifc ? UINT64_MAX : 0; /* edit.rs `Editor::imported`: starts dirty */
    /* edit.rs `Editor::new`: the lowest storey at or above -1 mm, else the first one. */
    const cc_storey *best = NULL;
    for (size_t i = 0; i < S.model.nstoreys; i++) {
        const cc_storey *st = &S.model.storeys[i];
        if (st->elevation >= -1.0 && (!best || st->elevation < best->elevation)) best = st;
    }
    if (!best && S.model.nstoreys) best = &S.model.storeys[0];
    if (best) S.has_storey = 1, S.storey = best->id;
    S.kind = CC_KIND_COLUMN;
    double sizes[5] = {300.0, 250.0, 500.0, 200.0, 250.0}; /* verbs.rs:86-96 */
    memcpy(S.sizes, sizes, sizeof sizes);
    S.offset[0] = 1000.0, S.offset[1] = 0.0, S.offset[2] = 0.0;
    S.tab_open = 1; /* the 3D view is shown on load */
    S.load_ms = now_ms() - t0;
    return 0;
}

/* ---- the session file (session.h, ticket T12) ---- */

#ifdef __wasm__
/* No files in the module: persistence is never on. */
static void persist_if_changed(void) {}
int cc_session_persist_start(const char *session_path, const char *model_path) {
    (void)session_path, (void)model_path;
    return 0;
}
void cc_session_persist_now(void) {}
#else

#define SESSION_FORMAT 1
#define SESSION_MAX_LABEL 4096

static char sess_path[4096], sess_model[4096];
static uint64_t sess_fp;
static int sess_has_fp;
static cc_cmds sess_cmds; /* the restored steps' commands: they live as long as the model */

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

/* The model file's size and mtime (ns): Save rewrites it, an outside edit too. */
static int file_stamp(const char *path, uint64_t *size, uint64_t *mtime_ns) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *size = (uint64_t)st.st_size;
    *mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
    return 0;
}

/* Everything the file holds that can change, hashed (the model text changes only with rev). */
static uint64_t fingerprint(void) {
    uint64_t h = 1469598103934665603ull, size = 0, mt = 0;
    file_stamp(sess_model, &size, &mt);
    h = fnv(h, &size, sizeof size), h = fnv(h, &mt, sizeof mt);
    h = fnv(h, &S.open, sizeof S.open), h = fnv(h, &S.rev, sizeof S.rev);
    h = fnv(h, &S.next_step, sizeof S.next_step), h = fnv(h, &S.saved_step, sizeof S.saved_step);
    h = fnv(h, &S.nundo_ids, sizeof S.nundo_ids), h = fnv(h, &S.nredo_ids, sizeof S.nredo_ids);
    if (S.nundo_ids) h = fnv(h, S.undo_ids, S.nundo_ids * sizeof *S.undo_ids);
    if (S.nredo_ids) h = fnv(h, S.redo_ids, S.nredo_ids * sizeof *S.redo_ids);
    h = fnv(h, &S.has_primary, sizeof S.has_primary);
    if (S.has_primary) h = fnv(h, S.primary.c, 22);
    h = fnv(h, &S.nalso, sizeof S.nalso);
    if (S.nalso) h = fnv(h, S.also, S.nalso * sizeof *S.also);
    return h;
}

static void write_steps(JsonW *w, const cc_step *st, const uint64_t *ids, size_t n) {
    jw_arr_begin(w);
    for (size_t i = 0; i < n; i++) {
        Buf c;
        buf_init(&c);
        cc_commands_json(st[i].cmds, st[i].n, &c);
        jw_obj_begin(w);
        jw_key(w, "cmds"), jw_str_n(w, c.p ? c.p : "[]", c.p ? c.len : 2);
        jw_key(w, "id"), jw_u64(w, ids[i]);
        jw_key(w, "label"), jw_str(w, st[i].label ? st[i].label : "");
        jw_obj_end(w);
        if (c.oom) w->b->oom = 1;
        buf_free(&c);
    }
    jw_arr_end(w);
}

/* 0 written; -1 with the reason logged. */
static int write_session(void) {
    uint64_t size = 0, mt = 0;
    if (file_stamp(sess_model, &size, &mt)) {
        fprintf(stderr, "bimble-ccore: session not written: %s: cannot stat\n", sess_model);
        return -1;
    }
    Buf model, b;
    buf_init(&model), buf_init(&b);
    int rc = -1;
    if (cc_model_save(&S.model, &model) != 0 || model.oom) goto end;
    JsonW w;
    jw_init(&w, &b, 0);
    jw_obj_begin(&w);
    jw_key(&w, "also"), jw_arr_begin(&w);
    for (size_t i = 0; i < S.nalso; i++) jw_str_n(&w, S.also[i].c, 22);
    jw_arr_end(&w);
    char mts[24];
    snprintf(mts, sizeof mts, "%llu", (unsigned long long)mt);
    jw_key(&w, "file_mtime_ns"), jw_str(&w, mts); /* a string: above 2^53 */
    jw_key(&w, "file_size"), jw_u64(&w, size);
    jw_key(&w, "format"), jw_u64(&w, SESSION_FORMAT);
    jw_key(&w, "model"), jw_str_n(&w, model.p, model.len);
    jw_key(&w, "model_path"), jw_str(&w, sess_model);
    jw_key(&w, "next_step"), jw_u64(&w, S.next_step);
    jw_key(&w, "primary");
    if (S.has_primary) jw_str_n(&w, S.primary.c, 22);
    else jw_null(&w);
    jw_key(&w, "redo"), write_steps(&w, S.hist.redo, S.redo_ids, S.nredo_ids);
    jw_key(&w, "rev"), jw_u64(&w, S.rev);
    jw_key(&w, "saved_step");
    if (S.saved_step == UINT64_MAX) jw_null(&w);
    else jw_u64(&w, S.saved_step);
    jw_key(&w, "undo"), write_steps(&w, S.hist.undo, S.undo_ids, S.nundo_ids);
    jw_obj_end(&w);
    if (jw_finish(&w) != JW_OK || b.oom) goto end;
    char tmp[sizeof sess_path + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", sess_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) goto end;
    int bad = fwrite(b.p, 1, b.len, f) != b.len;
    bad |= fclose(f) != 0;
    if (bad || rename(tmp, sess_path) != 0) {
        unlink(tmp);
        goto end;
    }
    rc = 0;
end:
    if (rc) fprintf(stderr, "bimble-ccore: session not written to %s\n", sess_path);
    buf_free(&model), buf_free(&b);
    return rc;
}

static void persist_if_changed(void) {
    if (!S.open) return;
    /* Mid-call (a step pushed, its id not yet): wait for the next look. */
    if (S.nundo_ids != S.hist.nundo || S.nredo_ids != S.hist.nredo) return;
    uint64_t fp = fingerprint();
    if (sess_has_fp && fp == sess_fp) return;
    persist_busy = 1;
    if (write_session() == 0) sess_fp = fp, sess_has_fp = 1;
    persist_busy = 0;
}

void cc_session_persist_now(void) {
    if (persist_on && !persist_busy) persist_if_changed();
}

/* An integer in [lo, hi] (JSON number without fraction or exponent). */
static int get_u64(const JrVal *o, const char *key, uint64_t lo, uint64_t hi, uint64_t *out) {
    const JrVal *v = jr_get(o, key);
    if (!v || v->type != JR_NUM || !v->num_int || v->num_neg || v->num > 9007199254740992.0) return -1;
    uint64_t x = (uint64_t)v->num;
    if (x < lo || x > hi) return -1;
    *out = x;
    return 0;
}

static int get_gid(const JrVal *v, const cc_model *m, cc_gid *out) {
    if (!v || v->type != JR_STR || v->n != 22 || !cc_gid_valid(v->s, 22)) return -1;
    memcpy(out->c, v->s, 22);
    return cc_model_element(m, out) ? 0 : -1;
}

static int gid_sort(const void *a, const void *b) { return cc_gid_cmp(a, b); }

/* Steps of one stack into `st`/`ids` (malloc'd, `n` of them); commands into sess_cmds. */
static int read_steps(const JrVal *arr, uint64_t next_step, cc_step **st, uint64_t **ids, size_t *n,
                      const char **why) {
    *st = NULL, *ids = NULL, *n = 0;
    if (!arr || arr->type != JR_ARR) return *why = "undo/redo: not an array", -1;
    if (!arr->n) return 0;
    *st = calloc(arr->n, sizeof **st);
    *ids = calloc(arr->n, sizeof **ids);
    if (!*st || !*ids) return *why = "out of memory", -1;
    for (size_t i = 0; i < arr->n; i++) {
        const JrVal *o = &arr->items[i];
        if (o->type != JR_OBJ) return *why = "step: not an object", -1;
        if (get_u64(o, "id", 1, next_step - 1, &(*ids)[i])) return *why = "step: bad id", -1;
        const JrVal *label = jr_get(o, "label"), *cmds = jr_get(o, "cmds");
        if (!label || label->type != JR_STR || label->n > SESSION_MAX_LABEL || memchr(label->s, 0, label->n))
            return *why = "step: bad label", -1;
        if (!cmds || cmds->type != JR_STR) return *why = "step: bad cmds", -1;
        size_t first = sess_cmds.n;
        Buf err;
        buf_init(&err);
        int bad = cc_commands_parse(cmds->s, cmds->n, &sess_cmds, &err);
        buf_free(&err);
        if (bad) return *why = "step: commands do not parse", -1;
        size_t k = sess_cmds.n - first;
        cc_step *s = &(*st)[*n];
        s->label = malloc(label->n + 1);
        s->cmds = malloc((k ? k : 1) * sizeof *s->cmds);
        if (!s->label || !s->cmds) {
            free(s->label), free(s->cmds);
            return *why = "out of memory", -1;
        }
        memcpy(s->label, label->s, label->n), s->label[label->n] = 0;
        if (k) memcpy(s->cmds, sess_cmds.v + first, k * sizeof *s->cmds);
        s->n = k;
        (*n)++;
    }
    return 0;
}

static void free_steps(cc_step *st, size_t n) {
    for (size_t i = 0; i < n; i++) free(st[i].label), free(st[i].cmds);
    free(st);
}

/* 1 restored, 0 not (no file, or ignored with the reason logged). */
static int restore(void) {
    cc_error err;
    size_t n = 0;
    if (access(sess_path, F_OK) != 0) return 0;
    char *text = read_all(sess_path, &n, &err);
    if (!text) {
        fprintf(stderr, "bimble-ccore: session %s ignored: %s\n", sess_path, err.msg);
        return 0;
    }
    const char *why = NULL;
    Arena a;
    arena_init(&a, 0);
    cc_model tmp;
    int tmp_live = 0;
    cc_step *undo = NULL, *redo = NULL;
    uint64_t *uids = NULL, *rids = NULL;
    size_t nu = 0, nr = 0;
    cc_gid primary, *also = NULL;
    int has_primary = 0;
    size_t nalso = 0;
    uint64_t fmt, size, mt, fsize, rev, next_step, saved_step = UINT64_MAX;
    cc_cmds_init(&sess_cmds);
    JrError je;
    const JrVal *root = jr_parse(&a, text, n, &je);
    if (!root) { why = "not JSON"; goto fail; }
    if (root->type != JR_OBJ) { why = "not an object"; goto fail; }
    if (get_u64(root, "format", 0, UINT64_MAX, &fmt) || fmt != SESSION_FORMAT) { why = "unknown format"; goto fail; }
    const JrVal *mp = jr_get(root, "model_path");
    if (!mp || mp->type != JR_STR || strlen(sess_model) != mp->n || memcmp(mp->s, sess_model, mp->n)) {
        why = "written for another model path";
        goto fail;
    }
    const JrVal *mtv = jr_get(root, "file_mtime_ns");
    if (file_stamp(sess_model, &size, &mt) || get_u64(root, "file_size", 0, 1ull << 52, &fsize) || !mtv ||
        mtv->type != JR_STR) {
        why = "no file stamp";
        goto fail;
    }
    char mts[24];
    snprintf(mts, sizeof mts, "%llu", (unsigned long long)mt);
    if (fsize != size || strlen(mts) != mtv->n || memcmp(mts, mtv->s, mtv->n)) {
        why = "the model file changed since";
        goto fail;
    }
    if (get_u64(root, "rev", 1, 1ull << 52, &rev) || get_u64(root, "next_step", 1, 1ull << 52, &next_step)) {
        why = "bad rev or next_step";
        goto fail;
    }
    const JrVal *sv = jr_get(root, "saved_step");
    if (!sv) { why = "no saved_step"; goto fail; }
    if (sv->type != JR_NULL && get_u64(root, "saved_step", 0, next_step - 1, &saved_step)) { why = "bad saved_step"; goto fail; }
    const JrVal *mt_text = jr_get(root, "model");
    if (!mt_text || mt_text->type != JR_STR) { why = "no model text"; goto fail; }
    tmp_live = 1;
    if (cc_model_load(&tmp, mt_text->s, mt_text->n, &err)) { why = "the model text does not load"; goto fail; }
    const JrVal *pv = jr_get(root, "primary");
    if (!pv) { why = "no primary"; goto fail; }
    if (pv->type != JR_NULL) {
        if (get_gid(pv, &tmp, &primary)) { why = "primary: not an element"; goto fail; }
        has_primary = 1;
    }
    const JrVal *av = jr_get(root, "also");
    if (!av || av->type != JR_ARR || (av->n && !has_primary)) { why = "bad also"; goto fail; }
    if (av->n) {
        also = malloc(av->n * sizeof *also);
        if (!also) { why = "out of memory"; goto fail; }
        for (size_t i = 0; i < av->n; i++)
            if (get_gid(&av->items[i], &tmp, &also[i])) { why = "also: not an element"; goto fail; }
        qsort(also, av->n, sizeof *also, gid_sort);
        for (size_t i = 0; i < av->n; i++)
            if (!nalso || cc_gid_cmp(&also[nalso - 1], &also[i])) also[nalso++] = also[i];
    }
    if (read_steps(jr_get(root, "undo"), next_step, &undo, &uids, &nu, &why)) goto fail;
    if (read_steps(jr_get(root, "redo"), next_step, &redo, &rids, &nr, &why)) goto fail;
    for (size_t i = 1; i < nu; i++)
        if (uids[i] <= uids[i - 1]) { why = "undo ids not increasing"; goto fail; }
    for (size_t i = 1; i < nr; i++)
        if (rids[i] >= rids[i - 1]) { why = "redo ids not decreasing"; goto fail; }

    /* All checked: replace the session's model (loaded again in place: same text, same result). */
    cc_model_free(&tmp);
    tmp_live = 0;
    cc_history_free(&S.hist);
    cc_model_free(&S.model);
    if (cc_model_load(&S.model, mt_text->s, mt_text->n, &err)) { /* cannot happen: it loaded above */
        S.open = 0;
        why = "the model text does not load twice";
        goto fail;
    }
    cc_history_init(&S.hist);
    S.hist.undo = undo, S.hist.nundo = S.hist.capundo = nu;
    S.hist.redo = redo, S.hist.nredo = S.hist.capredo = nr;
    free(S.undo_ids), free(S.redo_ids);
    S.undo_ids = uids, S.nundo_ids = S.cap_undo_ids = nu;
    S.redo_ids = rids, S.nredo_ids = S.cap_redo_ids = nr;
    S.rev = rev, S.next_step = next_step, S.saved_step = saved_step;
    S.has_primary = has_primary;
    if (has_primary) S.primary = primary;
    free(S.also);
    S.also = also, S.nalso = nalso, S.capalso = nalso;
    fprintf(stderr, "bimble-ccore: session restored from %s (rev %llu, %zu undo, %zu redo, %zu selected)\n", sess_path,
            (unsigned long long)rev, nu, nr, (size_t)has_primary + nalso);
    arena_free(&a);
    free(text);
    return 1;
fail:
    fprintf(stderr, "bimble-ccore: session %s ignored: %s; starting from %s\n", sess_path, why ? why : "invalid",
            sess_model);
    if (tmp_live) cc_model_free(&tmp);
    free_steps(undo, nu), free_steps(redo, nr);
    free(uids), free(rids), free(also);
    cc_cmds_free(&sess_cmds);
    arena_free(&a);
    free(text);
    return 0;
}

int cc_session_persist_start(const char *session_path, const char *model_path) {
    if (!S.open || strlen(session_path) >= sizeof sess_path || strlen(model_path) >= sizeof sess_model) {
        fprintf(stderr, "bimble-ccore: no session file (no model, or a path too long)\n");
        return 0;
    }
    strcpy(sess_path, session_path);
    strcpy(sess_model, model_path);
    int restored = restore();
    persist_on = 1;
    sess_has_fp = 0; /* the first look writes the file (a corrupt one is replaced at once) */
    return restored;
}
#endif
