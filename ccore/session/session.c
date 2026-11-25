/* A session: see session.h. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "command.h"
#include "file.h"
#include "ifc_in.h"
#include "json_r.h"
#include "json_w.h"
#include "session.h"

cc_session *cc_session_new(const cc_platform *pf) {
    cc_session *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->pf = pf;
    buf_init(&s->mesh);
    return s;
}

static void persist_free(cc_session *s);

void cc_session_free(cc_session *s) {
    if (!s) return;
    if (s->open) cc_history_free(&s->hist), cc_model_free(&s->model);
    free(s->undo_ids), free(s->redo_ids), free(s->also), free(s->material);
    if (s->scene_rev) cc_scene_free(&s->scene);
    buf_free(&s->mesh);
    persist_free(s);
    free(s);
}

const cc_model *cc_session_model(const cc_session *s) { return s->open ? &s->model : NULL; }
uint32_t cc_session_rev(const cc_session *s) { return (uint32_t)s->rev; }

uint64_t cc_session_step(const cc_session *s) { return s->nundo_ids ? s->undo_ids[s->nundo_ids - 1] : 0; }
int cc_session_unsaved(const cc_session *s) { return s->open && cc_session_step(s) != s->saved_step; }

/* The whole file through the platform, NUL-terminated, at most CC_MAX_FILE_BYTES. NULL with `err`. */
static char *read_all(const cc_session *s, const char *path, size_t *n, cc_error *err) {
    char *p = NULL;
    int st = s->pf->read_file(s->pf->ctx, path, CC_MAX_FILE_BYTES, &p, n);
    if (st == CC_PF_OK) return p;
    if (st == CC_PF_NOT_FOUND) snprintf(err->msg, sizeof err->msg, "%s: cannot open", path);
    else if (st == CC_PF_TOO_LARGE)
        snprintf(err->msg, sizeof err->msg, "%s: larger than %zu bytes", path, (size_t)CC_MAX_FILE_BYTES);
    else snprintf(err->msg, sizeof err->msg, "%s: %s", path, st == CC_PF_IO ? "read error" : "out of memory");
    return NULL;
}

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
static int editable_copy_path(const cc_session *s, const char *ifc, char *out, size_t cap) {
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
        if (!s->pf->exists(s->pf->ctx, out)) return 0; /* without files, the first name is free */
    }
    return -1;
}

/* Opens `path` (cc_session_open); with `model_text`, the model is that .bimble text instead of
 * the file's content (a session snapshot: the file is not read), the rest as for the file. */
