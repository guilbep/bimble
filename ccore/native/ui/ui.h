/* bimble, the native Linux app (ticket NUI1, ADR 0084; spike NAT2's prototype I): the C session
 * in-process (cc_call, as the host's POST /<op>/<name>), the native renderer (../render.h) and a Dear
 * ImGui shell (cimgui, C) drawn in the SAME GPU command buffer. Its declarative parts come from the web
 * UI's own data (data/ JSON files, written by web-ui/scripts/export-shell-data.mjs, embedded at build time):
 * the tabs, the ribbon, the key table, the strings, the shell machine's shape. */
#ifndef BIMBLE_NATIVE_UI_H
#define BIMBLE_NATIVE_UI_H
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include "../render.h"
#include "../loader.h"
#include "arena.h"
#include "buf.h"
#include "json_r.h"
#include "session.h"

/* ---- colours: web-ui/src/brand/tokens.css (ImU32 is ABGR) ---- */
typedef struct {
    ImU32 cream, sheet, sunk, ink, stone, line, clay, pink, ochre, slate, moss, muted, view_bg;
} Theme;
extern Theme TH;
#define C_CREAM TH.cream
#define C_SHEET TH.sheet
#define C_SUNK TH.sunk
#define C_INK TH.ink
#define C_STONE TH.stone
#define C_LINE TH.line
#define C_CLAY TH.clay
#define C_PINK TH.pink
#define C_OCHRE TH.ochre
#define C_SLATE TH.slate
#define C_MOSS TH.moss
#define C_MUTED TH.muted

/* A JSON answer kept with its arena. */
typedef struct {
    Arena arena;
    JrVal *root; /* NULL: none (or an error, in `err`) */
    char err[200];
} Json;

/* The ribbon from data/ribbon.json. */
typedef struct {
    const char *key, *action, *page, *dialog, *panel;
    int icon; /* index in the icon atlas, -1 none */
} RTool;
typedef struct {
    const char *key;
    RTool *tools;
    int ntools;
    int has_box;
} RGroup;
typedef struct {
    const char *id, *group;
    RGroup *groups;
    int ngroups;
} RTab;

enum { SCR_HOME, SCR_OPENING, SCR_MAIN };
enum { SHOW_VIEW, SHOW_PAGE };
enum { DLG_NONE, DLG_OPEN, DLG_LINK, DLG_SAVEAS, DLG_SETTINGS, DLG_PERF, DLG_KEYS, DLG_DISCARD };

/* A link as the scene draws it (view3d.c). */
typedef struct {
    uint32_t uid;
    const void *ident;   /* its data (loader.h nv_link_ident) */
    double c, s, off[3]; /* its placement in the scene */
    int colour, visible;
    unsigned proxies_gen;
    nv_part *proxy, *detail;
    int proxy_state, detail_state; /* 0 none, 1 queued, 2 drawn, 3 failed, 4 left as proxies (the GPU budget) */
    uint32_t elem0, nelem, gen;
    char name[256];
    double lo[3], hi[3];
    int has_box;
    uint64_t tris;
    double t_ready, t_proxy, t_detail, load_ms;
    nv_tileset *ts;      /* NUI2: drawn by tiles (its cache's section TILE); NULL: as parts */
    char (*ids)[22];     /* its elements' GlobalIds (tiles: no part holds them) */
    int tiles_inflight;
} SLink;

typedef struct App App;

/* The scripted driver (drive.c): one step a line. */
typedef struct {
    double t_ms;   /* when (since the script started) */
    char op[24];
    char arg[512];
    float x, y;
} Step;

typedef struct { double t; double ms; char what[48]; } InputTime;

