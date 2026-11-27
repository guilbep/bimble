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
#include <stdatomic.h>
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
        if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat) input_start(a, ev, SDL_GetKeyName(ev->key.key)), core_key(a, ev->key.key, ev->key.mod, igGetIO_Nil()->WantTextInput), a->dirty = 1;
        return;
    }
    switch (ev->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        float x = ev->button.x, y = ev->button.y;
        if (getenv("BIMBLE_DEBUG_TOOLS")) fprintf(stderr, "press at %.0f %.0f: ui %d, in view %d, box %d, measure %d\n", x, y, ui_wants_mouse, in_view(a, x, y), a->box_on, a->measure_on);
        if (ui_wants_mouse || !in_view(a, x, y)) break;
        a->ctx_menu = 0;
        if (view_cube(a, NULL, x, y, 1)) { input_start(a, ev, "view cube"); break; }
        if (a->box_on && ev->button.button == SDL_BUTTON_LEFT && !a->measure_on && tool_box_input(a, "box_press", x, y)) {
            a->box_drag = 1; /* a handle of the section box is held: no orbit */
            slog("section box: a handle held at (%.0f, %.0f) px in the view", x - a->vx, y - a->vy);
            input_start(a, ev, "box handle");
            break;
        }
        a->drag = ev->button.button == SDL_BUTTON_LEFT ? 1 : ev->button.button == SDL_BUTTON_RIGHT ? 3 : 2;
        a->lx = a->dx0 = x, a->ly = a->dy0 = y, a->drag_moved = 0;
        input_start(a, ev, "press");
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (a->box_drag) {
            tool_box_input(a, "box_release", ev->button.x, ev->button.y);
            a->box_drag = 0, a->vis_dirty = 1;
            slog("section box: released, min (%.0f, %.0f, %.0f) max (%.0f, %.0f, %.0f) mm", a->box_min[0], a->box_min[1], a->box_min[2], a->box_max[0], a->box_max[1], a->box_max[2]);
            break;
        }
        if (a->drag == 1 && !a->drag_moved) {
            input_start(a, ev, a->measure_on ? "measure click" : "pick");
            if (a->measure_on) tool_measure_input(a, ev->button.x, ev->button.y, 1);
            else app_pick_at(a, ev->button.x, ev->button.y), a->pick_add = (SDL_GetModState() & SDL_KMOD_CTRL) != 0 || a->drive_ctrl, a->drive_ctrl = 0;
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
            char kw[48];
            snprintf(kw, sizeof kw, "key %s%s", (ev->key.mod & SDL_KMOD_CTRL) ? "Ctrl " : "", SDL_GetKeyName(ev->key.key)); /* which key (the session log's slow inputs) */
            input_start(a, ev, kw);
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
    atomic_int done;
    int rc;
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
    atomic_store_explicit(&o->done, 1, memory_order_release);
    return NULL;
}

static void opening_tick(App *a) {
    if (!opener_on) {
        scene_reset(a);
        slog("opening %s", a->model_path);
        memset(&opener, 0, sizeof opener);
        opener.a = a;
        snprintf(opener.path, sizeof opener.path, "%s", a->model_path);
        if (pthread_create(&opener_th, NULL, open_run, &opener) == 0) opener_on = 1;
        else open_run(&opener), opener_on = 1;
        return;
    }
    if (!atomic_load_explicit(&opener.done, memory_order_acquire)) return;
    pthread_join(opener_th, NULL);
    opener_on = 0;
    if (opener.rc != CC_OK) {
        snprintf(a->open_err, sizeof a->open_err, "%s: %s", a->model_path, opener.err);
        fprintf(stderr, "bimble: %s\n", a->open_err);
        slog("open failed after %.0f ms: %s", app_now() - a->t_open, a->open_err);
        a->screen_kind = SCR_HOME;
        core_query(a, "recent_files", NULL, &a->recent);
        a->fails++;
        return;
    }
    a->screen_kind = SCR_MAIN;
    a->show = SHOW_VIEW;
    a->core_rev++;
    slog("opened %s in %.0f ms", a->model_path, app_now() - a->t_open);
    if (core_query(a, "settings", NULL, &a->settings) == 0) optimisations_apply(a); /* the same keys as the lab's */
    {
        Buf sb;
        buf_init(&sb);
        if (core_call(a, "query", "settings", "", &sb) == CC_OK) slog("settings in force: %.*s", (int)(sb.len < 1500 ? sb.len : 1500), sb.p ? sb.p : "");
        buf_free(&sb);
    }
    a->view_name[0] = 0, a->cam.ortho = 0, a->view_clip = 0;
    a->cam.yaw = -55 * NV_PI / 180, a->cam.pitch = 28 * NV_PI / 180;
    json_free(&a->measure), json_free(&a->sel_el);
    a->dirty = 1;
}

/* ---- the window's picture at the size acquired this frame (w x h pixels): the composed picture made again when it
 * changes; the logical size the window's (offscreen: the pixels). 0 ok. ---- */
static int app_target(App *a, uint32_t w, uint32_t h) {
    int lw = (int)w, lh = (int)h;
    if (!a->offscreen) {
        SDL_GetWindowSize(a->win, &lw, &lh);
        if (lw < 1 || lh < 1) lw = (int)w, lh = (int)h;
    }
    if (a->screen && w == a->pw && h == a->ph && (uint32_t)lw == a->lw && (uint32_t)lh == a->lh) return 0;
    int pw = 0, ph = 0;
    if (!a->offscreen) SDL_GetWindowSizeInPixels(a->win, &pw, &ph);
    slog("sizes: swapchain %ux%u px, window %dx%d (logical), SDL's pixels %dx%d, display scale %.2f, pixel density %.2f", w, h, lw, lh, pw, ph,
         a->offscreen ? 1.0 : (double)SDL_GetWindowDisplayScale(a->win), a->offscreen ? 1.0 : (double)SDL_GetWindowPixelDensity(a->win));
    if (!a->screen || w != a->pw || h != a->ph) {
        if (a->screen) SDL_WaitForGPUIdle(a->dev), SDL_ReleaseGPUTexture(a->dev, a->screen), a->screen = NULL;
        SDL_GPUTextureCreateInfo ci = {.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                       .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = w, .height = h,
                                       .layer_count_or_depth = 1, .num_levels = 1};
        a->screen = SDL_CreateGPUTexture(a->dev, &ci);
        if (!a->screen) { fprintf(stderr, "bimble: no picture of %ux%u: %s\n", w, h, SDL_GetError()); slog("no picture of %ux%u: %s", w, h, SDL_GetError()); return -1; }
    }
    a->pw = w, a->ph = h, a->lw = (uint32_t)lw, a->lh = (uint32_t)lh;
    a->dirty = 1;
    return 0;
}

/* offscreen --resize-script: the next size, the window now and the target a frame later (a compositor's order) */
static void app_resize_step(App *a) {
    const char *p = a->resize_script;
    for (int i = 0; i < a->resize_i && p; i++) p = strchr(p, ','), p = p ? p + 1 : NULL;
    unsigned w, h;
    if (!p || sscanf(p, "%ux%u", &w, &h) != 2 || w < 1 || h < 1 || w > 8192 || h > 8192) return;
    a->resize_i++;
    SDL_SetWindowSize(a->win, (int)w, (int)h);
    a->resize_w = w, a->resize_h = h, a->resize_wait = 2;
}

/* ---- NUI2's truth test: the frame's id target drawn without occlusion (0) and with it (1), read back; an element in the
 * first and not in the second is missing (occlusion dropped something visible) ---- */
static SDL_GPUTransferBuffer *truth_tb[2], *truth_db[2]; /* ids, depths */
static uint32_t truth_cap;
static void truth_grab(App *a, SDL_GPUCommandBuffer *cmd, int which) {
    uint32_t w = a->vpw, h = a->vph, need = w * h * 4;
    if (!need) return;
    if (truth_cap < need) {
        for (int k = 0; k < 2; k++) {
            if (truth_tb[k]) SDL_ReleaseGPUTransferBuffer(a->dev, truth_tb[k]);
            if (truth_db[k]) SDL_ReleaseGPUTransferBuffer(a->dev, truth_db[k]);
            SDL_GPUTransferBufferCreateInfo tci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = need};
            truth_tb[k] = SDL_CreateGPUTransferBuffer(a->dev, &tci);
            truth_db[k] = SDL_CreateGPUTransferBuffer(a->dev, &tci);
        }
        truth_cap = need;
    }
    if (!truth_tb[which]) return;
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src = {.texture = nv_render_ids(a->r), .w = w, .h = h, .d = 1};
    SDL_GPUTextureTransferInfo dst = {.transfer_buffer = truth_tb[which]};
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    if (truth_db[which]) {
        SDL_GPUTextureRegion ds = {.texture = nv_render_depth(a->r), .w = w, .h = h, .d = 1};
        SDL_GPUTextureTransferInfo dd = {.transfer_buffer = truth_db[which]};
        SDL_DownloadFromGPUTexture(cp, &ds, &dd);
    }
    SDL_EndGPUCopyPass(cp);
}
/* An element in the reference only is MISSING when one of its pixels shows something farther with occlusion (its surface
 * was not drawn there). Otherwise another element at the same depth won the pixel (coplanar faces: the order of the
 * draws, pass 1 or 2, decides): a tie, counted apart (truth_ties), not a loss. */
