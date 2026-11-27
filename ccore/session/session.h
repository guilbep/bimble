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
#include "hlod.h" /* 3D2 */
#include "ifc_read.h"
#include "measure.h"
#include "model.h"
#include "platform.h"
#include "scene.h"
#include "setting_out.h" /* SETOUT */
#include "refplanes.h" /* WAVE1B */
#include "view_sheet.h"  /* VIEWSHEET */
#include "tool.h"
#include "viewfile.h"
#include "visibility.h"

typedef enum {
    CC_KIND_COLUMN, CC_KIND_BEAM, CC_KIND_WALL, CC_KIND_SLAB, /* verbs.rs:29 */
    /* CONC1 (ADR 0077, concrete.h): the Concrete tab's kinds, run by their own verbs */
    CC_KIND_FOOTING, CC_KIND_STRIP_FOOTING, CC_KIND_RAFT, CC_KIND_OPENING, CC_KIND_SLEEVE,
    CC_KIND_GRID_AXIS, /* WAVE1B (A2): an axis of the model's grid by two clicks (verb grid_axis_add) */
    CC_KIND_COUNT
} cc_kind;
#define CC_CONC_N 11 /* concrete.c: the sizes of the CONC1 kinds */

enum { CC_VIEW_NONE, CC_VIEW_IFC, CC_VIEW_FILE };