struct App {
    /* the process */
    double t_start, t_first_picture, t_complete, t_open;
    int offscreen;
    float scale;         /* UI scale (offscreen: --scale; a window: its framebuffer's density) */
    uint32_t lw, lh;     /* window size, logical px */
    uint32_t pw, ph;     /* pixels */
    SDL_Window *win;
    SDL_GPUDevice *dev;
    SDL_GPUTexture *screen; /* the composed picture: 3D + UI (RGBA8, pw x ph) */
    const char *gpu_name, *gpu_driver;
    int quit;
    /* the core */
    cc_session *ses;
    nv_loader *loader;
    char model_path[1024];
    char config_dir[1024];
    Json actions, caps, state, views, links, sel_el, status, recent, settings, notes, mesh_key;
    int core_rev;        /* bumped after each call that may change the model: queries are asked again */
    int asked_rev;
    int links_rev, links_synced, links_busy;
    int mesh_draw_key;
    double links_polled;
    /* data shared with the web UI */
    Arena data;
    RTab *tabs;
    int ntabs, tab;
    JrVal *strings;      /* object key -> text (current language) */
    JrVal *strings_fr, *strings_en;
    int lang_en;
    JrVal *keys;         /* array of KeyRow */
    JrVal *action_cap;   /* object action -> capability */
    JrVal *machine;      /* the shell machine's shape (machine.json) */
    /* icons (icons.c) */
    SDL_GPUTexture *icon_tex;
    int icon_n, icon_cols, icon_px;
    const char **icon_src;
    /* fonts */
    ImFont *f_text, *f_semi, *f_disp, *f_mono;
    int theme;           /* 0 light, 1 dark */
    /* the 3D view */
    nv_render *r;
    nv_render_opts ro;
    nv_camera cam;
    int hlod;            /* links' proxies for far cells (default on) */
    uint64_t gpu_budget; /* the links' detail on the GPU, bytes (settings link_gpu_mb; 0: off, no limit) */
    int moving_fps;      /* settings moving_fps: the moving budget's target (0: off, every frame whole) */
    nv_part *model_part;
    SLink links_s[64];
    int nlinks;
    uint32_t link_gen, next_elem;
    double origin0[3];
    double core_origin[3];     /* the open scene's origin (world mm): the core's rays and boxes are relative to it */
    int have_origin, draw_key, fitted, ids_dirty, vis_dirty;
    uint32_t *id_slot;
    size_t id_cap;
    double load_ms_total;
    uint64_t load_peak_bytes;
    float vx, vy, vw, vh;      /* the viewport rect (logical px) */
    uint32_t vpw, vph;         /* its pixels */
    int dirty;                 /* the 3D picture must be drawn again */
    int moving;                /* a drag or a wheel in the last 150 ms */
    double last_move;
    float moving_cut, still_cut, cut_live; /* cut_live: the moving budget's cut now */
    int adaptive;
    int drag;                  /* 0, 1 left, 2 right/middle */
    float lx, ly, dx0, dy0;
    int drag_moved;
    int pick_pending, pick_x, pick_y, pick_add;
    uint32_t msel[256];        /* NUI2, Ctrl-click: the other selected elements (global + 1), the last picked is `selected` */
    int nmsel;
    int drive_ctrl;            /* the driver's Ctrl held for the next click */
    uint32_t selected;         /* global element + 1, 0 none */
    char sel_id[23], sel_link[256];
    int box_on, box_drag;      /* the section box (scene mm) and its handle held */
    double box_min[3], box_max[3];
    char view_name[128];       /* the view shown ("" the 3D view) */
    double view_lo[3], view_hi[3]; /* the view's clip (scene mm) while a plan or section is shown */
    int view_clip;
    /* the shell */
    int screen_kind;           /* SCR_* */
    int show;                  /* SHOW_VIEW / SHOW_PAGE */
    char page[64];
    int dialog;
    int file_menu;
    int links_panel;
    int ctx_menu;
    float ctx_x, ctx_y;
    char fs_dir[1024];
    char fs_purpose[24];
    Json fs;
    uint8_t fs_sel[5000];
    char fs_name[256];
    int measure_on;
    Json measure;
    char notice[512];
    double notice_t;
    char pending_open[1024];   /* the file to open after the unsaved question */
    char open_err[512];
    /* numbers */
    double frame_ms, ui_ms, gpu_ms;     /* the last frame: 3D record->fence, UI build, whole frame */
    double last3d_ms;
    uint64_t frames;
    double t_ui_done;          /* the last frame's UI and core work done (before the GPU) */
    double input_t;            /* time of the oldest input not yet answered by a frame (ms), 0 none */
    char input_what[48];
    float ring_move[2048], ring_still[2048];
    int rec_move0, rec_still0, rec_inputs0, ninputs;
    double rec_seconds, rec_slowest;
    int rec_done;
    int nmove, nstill;
    InputTime slow[8];
    int nslow;
    double rec_until;          /* Performance: Record 10 s */
    FILE *rec;
    char rec_path[1024];
    uint64_t peak_rss, rss;
    /* the driver (drive.c) */
    const char *script, *preset, *out_dir;
    Step *steps;
    int nsteps, step_i;
    double script_t0;
    int script_wait;           /* waiting for: 1 complete, 2 a frame */
    FILE *flog, *ilog, *mlog;
    double next_mem;
    int quit_after;            /* --quit-after N frames once complete */
    int fails;
    /* NUI2: the window's sizes as acquired, the session log, SDL's assertions */
    uint32_t swap_w, swap_h;     /* the target acquired this frame (the swapchain's; offscreen: the virtual one) */
    int session_log;             /* native.json session_log (default on in a window) */
    unsigned sdl_asserts;
    char *resize_script;         /* offscreen: "WxH,WxH,..." one every 3 frames, the swapchain a frame after the window */
    int resize_i, resize_wait;
    uint32_t resize_w, resize_h; /* the virtual swapchain's next size (applied a frame after the window's) */
    char notice_path[1200];      /* the last report saved (Performance) */
    /* NUI2: the last frame's time by stage (ms): the app's logic (core, loader results, the walk), the swapchain's
     * wait, the UI's build, the 3D frame's recording (CPU), the UI's recording, the GPU (submit -> fence) */
    double st_logic, st_acquire, st_ui, st_rec3d, st_recui, st_gpu;
    unsigned bench_skip;
    /* NUI2: the tile path (renderer_path=tiles), its options, the tile jobs on their way */
    int use_tiles;
    nv_tiles_opts topts;
    int tile_jobs;
    nv_tileset *model_ts;        /* the open model's pyramid (built by a loader job after the open) */
    int model_tiles;             /* 0 none, 1 building, 2 drawn by tiles, 3 failed (drawn as a part) */
    const void *model_ident;     /* the session's scene the pyramid was asked for */
    double model_off[3];
    char (*model_ids)[22];       /* a view file opened (no mesh answer): its elements' ids for the tiles */
    uint32_t model_elem0, model_nE;
    uint64_t tri_live;           /* the moving budget's triangles for the walker (0: none) */
    double tiles_checked;
    int truth;                   /* --truth: each 3D frame drawn with and without occlusion, the id buffers compared */
    char settings_line[600];     /* the optimisations in force (the report's line, for the summary) */
    int truth_missing, truth_ties, truth_small; /* truth_small: of the missing, those with 8 pixels or fewer (edge-on slivers) */           /* the truth test: element ids missing from this frame with occlusion (-1: not tested) */
};

