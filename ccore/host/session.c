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

cc_session *cc_session_get(void) { return &S; }

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

/* Opens `path` (cc_session_open); with `model_text`, the model is that .bimble text instead of
 * the file's content (a session snapshot: the file is not read), the rest as for the file. */
static int open_impl(const char *path, const char *model_text, size_t model_n, cc_error *err) {
    err->offset = CC_NO_OFFSET;
    err->msg[0] = 0;
    double t0 = now_ms();
    size_t n = 0;
    char *text = NULL;
    if (!model_text) {
        text = read_all(path, &n, err);
        if (!text) return -1;
    }
    int ifc = ends_with_ci(path, ".ifc");
    cc_model_init(&S.model);
    int rc;
    if (model_text) {
        cc_model_free(&S.model);
        rc = cc_model_load(&S.model, model_text, model_n, err);
        if (rc == 0 && (ifc ? editable_copy_path(path, S.path, sizeof S.path) != 0 : strlen(path) >= sizeof S.path)) {
            snprintf(err->msg, sizeof err->msg, "%s: path too long", path);
            rc = -1;
        } else if (rc == 0 && !ifc) strcpy(S.path, path);
    } else if (ifc) {
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

int cc_session_open(const char *path, cc_error *err) { return open_impl(path, NULL, 0, err); }

/* ---- the session file (session.h, tickets T12, T18) ---- */

#ifdef __wasm__
/* No files in the module: persistence is never on. */
int cc_session_open_persisted(const char *model_path, const char *session_path, cc_error *err) {
    (void)session_path;
    return cc_session_open(model_path, err);
}
void cc_session_persist_now(void) {}
void cc_session_note_apply(const cc_command *cmds, size_t n) { (void)cmds, (void)n; }
#else
#include <fcntl.h>

#define SESSION_FORMAT 2
#define SESSION_MAX_LABEL 4096
#define SESSION_MAX_ENTRIES 2000   /* entries after the snapshot before a new snapshot */
#define SESSION_LOG_FACTOR 4       /* or entry bytes above 4x the snapshot (model text or file) */
#define SESSION_MAX_LINES 1000000  /* replay bound (untrusted file) */

static char sess_path[4096], sess_model[4096];
static int persist_on, sess_fd = -1;
static void close_session(void);
static cc_cmds sess_cmds; /* the snapshot's steps' commands: they live as long as the model */

/* What the file says now (the state after its last line), to tell what a call changed. */
static struct {
    uint64_t size, mtime; /* the model file's stamp in the snapshot */
    uint64_t rev, next_step, saved_step;
    uint64_t *undo, *redo;
    size_t nundo, nredo, capundo, capredo;
    int has_primary;
    cc_gid primary;
    cc_gid *also;
    size_t nalso, capalso;
} W;
static int model_is_file; /* the open model is the model file's content (no change since it was read) */
static size_t snap_bytes, log_bytes, log_entries; /* the file: snapshot line, then entry lines */
static uint64_t model_file_bytes;
static int snap_null; /* the snapshot line has no model text (it stands for the model file) */

/* The forward commands of the step the current call pushed (cc_session_note_apply). */
static Buf pend_cmds;
static uint64_t pend_id;
static int pend_ok;

/* The model file's size and mtime (ns): Save rewrites it, an outside edit too. */
static int file_stamp(const char *path, uint64_t *size, uint64_t *mtime_ns) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *size = (uint64_t)st.st_size;
    *mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
    return 0;
}

static int copy_u64s(uint64_t **dst, size_t *n, size_t *cap, const uint64_t *src, size_t k) {
    if (k > *cap) {
        uint64_t *p = realloc(*dst, k * sizeof *p);
        if (!p) return -1;
        *dst = p, *cap = k;
    }
    if (k) memcpy(*dst, src, k * sizeof *src);
    *n = k;
    return 0;
}

/* W := the session now. -1 out of memory (W then forces a snapshot next time: nundo = SIZE_MAX). */
static int remember(void) {
    W.rev = S.rev, W.next_step = S.next_step, W.saved_step = S.saved_step;
    W.has_primary = S.has_primary;
    if (S.has_primary) W.primary = S.primary;
    int bad = copy_u64s(&W.undo, &W.nundo, &W.capundo, S.undo_ids, S.nundo_ids);
    bad |= copy_u64s(&W.redo, &W.nredo, &W.capredo, S.redo_ids, S.nredo_ids);
    if (!bad && S.nalso > W.capalso) {
        cc_gid *p = realloc(W.also, S.nalso * sizeof *p);
        if (!p) bad = 1;
        else W.also = p, W.capalso = S.nalso;
    }
    if (!bad) {
        if (S.nalso) memcpy(W.also, S.also, S.nalso * sizeof *S.also);
        W.nalso = S.nalso;
    }
    if (bad) W.nundo = SIZE_MAX;
    return bad ? -1 : 0;
}

static int same_ids(const uint64_t *a, size_t na, const uint64_t *b, size_t nb) {
    return na == nb && (!na || !memcmp(a, b, na * sizeof *a));
}

static void write_sel(JsonW *w) {
    jw_key(w, "also"), jw_arr_begin(w);
    for (size_t i = 0; i < S.nalso; i++) jw_str_n(w, S.also[i].c, 22);
    jw_arr_end(w);
}
static void write_counters(JsonW *w, int with_primary) {
    jw_key(w, "next_step"), jw_u64(w, S.next_step);
    if (with_primary) {
        jw_key(w, "primary");
        if (S.has_primary) jw_str_n(w, S.primary.c, 22);
        else jw_null(w);
    }
}
static void write_saved(JsonW *w) {
    jw_key(w, "saved_step");
    if (S.saved_step == UINT64_MAX) jw_null(w);
    else jw_u64(w, S.saved_step);
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

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k, n -= (size_t)k;
    }
    return 0;
}

