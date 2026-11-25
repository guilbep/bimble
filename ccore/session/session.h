/* A session (L5): an open model and what the viewer keeps beside it (crates/bimble-viewer:
 * `Viewer::loaded`, `Editor`): history, selection, rev, placing state, the file path, the saved
 * step, and the caches built from it (the scene and the /mesh bytes of the current rev).
 * A handle: a process holds as many as it wants (each with its own model; Mission 1, superposed
 * files). The carrier creates one with its platform (platform.h); calls.c reads and changes it.
 * No file-scope mutable state: everything lives in the cc_session. */
#ifndef CCORE_SESSION_SESSION_H
#define CCORE_SESSION_SESSION_H
#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "history.h"
#include "model.h"
#include "platform.h"
#include "scene.h"

typedef enum { CC_KIND_COLUMN, CC_KIND_BEAM, CC_KIND_WALL, CC_KIND_SLAB } cc_kind; /* verbs.rs:29 */

typedef struct cc_persist cc_persist; /* the session file (session.c), NULL when off */

typedef struct cc_session {
    const cc_platform *pf; /* the carrier's, outlives the session */
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
    /* The scene of the current rev (calls.c), built once per rev, shared by query status and
     * GET /mesh. scene_rev 0: none built. */
    cc_scene scene;
    int scene_ok;
    uint64_t scene_rev;
    /* The /mesh answer (mesh.c): BMSH bytes or the error text, per (model, rev). */
    Buf mesh;
    int mesh_status; /* 0: nothing cached */
    const cc_model *mesh_model;
    uint32_t mesh_rev;
    cc_persist *persist;
} cc_session;

/* A new session with no model, on `pf` (kept by pointer). NULL when out of memory. */
cc_session *cc_session_new(const cc_platform *pf);
/* Frees the model, history, caches and the session file state (writes nothing). NULL is fine. */
void cc_session_free(cc_session *s);

/* Opens `path`: .ifc (any case) is imported (lib.rs:526-538: project named after the file stem,
 * Save writes `editable_copy_path`, starts unsaved), anything else is read as the .bimble JSON
 * text form. 0 ok; -1 with `err`. */
int cc_session_open(cc_session *s, const char *path, cc_error *err);

/* The current history step id (0: none). */
uint64_t cc_session_step(const cc_session *s);
int cc_session_unsaved(const cc_session *s);

/* The open model, NULL without one; the rev of the scene it gives (for the /mesh endpoint). */
const cc_model *cc_session_model(const cc_session *s);
uint32_t cc_session_rev(const cc_session *s);

/* The scene of the open model at its rev, built once per rev (calls.c) and shared by query
 * status and GET /mesh; NULL when there is no model or the build failed. */
const cc_scene *cc_session_scene(cc_session *s);

/* The session file (tickets T12, T18): the state a restart of the host must keep, so the lab
 * page keeps its model edits, undo steps and selection across an edit -> rebuild -> restart loop.
 * Lines of JSON (format 2). Line 1, the snapshot: the model path it belongs to, the size and
 * mtime of that file when written, the model text (null while the model is the file's content),
 * history steps (id, label, inverse commands; oldest first), the step counters, rev and the
 * selection. Then one entry line per change, appended (no fsync: it survives the host process,
 * not a power loss): `apply` (the commands as given, the inverses kept, label, step id), `undo`,
 * `redo` (the step id), `state` (selection or rev only); each with the counters, rev and
 * selection after it. A new snapshot (through `<path>.tmp` + rename) at the start, after a Save,
 * when a call made more than one history change, and when the entries pass 2,000 or 4x the
 * snapshot's bytes (with the model file's when the text is null).
 *
 * cc_session_open_persisted: opens the model with its session file. When `session_path` exists,
 * belongs to `model_path` and the model file has the same size and mtime as in the snapshot, the
 * session is the snapshot (its model text loaded instead of the model file) with the entries
 * replayed through the history code (each validated: commands applied, the inverses compared, ids
 * and selection checked). A bad snapshot: the session file is ignored, the reason logged, and the
 * model opened as cc_session_open does. A bad or truncated entry: the replay stops there, logged,
 * the lines before are kept. Then persistence is on. 0 ok; -1 with `err` (the model file).
 *
 * cc_session_persist_now: after each call (server.c, before the answer) and at exit: appends the
 * entry for what the call changed, if anything. cc_session_note_apply: calls.c hands the commands
 * of the step it just pushed. Native only (POSIX append and stat): in the wasm module the
 * session file is never on. */
#include "command.h"
int cc_session_open_persisted(cc_session *s, const char *model_path, const char *session_path, cc_error *err);
void cc_session_persist_now(cc_session *s);
void cc_session_note_apply(cc_session *s, const cc_command *cmds, size_t n);
#endif