static int open_impl(cc_session *s, const char *path, const char *model_text, size_t model_n, cc_error *err) {
    err->offset = CC_NO_OFFSET;
    err->msg[0] = 0;
    double t0 = s->pf->now_ms(s->pf->ctx);
    if (s->scene_rev) cc_scene_free(&s->scene); /* the caches are per rev: a new model starts at 1 */
    s->scene_rev = 0, s->mesh_status = 0;
    size_t n = 0;
    char *text = NULL;
    if (!model_text) {
        text = read_all(s, path, &n, err);
        if (!text) return -1;
    }
    int ifc = ends_with_ci(path, ".ifc");
    cc_model_init(&s->model);
    int rc;
    if (model_text) {
        cc_model_free(&s->model);
        rc = cc_model_load(&s->model, model_text, model_n, err);
        if (rc == 0 && (ifc ? editable_copy_path(s, path, s->path, sizeof s->path) != 0 : strlen(path) >= sizeof s->path)) {
            snprintf(err->msg, sizeof err->msg, "%s: path too long", path);
            rc = -1;
        } else if (rc == 0 && !ifc) strcpy(s->path, path);
    } else if (ifc) {
        cc_ifc_report rep;
        cc_model_free(&s->model);
        rc = cc_ifc_import(&s->model, &rep, text, n, err);
        cc_ifc_report_free(&rep);
        if (rc == 0) { /* lib.rs:529-536: the project takes the file stem, then validate */
            const char *name = strrchr(path, '/');
            name = name ? name + 1 : path;
            const char *dot = strrchr(name, '.');
            size_t k = dot && dot != name ? (size_t)(dot - name) : strlen(name);
            char *p = arena_dup(&s->model.arena, name, k ? k : 1);
            if (p && k) s->model.project_name = (cc_str){p, k};
            cc_error ve;
            if (cc_model_validate(&s->model, &ve)) {
                snprintf(err->msg, sizeof err->msg, "imported model invalid: %s", ve.msg);
                rc = -1;
            }
        }
        if (rc == 0 && editable_copy_path(s, path, s->path, sizeof s->path)) {
            snprintf(err->msg, sizeof err->msg, "%s: no name for the editable copy", path);
            rc = -1;
        }
    } else {
        cc_model_free(&s->model);
        rc = cc_model_load(&s->model, text, n, err);
        if (rc == 0) {
            if (strlen(path) >= sizeof s->path) {
                snprintf(err->msg, sizeof err->msg, "path too long");
                rc = -1;
            } else strcpy(s->path, path);
        }
    }
    free(text);
    if (rc) {
        cc_model_free(&s->model);
        return -1;
    }
    cc_history_init(&s->hist);
    const char *slash = strrchr(s->path, '/');
    s->file = slash ? slash + 1 : s->path;
    s->schema_bimble = strlen(s->file) > 7 && !strcmp(s->file + strlen(s->file) - 7, ".bimble");
    s->open = 1;
    s->rev = 1;            /* lib.rs:1679 */
    s->next_step = 1;
    s->saved_step = ifc ? UINT64_MAX : 0; /* edit.rs `Editor::imported`: starts dirty */
    /* edit.rs `Editor::new`: the lowest storey at or above -1 mm, else the first one. */
    const cc_storey *best = NULL;
    for (size_t i = 0; i < s->model.nstoreys; i++) {
        const cc_storey *st = &s->model.storeys[i];
        if (st->elevation >= -1.0 && (!best || st->elevation < best->elevation)) best = st;
    }
    if (!best && s->model.nstoreys) best = &s->model.storeys[0];
    if (best) s->has_storey = 1, s->storey = best->id;
    s->kind = CC_KIND_COLUMN;
    double sizes[5] = {300.0, 250.0, 500.0, 200.0, 250.0}; /* verbs.rs:86-96 */
    memcpy(s->sizes, sizes, sizeof sizes);
    s->offset[0] = 1000.0, s->offset[1] = 0.0, s->offset[2] = 0.0;
    s->tab_open = 1; /* the 3D view is shown on load */
    s->load_ms = s->pf->now_ms(s->pf->ctx) - t0;
    return 0;
}

int cc_session_open(cc_session *s, const char *path, cc_error *err) { return open_impl(s, path, NULL, 0, err); }

/* ---- the session file (session.h, tickets T12, T18) ---- */

#ifdef __wasm__
/* No files in the module: persistence is never on. */
int cc_session_open_persisted(cc_session *s, const char *model_path, const char *session_path, cc_error *err) {
    (void)session_path;
    return cc_session_open(s, model_path, err);
}
void cc_session_persist_now(cc_session *s) { (void)s; }
void cc_session_note_apply(cc_session *s, const cc_command *cmds, size_t n) { (void)s, (void)cmds, (void)n; }
static void persist_free(cc_session *s) { (void)s; }
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define SESSION_FORMAT 2
#define SESSION_MAX_LABEL 4096
#define SESSION_MAX_ENTRIES 2000   /* entries after the snapshot before a new snapshot */
#define SESSION_LOG_FACTOR 4       /* or entry bytes above 4x the snapshot (model text or file) */
#define SESSION_MAX_LINES 1000000  /* replay bound (untrusted file) */

/* The session file's state, per session (s->persist; NULL: persistence off). */
struct cc_persist {
    char sess_path[4096], sess_model[4096];
    int on, sess_fd;
    cc_cmds sess_cmds; /* the snapshot's steps' commands: they live as long as the model */
    /* What the file says now (the state after its last line), to tell what a call changed. */
    struct {
        uint64_t size, mtime; /* the model file's stamp in the snapshot */
        uint64_t rev, next_step, saved_step;
        uint64_t *undo, *redo;
        size_t nundo, nredo, capundo, capredo;
        int has_primary;
        cc_gid primary;
        cc_gid *also;
        size_t nalso, capalso;
    } W;
    int model_is_file; /* the open model is the model file's content (no change since it was read) */
    size_t snap_bytes, log_bytes, log_entries; /* the file: snapshot line, then entry lines */
    uint64_t model_file_bytes;
    int snap_null; /* the snapshot line has no model text (it stands for the model file) */
    /* The forward commands of the step the current call pushed (cc_session_note_apply). */
    Buf pend_cmds;
    uint64_t pend_id;
    int pend_ok;
};
static void close_session(cc_session *s);