/* A new file: the snapshot line (format 2), through `<path>.tmp` + rename; the entries start
 * after it. The model text is left out (null) while the model is the model file's content.
 * 0 written; -1 with the reason logged. */
static int write_snapshot(void) {
    uint64_t size = 0, mt = 0;
    if (file_stamp(sess_model, &size, &mt)) {
        fprintf(stderr, "bimble-ccore: session not written: %s: cannot stat\n", sess_model);
        return -1;
    }
    Buf model, b;
    buf_init(&model), buf_init(&b);
    int rc = -1, fd = -1;
    if (!model_is_file && (cc_model_save(&S.model, &model) != 0 || model.oom)) goto end;
    JsonW w;
    jw_init(&w, &b, 0);
    jw_obj_begin(&w);
    write_sel(&w);
    char mts[24];
    snprintf(mts, sizeof mts, "%llu", (unsigned long long)mt);
    jw_key(&w, "file_mtime_ns"), jw_str(&w, mts); /* a string: above 2^53 */
    jw_key(&w, "file_size"), jw_u64(&w, size);
    jw_key(&w, "format"), jw_u64(&w, SESSION_FORMAT);
    jw_key(&w, "model");
    if (model_is_file) jw_null(&w);
    else jw_str_n(&w, model.p, model.len);
    jw_key(&w, "model_path"), jw_str(&w, sess_model);
    write_counters(&w, 1);
    jw_key(&w, "redo"), write_steps(&w, S.hist.redo, S.redo_ids, S.nredo_ids);
    jw_key(&w, "rev"), jw_u64(&w, S.rev);
    write_saved(&w);
    jw_key(&w, "undo"), write_steps(&w, S.hist.undo, S.undo_ids, S.nundo_ids);
    jw_obj_end(&w);
    if (jw_finish(&w) != JW_OK || b.oom) goto end;
    buf_putc(&b, '\n');
    if (b.oom) goto end;
    char tmp[sizeof sess_path + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", sess_path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) goto end;
    int bad = write_all(fd, b.p, b.len);
    bad |= close(fd) != 0;
    fd = -1;
    if (bad || rename(tmp, sess_path) != 0) {
        unlink(tmp);
        goto end;
    }
    if (sess_fd >= 0) close(sess_fd);
    sess_fd = open(sess_path, O_WRONLY | O_APPEND);
    if (sess_fd < 0) goto end;
    snap_bytes = b.len, log_bytes = 0, log_entries = 0, snap_null = model_is_file;
    W.size = size, W.mtime = mt, model_file_bytes = size;
    rc = remember();
end:
    if (rc) fprintf(stderr, "bimble-ccore: session not written to %s\n", sess_path);
    buf_free(&model), buf_free(&b);
    return rc;
}

/* One entry line after the snapshot: `op` apply / undo / redo / state, then the state fields.
 * apply: the forward commands (as given to the call), the inverses the history keeps, label, id;
 * undo / redo: the id of the step moved. Written with one write(2) before the call answers. */
static int append_entry(const char *op) {
    Buf b, inv;
    buf_init(&b), buf_init(&inv);
    JsonW w;
    jw_init(&w, &b, 0);
    jw_obj_begin(&w);
    write_sel(&w);
    const cc_step *top = S.hist.nundo ? &S.hist.undo[S.hist.nundo - 1] : NULL;
    if (!strcmp(op, "apply")) {
        cc_commands_json(top->cmds, top->n, &inv);
        jw_key(&w, "cmds"), jw_str_n(&w, pend_cmds.p ? pend_cmds.p : "[]", pend_cmds.p ? pend_cmds.len : 2);
        jw_key(&w, "id"), jw_u64(&w, S.undo_ids[S.nundo_ids - 1]);
        jw_key(&w, "inv"), jw_str_n(&w, inv.p ? inv.p : "[]", inv.p ? inv.len : 2);
        jw_key(&w, "label"), jw_str(&w, top->label ? top->label : "");
    } else if (!strcmp(op, "undo")) {
        jw_key(&w, "id"), jw_u64(&w, S.redo_ids[S.nredo_ids - 1]);
    } else if (!strcmp(op, "redo")) {
        jw_key(&w, "id"), jw_u64(&w, S.undo_ids[S.nundo_ids - 1]);
    }
    write_counters(&w, 0);
    jw_key(&w, "op"), jw_str(&w, op);
    jw_key(&w, "primary");
    if (S.has_primary) jw_str_n(&w, S.primary.c, 22);
    else jw_null(&w);
    jw_key(&w, "rev"), jw_u64(&w, S.rev);
    write_saved(&w);
    jw_obj_end(&w);
    int rc = -1;
    if (jw_finish(&w) == JW_OK && !b.oom && !inv.oom) {
        buf_putc(&b, '\n');
        if (!b.oom && sess_fd >= 0 && write_all(sess_fd, b.p, b.len) == 0) {
            log_bytes += b.len, log_entries++;
            rc = remember();
        }
    }
    buf_free(&b), buf_free(&inv);
    return rc;
}

void cc_session_note_apply(const cc_command *cmds, size_t n) {
    if (!persist_on) return;
    buf_clear(&pend_cmds);
    pend_cmds.oom = 0;
    cc_commands_json(cmds, n, &pend_cmds);
    pend_ok = !pend_cmds.oom;
    pend_id = S.nundo_ids ? S.undo_ids[S.nundo_ids - 1] : 0;
}

void cc_session_persist_now(void) {
    if (!persist_on || !S.open) return;
    /* Mid-call (a step pushed, its id not yet): wait for the next look. */
    if (S.nundo_ids != S.hist.nundo || S.nredo_ids != S.hist.nredo) return;
    uint64_t size = 0, mt = 0;
    file_stamp(sess_model, &size, &mt);
    int stack_same = same_ids(S.undo_ids, S.nundo_ids, W.undo, W.nundo) && same_ids(S.redo_ids, S.nredo_ids, W.redo, W.nredo);
    const char *op = NULL;
    if (size != W.size || mt != W.mtime || sess_fd < 0) {
        /* Saved (or changed by another program): a new snapshot. After a Save to the model file
         * itself, the model is that file again: no model text. */
        (void)stack_same;
        model_is_file = S.saved_step != W.saved_step && !strcmp(S.path, sess_model) && cc_session_step(&S) == S.saved_step;
        write_snapshot();
        pend_ok = 0;
        return;
    }
    if (stack_same) {
        if (S.rev == W.rev && S.next_step == W.next_step && S.saved_step == W.saved_step &&
            S.has_primary == W.has_primary && (!S.has_primary || !cc_gid_cmp(&S.primary, &W.primary)) &&
            S.nalso == W.nalso && (!S.nalso || !memcmp(S.also, W.also, S.nalso * sizeof *S.also)))
            return; /* nothing changed */
        op = "state";
    } else if (W.nundo != SIZE_MAX && S.nundo_ids == W.nundo + 1 && same_ids(S.undo_ids, W.nundo, W.undo, W.nundo) &&
               S.nredo_ids == 0 && pend_ok && pend_id == S.undo_ids[W.nundo])
        op = "apply";
    else if (W.nundo != SIZE_MAX && W.nundo && S.nundo_ids == W.nundo - 1 && same_ids(S.undo_ids, S.nundo_ids, W.undo, S.nundo_ids) &&
             S.nredo_ids == W.nredo + 1 && same_ids(S.redo_ids, W.nredo, W.redo, W.nredo) &&
             S.redo_ids[W.nredo] == W.undo[W.nundo - 1])
        op = "undo";
    else if (W.nundo != SIZE_MAX && W.nredo && S.nredo_ids == W.nredo - 1 && same_ids(S.redo_ids, S.nredo_ids, W.redo, S.nredo_ids) &&
             S.nundo_ids == W.nundo + 1 && same_ids(S.undo_ids, W.nundo, W.undo, W.nundo) &&
             S.undo_ids[W.nundo] == W.redo[W.nredo - 1])
        op = "redo";
    pend_ok = 0;
    if (op && strcmp(op, "state")) model_is_file = 0;
    size_t base = snap_bytes + (snap_null ? (size_t)model_file_bytes : 0);
    if (!op || log_entries >= SESSION_MAX_ENTRIES || log_bytes > SESSION_LOG_FACTOR * base) {
        if (!op) model_is_file = 0; /* more than one change in a call: no telling */
        write_snapshot();
        return;
    }
    if (append_entry(op)) {
        fprintf(stderr, "bimble-ccore: session entry not written to %s; writing a snapshot\n", sess_path);
        write_snapshot();
    }
}

/* ---- restore ---- */

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

/* The selection of a snapshot or entry, checked against the open model: S's on success. */
static int read_selection(const JrVal *root, const char **why) {
    cc_gid primary, *also = NULL;
    int has_primary = 0;
    size_t nalso = 0;
    const JrVal *pv = jr_get(root, "primary");
    if (!pv) return *why = "no primary", -1;
    if (pv->type != JR_NULL) {
        if (get_gid(pv, &S.model, &primary)) return *why = "primary: not an element", -1;
        has_primary = 1;
    }
    const JrVal *av = jr_get(root, "also");
    if (!av || av->type != JR_ARR || (av->n && !has_primary)) return *why = "bad also", -1;
    if (av->n) {
        also = malloc(av->n * sizeof *also);
        if (!also) return *why = "out of memory", -1;
        for (size_t i = 0; i < av->n; i++)
            if (get_gid(&av->items[i], &S.model, &also[i])) {
                free(also);
                return *why = "also: not an element", -1;
            }
        qsort(also, av->n, sizeof *also, gid_sort);
        for (size_t i = 0; i < av->n; i++)
            if (!nalso || cc_gid_cmp(&also[nalso - 1], &also[i])) also[nalso++] = also[i];
    }
    S.has_primary = has_primary;
    if (has_primary) S.primary = primary;
    free(S.also);
    S.also = also, S.nalso = nalso, S.capalso = nalso;
    return 0;
}

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

static int push_u64(uint64_t **v, size_t *n, size_t *cap, uint64_t x) {
    if (*n == *cap) {
        size_t c = *cap ? 2 * *cap : 16;
        uint64_t *p = realloc(*v, c * sizeof *p);
        if (!p) return -1;
        *v = p, *cap = c;
    }
    (*v)[(*n)++] = x;
    return 0;
}

/* Replays one entry line on the open session through the same history code as the calls.
 * 0 ok; -1 with `why` (the session keeps the lines before; a failed apply changed nothing). */
static int replay_entry(const char *line, size_t n, const char **why) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    int rc = -1;
    cc_cmds cmds;
    cc_cmds_init(&cmds);
    Buf err, inv;
    buf_init(&err), buf_init(&inv);
    const JrVal *o = jr_parse(&a, line, n, &je);
    const JrVal *opv = o && o->type == JR_OBJ ? jr_get(o, "op") : NULL;
    uint64_t id = 0, rev, next_step, saved_step = UINT64_MAX;
    if (!opv || opv->type != JR_STR) { *why = "not an entry"; goto end; }
    if (get_u64(o, "rev", 1, 1ull << 52, &rev) || get_u64(o, "next_step", 1, 1ull << 52, &next_step)) {
        *why = "bad rev or next_step";
        goto end;
    }
    const JrVal *sv = jr_get(o, "saved_step");
    if (!sv || (sv->type != JR_NULL && get_u64(o, "saved_step", 0, next_step - 1, &saved_step))) {
        *why = "bad saved_step";
        goto end;
    }
    const char *op = opv->s;
    if (!strcmp(op, "apply")) {
        const JrVal *c = jr_get(o, "cmds"), *iv = jr_get(o, "inv"), *label = jr_get(o, "label");
        if (get_u64(o, "id", 1, 1ull << 52, &id) || id != S.next_step || next_step != id + 1) { *why = "apply: bad id"; goto end; }
        if (!c || c->type != JR_STR || !iv || iv->type != JR_STR) { *why = "apply: bad cmds"; goto end; }
        if (!label || label->type != JR_STR || label->n > SESSION_MAX_LABEL || memchr(label->s, 0, label->n)) {
            *why = "apply: bad label";
            goto end;
        }
        if (cc_commands_parse(c->s, c->n, &cmds, &err)) { *why = "apply: commands do not parse"; goto end; }
        if (push_u64(&S.undo_ids, &S.nundo_ids, &S.cap_undo_ids, id)) { *why = "out of memory"; goto end; }
        if (cc_history_apply(&S.hist, &S.model, cmds.v, cmds.n, &err)) {
            S.nundo_ids--;
            *why = "apply: the commands fail on the model";
            goto end;
        }
        S.nredo_ids = 0;
        S.next_step = id + 1;
        cc_step *top = &S.hist.undo[S.hist.nundo - 1];
        cc_commands_json(top->cmds, top->n, &inv);
        char *l = malloc(label->n + 1);
        if (inv.oom || inv.len != iv->n || memcmp(inv.p, iv->s, iv->n) || !l) {
            /* Not the step that was logged: take it back (the redo stack was cleared by it). */
            free(l);
            cc_history_undo(&S.hist, &S.model);
            if (S.hist.nredo) {
                cc_step *r = &S.hist.redo[--S.hist.nredo];
                free(r->label), free(r->cmds);
            }
            S.nundo_ids--, S.next_step = id;
            *why = "apply: the inverse differs from the logged one";
            goto end;
        }
        memcpy(l, label->s, label->n), l[label->n] = 0;
        free(top->label), top->label = l;
    } else if (!strcmp(op, "undo") || !strcmp(op, "redo")) {
        int undo = op[0] == 'u';
        uint64_t *from = undo ? S.undo_ids : S.redo_ids;
        size_t nfrom = undo ? S.nundo_ids : S.nredo_ids;
        if (get_u64(o, "id", 1, 1ull << 52, &id) || !nfrom || from[nfrom - 1] != id || next_step != S.next_step) {
            *why = "undo/redo: bad id";
            goto end;
        }
        if (undo ? push_u64(&S.redo_ids, &S.nredo_ids, &S.cap_redo_ids, id) : push_u64(&S.undo_ids, &S.nundo_ids, &S.cap_undo_ids, id)) {
            *why = "out of memory";
            goto end;
        }
        if (!(undo ? cc_history_undo(&S.hist, &S.model) : cc_history_redo(&S.hist, &S.model))) {
            if (undo) S.nredo_ids--;
            else S.nundo_ids--;
            *why = "undo/redo: nothing to take";
            goto end;
        }
        if (undo) S.nundo_ids--;
        else S.nredo_ids--;
    } else if (!strcmp(op, "state")) {
        if (next_step != S.next_step) { *why = "state: bad next_step"; goto end; }
    } else {
        *why = "unknown op";
        goto end;
    }
    if (strcmp(op, "state")) model_is_file = 0;
    S.rev = rev, S.saved_step = saved_step;
    if (read_selection(o, why)) {
        S.has_primary = 0, S.nalso = 0; /* the op is kept; the selection is not */
        goto end;
    }
    rc = 0;
end:
    cc_cmds_free(&cmds);
    buf_free(&err), buf_free(&inv);
    arena_free(&a);
    return rc;
}

