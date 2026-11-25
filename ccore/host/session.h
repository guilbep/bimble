/* The native host's open model and what the viewer keeps beside it (crates/bimble-viewer:
 * `Viewer::loaded`, `Editor`): history, selection, rev, placing state, the file path, the saved
 * step. One session per process; calls.c reads and changes it, main.c opens it. */
#ifndef CCORE_HOST_SESSION_H
#define CCORE_HOST_SESSION_H
#include <stddef.h>
#include <stdint.h>
#include "history.h"
#include "model.h"

typedef enum { CC_KIND_COLUMN, CC_KIND_BEAM, CC_KIND_WALL, CC_KIND_SLAB } cc_kind; /* verbs.rs:29 */

typedef struct {
    int open;            /* a model is loaded (always editable: the C core imports IFC, spec §1) */
    cc_model model;
    cc_history hist;
    char path[4096];     /* where Save writes (lib.rs:1830 `l.path`) */
    const char *file;    /* the file name of `path` (calls.rs:150 `file_name`) */
    int schema_bimble;   /* path ends in .bimble (calls.rs:244) */
    uint64_t rev;        /* lib.rs:1054 scene_rev: 1 after load (lib.rs:1679), +1 per rebuild */
    double load_ms;
    /* Unsaved: the current step differs from the saved one. Each history step gets an id (1, 2,
     * …); the current step is the newest undo step's id, 0 with none. */
    uint64_t *undo_ids, *redo_ids;
    size_t nundo_ids, nredo_ids, cap_undo_ids, cap_redo_ids;
    uint64_t next_step, saved_step; /* saved_step UINT64_MAX: never saved (an imported IFC) */
    /* Selection (lib.rs `selected`, `also_selected`), by GlobalId: primary first. */
    int has_primary;
    cc_gid primary;
    cc_gid *also; /* sorted (scene order), unique */
    size_t nalso, capalso;
    /* Editor's placing fields (edit.rs:142-180). */
    cc_kind kind;
    int placing; /* edit.rs `placing.is_some()`; points stay empty: the host has no clicks */
    cc_kind placing_kind;
    int has_storey;
    cc_gid storey;
    char *material; /* malloc'd, NULL: None */
    double sizes[5]; /* column, beam_width, beam_height, wall, slab (verbs.rs:78-96) */
    double offset[3];
    double top_offset_mm;
    int polygon;
    int tab_open; /* the 3D view's tab is open and current */
} cc_session;

cc_session *cc_session_get(void);

/* Opens `path`: .ifc (any case) is imported (lib.rs:526-538: project named after the file stem,
 * Save writes `editable_copy_path`, starts unsaved), anything else is read as the .bimble JSON
 * text form. 0 ok; -1 with `err`. */
int cc_session_open(const char *path, cc_error *err);

/* The current history step id (0: none). */
uint64_t cc_session_step(const cc_session *s);
int cc_session_unsaved(const cc_session *s);

/* The open model, NULL without one; the rev of the scene it gives (for the /mesh endpoint). */
const cc_model *cc_host_model(void);
uint32_t cc_host_rev(void);

/* The session file (ticket T12): the state a restart of the host must keep, so the lab page
 * keeps its model edits, undo steps and selection across an edit -> rebuild -> restart loop.
 * One JSON object (format 1): the model path it belongs to, the size and mtime of that file when
 * written, the model text, history steps (id, label, inverse commands; oldest first), the step
 * counters, rev and the selection. Written through `<path>.tmp` + rename (no fsync: it survives
 * the host process, not a power loss).
 *
 * cc_session_persist_start: when `session_path` exists, belongs to `model_path` and the model
 * file has the same size and mtime as when it was written, replaces the open session by it.
 * The file is untrusted input: on any error it is ignored, the reason logged on stderr, and the
 * session stays as cc_session_open made it. Then turns persistence on. Returns 1 when restored.
 *
 * Writes: cc_session_get (every call and the change feed go through it, so after each call)
 * writes the file when the persisted fields changed since the last write and the history is
 * consistent (step ids beside steps); cc_session_persist_now writes now if changed (at exit). */
int cc_session_persist_start(const char *session_path, const char *model_path);
void cc_session_persist_now(void);
#endif
