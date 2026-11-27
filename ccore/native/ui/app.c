/* bimble, the native Linux app (ticket NUI1, ADR 0084). One process: the C session (cc_call
 * in-process, no HTTP), the native renderer (../render.h) with its loader threads (../loader.h), and a
 * Dear ImGui shell (cimgui, C) drawn in the SAME command buffer: the 3D frame into its target, then the
 * UI's render pass over the window's picture, the 3D target drawn by the UI as an image.
 *
 *   bimble [--en|--fr] [--dark] [file.bimble|file.ifc|file.bimbleview|file.json]
 *   bimble --offscreen [--size WxH] [--scale k] [--script file | --preset orbit-zoom] [--out dir]
 *          [--shot file.png] [--quit-after n] [file]
 *   options: --edges lines|screen|none, --no-hlod, --loaders n, --config-dir dir, --link-folder dir
 *
 * The window: left drag orbits, right or middle drag pans, the wheel zooms about the cursor, a click
 * picks (Properties, the core's selection), the keys of the key table (data/keys.json). No file: the
 * Home screen (recent files, New, Open). */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ui.h"
#include "backend.h"
#include "calls.h"
#include "fs.h"
#include "host.h"
#include "links.h"
#include "settings_file.h"

#define BIMBLE_SDL_MIN_MAJOR 3
#define BIMBLE_SDL_MIN_MINOR 4 /* the GPU API (3.2) and SDL_SavePNG (3.4) */

double app_now(void) { return nv_now_ms(); }
static double sdl_base; /* app_now() - SDL ticks: an event's own time on our clock */

uint64_t app_rss(uint64_t *peak) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    uint64_t rss = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long v;
        if (sscanf(line, "VmRSS: %llu kB", &v) == 1) rss = v << 10;
        if (peak && sscanf(line, "VmHWM: %llu kB", &v) == 1) *peak = v << 10;
    }
    fclose(f);
    return rss;
}

void app_fit(App *a) {
    double lo[3], hi[3];
    if (!scene_box(a, lo, hi)) return;
    int ortho = a->cam.ortho;
    double yaw = a->cam.yaw, pitch = a->cam.pitch;
    nv_cam_fit_box(&a->cam, lo, hi);
    a->cam.ortho = ortho, a->cam.yaw = yaw, a->cam.pitch = pitch;
    a->dirty = 1;
}

void app_pick_at(App *a, float x, float y) {
    a->pick_pending = 1;
    a->pick_x = (int)((x - a->vx) / a->vw * a->vpw), a->pick_y = (int)((y - a->vy) / a->vh * a->vph);
    a->dirty = 1;
}

static void input_start(App *a, const SDL_Event *ev, const char *what) {
    if (a->input_t) return;
    double t = ev->common.timestamp ? sdl_base + ev->common.timestamp / 1e6 : app_now();
    double now = app_now();
    a->input_t = t > now || t < now - 1000 ? now : t;
    snprintf(a->input_what, sizeof a->input_what, "%s", what);
}

/* ---- the 3D view's input ---- */
static int in_view(App *a, float x, float y) { return x >= a->vx && y >= a->vy && x < a->vx + a->vw && y < a->vy + a->vh; }