static void close_session(void) {
    if (!S.open) return;
    cc_history_free(&S.hist);
    cc_model_free(&S.model);
    S.nundo_ids = S.nredo_ids = 0;
    S.has_primary = 0, S.nalso = 0;
    S.open = 0;
}

/* Opens the session from the session file. 1 restored (S open; *keep: bytes of the file that are
 * valid, *rewrite: the file needs a new snapshot), 0 not (no file, or ignored with the reason
 * logged: S closed). */
static int restore(size_t *keep, int *rewrite) {
    cc_error err;
    size_t n = 0;
    *keep = 0, *rewrite = 0;
    if (access(sess_path, F_OK) != 0) return 0;
    char *text = read_all(sess_path, &n, &err);
    if (!text) {
        fprintf(stderr, "bimble-ccore: session %s ignored: %s\n", sess_path, err.msg);
        return 0;
    }
    const char *why = NULL;
    Arena a;
    arena_init(&a, 0);
    cc_step *undo = NULL, *redo = NULL;
    uint64_t *uids = NULL, *rids = NULL;
    size_t nu = 0, nr = 0;
    uint64_t fmt, size, mt, fsize, rev, next_step, saved_step = UINT64_MAX;
    cc_cmds_init(&sess_cmds);
    const char *nl = memchr(text, '\n', n);
    size_t first = nl ? (size_t)(nl - text) : n;
    JrError je;
    const JrVal *root = nl ? jr_parse(&a, text, first, &je) : NULL;
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
    if (!mt_text || (mt_text->type != JR_STR && mt_text->type != JR_NULL)) { why = "no model text"; goto fail; }
    if (read_steps(jr_get(root, "undo"), next_step, &undo, &uids, &nu, &why)) goto fail;
    if (read_steps(jr_get(root, "redo"), next_step, &redo, &rids, &nr, &why)) goto fail;
    for (size_t i = 1; i < nu; i++)
        if (uids[i] <= uids[i - 1]) { why = "undo ids not increasing"; goto fail; }
    for (size_t i = 1; i < nr; i++)
        if (rids[i] >= rids[i - 1]) { why = "redo ids not decreasing"; goto fail; }
    /* The model: the snapshot's text (the model file is not read), or the model file itself. */
    model_is_file = snap_null = mt_text->type == JR_NULL;
    if (model_is_file ? cc_session_open(sess_model, &err) : open_impl(sess_model, mt_text->s, mt_text->n, &err)) {
        why = model_is_file ? "the model file does not load" : "the model text does not load";
        goto fail;
    }
    if (read_selection(root, &why)) goto fail;
    cc_history_free(&S.hist);
    cc_history_init(&S.hist);
    S.hist.undo = undo, S.hist.nundo = S.hist.capundo = nu;
    S.hist.redo = redo, S.hist.nredo = S.hist.capredo = nr;
    undo = redo = NULL;
    free(S.undo_ids), free(S.redo_ids);
    S.undo_ids = uids, S.nundo_ids = S.cap_undo_ids = nu;
    S.redo_ids = rids, S.nredo_ids = S.cap_redo_ids = nr;
    uids = rids = NULL;
    S.rev = rev, S.next_step = next_step, S.saved_step = saved_step;
    snap_bytes = first + 1, model_file_bytes = size;
    W.size = size, W.mtime = mt;

    /* The entries: each replayed; a bad one ends the replay, the lines before are kept. */
    size_t at = first + 1, ok = 0;
    while (at < n) {
        const char *e = memchr(text + at, '\n', n - at);
        if (!e) {
            why = "a truncated last line";
            break;
        }
        size_t len = (size_t)(e - (text + at));
        if (ok >= SESSION_MAX_LINES) { why = "too many lines"; *rewrite = 1; break; }
        if (replay_entry(text + at, len, &why)) { *rewrite = 1; break; }
        at += len + 1, ok++;
    }
    *keep = at;
    log_bytes = at - snap_bytes, log_entries = ok;
    if (at < n)
        fprintf(stderr, "bimble-ccore: session %s ignored: from line %zu: %s; kept the snapshot and %zu entries\n",
                sess_path, ok + 2, why, ok);
    fprintf(stderr, "bimble-ccore: session restored from %s (rev %llu, %zu undo, %zu redo, %zu selected, %zu entries replayed)\n",
            sess_path, (unsigned long long)S.rev, S.nundo_ids, S.nredo_ids, (size_t)S.has_primary + S.nalso, log_entries);
    arena_free(&a);
    free(text);
    return 1;
fail:
    fprintf(stderr, "bimble-ccore: session %s ignored: %s; starting from %s\n", sess_path, why ? why : "invalid",
            sess_model);
    close_session();
    free_steps(undo, nu), free_steps(redo, nr);
    free(uids), free(rids);
    cc_cmds_free(&sess_cmds);
    arena_free(&a);
    free(text);
    return 0;
}

int cc_session_open_persisted(const char *model_path, const char *session_path, cc_error *err) {
    if (strlen(session_path) >= sizeof sess_path || strlen(model_path) >= sizeof sess_model) {
        fprintf(stderr, "bimble-ccore: no session file (a path too long)\n");
        return cc_session_open(model_path, err);
    }
    strcpy(sess_path, session_path);
    strcpy(sess_model, model_path);
    size_t keep = 0;
    int rewrite = 0;
    if (restore(&keep, &rewrite)) {
        persist_on = 1;
        if (!rewrite) {
            sess_fd = open(sess_path, O_WRONLY | O_APPEND);
            if (sess_fd >= 0 && ftruncate(sess_fd, (off_t)keep) == 0 && remember() == 0) return 0;
        }
        write_snapshot();
        return 0;
    }
    if (cc_session_open(model_path, err)) return -1;
    persist_on = 1;
    model_is_file = 1;
    write_snapshot(); /* no model text: cheap */
    return 0;
}
#endif