static void persist_free(cc_session *s) {
    cc_persist *P = s->persist;
    if (!P) return;
    if (P->sess_fd >= 0) close(P->sess_fd);
    free(P->W.undo), free(P->W.redo), free(P->W.also);
    buf_free(&P->pend_cmds);
    cc_cmds_free(&P->sess_cmds); /* after the history that borrows from it (cc_session_free) */
    free(P);
    s->persist = NULL;
}

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
static int remember(cc_session *s) {
    cc_persist *P = s->persist;
    P->W.rev = s->rev, P->W.next_step = s->next_step, P->W.saved_step = s->saved_step;
    P->W.has_primary = s->has_primary;
    if (s->has_primary) P->W.primary = s->primary;
    int bad = copy_u64s(&P->W.undo, &P->W.nundo, &P->W.capundo, s->undo_ids, s->nundo_ids);
    bad |= copy_u64s(&P->W.redo, &P->W.nredo, &P->W.capredo, s->redo_ids, s->nredo_ids);
    if (!bad && s->nalso > P->W.capalso) {
        cc_gid *p = realloc(P->W.also, s->nalso * sizeof *p);
        if (!p) bad = 1;
        else P->W.also = p, P->W.capalso = s->nalso;
    }
    if (!bad) {
        if (s->nalso) memcpy(P->W.also, s->also, s->nalso * sizeof *s->also);
        P->W.nalso = s->nalso;
    }
    if (bad) P->W.nundo = SIZE_MAX;
    return bad ? -1 : 0;
}

static int same_ids(const uint64_t *a, size_t na, const uint64_t *b, size_t nb) {
    return na == nb && (!na || !memcmp(a, b, na * sizeof *a));
}