typedef struct cc_persist cc_persist; /* the session file (session.c), NULL when off */
typedef struct cc_link cc_link;       /* a linked model (links.h) */

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
    int placing; /* edit.rs `placing.is_some()`; its points are tool.points */
    cc_kind placing_kind;
    int has_storey;
    cc_gid storey;
    char *material; /* malloc'd, NULL: None */
    double sizes[5]; /* column, beam_width, beam_height, wall, slab (verbs.rs:78-96) */
    double offset[3];
    double top_offset_mm;
    int polygon;
    double conc[CC_CONC_N]; /* CONC1: the Concrete tab's sizes (concrete.h) */
    cc_tool tool; /* the place tool's points and pointer (tool.h); cleared with the placing state */
    int tab_open; /* a view tab is current (`active`); 0: Home (views.h) */
    /* The views (ticket C1, views.h): the active view and the open view tabs, in order. */
    int has_active;
    cc_gid active;
    cc_gid *tabs;
    size_t ntabs, captabs;
    cc_vis vis;   /* hidden, isolated, the section box (visibility.h): session state, not model */
    cc_measure measure; /* the measure tool (measure.h, ticket T1): session state, not model */
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
    int mesh_form; /* 3D1: the BMSH version cached (1, or 3: shapes once, scene_bmsh.h) */
    /* 3D2 (ADR 0078): the HLOD proxies of the scene /mesh draws (mesh.h cc_mesh_proxies), built in
     * steps the page asks for, then their BPRX bytes; per (scene, rev). */
    cc_hlod *hlod;
    const cc_scene *hlod_scene;
    uint32_t hlod_rev;
    int hlod_status; /* 0 none, 202 building, 200 built (hlod_out: BPRX), 500 (hlod_out: the error) */
    Buf hlod_out;
    Buf link_prx_out; /* LNK3D: the last link proxies answered (mesh.h cc_link_proxies) */
    Buf link_mesh_out; /* SIZE2: the last link mesh answered (mesh.h cc_link_mesh) */
    double hlod_ms;  /* build time so far */
    cc_persist *persist;
    /* Linked models (ticket B5, links.h): read-only scenes drawn with the open one, in link
     * order; dropped with the open model (lib.rs:1668). links_gen + 1 on every change to them
     * (add, reload, remove, show, hide): the /mesh cache and the change JSON follow it. */
    cc_link *links;
    size_t nlinks, caplinks;
    uint64_t links_gen, mesh_links_gen;
    /* LNK3D (ADR 0078 amendment 3): + 1 only when a link's geometry changes (added, removed, read,
     * converted, unloaded): the /mesh cache and the change JSON's "links_mesh" follow it, so a hide,
     * a move, a colour or a visibility rule never makes the page fetch the mesh again. mesh_links_head:
     * the trailer's light fields (visible, placement, colour) the cached /mesh was written with. */
    uint64_t links_mesh_gen, mesh_links_mesh_gen, mesh_links_head;
    /* LNK3D: + 1 with every rev that may change what the scene draws (not a change of views, sheets or
     * schedules alone: a hide in a view, a sheet): query mesh_key, so the page fetches /mesh only then. */
    uint64_t draw_gen;
    uint64_t vis_links_gen; /* M1LINKS: the links_gen the view's rules were last folded at (m1links.h) */
    uint64_t vis_sig;       /* M1LINKS: cc_vis_sig when the /mesh trailer was last told (links_gen) */
    int has_vis_sig;        /* M1LINKS: vis_sig was taken (the first one bumps nothing) */
    struct cc_note *notes;  /* M1LINKS: the notes on linked elements (m1links.h), in the order made */
    size_t nnotes, capnotes;
    /* An element of a linked model selected (lib.rs:945 link_selected): its link (-1: none) and
     * its id text as the link's BMSH rows give it. The open model's selection is then empty. */
    int link_sel;
    char link_sel_id[22];
    /* The last sheet drawn (sheet_svg.h): what it was made for (rev, sheet, date, path), the
     * sheet_svg answer, and each drawn placed view's box (sheet_layout). */
    Buf paper_svg, paper_key;
    cc_gid *paper_ids;
    double (*paper_rects)[4];
    size_t npaper;
    /* D7: the PDF export (sheet_svg.h cc_paper_pdf_query): the Roboto files the carrier handed over
     * or the folder to read them from on the first export (malloc'd), and the version the drawing
     * names (NULL: the C core's; set by tests to compare with Rust's goldens). */
    unsigned char *pdf_font[2];
    size_t pdf_font_n[2];
    char *pdf_font_dir;
    char *paper_version;
    /* ---- Q2: the last schedule table asked (schedules.h), NULL: none ---- */
    struct cc_sched_cache *sched;
    /* ---- Q2: the ribbon's view switches (ribbon.rs:625-627 action_on): session state, not model;
     * the page draws them (edges on, hatching on, orthographic off: bimble-render lib.rs:1431-1432,
     * :236). recent: the last actions run, newest first (ribbon.rs:495-503), as ACTIONS indices. */
    int edges, hatching, orthographic;
    int recent[5];
    int nrecent;
    /* ---- FAKE1: the sheet shown (layout.rs `layout`: open_sheet, close_sheet), session state;
     * shown only while it is still a sheet of the open model (layout.rs:181) ---- */
    int sheet_shown;
    cc_gid sheet;
    /* ---- end FAKE1 ---- */
    /* ---- FS1: the recent files (settings_file.h), newest first, malloc'd; the settings file
     * they are kept in (malloc'd), NULL: in memory only ---- */
    char *recent_files[12];
    size_t nrecent_files;
    char *settings_file;
    /* ---- end FS1 ---- */
    /* ---- SET1: the user's settings (settings.h), NULL until first asked ---- */
    struct cc_user_settings *user_settings;
    /* ---- end SET1 ---- */
    /* ---- KEY1: the plan annotation toggles (toggles.h), bit i set = off; 0 (all on) as new ---- */
    unsigned plan_off;
    /* ---- end KEY1 ---- */
    /* ---- MEAS1: the element boxes of each scene the pointer can hit (measure.h cc_mboxes), one
     * slot per layer of the measure tool (the shown scene, then the visible links), malloc'd, kept
     * while their scene's key (rev, links_gen, view) holds; dropped with the model ---- */
    cc_mboxes *mboxes;
    size_t nmboxes;
    /* ---- end MEAS1 ---- */
    /* ---- SETOUT: the setting-out markers shown (setting_out.h), session state ---- */
    cc_setout setout;
    /* ---- WAVE1B: the work plane and the three-point plane tool (refplanes.h), session state ---- */
    cc_wplane wp;
    /* ---- VIEWSHEET: the section tool and the annotation types (view_sheet.h), session state ---- */
    cc_viewsheet vsh;
    /* ---- PAPER1: paper.rs paper_views, the views shown On paper (by id, unsorted, unique;
     * cleared with the model, lib.rs:1661); plan_window: the Export view window (ribbon.rs:722
     * Action::OnPaper toggles it, :630 drawn pressed) ---- */
    cc_gid *paper_views;
    size_t npaper_views, cappaper_views;
    int plan_window;
    /* ---- end PAPER1 ---- */
    /* ---- LNK1: the links list as last saved or opened (links.h cc_links_changed), NULL: none ---- */
    char *links_saved;
    size_t links_saved_n;
    /* ---- LINKF: the link conversions (links.h cc_links_tick): one at a time, smallest first ---- */
    struct cc_link_job *link_job; /* running (on the carrier's thread, or done and not yet taken) */
    uint32_t link_job_uid;        /* the link it is for (cc_link uid); 0: the link went */
    uint32_t link_uid_next;
    uint64_t link_cache_min;      /* CC_LINK_CACHE_MIN unless the carrier sets less (BIMBLE_LINK_CACHE_MIN) */
    int link_no_beside;           /* 1: caches only in the platform's cache folder (BIMBLE_LINK_CACHE_BESIDE=0) */
    struct cc_links_undo *links_undo, *links_redo; /* link_folder's undo and redo steps (links.h) */
    size_t nlinks_undo, nlinks_redo, caplinks_undo, caplinks_redo;
    /* ---- end LNK1 ---- */
    /* ---- H2a: query pending's cache (versions.c), NULL: none ---- */
    struct cc_versions_cache *versions_cache;
    /* ---- end H2a ---- */
} cc_session;

