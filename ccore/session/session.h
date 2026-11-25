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
#include "ifc_read.h"
#include "model.h"
#include "platform.h"
#include "scene.h"
#include "viewfile.h"

typedef enum { CC_KIND_COLUMN, CC_KIND_BEAM, CC_KIND_WALL, CC_KIND_SLAB } cc_kind; /* verbs.rs:29 */

enum { CC_VIEW_NONE, CC_VIEW_IFC, CC_VIEW_FILE };

typedef struct cc_persist cc_persist; /* the session file (session.c), NULL when off */

typedef struct cc_session {
    const cc_platform *pf; /* the carrier's, outlives the session */
    int open;            /* an editable model is loaded (`model`) */
    cc_model model;
    /* Or a read-only scene, no model (lib.rs:494-498 load_bytes, calls.rs:65-71 `model(v)` fails):
     * an IFC opened as it is (ifc_read.h) or a .bimbleview (viewfile.h). At most one of `open`
     * and `view` is set. `vscene` is the cc_scene over `vf` (its own arena holds the element
     * rows; the shapes and meshes are vf's). */
    int view;            /* CC_VIEW_NONE, CC_VIEW_IFC, CC_VIEW_FILE */
    cc_ifc_scene *ifc;   /* malloc'd while view == CC_VIEW_IFC */
    cc_viewfile *vf;     /* malloc'd while view == CC_VIEW_FILE */
    cc_scene vscene;
    cc_history hist;
    char path[4096];     /* where Save writes (lib.rs:1830 `l.path`) */
    const char *file;    /* the file name of `path` (calls.rs:150 `file_name`) */
    int schema_bimble;   /* path ends in .bimble (calls.rs:244) */
    int untitled;        /* a new model (File -> New) that has no file yet: path is CC_UNTITLED */
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

/* lib.rs:191: the path of a new model until it is saved somewhere. */
#define CC_UNTITLED "Untitled.bimble"

/* Opens `path`: .ifc (any case) is imported (lib.rs:526-538: project named after the file stem,
 * Save writes `editable_copy_path`, starts unsaved), anything else is read as a .bimble file
 * (file.h: the v1 JSON text or the v2 binary, the magic tells). A model already open is replaced
 * only when the new one loaded (its history, selection and placing go; rev goes on: +1, as
 * lib.rs:1679 `scene_rev += 1`). With the session file on, it follows (cc_session_open_persisted).
 * 0 ok; -1 with `err` (the open model, if any, is kept). */
int cc_session_open(cc_session *s, const char *path, cc_error *err);

/* File -> Open as the Rust viewer opens (lib.rs:466-498 load_any): a path ending in .ifc (any
 * case) is read for display (cc_ifc_read), read-only; .bimbleview (any case) is read as a view
 * file, read-only; anything else is cc_session_open. A read-only scene has no model: state
 * `editable` false, the edit calls answer "this file is read only: import it to edit", /mesh
 * draws the scene. Replaces what is open only when the new file loaded; rev + 1; the session file
 * pauses (nothing to keep). 0 ok; -1 with `err`. */
int cc_session_open_file(cc_session *s, const char *path, cc_error *err);

/* File -> New (lib.rs:1688-1712 start_new, verbs.rs:101 new_model): project "Untitled" with a new
 * GlobalId, one storey "Level 0" at 0 mm, path CC_UNTITLED, editable and unsaved (as an imported
 * copy). Replaces an open model; the session file pauses until a save gives the model a file.
 * 0 ok; -1 with `err` (out of memory). */
int cc_session_new_model(cc_session *s, cc_error *err);

/* Closes the open model or read-only scene (nothing after: the viewer's empty state); rev + 1. The session file
 * pauses. Writes nothing. */
void cc_session_close(cc_session *s);

/* Save (lib.rs:1810-1857) and Save as: writes the model to `path` (NULL: the session's path)
 * through the platform, file::save's form (bimble-core/src/file.rs:143-151): the v1 JSON text
 * when the path ends in ".json", else the v2 binary. Then the session's path is `path` (Save as:
 * file name and schema follow, no longer untitled), the current step is the saved one, and the
 * session file follows the saved path. 0 ok; -1 with `err` ("<path>: cannot write"). */
int cc_session_save(cc_session *s, const char *path, cc_error *err);

/* The current history step id (0: none). */
uint64_t cc_session_step(const cc_session *s);
int cc_session_unsaved(const cc_session *s);

/* The open model, NULL without one; the rev of the scene it gives (for the /mesh endpoint). */
const cc_model *cc_session_model(const cc_session *s);
uint32_t cc_session_rev(const cc_session *s);

/* The scene shown: the open model's at its rev, built once per rev (calls.c) and shared by query
 * status and GET /mesh, or the read-only scene; NULL when nothing is open or the build failed. */
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
 * The session file follows the file actions (ticket A5): after cc_session_open of another file,
 * and after a save to another path (Save as, or the first Save of an imported IFC's copy), the
 * session belongs to that file (a new snapshot; the model file's content, so no model text). When
 * `session_path` was the default `<model_path>.ccore-session` it moves to `<new>.ccore-session`;
 * a session file named explicitly keeps its name. A new model (no file yet) or a close pauses it
 * until the next open or save.
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