static void write_sel(const cc_session *s, JsonW *w) {
    jw_key(w, "also"), jw_arr_begin(w);
    for (size_t i = 0; i < s->nalso; i++) jw_str_n(w, s->also[i].c, 22);
    jw_arr_end(w);
}
static void write_counters(const cc_session *s, JsonW *w, int with_primary) {
    jw_key(w, "next_step"), jw_u64(w, s->next_step);
    if (with_primary) {
        jw_key(w, "primary");
        if (s->has_primary) jw_str_n(w, s->primary.c, 22);
        else jw_null(w);
    }
}
static void write_saved(const cc_session *s, JsonW *w) {
    jw_key(w, "saved_step");
    if (s->saved_step == UINT64_MAX) jw_null(w);
    else jw_u64(w, s->saved_step);
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
static int write_snapshot(cc_session *s) {
    cc_persist *P = s->persist;
    uint64_t size = 0, mt = 0;
    if (file_stamp(P->sess_model, &size, &mt)) {
        cc_log(s->pf, "bimble-ccore: session not written: %s: cannot stat", P->sess_model);
        return -1;
    }
    Buf model, b;
    buf_init(&model), buf_init(&b);
    int rc = -1, fd = -1;
    if (!P->model_is_file && (cc_model_save(&s->model, &model) != 0 || model.oom)) goto end;
    JsonW w;
    jw_init(&w, &b, 0);
    jw_obj_begin(&w);
    write_sel(s, &w);
    char mts[24];
    snprintf(mts, sizeof mts, "%llu", (unsigned long long)mt);
    jw_key(&w, "file_mtime_ns"), jw_str(&w, mts); /* a string: above 2^53 */
    jw_key(&w, "file_size"), jw_u64(&w, size);
    jw_key(&w, "format"), jw_u64(&w, SESSION_FORMAT);
    jw_key(&w, "model");
    if (P->model_is_file) jw_null(&w);
    else jw_str_n(&w, model.p, model.len);
    jw_key(&w, "model_path"), jw_str(&w, P->sess_model);
    write_counters(s, &w, 1);
    jw_key(&w, "redo"), write_steps(&w, s->hist.redo, s->redo_ids, s->nredo_ids);
    jw_key(&w, "rev"), jw_u64(&w, s->rev);
    write_saved(s, &w);
    jw_key(&w, "undo"), write_steps(&w, s->hist.undo, s->undo_ids, s->nundo_ids);
    jw_obj_end(&w);
    if (jw_finish(&w) != JW_OK || b.oom) goto end;
    buf_putc(&b, '\n');
    if (b.oom) goto end;
    char tmp[sizeof P->sess_path + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", P->sess_path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) goto end;
    int bad = write_all(fd, b.p, b.len);
    bad |= close(fd) != 0;
    fd = -1;
    if (bad || rename(tmp, P->sess_path) != 0) {
        unlink(tmp);
        goto end;
    }
    if (P->sess_fd >= 0) close(P->sess_fd);
    P->sess_fd = open(P->sess_path, O_WRONLY | O_APPEND);
    if (P->sess_fd < 0) goto end;
    P->snap_bytes = b.len, P->log_bytes = 0, P->log_entries = 0, P->snap_null = P->model_is_file;
    P->W.size = size, P->W.mtime = mt, P->model_file_bytes = size;
    rc = remember(s);
end:
    if (rc) cc_log(s->pf, "bimble-ccore: session not written to %s", P->sess_path);
    buf_free(&model), buf_free(&b);
    return rc;
}

/* One entry line after the snapshot: `op` apply / undo / redo / state, then the state fields.
 * apply: the forward commands (as given to the call), the inverses the history keeps, label, id;
 * undo / redo: the id of the step moved. Written with one write(2) before the call answers. */
static int append_entry(cc_session *s, const char *op) {
    cc_persist *P = s->persist;
    Buf b, inv;
    buf_init(&b), buf_init(&inv);
    JsonW w;
    jw_init(&w, &b, 0);
    jw_obj_begin(&w);
    write_sel(s, &w);
    const cc_step *top = s->hist.nundo ? &s->hist.undo[s->hist.nundo - 1] : NULL;
    if (!strcmp(op, "apply")) {
        cc_commands_json(top->cmds, top->n, &inv);
        jw_key(&w, "cmds"), jw_str_n(&w, P->pend_cmds.p ? P->pend_cmds.p : "[]", P->pend_cmds.p ? P->pend_cmds.len : 2);
        jw_key(&w, "id"), jw_u64(&w, s->undo_ids[s->nundo_ids - 1]);
        jw_key(&w, "inv"), jw_str_n(&w, inv.p ? inv.p : "[]", inv.p ? inv.len : 2);
        jw_key(&w, "label"), jw_str(&w, top->label ? top->label : "");
    } else if (!strcmp(op, "undo")) {
        jw_key(&w, "id"), jw_u64(&w, s->redo_ids[s->nredo_ids - 1]);
    } else if (!strcmp(op, "redo")) {
        jw_key(&w, "id"), jw_u64(&w, s->undo_ids[s->nundo_ids - 1]);
    }
    write_counters(s, &w, 0);
    jw_key(&w, "op"), jw_str(&w, op);
    jw_key(&w, "primary");
    if (s->has_primary) jw_str_n(&w, s->primary.c, 22);
    else jw_null(&w);
    jw_key(&w, "rev"), jw_u64(&w, s->rev);
    write_saved(s, &w);
    jw_obj_end(&w);
    int rc = -1;
    if (jw_finish(&w) == JW_OK && !b.oom && !inv.oom) {
        buf_putc(&b, '\n');
        if (!b.oom && P->sess_fd >= 0 && write_all(P->sess_fd, b.p, b.len) == 0) {
            P->log_bytes += b.len, P->log_entries++;
            rc = remember(s);
        }
    }
    buf_free(&b), buf_free(&inv);
    return rc;
}

void cc_session_note_apply(cc_session *s, const cc_command *cmds, size_t n) {
    cc_persist *P = s->persist;
    if (!P || !P->on) return;
    buf_clear(&P->pend_cmds);
    P->pend_cmds.oom = 0;
    cc_commands_json(cmds, n, &P->pend_cmds);
    P->pend_ok = !P->pend_cmds.oom;
    P->pend_id = s->nundo_ids ? s->undo_ids[s->nundo_ids - 1] : 0;
}

void cc_session_persist_now(cc_session *s) {
    cc_persist *P = s->persist;
    if (!P || !P->on || !s->open) return;
    /* Mid-call (a step pushed, its id not yet): wait for the next look. */
    if (s->nundo_ids != s->hist.nundo || s->nredo_ids != s->hist.nredo) return;
    uint64_t size = 0, mt = 0;
    file_stamp(P->sess_model, &size, &mt);
    int stack_same = same_ids(s->undo_ids, s->nundo_ids, P->W.undo, P->W.nundo) && same_ids(s->redo_ids, s->nredo_ids, P->W.redo, P->W.nredo);
    const char *op = NULL;
    if (size != P->W.size || mt != P->W.mtime || P->sess_fd < 0) {
        /* Saved (or changed by another program): a new snapshot. After a Save to the model file
         * itself, the model is that file again: no model text. */
        (void)stack_same;
        P->model_is_file = s->saved_step != P->W.saved_step && !strcmp(s->path, P->sess_model) && cc_session_step(s) == s->saved_step;
        write_snapshot(s);
        P->pend_ok = 0;
        return;
    }
    if (stack_same) {
        if (s->rev == P->W.rev && s->next_step == P->W.next_step && s->saved_step == P->W.saved_step &&
            s->has_primary == P->W.has_primary && (!s->has_primary || !cc_gid_cmp(&s->primary, &P->W.primary)) &&
            s->nalso == P->W.nalso && (!s->nalso || !memcmp(s->also, P->W.also, s->nalso * sizeof *s->also)))
            return; /* nothing changed */
        op = "state";
    } else if (P->W.nundo != SIZE_MAX && s->nundo_ids == P->W.nundo + 1 && same_ids(s->undo_ids, P->W.nundo, P->W.undo, P->W.nundo) &&
               s->nredo_ids == 0 && P->pend_ok && P->pend_id == s->undo_ids[P->W.nundo])
        op = "apply";
    else if (P->W.nundo != SIZE_MAX && P->W.nundo && s->nundo_ids == P->W.nundo - 1 && same_ids(s->undo_ids, s->nundo_ids, P->W.undo, s->nundo_ids) &&
             s->nredo_ids == P->W.nredo + 1 && same_ids(s->redo_ids, P->W.nredo, P->W.redo, P->W.nredo) &&
             s->redo_ids[P->W.nredo] == P->W.undo[P->W.nundo - 1])
        op = "undo";
    else if (P->W.nundo != SIZE_MAX && P->W.nredo && s->nredo_ids == P->W.nredo - 1 && same_ids(s->redo_ids, s->nredo_ids, P->W.redo, s->nredo_ids) &&
             s->nundo_ids == P->W.nundo + 1 && same_ids(s->undo_ids, P->W.nundo, P->W.undo, P->W.nundo) &&
             s->undo_ids[P->W.nundo] == P->W.redo[P->W.nredo - 1])
        op = "redo";
    P->pend_ok = 0;
    if (op && strcmp(op, "state")) P->model_is_file = 0;
    size_t base = P->snap_bytes + (P->snap_null ? (size_t)P->model_file_bytes : 0);
    if (!op || P->log_entries >= SESSION_MAX_ENTRIES || P->log_bytes > SESSION_LOG_FACTOR * base) {
        if (!op) P->model_is_file = 0; /* more than one change in a call: no telling */
        write_snapshot(s);
        return;
    }
    if (append_entry(s, op)) {
        cc_log(s->pf, "bimble-ccore: session entry not written to %s; writing a snapshot", P->sess_path);
        write_snapshot(s);
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
static int read_selection(cc_session *s, const JrVal *root, const char **why) {
    cc_gid primary, *also = NULL;
    int has_primary = 0;
    size_t nalso = 0;
    const JrVal *pv = jr_get(root, "primary");
    if (!pv) return *why = "no primary", -1;
    if (pv->type != JR_NULL) {
        if (get_gid(pv, &s->model, &primary)) return *why = "primary: not an element", -1;
        has_primary = 1;
    }
    const JrVal *av = jr_get(root, "also");
    if (!av || av->type != JR_ARR || (av->n && !has_primary)) return *why = "bad also", -1;
    if (av->n) {
        also = malloc(av->n * sizeof *also);
        if (!also) return *why = "out of memory", -1;
        for (size_t i = 0; i < av->n; i++)
            if (get_gid(&av->items[i], &s->model, &also[i])) {
                free(also);
                return *why = "also: not an element", -1;
            }
        qsort(also, av->n, sizeof *also, gid_sort);
        for (size_t i = 0; i < av->n; i++)
            if (!nalso || cc_gid_cmp(&also[nalso - 1], &also[i])) also[nalso++] = also[i];
    }
    s->has_primary = has_primary;
    if (has_primary) s->primary = primary;
    free(s->also);
    s->also = also, s->nalso = nalso, s->capalso = nalso;
    return 0;
}

/* Steps of one stack into `st`/`ids` (malloc'd, `n` of them); commands into P->sess_cmds. */
static int read_steps(cc_persist *P, const JrVal *arr, uint64_t next_step, cc_step **st, uint64_t **ids, size_t *n,
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
        size_t first = P->sess_cmds.n;
        Buf err;
        buf_init(&err);
        int bad = cc_commands_parse(cmds->s, cmds->n, &P->sess_cmds, &err);
        buf_free(&err);
        if (bad) return *why = "step: commands do not parse", -1;
        size_t k = P->sess_cmds.n - first;
        cc_step *sp = &(*st)[*n];
        sp->label = malloc(label->n + 1);
        sp->cmds = malloc((k ? k : 1) * sizeof *sp->cmds);
        if (!sp->label || !sp->cmds) {
            free(sp->label), free(sp->cmds);
            return *why = "out of memory", -1;
        }
        memcpy(sp->label, label->s, label->n), sp->label[label->n] = 0;
        if (k) memcpy(sp->cmds, P->sess_cmds.v + first, k * sizeof *sp->cmds);
        sp->n = k;
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
static int replay_entry(cc_session *s, const char *line, size_t n, const char **why) {
    cc_persist *P = s->persist;
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
        if (get_u64(o, "id", 1, 1ull << 52, &id) || id != s->next_step || next_step != id + 1) { *why = "apply: bad id"; goto end; }
        if (!c || c->type != JR_STR || !iv || iv->type != JR_STR) { *why = "apply: bad cmds"; goto end; }
        if (!label || label->type != JR_STR || label->n > SESSION_MAX_LABEL || memchr(label->s, 0, label->n)) {
            *why = "apply: bad label";
            goto end;
        }
        if (cc_commands_parse(c->s, c->n, &cmds, &err)) { *why = "apply: commands do not parse"; goto end; }
        if (push_u64(&s->undo_ids, &s->nundo_ids, &s->cap_undo_ids, id)) { *why = "out of memory"; goto end; }
        if (cc_history_apply(&s->hist, &s->model, cmds.v, cmds.n, &err)) {
            s->nundo_ids--;
            *why = "apply: the commands fail on the model";
            goto end;
        }
        s->nredo_ids = 0;
        s->next_step = id + 1;
        cc_step *top = &s->hist.undo[s->hist.nundo - 1];
        cc_commands_json(top->cmds, top->n, &inv);
        char *l = malloc(label->n + 1);
        if (inv.oom || inv.len != iv->n || memcmp(inv.p, iv->s, iv->n) || !l) {
            /* Not the step that was logged: take it back (the redo stack was cleared by it). */
            free(l);
            cc_history_undo(&s->hist, &s->model);
            if (s->hist.nredo) {
                cc_step *r = &s->hist.redo[--s->hist.nredo];
                free(r->label), free(r->cmds);
            }
            s->nundo_ids--, s->next_step = id;
            *why = "apply: the inverse differs from the logged one";
            goto end;
        }
        memcpy(l, label->s, label->n), l[label->n] = 0;
        free(top->label), top->label = l;
    } else if (!strcmp(op, "undo") || !strcmp(op, "redo")) {
        int undo = op[0] == 'u';
        uint64_t *from = undo ? s->undo_ids : s->redo_ids;
        size_t nfrom = undo ? s->nundo_ids : s->nredo_ids;
        if (get_u64(o, "id", 1, 1ull << 52, &id) || !nfrom || from[nfrom - 1] != id || next_step != s->next_step) {
            *why = "undo/redo: bad id";
            goto end;
        }
        if (undo ? push_u64(&s->redo_ids, &s->nredo_ids, &s->cap_redo_ids, id) : push_u64(&s->undo_ids, &s->nundo_ids, &s->cap_undo_ids, id)) {
            *why = "out of memory";
            goto end;
        }
        if (!(undo ? cc_history_undo(&s->hist, &s->model) : cc_history_redo(&s->hist, &s->model))) {
            if (undo) s->nredo_ids--;
            else s->nundo_ids--;
            *why = "undo/redo: nothing to take";
            goto end;
        }
        if (undo) s->nundo_ids--;
        else s->nredo_ids--;
    } else if (!strcmp(op, "state")) {
        if (next_step != s->next_step) { *why = "state: bad next_step"; goto end; }
    } else {
        *why = "unknown op";
        goto end;
    }
    if (strcmp(op, "state")) P->model_is_file = 0;
    s->rev = rev, s->saved_step = saved_step;
    if (read_selection(s, o, why)) {
        s->has_primary = 0, s->nalso = 0; /* the op is kept; the selection is not */
        goto end;
    }
    rc = 0;
end:
    cc_cmds_free(&cmds);
    buf_free(&err), buf_free(&inv);
    arena_free(&a);
    return rc;
}

static void close_session(cc_session *s) {
    if (!s->open) return;
    cc_history_free(&s->hist);
    cc_model_free(&s->model);
    s->nundo_ids = s->nredo_ids = 0;
    s->has_primary = 0, s->nalso = 0;
    s->open = 0;
}

/* Opens the session from the session file. 1 restored (S open; *keep: bytes of the file that are
 * valid, *rewrite: the file needs a new snapshot), 0 not (no file, or ignored with the reason
 * logged: S closed). */
static int restore(cc_session *s, size_t *keep, int *rewrite) {
    cc_persist *P = s->persist;
    cc_error err;
    size_t n = 0;
    *keep = 0, *rewrite = 0;
    if (access(P->sess_path, F_OK) != 0) return 0;
    char *text = read_all(s, P->sess_path, &n, &err);
    if (!text) {
        cc_log(s->pf, "bimble-ccore: session %s ignored: %s", P->sess_path, err.msg);
        return 0;
    }
    const char *why = NULL;
    Arena a;
    arena_init(&a, 0);
    cc_step *undo = NULL, *redo = NULL;
    uint64_t *uids = NULL, *rids = NULL;
    size_t nu = 0, nr = 0;
    uint64_t fmt, size, mt, fsize, rev, next_step, saved_step = UINT64_MAX;
    cc_cmds_init(&P->sess_cmds);
    const char *nl = memchr(text, '\n', n);
    size_t first = nl ? (size_t)(nl - text) : n;
    JrError je;
    const JrVal *root = nl ? jr_parse(&a, text, first, &je) : NULL;
    if (!root) { why = "not JSON"; goto fail; }
    if (root->type != JR_OBJ) { why = "not an object"; goto fail; }
    if (get_u64(root, "format", 0, UINT64_MAX, &fmt) || fmt != SESSION_FORMAT) { why = "unknown format"; goto fail; }
    const JrVal *mp = jr_get(root, "model_path");
    if (!mp || mp->type != JR_STR || strlen(P->sess_model) != mp->n || memcmp(mp->s, P->sess_model, mp->n)) {
        why = "written for another model path";
        goto fail;
    }
    const JrVal *mtv = jr_get(root, "file_mtime_ns");
    if (file_stamp(P->sess_model, &size, &mt) || get_u64(root, "file_size", 0, 1ull << 52, &fsize) || !mtv ||
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
    if (read_steps(P, jr_get(root, "undo"), next_step, &undo, &uids, &nu, &why)) goto fail;
    if (read_steps(P, jr_get(root, "redo"), next_step, &redo, &rids, &nr, &why)) goto fail;
    for (size_t i = 1; i < nu; i++)
        if (uids[i] <= uids[i - 1]) { why = "undo ids not increasing"; goto fail; }
    for (size_t i = 1; i < nr; i++)
        if (rids[i] >= rids[i - 1]) { why = "redo ids not decreasing"; goto fail; }
    /* The model: the snapshot's text (the model file is not read), or the model file itself. */
    P->model_is_file = P->snap_null = mt_text->type == JR_NULL;
    if (P->model_is_file ? cc_session_open(s, P->sess_model, &err) : open_impl(s, P->sess_model, mt_text->s, mt_text->n, &err)) {
        why = P->model_is_file ? "the model file does not load" : "the model text does not load";
        goto fail;
    }
    if (read_selection(s, root, &why)) goto fail;
    cc_history_free(&s->hist);
    cc_history_init(&s->hist);
    s->hist.undo = undo, s->hist.nundo = s->hist.capundo = nu;
    s->hist.redo = redo, s->hist.nredo = s->hist.capredo = nr;
    undo = redo = NULL;
    free(s->undo_ids), free(s->redo_ids);
    s->undo_ids = uids, s->nundo_ids = s->cap_undo_ids = nu;
    s->redo_ids = rids, s->nredo_ids = s->cap_redo_ids = nr;
    uids = rids = NULL;
    s->rev = rev, s->next_step = next_step, s->saved_step = saved_step;
    P->snap_bytes = first + 1, P->model_file_bytes = size;
    P->W.size = size, P->W.mtime = mt;

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
        if (replay_entry(s, text + at, len, &why)) { *rewrite = 1; break; }
        at += len + 1, ok++;
    }
    *keep = at;
    P->log_bytes = at - P->snap_bytes, P->log_entries = ok;
    if (at < n)
        cc_log(s->pf, "bimble-ccore: session %s ignored: from line %zu: %s; kept the snapshot and %zu entries",
                P->sess_path, ok + 2, why, ok);
    cc_log(s->pf, "bimble-ccore: session restored from %s (rev %llu, %zu undo, %zu redo, %zu selected, %zu entries replayed)",
            P->sess_path, (unsigned long long)s->rev, s->nundo_ids, s->nredo_ids, (size_t)s->has_primary + s->nalso, P->log_entries);
    arena_free(&a);
    free(text);
    return 1;
fail:
    cc_log(s->pf, "bimble-ccore: session %s ignored: %s; starting from %s", P->sess_path, why ? why : "invalid",
            P->sess_model);
    close_session(s);
    free_steps(undo, nu), free_steps(redo, nr);
    free(uids), free(rids);
    cc_cmds_free(&P->sess_cmds);
    arena_free(&a);
    free(text);
    return 0;
}

int cc_session_open_persisted(cc_session *s, const char *model_path, const char *session_path, cc_error *err) {
    persist_free(s);
    cc_persist *P = calloc(1, sizeof *P);
    if (!P) {
        cc_log(s->pf, "bimble-ccore: no session file (out of memory)");
        return cc_session_open(s, model_path, err);
    }
    P->sess_fd = -1;
    buf_init(&P->pend_cmds);
    cc_cmds_init(&P->sess_cmds);
    s->persist = P;
    if (strlen(session_path) >= sizeof P->sess_path || strlen(model_path) >= sizeof P->sess_model) {
        cc_log(s->pf, "bimble-ccore: no session file (a path too long)");
        persist_free(s);
        return cc_session_open(s, model_path, err);
    }
    strcpy(P->sess_path, session_path);
    strcpy(P->sess_model, model_path);
    size_t keep = 0;
    int rewrite = 0;
    if (restore(s, &keep, &rewrite)) {
        P->on = 1;
        if (!rewrite) {
            P->sess_fd = open(P->sess_path, O_WRONLY | O_APPEND);
            if (P->sess_fd >= 0 && ftruncate(P->sess_fd, (off_t)keep) == 0 && remember(s) == 0) return 0;
        }
        write_snapshot(s);
        return 0;
    }
    if (cc_session_open(s, model_path, err)) {
        persist_free(s);
        return -1;
    }
    P->on = 1;
    P->model_is_file = 1;
    write_snapshot(s); /* no model text: cheap */
    return 0;
}
#endif