static void view_input(App *a, const SDL_Event *ev, int ui_wants_mouse) {
    if (a->screen_kind != SCR_MAIN || a->show != SHOW_VIEW) {
        if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat) input_start(a, ev, "key"), core_key(a, ev->key.key, ev->key.mod, igGetIO_Nil()->WantTextInput), a->dirty = 1;
        return;
    }
    switch (ev->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        float x = ev->button.x, y = ev->button.y;
        if (ui_wants_mouse || !in_view(a, x, y)) break;
        a->ctx_menu = 0;
        if (view_cube(a, NULL, x, y, 1)) { input_start(a, ev, "view cube"); break; }
        if (a->box_on && ev->button.button == SDL_BUTTON_LEFT && !a->measure_on && tool_box_input(a, "box_press", x, y)) {
            a->box_drag = 1; /* a handle of the section box is held: no orbit */
            input_start(a, ev, "box handle");
            break;
        }
        a->drag = ev->button.button == SDL_BUTTON_LEFT ? 1 : ev->button.button == SDL_BUTTON_RIGHT ? 3 : 2;
        a->lx = a->dx0 = x, a->ly = a->dy0 = y, a->drag_moved = 0;
        input_start(a, ev, "press");
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (a->box_drag) { tool_box_input(a, "box_release", ev->button.x, ev->button.y); a->box_drag = 0; a->vis_dirty = 1; break; }
        if (a->drag == 1 && !a->drag_moved) {
            input_start(a, ev, a->measure_on ? "measure click" : "pick");
            if (a->measure_on) tool_measure_input(a, ev->button.x, ev->button.y, 1);
            else app_pick_at(a, ev->button.x, ev->button.y), a->pick_add = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
            a->dirty = 1;
        } else if (a->drag == 3 && !a->drag_moved) { /* a right click: the context menu (the element under it picked first) */
            input_start(a, ev, "context menu");
            app_pick_at(a, ev->button.x, ev->button.y);
            a->ctx_menu = 1, a->ctx_x = ev->button.x, a->ctx_y = ev->button.y;
        }
        a->drag = 0;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (a->box_drag) {
            input_start(a, ev, "box drag");
            tool_box_input(a, "box_move", ev->motion.x, ev->motion.y);
            a->moving = 1, a->last_move = app_now();
            break;
        }
        if (!a->drag) {
            if (a->measure_on && in_view(a, ev->motion.x, ev->motion.y) && !ui_wants_mouse) {
                input_start(a, ev, "measure move");
                tool_measure_input(a, ev->motion.x, ev->motion.y, 0);
                a->dirty = 1;
            }
            break;
        }
        if (fabsf(ev->motion.x - a->dx0) + fabsf(ev->motion.y - a->dy0) > 3) a->drag_moved = 1;
        if (!a->drag_moved) break;
        input_start(a, ev, a->drag == 1 ? "orbit" : "pan");
        if (a->drag == 1 && !a->cam.ortho) nv_cam_orbit(&a->cam, ev->motion.x - a->lx, ev->motion.y - a->ly);
        else nv_cam_pan(&a->cam, ev->motion.x - a->lx, ev->motion.y - a->ly, a->vh);
        a->lx = ev->motion.x, a->ly = ev->motion.y;
        a->moving = 1, a->last_move = app_now(), a->dirty = 1;
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float mx = ev->wheel.mouse_x, my = ev->wheel.mouse_y;
        if (ui_wants_mouse || !in_view(a, mx, my)) break;
        input_start(a, ev, "wheel");
        nv_cam_zoom_at(&a->cam, ev->wheel.y > 0 ? 1 : -1, mx - a->vx, my - a->vy, a->vw, a->vh);
        a->moving = 1, a->last_move = app_now(), a->dirty = 1;
        break;
    }
    case SDL_EVENT_KEY_DOWN: /* the key table, data/keys.json (keys.ts KEYS) */
        if (!ev->key.repeat) {
            input_start(a, ev, "key");
            core_key(a, ev->key.key, ev->key.mod, igGetIO_Nil()->WantTextInput);
        }
        a->dirty = 1;
        break;
    default: break;
    }
}

/* ---- the open, on a thread of its own (an IFC read for display takes seconds): the UI shows its
 * progress and makes no call meanwhile ---- */
typedef struct {
    App *a;
    char path[1024];
    int done, rc;
    char err[512];
} Opener;
static Opener opener;
static pthread_t opener_th;
static int opener_on;