/* core.c */
int core_call(App *a, const char *op, const char *name, const char *body, Buf *out);
int core_query(App *a, const char *name, const char *body, Json *j);
int json_parse(Json *j, const char *text, size_t n);
void json_free(Json *j);
const char *js(const JrVal *v, const char *key);             /* a string member, or "" */
double jnum(const JrVal *v, const char *key, double dflt);
int jb(const JrVal *v, const char *key);
const JrVal *action_row(App *a, const char *name);
int cap_has(App *a, const char *name);
int action_not_yet(App *a, const char *name);
int native_not_yet(const char *what);                         /* a page or dialog the native shell has not built */
void core_refresh(App *a);                                    /* the queries, when the model changed */
void core_poll(App *a);                                       /* links' conversions, the session's tick */
int core_action(App *a, const char *name, const char *body);
const char *T(App *a, const char *key);                       /* the string for `key` (the key itself if none) */
const char *Tf(App *a, char *out, size_t n, const char *key, const char *var, const char *val);
int data_load(App *a);
void core_select(App *a, uint32_t elem1);
void core_select_add(App *a, uint32_t elem1); /* Ctrl-click: added, or removed when it was selected */
void core_visibility(App *a);
int core_key(App *a, SDL_Keycode k, SDL_Keymod mod, int typing);
void json_str(Buf *b, const char *s);                         /* a JSON string literal */
void app_open(App *a, const char *path);                      /* File > Open (asks about unsaved changes) */
void app_new(App *a);
void app_close(App *a);
/* view3d.c */
void scene_reset(App *a);
void scene_tick(App *a);
int scene_complete(App *a);
int scene_box(App *a, double lo[3], double hi[3]);
void scene_ids(App *a);
uint32_t scene_find(App *a, const char *id, size_t n);
void tiles_check(App *a);
int scene_elem(App *a, uint32_t elem, char id[23], char *link, size_t nlink);
/* icons.c */
int icons_build(App *a);
void icon_draw(App *a, ImDrawList *dl, int icon, float x, float y, float size, ImU32 tint);
int icon_for(App *a, const char *action);
/* theme.c */
int theme_apply(App *a);
void theme_set(App *a, int which);
int theme_count(void);
const char *theme_id(int k);
/* shell.c */
void shell_top(App *a);
void shell_status(App *a);
void shell_context_menu(App *a);
int text_button(App *a, const char *id, const char *label, int primary, int on);
void tool_run_action(App *a, const char *action);
/* panels.c */
void panel_navigator(App *a, float x, float y, float w, float h);
void panel_properties(App *a, float x, float y, float w, float h);
void panel_links(App *a);
/* home.c */
void home_screen(App *a);
void dialogs(App *a);
void fs_open(App *a, const char *dir, const char *purpose);
void perf_report(App *a, Buf *b);
void perf_copy(App *a);              /* Performance › Copy (also to a file) */
void perf_save_as(App *a);           /* Performance › Save report… */
void optimisations_apply(App *a);   /* the settings' optimisations into the renderer (at open) */
void optimisations_ui(App *a);      /* the "Optimisations" group (Settings, Performance) */
/* tools.c */
void tool_measure_input(App *a, float mx, float my, int click);
void tool_measure_draw(App *a, ImDrawList *dl);
int tool_box_input(App *a, const char *what, float mx, float my);
void tool_box_draw(App *a, ImDrawList *dl);
int view_cube(App *a, ImDrawList *dl, float mx, float my, int click);
void view_set(App *a, const char *name);                     /* a view of `query views` (plans, sections, 3D) */
void view_top(App *a);
void view_home(App *a);
/* page_quantities.c */
void page_quantities(App *a, float x, float y, float w, float h);
/* drive.c */
int drive_setup(App *a);
void drive_step(App *a, double now);
void drive_frame(App *a, double now, int drew3d);
void drive_finish(App *a);
void app_shot(App *a, const char *path);
void app_input_done(App *a, double now);
/* slog.c: the local session log */
int slog_open(App *a);
void slog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void slog_sdl_hooks(App *a);
void slog_frame(App *a, double now, int drew3d, double frame_ms);
void slog_input(App *a, const char *what, double app_ms, double ms);
void slog_report(App *a, const char *name);
void slog_close(App *a);
const char *slog_dir(void);
int slog_last(void);
int slog_save_report(App *a, char *path, size_t n);
/* nset.c: the native app's own settings (native.json) */
void nset_load(App *a);
const char *nset_get(const char *key);
double nset_num(const char *key, double dflt);
void nset_set(const char *key, const char *json, int run);
/* compare.c */
int compare_main(int argc, char **argv);
/* app.c */
void app_fit(App *a);
double app_now(void);
uint64_t app_rss(uint64_t *peak);
void app_pick_at(App *a, float x, float y);
static inline ImVec2 V2(float x, float y) { ImVec2 v = {x, y}; return v; }
static inline ImVec4 col4(ImU32 c) { ImVec4 v = {(c & 255) / 255.f, ((c >> 8) & 255) / 255.f, ((c >> 16) & 255) / 255.f, ((c >> 24) & 255) / 255.f}; return v; }
static inline ImVec4 V4(float x, float y, float z, float w) { ImVec4 v = {x, y, z, w}; return v; }
#endif