static int truth_compare(App *a) {
    uint32_t n = a->vpw * a->vph, ne = a->next_elem + 1;
    if (!truth_tb[0] || !truth_tb[1] || !n) return -1;
    const uint32_t *x = SDL_MapGPUTransferBuffer(a->dev, truth_tb[0], false), *y = SDL_MapGPUTransferBuffer(a->dev, truth_tb[1], false);
    const float *dx = truth_db[0] ? SDL_MapGPUTransferBuffer(a->dev, truth_db[0], false) : NULL, *dy = truth_db[1] ? SDL_MapGPUTransferBuffer(a->dev, truth_db[1], false) : NULL;
    uint8_t *seen = calloc(ne, 1);
    uint16_t *px = calloc(ne, 2); /* the reference's pixels an element (to 65535) */
    int missing = -1, ties = 0, small = 0;
    if (x && y && seen && px) {
        for (uint32_t i = 0; i < n; i++) if (x[i] > NV_ELEM0 && x[i] < ne) seen[x[i]] |= 1, px[x[i]] += px[x[i]] < 65535;
        for (uint32_t i = 0; i < n; i++) if (y[i] > NV_ELEM0 && y[i] < ne) seen[y[i]] |= 2;
        for (uint32_t i = 0; dx && dy && i < n; i++)
            if (x[i] > NV_ELEM0 && x[i] < ne && seen[x[i]] == 1 && dy[i] < dx[i] * (1 - 1e-5f)) seen[x[i]] |= 4;
        missing = 0;
        for (uint32_t e = 0; e < ne; e++) {
            if ((seen[e] & 3) != 1) continue;
            if (seen[e] & 4 || !(dx && dy)) missing++, small += px[e] <= 8;
            else ties++;
        }
        if (missing) {
            static int said;
            uint32_t first = 0;
            for (uint32_t e = 0; e < ne && !first; e++) if ((seen[e] & 7) == 5) first = e;
            if (said++ < 10) {
                char id[23] = "", link[256] = "";
                if (first) scene_elem(a, first - 1, id, link, sizeof link), nv_tiles_debug_elem(a->r, first - 1);
                fprintf(stderr, "bimble: truth: %d elements missing with occlusion at frame %llu (first: %s)\n", missing, (unsigned long long)a->frames, id);
                for (uint32_t i = 0, c = 0; dx && dy && i < n && c < 3; i++)
                    if (x[i] == first) fprintf(stderr, "truth px (%u, %u): reference %u depth %g, with occlusion %u depth %g\n", i % a->vpw, i / a->vpw, x[i], dx[i], y[i], dy[i]), c++;
            }
        }
    }
    if (x) SDL_UnmapGPUTransferBuffer(a->dev, truth_tb[0]);
    if (y) SDL_UnmapGPUTransferBuffer(a->dev, truth_tb[1]);
    if (dx) SDL_UnmapGPUTransferBuffer(a->dev, truth_db[0]);
    if (dy) SDL_UnmapGPUTransferBuffer(a->dev, truth_db[1]);
    free(seen), free(px);
    a->truth_ties = ties, a->truth_small = small;
    return missing;
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
    /* NUI2: the target of THIS frame first (the swapchain's texture: on Wayland with fractional scaling or a tiling
     * compositor its size differs from SDL_GetWindowSizeInPixels and changes a frame after the resize event); every
     * size below (the picture, ImGui's display and framebuffer scale, the 3D view's targets) comes from it */
    double t_logic = app_now();
    a->st_logic = t_logic - t0;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(a->dev);
    if (!cmd) { fprintf(stderr, "bimble: no command buffer: %s\n", SDL_GetError()); slog("no command buffer: %s", SDL_GetError()); a->quit = 1; return; }
    SDL_GPUTexture *sw = NULL;
    Uint32 sw_w = 0, sw_h = 0;
    if (!a->offscreen) {
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, a->win, &sw, &sw_w, &sw_h)) {
            static int said;
            if (said++ < 20) slog("no swapchain texture: %s", SDL_GetError());
        }
        if (!sw || sw_w < 1 || sw_h < 1) { /* minimised, occluded, or lost: nothing drawn this frame */
            SDL_SubmitGPUCommandBuffer(cmd);
            a->dirty = 1;
            SDL_Delay(16);
            return;
        }
    } else {
        if (a->resize_wait && !--a->resize_wait) a->swap_w = a->resize_w, a->swap_h = a->resize_h; /* the window first, the target a frame later */
        sw_w = a->swap_w ? a->swap_w : a->pw, sw_h = a->swap_h ? a->swap_h : a->ph;
    }
    if (app_target(a, sw_w, sw_h) != 0) { SDL_SubmitGPUCommandBuffer(cmd); a->quit = 1; return; }
    double t_acq = app_now();
    a->st_acquire = t_acq - t_logic;
    float s = a->scale, W = (float)a->lw, H = (float)a->lh;
    float top = 178 * s, bottom = 32 * s, left = 296 * s, right = 0; /* LeftPanel.vue: Properties under the navigator */
    a->vx = left, a->vy = top + 38 * s, a->vw = W - left - right, a->vh = H - top - bottom - 38 * s;
    if (a->vw < 8) a->vw = 8; /* a window smaller than the panels: a sliver of view, never a negative size */
    if (a->vh < 8) a->vh = 8;
    float px = (float)a->pw / (float)a->lw;
    uint32_t vpw = (uint32_t)(a->vw * px), vph = (uint32_t)(a->vh * px);
    if (vpw != a->vpw || vph != a->vph) a->vpw = vpw, a->vph = vph, nv_render_targets(a->r, vpw, vph), a->dirty = 1;
    nui_backend_new_frame();
    { /* ImGui's display from the same numbers (its backend reads the window, which may be a frame ahead) */
        ImGuiIO *io = igGetIO_Nil();
        io->DisplaySize = V2((float)a->lw, (float)a->lh);
        io->DisplayFramebufferScale = V2((float)a->pw / (float)a->lw, (float)a->ph / (float)a->lh);
    }
    igNewFrame();
    if (a->screen_kind != SCR_MAIN) home_screen(a);
    else {
        shell_top(a);
        float side = H - top - bottom, nav = a->selected ? (float)floor(side * 0.45) : side;
        panel_navigator(a, 0, top, left, nav);
        if (a->selected) panel_properties(a, 0, top + nav, left, side - nav);
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
            ImDrawList *fg = bg; /* the overlays under every window (a dialog or a menu covers them) */
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
    a->st_ui = t_ui - t_acq;
    a->ui_ms = t_ui - t0;
    a->t_ui_done = t_ui;
    int drew3d = 0, pick = a->pick_pending;
    if ((a->dirty || pick) && a->show == SHOW_VIEW && a->screen_kind == SCR_MAIN && a->r) {
        int mv = a->moving && a->adaptive && a->moving_fps > 0;
        float cut = mv ? (a->cut_live > a->still_cut ? a->cut_live : a->still_cut) : a->still_cut;
        nv_render_set_cut(a->r, cut);
        nv_render_set_skip(a->r, a->bench_skip);
        nv_tiles_opts to = a->topts;
        to.tri_budget = mv ? a->tri_live : 0;
        if (a->truth && a->use_tiles) { /* the truth test: the same frame without occlusion first, its ids kept */
            to.reference = 1;
            nv_render_tiles_opts(a->r, &to);
            nv_render_record(a->r, cmd, &a->cam, 0, 0, 0);
            truth_grab(a, cmd, 0);
            to.reference = 0;
        }
        nv_render_tiles_opts(a->r, &to);
        nv_render_record(a->r, cmd, &a->cam, pick, a->pick_x, a->pick_y);
        if (a->truth && a->use_tiles) truth_grab(a, cmd, 1);
        drew3d = 1, a->dirty = 0;
    }
    double t_rec = app_now();
    a->st_rec3d = t_rec - t_ui;
    ImVec4 bgc = col4(C_CREAM);
    float clear[4] = {bgc.x, bgc.y, bgc.z, 1};
    if (a->bench_skip & NV_SKIP_UI) { igRender(); /* the UI built, not drawn: the picture is the 3D view's, blitted */
        SDL_GPUBlitInfo vb = {.source = {.texture = nv_render_color(a->r), .w = a->vpw, .h = a->vph}, .destination = {.texture = a->screen, .w = a->pw, .h = a->ph}, .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_NEAREST};
        if (nv_render_color(a->r) && a->vpw && a->vph) SDL_BlitGPUTexture(cmd, &vb);
    } else nui_backend_render(cmd, a->screen, 1, clear);
    double t_recui = app_now();
    a->st_recui = t_recui - t_rec;
    if (sw) { /* the picture and the swapchain have one size (app_target) */
        SDL_GPUBlitInfo bi = {.source = {.texture = a->screen, .w = a->pw, .h = a->ph}, .destination = {.texture = sw, .w = sw_w, .h = sw_h},
                              .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_NEAREST};
        SDL_BlitGPUTexture(cmd, &bi);
    }
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (f) SDL_WaitForGPUFences(a->dev, true, &f, 1), SDL_ReleaseGPUFence(a->dev, f);
    double t1 = app_now();
    a->st_gpu = t1 - t_recui;
    a->gpu_ms = t1 - t_ui;
    a->frame_ms = drew3d ? t1 - t_ui : 0;
    if (drew3d) {
        a->last3d_ms = a->frame_ms;
        if (a->moving) a->ring_move[a->nmove++ % 2048] = (float)a->frame_ms;
        else a->ring_still[a->nstill++ % 2048] = (float)a->frame_ms;
    }
    /* the moving budget (the page's idea, ADR 0078): while moving, the screen-size cut grows until a
     * frame fits in ~16 ms, and shrinks back when there is room; still frames are always whole */
    if (drew3d && a->moving && a->adaptive && a->moving_fps > 0) { /* the target: settings moving_fps (30: 33 ms) */
        double target = 1000.0 / a->moving_fps;
        if (a->frame_ms > target * 0.75 && a->cut_live < 16) a->cut_live = a->cut_live < 1 ? 1 : a->cut_live * 1.5f; /* NUI2: 0.6 -> 0.75 and 0.25 -> 0.45: nearer the target, more detail while moving */
        else if (a->frame_ms < target * 0.45 && a->cut_live > a->moving_cut) a->cut_live = a->cut_live / 1.5f < a->moving_cut ? a->moving_cut : a->cut_live / 1.5f;
    }
    if (drew3d) {
        uint32_t picked = 0;
        nv_render_collect(a->r, pick ? &picked : NULL);
        a->truth_missing = a->truth && a->use_tiles ? truth_compare(a) : -1;
        if (a->moving && a->adaptive && a->moving_fps > 0 && a->use_tiles) { /* the moving budget's triangles (the walker's) */
            double target = 1000.0 / a->moving_fps;
            uint64_t drawn = nv_render_get_stats(a->r).tris;
            if (a->frame_ms > target * 0.6) a->tri_live = drawn * 7 / 10 > 500000 ? drawn * 7 / 10 : 500000;
            else if (a->frame_ms < target * 0.25 && a->tri_live) a->tri_live = a->tri_live * 3 / 2 > drawn * 4 ? 0 : a->tri_live * 3 / 2;
        } else if (!a->moving) a->tri_live = 0;
        if (pick) {
            a->pick_pending = 0;
            if (a->pick_add) { if (picked > NV_ELEM0) core_select_add(a, picked); } /* Ctrl-click (on nothing: the selection kept) */
            else core_select(a, picked > NV_ELEM0 ? picked : 0);
            a->pick_add = 0;
        }
        /* the first picture: something drawn, or nothing left to draw (an empty model) */
        if (!a->t_first_picture && (nv_render_nparts(a->r) > 0 || scene_complete(a))) a->t_first_picture = t1;
    }
    if (a->screen_kind == SCR_MAIN && !a->t_complete && a->t_first_picture && scene_complete(a)) a->t_complete = t1;
    if (a->input_t) app_input_done(a, t1);
    drive_frame(a, t1, drew3d);
    slog_frame(a, t1, drew3d, drew3d ? a->frame_ms : a->gpu_ms);
    { /* the notices (warnings, errors) and the first picture and completion, in the session log */
        static char logged[512];
        if (a->notice[0] && strcmp(logged, a->notice)) snprintf(logged, sizeof logged, "%s", a->notice), slog("notice: %s", a->notice);
        static double first_logged, complete_logged;
        if (a->t_first_picture && a->t_first_picture != first_logged) first_logged = a->t_first_picture, slog("first picture %.0f ms after the open", a->t_first_picture - a->t_open);
        if (a->t_complete && a->t_complete != complete_logged)
            complete_logged = a->t_complete, slog("complete %.0f ms after the open: %d links, GPU %.0f MB", a->t_complete - a->t_open, a->nlinks, nv_render_get_stats(a->r).gpu_bytes / 1048576.0);
    }
    if (a->offscreen && a->resize_script && a->frames % 3 == 2) app_resize_step(a);
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

/* --selftest-window (NUI2): the window path checked without showing anything: a HIDDEN window (on Wayland a hidden window
 * has no shell surface, so nothing is mapped) claimed by the GPU device, its scale, density, sizes and present modes
 * printed; no swapchain texture is acquired (that needs a shown window). Exit 0 when every step works. */
static int selftest_window(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { printf("selftest-window: no video: %s\n", SDL_GetError()); return 3; }
    printf("selftest-window: video driver %s\n", SDL_GetCurrentVideoDriver());
    SDL_GPUDevice *dev = nv_gpu_device(NULL, NULL);
    if (!dev) { printf("selftest-window: no GPU device: %s\n", SDL_GetError()); SDL_Quit(); return 3; }
    SDL_Window *w = SDL_CreateWindow("Bimble selftest", 640, 400, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    int ok = w != NULL;
    printf("selftest-window: hidden window %s\n", ok ? "made" : SDL_GetError());
    if (ok) {
        ok = SDL_ClaimWindowForGPUDevice(dev, w);
        printf("selftest-window: claimed by the GPU device: %s\n", ok ? "yes" : SDL_GetError());
        int lw, lh, pw, ph;
        SDL_GetWindowSize(w, &lw, &lh), SDL_GetWindowSizeInPixels(w, &pw, &ph);
        printf("selftest-window: size %dx%d logical, %dx%d pixels, display scale %.2f, pixel density %.2f\n", lw, lh, pw, ph, SDL_GetWindowDisplayScale(w),
               SDL_GetWindowPixelDensity(w));
        if (ok) {
            static const char *const PM[3] = {"vsync", "immediate", "mailbox"};
            for (int m = 0; m < 3; m++) printf("selftest-window: present mode %s: %s\n", PM[m], SDL_WindowSupportsGPUPresentMode(dev, w, (SDL_GPUPresentMode)m) ? "yes" : "no");
            printf("selftest-window: swapchain format %d\n", (int)SDL_GetGPUSwapchainTextureFormat(dev, w));
            SDL_ReleaseWindowFromGPUDevice(dev, w);
        }
        SDL_DestroyWindow(w);
    }
    SDL_DestroyGPUDevice(dev);
    SDL_Quit();
    printf("selftest-window: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

static void usage(void) {
    fprintf(stderr, "usage: bimble [--en|--fr] [--dark] [file]\n"
                    "       bimble --offscreen [--size WxH] [--scale k] [--script file | --preset orbit-zoom] [--out dir]\n"
                    "              [--shot file.png] [--quit-after n] [file]\n"
                    "              [--resize-script WxH,WxH,...]\n"
                    "       options: --edges lines|screen|none, --no-hlod, --loaders n, --config-dir dir, --link-folder dir,\n"
                    "                --set key=value (an optimisation for this run), --last-log (the newest session log's folder), --version,\n"
                    "                --truth (each frame with and without occlusion, the id buffers compared), --compare k=v,... (repeatable),\n"
                    "                --selftest-window (a hidden window's checks)\n");
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
    for (int i = 1; i < argc; i++) /* --selftest-window: the window path's checks on a window never shown */
        if (!strcmp(argv[i], "--selftest-window")) return selftest_window();
    for (int i = 1; i < argc; i++) /* --compare: this binary again per settings set, the summaries side by side (compare.c) */
        if (!strcmp(argv[i], "--compare")) return compare_main(argc, argv);
    static App app;
    App *a = &app;
    a->t_start = app_now();
    a->scale = 1, a->lw = 1492, a->lh = 790;
    a->ro = (nv_render_opts){.edges = NV_EDGES_SCREEN, .hlod = 1, .hlod_px = 1};
    a->hlod = 1;
    a->moving_cut = 2, a->still_cut = 0, a->adaptive = 1, a->moving_fps = 30, a->gpu_budget = (uint64_t)768 << 20;
    a->draw_key = -1, a->links_synced = -1, a->truth_missing = -1;
    const char *model = NULL, *shot = NULL, *link_folder = NULL, *conf = NULL;
    int loaders = 2, dark = 0, lang = -1;
    for (int i = 1; i < argc; i++) {
        const char *x = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(x, "--version")) { printf("bimble %s (native)\n", CC_RELEASE_VERSION); return 0; }
        else if (!strcmp(x, "--last-log")) return slog_last();
        else if (!strcmp(x, "--resize-script") && v) a->resize_script = (char *)v, i++;
        else if (!strcmp(x, "--set") && v) i++; /* after the settings are read (below) */
        else if (!strcmp(x, "--truth")) a->truth = 1;
        else if (!strcmp(x, "--compare") && v) i++; /* compare.c */
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
    if (a->scale < 0.5f || a->scale > 4) { fprintf(stderr, "bimble: --scale %.2f: from 0.5 to 4\n", a->scale); return 2; }
    if (a->lw < 64 || a->lh < 64 || a->lw > 8192 || a->lh > 8192) { /* a small window still works: the view keeps a sliver */
        fprintf(stderr, "bimble: --size %ux%u: each side from 64 to 8192 logical px\n", a->lw, a->lh);
        return 2;
    }
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
    nset_load(a); /* the native app's own settings (native.json), then the run's --set */
    a->bench_skip = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            char k[40] = "", v[64] = "";
            if (sscanf(argv[i + 1], "%39[^=]=%63s", k, v) == 2) {
                char j[70];
                int num = (v[0] >= '0' && v[0] <= '9') || v[0] == '-' || v[0] == '.' || !strcmp(v, "true") || !strcmp(v, "false");
                snprintf(j, sizeof j, num ? "%s" : "\"%s\"", v);
                nset_set(k, j, 1);
            }
        }
    a->bench_skip = (unsigned)nset_num("bench_skip", 0);
    { /* the local session log: on in a window unless session_log is false; offscreen only when BIMBLE_LOG_DIR is set */
        const char *ld = getenv("BIMBLE_LOG_DIR");
        a->session_log = (int)nset_num("session_log", a->offscreen ? (ld && *ld) : 1);
        slog_open(a);
    }
    slog_sdl_hooks(a);
    if (model) slog("asked to open %s", model);

    /* the window and the GPU */
    if (a->offscreen) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO)) { no_gpu("no video", model); return 3; }
    sdl_base = app_now() - SDL_GetTicksNS() / 1e6;
    a->dev = nv_gpu_device(&a->gpu_name, &a->gpu_driver);
    if (!a->dev) { no_gpu("no GPU device", model); slog("no GPU device: %s", SDL_GetError()); slog_close(a); return 3; }
    slog("GPU %s, driver %s; video driver %s; %s", a->gpu_name, a->gpu_driver, SDL_GetCurrentVideoDriver(), a->offscreen ? "offscreen" : "a window");
    a->win = SDL_CreateWindow("Bimble", (int)a->lw, (int)a->lh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!a->win || !SDL_ClaimWindowForGPUDevice(a->dev, a->win)) { no_gpu("no window", model); return 3; }
    if (!a->offscreen) {
        int pw, ph, lw, lh;
        SDL_GetWindowSizeInPixels(a->win, &pw, &ph);
        SDL_GetWindowSize(a->win, &lw, &lh);
        a->pw = (uint32_t)pw, a->ph = (uint32_t)ph, a->lw = (uint32_t)lw, a->lh = (uint32_t)lh;
        a->scale = 1; /* ImGui works in the window's logical px; the pixel density scales the frame (app_target) */
        if (!SDL_SetGPUSwapchainParameters(a->dev, a->win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC)) { /* VSYNC is always there; else what is */
            slog("vsync refused: %s", SDL_GetError());
            if (!SDL_SetGPUSwapchainParameters(a->dev, a->win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_MAILBOX))
                slog("mailbox refused too: %s (the default present mode)", SDL_GetError());
        }
        SDL_SetGPUAllowedFramesInFlight(a->dev, 2);
        slog("present: %s (vsync: the swapchain's acquire waits for the display; the acquire is its own stage in frames.csv)",
             SDL_WindowSupportsGPUPresentMode(a->dev, a->win, SDL_GPU_PRESENTMODE_VSYNC) ? "vsync" : "the driver's default");
    }
    a->screen = NULL; /* app_target makes it at the first frame's size */
    char err[256];
    a->r = nv_render_new(a->dev, &a->ro, err, sizeof err);
    if (!a->r) { no_gpu(err, model); return 3; }
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
            if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || ev.type == SDL_EVENT_WINDOW_RESIZED || ev.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                /* the sizes come from the next swapchain texture (app_target), not from the event */
                slog("window event %u: %d x %d", (unsigned)ev.type, (int)ev.window.data1, (int)ev.window.data2);
                a->dirty = 1;
            }
        }
        /* idle: wait for an event; loading (the open, a conversion, the loader): poll every few ms */
        int busy = a->dirty || a->moving || a->pick_pending || a->drag || a->box_drag;
        int loading = a->screen_kind == SCR_OPENING || nv_loader_busy(a->loader) || a->links_busy;
        if (!had && !busy && n > 2) {
            if (!a->offscreen) SDL_WaitEventTimeout(NULL, loading ? 8 : 100);
            else SDL_Delay(a->steps ? 1 : 4);
        }
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
    slog_close(a); /* its last report reads the renderer */
    nv_loader_free(a->loader);
    nui_backend_shutdown();
    igDestroyContext(NULL);
    nv_render_free(a->r);
    if (a->icon_tex) SDL_ReleaseGPUTexture(a->dev, a->icon_tex);
    if (a->screen) SDL_ReleaseGPUTexture(a->dev, a->screen);
    SDL_ReleaseWindowFromGPUDevice(a->dev, a->win);
    SDL_DestroyWindow(a->win);
    SDL_DestroyGPUDevice(a->dev);
    SDL_Quit();
    cc_session_free(a->ses);
    free(a->id_slot);
    return a->fails ? 1 : 0;
}