static void *open_run(void *arg) {
    Opener *o = arg;
    App *a = o->a;
    Buf out, body;
    buf_init(&out), buf_init(&body);
    nv_ses_wr(a->loader);
    if (!strcmp(o->path, ":new")) o->rc = cc_call(a->ses, "action", "new", "{\"discard\":true}", 16, &out);
    else {
        buf_puts(&body, "{\"discard\":true,\"path\":"), json_str(&body, o->path), buf_puts(&body, "}");
        o->rc = cc_call(a->ses, "action", "open", body.p, body.len, &out);
    }
    nv_ses_unwr(a->loader);
    if (o->rc != CC_OK) snprintf(o->err, sizeof o->err, "%.*s", (int)(out.len < 500 ? out.len : 500), out.p ? out.p : "");
    buf_free(&out), buf_free(&body);
    __atomic_store_n(&o->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void opening_tick(App *a) {
    if (!opener_on) {
        scene_reset(a);
        memset(&opener, 0, sizeof opener);
        opener.a = a;
        snprintf(opener.path, sizeof opener.path, "%s", a->model_path);
        if (pthread_create(&opener_th, NULL, open_run, &opener) == 0) opener_on = 1;
        else open_run(&opener), opener_on = 1;
        return;
    }
    if (!__atomic_load_n(&opener.done, __ATOMIC_ACQUIRE)) return;
    pthread_join(opener_th, NULL);
    opener_on = 0;
    if (opener.rc != CC_OK) {
        snprintf(a->open_err, sizeof a->open_err, "%s: %s", a->model_path, opener.err);
        fprintf(stderr, "bimble: %s\n", a->open_err);
        a->screen_kind = SCR_HOME;
        core_query(a, "recent_files", NULL, &a->recent);
        a->fails++;
        return;
    }
    a->screen_kind = SCR_MAIN;
    a->show = SHOW_VIEW;
    a->core_rev++;
    a->view_name[0] = 0, a->cam.ortho = 0, a->view_cut = 0;
    a->cam.yaw = -55 * NV_PI / 180, a->cam.pitch = 28 * NV_PI / 180;
    json_free(&a->measure), json_free(&a->sel_el);
    a->dirty = 1;
}

/* ---- one frame: the 3D (when dirty), then the UI over the whole window, one command buffer ---- */
static void frame(App *a) {
    double t0 = app_now();
    if (a->screen_kind == SCR_OPENING) opening_tick(a);
    core_poll(a);
    core_refresh(a);
    if (a->screen_kind == SCR_MAIN) {
        a->measure_on = jb(action_row(a, "measure"), "pressed");
        scene_tick(a);
    }
    float s = a->scale, W = (float)a->lw, H = (float)a->lh;
    float top = 178 * s, bottom = 32 * s, left = 296 * s, right = a->selected ? 340 * s : 0;
    a->vx = left, a->vy = top + 38 * s, a->vw = W - left - right, a->vh = H - top - bottom - 38 * s;
    float px = a->offscreen ? 1.0f : SDL_GetWindowPixelDensity(a->win);
    uint32_t vpw = (uint32_t)(a->vw * px), vph = (uint32_t)(a->vh * px);
    if (vpw != a->vpw || vph != a->vph) a->vpw = vpw, a->vph = vph, nv_render_targets(a->r, vpw, vph), a->dirty = 1;
    nui_backend_new_frame();
    igNewFrame();
    if (a->screen_kind != SCR_MAIN) home_screen(a);
    else {
        shell_top(a);
        panel_navigator(a, 0, top, left, H - top - bottom);
        if (right > 0) panel_properties(a, W - right, top, right, H - top - bottom);
        igSetNextWindowPos(V2(left, top), 0, V2(0, 0)); /* the view tabs */
        igSetNextWindowSize(V2(W - left - right, 38 * s), 0);
        igBegin("##viewtabs", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImDrawList *dl = igGetWindowDrawList();
        char key[96];
        const char *label = a->show == SHOW_PAGE ? (snprintf(key, sizeof key, "na.screen.%s", a->page), T(a, key)) : a->view_name[0] ? a->view_name : "3D";
        ImVec2 ls = igCalcTextSize(label, NULL, false, -1);
        ImDrawList_AddRectFilled(dl, V2(left + 10 * s, top + 6 * s), V2(left + 34 * s + ls.x, top + 32 * s), C_SHEET, 13 * s, 0);
        ImDrawList_AddRect(dl, V2(left + 10 * s, top + 6 * s), V2(left + 34 * s + ls.x, top + 32 * s), C_LINE, 13 * s, 1 * s, 0);
        ImDrawList_AddText_Vec2(dl, V2(left + 22 * s, top + 10 * s), C_INK, label, NULL);
        igEnd();
        if (a->show == SHOW_PAGE && !strcmp(a->page, "quantities")) page_quantities(a, a->vx, a->vy, a->vw, a->vh);
        else if (a->show == SHOW_PAGE) {
            igSetNextWindowPos(V2(a->vx, a->vy), 0, V2(0, 0));
            igSetNextWindowSize(V2(a->vw, a->vh), 0);
            igBegin("##page", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            igPushFont(a->f_disp, 20), igTextUnformatted(label, NULL), igPopFont();
            igTextWrapped("%s", T(a, "native.notYet"));
            if (text_button(a, "weblab", T(a, "native.openInLab"), 1, 0)) tool_run_action(a, ":weblab");
            igEnd();
        } else {
            ImDrawList *bg = igGetBackgroundDrawList_Nil();
            SDL_GPUTexture *t3d = a->r ? nv_render_color(a->r) : NULL; /* none yet: nothing drawn (a NULL texture crashes the backend) */
            ImTextureRef_c ref = {NULL, (ImTextureID)(uintptr_t)t3d};
            if (t3d) ImDrawList_AddImage(bg, ref, V2(a->vx, a->vy), V2(a->vx + a->vw, a->vy + a->vh), V2(0, 0), V2(1, 1), 0xFFFFFFFFu);
            ImDrawList *fg = igGetForegroundDrawList_Nil();
            tool_box_draw(a, fg);
            tool_measure_draw(a, fg);
            ImVec2 mp = igGetIO_Nil()->MousePos;
            view_cube(a, fg, mp.x, mp.y, 0);
        }
        panel_links(a);
        shell_context_menu(a);
        shell_status(a);
    }
    dialogs(a);
    if (a->moving && app_now() - a->last_move > 150) a->moving = 0, a->dirty = 1; /* still: the whole picture again */
    double t_ui = app_now();
    a->ui_ms = t_ui - t0;
    a->t_ui_done = t_ui;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(a->dev);
    if (!cmd) { fprintf(stderr, "bimble: no command buffer: %s\n", SDL_GetError()); a->quit = 1; return; }
    int drew3d = 0, pick = a->pick_pending;
    if ((a->dirty || pick) && a->show == SHOW_VIEW && a->screen_kind == SCR_MAIN && a->r) {
        nv_render_set_cut(a->r, a->moving ? a->cut_live : a->still_cut);
        nv_render_record(a->r, cmd, &a->cam, pick, a->pick_x, a->pick_y);
        drew3d = 1, a->dirty = 0;
    }
    ImVec4 bgc = col4(C_CREAM);
    float clear[4] = {bgc.x, bgc.y, bgc.z, 1};
    nui_backend_render(cmd, a->screen, 1, clear);
    SDL_GPUTexture *sw = NULL;
    Uint32 sw_w = 0, sw_h = 0;
    if (a->win && !a->offscreen && SDL_WaitAndAcquireGPUSwapchainTexture(cmd, a->win, &sw, &sw_w, &sw_h) && sw) {
        SDL_GPUBlitInfo bi = {.source = {.texture = a->screen, .w = a->pw, .h = a->ph}, .destination = {.texture = sw, .w = sw_w, .h = sw_h},
                              .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_NEAREST};
        SDL_BlitGPUTexture(cmd, &bi);
    }
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (f) SDL_WaitForGPUFences(a->dev, true, &f, 1), SDL_ReleaseGPUFence(a->dev, f);
    double t1 = app_now();
    a->gpu_ms = t1 - t_ui;
    a->frame_ms = drew3d ? t1 - t_ui : 0;
    if (drew3d) {
        a->last3d_ms = a->frame_ms;
        if (a->moving) a->ring_move[a->nmove++ % 2048] = (float)a->frame_ms;
        else a->ring_still[a->nstill++ % 2048] = (float)a->frame_ms;
    }
    /* the moving budget (the page's idea, ADR 0078): while moving, the screen-size cut grows until a
     * frame fits in ~16 ms, and shrinks back when there is room; still frames are always whole */
    if (drew3d && a->moving && a->adaptive) {
        if (a->frame_ms > 20 && a->cut_live < 16) a->cut_live = a->cut_live < 1 ? 1 : a->cut_live * 1.5f;
        else if (a->frame_ms < 8 && a->cut_live > a->moving_cut) a->cut_live = a->cut_live / 1.5f < a->moving_cut ? a->moving_cut : a->cut_live / 1.5f;
    }
    if (drew3d) {
        uint32_t picked = 0;
        nv_render_collect(a->r, pick ? &picked : NULL);
        if (pick) a->pick_pending = 0, core_select(a, picked > NV_ELEM0 ? picked : 0);
        if (!a->t_first_picture && nv_render_nparts(a->r) > 0) a->t_first_picture = t1;
    }
    if (a->screen_kind == SCR_MAIN && !a->t_complete && a->t_first_picture && scene_complete(a)) a->t_complete = t1;
    if (a->input_t) app_input_done(a, t1);
    drive_frame(a, t1, drew3d);
    a->frames++;
}

void app_shot(App *a, const char *path) {
    uint32_t bytes = a->pw * a->ph * 4;
    SDL_GPUTransferBufferCreateInfo tci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = bytes};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(a->dev, &tci);
    if (!tb) return;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(a->dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src = {.texture = a->screen, .w = a->pw, .h = a->ph, .d = 1};
    SDL_GPUTextureTransferInfo dst = {.transfer_buffer = tb};
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (f) SDL_WaitForGPUFences(a->dev, true, &f, 1), SDL_ReleaseGPUFence(a->dev, f);
    const uint8_t *m = SDL_MapGPUTransferBuffer(a->dev, tb, false);
    if (m) {
        if (nv_write_image(path, m, a->pw, a->ph)) fprintf(stderr, "bimble: cannot write %s\n", path);
        SDL_UnmapGPUTransferBuffer(a->dev, tb);
    }
    SDL_ReleaseGPUTransferBuffer(a->dev, tb);
}

static int config_dir(char *buf, size_t n, const char *given) {
    if (given && *given) return snprintf(buf, n, "%s", given) > 0;
    const char *e = getenv("BIMBLE_CONFIG_DIR");
    if (e && *e) return snprintf(buf, n, "%s", e) > 0;
    const char *x = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (x && *x) return snprintf(buf, n, "%s/bimble", x) > 0;
    if (home && *home) return snprintf(buf, n, "%s/.config/bimble", home) > 0;
    return 0;
}

static void usage(void) {
    fprintf(stderr, "usage: bimble [--en|--fr] [--dark] [file]\n"
                    "       bimble --offscreen [--size WxH] [--scale k] [--script file | --preset orbit-zoom] [--out dir]\n"
                    "              [--shot file.png] [--quit-after n] [file]\n"
                    "       options: --edges lines|screen|none, --no-hlod, --loaders n, --config-dir dir, --link-folder dir, --version\n");
    exit(2);
}

static void no_gpu(const char *what, const char *file) {
    fprintf(stderr,
            "bimble: %s: %s\n"
            "bimble: the native app needs SDL %d.%d or newer with a Vulkan device (Mesa, or the GPU's own driver).\n"
            "bimble: the web lab runs without it: bimble-ccore open%s%s\n",
            what, SDL_GetError(), BIMBLE_SDL_MIN_MAJOR, BIMBLE_SDL_MIN_MINOR, file ? " " : "", file ? file : "");
}

int main(int argc, char **argv) {
    static App app;
    App *a = &app;
    a->t_start = app_now();
    a->scale = 1, a->lw = 1492, a->lh = 790;
    a->ro = (nv_render_opts){.edges = NV_EDGES_SCREEN, .hlod = 1, .hlod_px = 1};
    a->hlod = 1;
    a->moving_cut = 2, a->still_cut = 0, a->adaptive = 1;
    a->draw_key = -1, a->links_synced = -1;
    const char *model = NULL, *shot = NULL, *link_folder = NULL, *conf = NULL;
    int loaders = 2, dark = 0, lang = -1;
    for (int i = 1; i < argc; i++) {
        const char *x = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(x, "--version")) { printf("bimble %s (native)\n", CC_RELEASE_VERSION); return 0; }
        else if (!strcmp(x, "--size") && v) { if (sscanf(v, "%ux%u", &a->lw, &a->lh) != 2) usage(); i++; }
        else if (!strcmp(x, "--scale") && v) a->scale = (float)atof(v), i++;
        else if (!strcmp(x, "--offscreen")) a->offscreen = 1;
        else if (!strcmp(x, "--en")) lang = 1;
        else if (!strcmp(x, "--fr")) lang = 0;
        else if (!strcmp(x, "--dark")) dark = 1;
        else if (!strcmp(x, "--edges") && v) a->ro.edges = !strcmp(v, "lines") ? NV_EDGES_LINES : !strcmp(v, "none") ? NV_EDGES_NONE : NV_EDGES_SCREEN, i++;
        else if (!strcmp(x, "--no-hlod")) a->hlod = 0, a->ro.hlod = 0;
        else if (!strcmp(x, "--no-budget")) a->adaptive = 0;
        else if (!strcmp(x, "--loaders") && v) loaders = atoi(v), i++;
        else if (!strcmp(x, "--link-folder") && v) link_folder = v, i++;
        else if (!strcmp(x, "--config-dir") && v) conf = v, i++;
        else if (!strcmp(x, "--shot") && v) shot = v, i++;
        else if (!strcmp(x, "--quit-after") && v) a->quit_after = atoi(v), i++;
        else if (!strcmp(x, "--script") && v) a->script = v, i++;
        else if (!strcmp(x, "--preset") && v) a->preset = v, i++;
        else if (!strcmp(x, "--out") && v) a->out_dir = v, i++;
        else if (x[0] == '-') usage();
        else model = x;
    }
    if (a->scale < 0.5f || a->scale > 4 || a->lw < 320 || a->lh < 240 || a->lw > 8192 || a->lh > 8192) usage();
    if (a->offscreen) a->pw = (uint32_t)(a->lw * a->scale), a->ph = (uint32_t)(a->lh * a->scale), a->lw = a->pw, a->lh = a->ph;
    { /* French by default, as the lab; LANG=en_* gives English */
        const char *l = getenv("LANG");
        a->lang_en = lang >= 0 ? lang : (l && !strncmp(l, "en", 2));
    }
    int sv = SDL_GetVersion();
    if (SDL_VERSIONNUM_MAJOR(sv) < BIMBLE_SDL_MIN_MAJOR || (SDL_VERSIONNUM_MAJOR(sv) == BIMBLE_SDL_MIN_MAJOR && SDL_VERSIONNUM_MINOR(sv) < BIMBLE_SDL_MIN_MINOR)) {
        fprintf(stderr, "bimble: SDL %d.%d.%d found, %d.%d or newer needed\n", SDL_VERSIONNUM_MAJOR(sv), SDL_VERSIONNUM_MINOR(sv),
                SDL_VERSIONNUM_MICRO(sv), BIMBLE_SDL_MIN_MAJOR, BIMBLE_SDL_MIN_MINOR);
        no_gpu("SDL too old", model);
        return 3;
    }

    /* the core: one session, as bimble-ccore's (host/main.c) */
    a->ses = cc_session_new(cc_platform_native());
    if (!a->ses) return 1;
    {
        const char *cm = getenv("BIMBLE_LINK_CACHE_MIN"), *cb = getenv("BIMBLE_LINK_CACHE_BESIDE"), *mb = getenv("BIMBLE_LINK_MESH_BUDGET_MB");
        if (cm && *cm) a->ses->link_cache_min = strtoull(cm, NULL, 10);
        a->ses->link_no_beside = cb && !strcmp(cb, "0");
        if (mb && *mb) a->ses->link_mesh_budget = strtoull(mb, NULL, 10) << 20;
        const char *nl = getenv("BIMBLE_NATIVE_LOADERS");
        if (nl && *nl) loaders = atoi(nl);
    }
    cc_host_fs_init();
    if (config_dir(a->config_dir, sizeof a->config_dir, conf)) {
        char f[1200];
        snprintf(f, sizeof f, "%s/settings.json", a->config_dir);
        cc_recent_setup(a->ses, f);
    }
    if (model) { /* the model's folder is one of the dialog's roots (host/main.c files_setup) */
        char abs[4096];
        if (realpath(model, abs)) {
            char *sl = strrchr(abs, '/');
            if (sl && sl != abs) *sl = 0, cc_host_fs_add_root(abs);
        }
    }
    if (data_load(a) != 0) { fprintf(stderr, "bimble: the shell's data is not in this build\n"); return 1; }

    /* the window and the GPU */
    if (a->offscreen) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO)) { no_gpu("no video", model); return 3; }
    sdl_base = app_now() - SDL_GetTicksNS() / 1e6;
    a->dev = nv_gpu_device(&a->gpu_name, &a->gpu_driver);
    if (!a->dev) { no_gpu("no GPU device", model); return 3; }
    a->win = SDL_CreateWindow("Bimble", (int)a->lw, (int)a->lh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!a->win || !SDL_ClaimWindowForGPUDevice(a->dev, a->win)) { no_gpu("no window", model); return 3; }
    if (!a->offscreen) {
        int pw, ph, lw, lh;
        SDL_GetWindowSizeInPixels(a->win, &pw, &ph);
        SDL_GetWindowSize(a->win, &lw, &lh);
        a->pw = (uint32_t)pw, a->ph = (uint32_t)ph, a->lw = (uint32_t)lw, a->lh = (uint32_t)lh;
        a->scale = SDL_GetWindowDisplayScale(a->win) > 0 ? 1 : 1; /* ImGui works in the window's logical px; the density scales the frame */
        SDL_SetGPUSwapchainParameters(a->dev, a->win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
    }
    SDL_GPUTextureCreateInfo ci = {.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                   .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = a->pw, .height = a->ph,
                                   .layer_count_or_depth = 1, .num_levels = 1};
    a->screen = SDL_CreateGPUTexture(a->dev, &ci);
    char err[256];
    a->r = nv_render_new(a->dev, &a->ro, err, sizeof err);
    if (!a->screen || !a->r) { no_gpu(a->r ? "no picture" : err, model); return 3; }
    a->loader = nv_loader_new(a->dev, loaders);
    igCreateContext(NULL);
    igGetIO_Nil()->IniFilename = NULL;
    theme_set(a, dark);
    if (theme_apply(a) != 0) fprintf(stderr, "bimble: the lab's fonts did not load: the default font\n");
    if (nui_backend_init(a->win, a->dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM) != 0) { no_gpu("the UI's renderer", model); return 3; }
    if (icons_build(a) != 0) fprintf(stderr, "bimble: no icons\n");
    a->cut_live = a->moving_cut;
    a->cam.yaw = -55 * NV_PI / 180, a->cam.pitch = 28 * NV_PI / 180, a->cam.dist = 20000;
    if (model) app_open(a, model);
    else {
        a->screen_kind = SCR_HOME;
        core_query(a, "recent_files", NULL, &a->recent);
    }
    if ((a->script || a->preset) && drive_setup(a) != 0) return 1;
    if (link_folder) { /* a folder linked through the core, as the Links panel does (once open) */
        while (a->screen_kind == SCR_OPENING) frame(a);
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"path\":"), json_str(&b, link_folder), buf_puts(&b, "}");
        core_action(a, "link_folder", buf_cstr(&b));
        buf_free(&b);
    }

    uint64_t n = 0;
    double complete_frame = 0;
    while (!a->quit) {
        SDL_Event ev;
        int had = 0;
        drive_step(a, app_now());
        while (SDL_PollEvent(&ev)) {
            had = 1;
            if (ev.type == SDL_EVENT_QUIT) a->quit = 1;
            nui_backend_event(&ev);
            ImGuiIO *io = igGetIO_Nil();
            view_input(a, &ev, io->WantCaptureMouse && !a->drag && ev.type != SDL_EVENT_MOUSE_BUTTON_UP);
            if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && !a->offscreen) {
                a->pw = (uint32_t)ev.window.data1, a->ph = (uint32_t)ev.window.data2;
                int lw, lh;
                SDL_GetWindowSize(a->win, &lw, &lh);
                a->lw = (uint32_t)lw, a->lh = (uint32_t)lh;
                SDL_WaitForGPUIdle(a->dev);
                SDL_ReleaseGPUTexture(a->dev, a->screen);
                ci.width = a->pw, ci.height = a->ph;
                a->screen = SDL_CreateGPUTexture(a->dev, &ci);
                a->dirty = 1;
            }
        }
        int busy = a->dirty || a->moving || a->screen_kind == SCR_OPENING || nv_loader_busy(a->loader) || a->links_busy || a->steps;
        if (!a->offscreen && !had && !busy && n > 2) SDL_WaitEventTimeout(NULL, 100);
        else if (a->offscreen && !had && !busy && n > 2) SDL_Delay(2);
        frame(a);
        n++;
        if (a->quit_after && a->t_complete) {
            if (!complete_frame) complete_frame = (double)n;
            if ((double)n - complete_frame >= a->quit_after) a->quit = 1;
        }
        if (a->quit_after && a->screen_kind == SCR_HOME && n > (uint64_t)a->quit_after + 2 && !a->steps) a->quit = 1;
        if (a->offscreen && a->quit_after && app_now() - a->t_start > 120000) {
            fprintf(stderr, "bimble: not complete after 120 s\n");
            a->fails++, a->quit = 1;
        }
    }
    if (shot) frame(a), frame(a), app_shot(a, shot);
    drive_finish(a);
    uint64_t peak = 0, rss = app_rss(&peak);
    fprintf(stderr, "bimble: first picture %.0f ms, complete %.0f ms, %llu frames, memory %.0f MB (peak %.0f MB)\n",
            a->t_first_picture ? a->t_first_picture - a->t_start : -1.0, a->t_complete ? a->t_complete - a->t_start : -1.0,
            (unsigned long long)n, rss / 1e6, peak / 1e6);
    nv_loader_free(a->loader);
    nui_backend_shutdown();
    igDestroyContext(NULL);
    nv_render_free(a->r);
    if (a->icon_tex) SDL_ReleaseGPUTexture(a->dev, a->icon_tex);
    SDL_ReleaseGPUTexture(a->dev, a->screen);
    SDL_ReleaseWindowFromGPUDevice(a->dev, a->win);
    SDL_DestroyWindow(a->win);
    SDL_DestroyGPUDevice(a->dev);
    SDL_Quit();
    cc_session_free(a->ses);
    free(a->id_slot);
    return a->fails ? 1 : 0;
}