/* A new session with no model, on `pf` (kept by pointer). NULL when out of memory. */
cc_session *cc_session_new(const cc_platform *pf);
/* Frees the model, history, caches and the session file state (writes nothing). NULL is fine. */
void cc_session_free(cc_session *s);
/* MEAS1: drops the measure tool's element boxes (calls.c keeps them, measure.h cc_mboxes). */
void cc_session_mboxes_free(cc_session *s);

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

/* LINKF: the largest file an open or a link of `path` reads, by its format (an IFC: the STEP
 * reader's, a .bimbleview: its reader's, else CC_MAX_FILE_BYTES), and the reader's peak memory per
 * input byte (`peak_per_byte`, optional). cc_session_fits: 0 when a file of `size` bytes at `path`
 * is within that limit and its reader's peak within the memory the platform says is free (none
 * said: no check); -1 with "<path>: larger than N bytes", -2 with "<path>: too large for this
 * machine: ..." in `err`. Every open and link checks it before a byte is read. */
size_t cc_session_read_limit(const char *path, size_t *peak_per_byte);
int cc_session_fits(const cc_platform *pf, const char *path, uint64_t size, cc_error *err);

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
 * "Belongs to": the same path text, or the same file once the carrier resolved both
 * (pf->real_path: `m.json` and `./m.json`). FIXSES: a session file the session does not keep
 * whole (ignored, or lines dropped) and that holds steps not saved is never overwritten: it is
 * renamed `<session_path>.orphan-<unix time>` (with its model side files) first, and
 * cc_session_set_aside gives that path (`query state` "session_set_aside"); NULL when none. When
 * it cannot be renamed it is left alone and the session runs without a file.
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
const char *cc_session_set_aside(const cc_session *s); /* FIXSES */
void cc_session_note_apply(cc_session *s, const cc_command *cmds, size_t n);
#endif
