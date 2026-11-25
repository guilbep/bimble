/* The native host's session: see session.h. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "file.h"
#include "ifc_in.h"
#include "session.h"

static cc_session S;

cc_session *cc_session_get(void) { return &S; }

const cc_model *cc_host_model(void) { return S.open ? &S.model : NULL; }
uint32_t cc_host_rev(void) { return (uint32_t)S.rev; }

uint64_t cc_session_step(const cc_session *s) { return s->nundo_ids ? s->undo_ids[s->nundo_ids - 1] : 0; }
int cc_session_unsaved(const cc_session *s) { return s->open && cc_session_step(s) != s->saved_step; }

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
        if (access(out, F_OK) != 0) return 0;
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
