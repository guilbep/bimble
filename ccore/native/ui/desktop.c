/* The native app on a Linux desktop (ticket NUI3, ADR 0084): the system's file dialogs (the desktop portal, through SDL;
 * the in-app dialog when there is none or it fails), files dropped on the window, the window's title and app id (a
 * compositor's rules: Hyprland's `class`), a touchpad's two-finger pan and pinch zoom, Ctrl + wheel zoom, the section
 * box by numbers, and a command palette (Ctrl+K). Every attempt that can fail on a desktop is in the session log. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "ui.h"
#include "calls.h"

/* ---- the system's file dialogs ---- */
static pthread_mutex_t dlg_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int state; /* 0 none, 1 shown, 2 answered */
    char purpose[16];
    char path[1400];
    int failed, cancelled;
    char err[256];
} dlg;
static int dlg_broken; /* the system dialog failed once this session: the in-app one from then on */

static void dlg_done(void *userdata, const char *const *files, int filter) {
    (void)userdata, (void)filter;
    pthread_mutex_lock(&dlg_mu);
    dlg.state = 2, dlg.failed = !files, dlg.cancelled = files && !files[0];
    if (files && files[0]) snprintf(dlg.path, sizeof dlg.path, "%s", files[0]);
    if (!files) snprintf(dlg.err, sizeof dlg.err, "%s", SDL_GetError());
    pthread_mutex_unlock(&dlg_mu);
}

static void in_app(App *a, const char *purpose) {
    fs_open(a, NULL, purpose);
    a->dialog = !strcmp(purpose, "open") ? DLG_OPEN : !strcmp(purpose, "save_as") ? DLG_SAVEAS : DLG_LINK;
}

/* File > Open, Link a model, Link a folder, Save as: the system's dialog when there is one (native.json system_dialog,
 * on), else the app's own. Offscreen (the driver, the tests): the app's own. */
void desk_dialog(App *a, const char *purpose) {
    if (a->offscreen || dlg_broken || nset_num("system_dialog", 1) == 0) { in_app(a, purpose); return; }
    pthread_mutex_lock(&dlg_mu);
    int busy = dlg.state == 1;
    if (!busy) memset(&dlg, 0, sizeof dlg), dlg.state = 1, snprintf(dlg.purpose, sizeof dlg.purpose, "%s", purpose);
    pthread_mutex_unlock(&dlg_mu);
    if (busy) return;
    static const SDL_DialogFileFilter OPEN[] = {{"Bimble, IFC", "bimble;ifc;bimbleview;json"}, {"Bimble", "bimble"}, {"IFC", "ifc"}, {"*", "*"}};
    static const SDL_DialogFileFilter IFC[] = {{"IFC", "ifc"}, {"*", "*"}};
    static const SDL_DialogFileFilter SAVE[] = {{"Bimble", "bimble"}};
    slog("file dialog (%s): the system's (SDL %s)", purpose, SDL_GetHint(SDL_HINT_FILE_DIALOG_DRIVER) ? SDL_GetHint(SDL_HINT_FILE_DIALOG_DRIVER) : "portal, else zenity");
    if (!strcmp(purpose, "link_folder")) SDL_ShowOpenFolderDialog(dlg_done, NULL, a->win, NULL, false);
    else if (!strcmp(purpose, "link_model")) SDL_ShowOpenFileDialog(dlg_done, NULL, a->win, IFC, 2, NULL, false);
    else if (!strcmp(purpose, "save_as")) SDL_ShowSaveFileDialog(dlg_done, NULL, a->win, SAVE, 1, NULL);
    else SDL_ShowOpenFileDialog(dlg_done, NULL, a->win, OPEN, 4, NULL, false);
}

static void link_path(App *a, const char *path, int folder) {
    Buf b;
    buf_init(&b);
    if (folder) buf_puts(&b, "{\"path\":"), json_str(&b, path), buf_puts(&b, "}");
    else buf_puts(&b, "{\"paths\":["), json_str(&b, path), buf_puts(&b, "]}");
    if (core_action(a, folder ? "link_folder" : "link_model", buf_cstr(&b)) == CC_OK) a->links_synced = -1;
    buf_free(&b);
}

/* ---- the window's title (the file's name), each frame when it changes ---- */
static void title_tick(App *a) {
    static char shown[1100] = "\x01";
    char t[1100];
    const char *p = a->screen_kind == SCR_HOME ? "" : a->model_path;
    const char *base = strrchr(p, '/');
    base = base ? base + 1 : p;
    if (!*base || !strcmp(base, ":new")) snprintf(t, sizeof t, "Bimble");
    else snprintf(t, sizeof t, "%s%s — Bimble", base, a->state.root && jb(a->state.root, "unsaved") ? " •" : "");
    if (strcmp(t, shown)) {
        snprintf(shown, sizeof shown, "%s", t);
        if (a->win) SDL_SetWindowTitle(a->win, t);
    }
}

void desk_tick(App *a) {
    title_tick(a);
    pthread_mutex_lock(&dlg_mu);
    int st = dlg.state;
    char purpose[16], path[1400], err[256];
    int failed = dlg.failed, cancelled = dlg.cancelled;
    snprintf(purpose, sizeof purpose, "%s", dlg.purpose), snprintf(path, sizeof path, "%s", dlg.path), snprintf(err, sizeof err, "%s", dlg.err);
    if (st == 2) dlg.state = 0;
    pthread_mutex_unlock(&dlg_mu);
    if (st != 2) return;
    if (failed) { /* no portal, no zenity, or it failed: the app's own dialog, for the rest of the session */
        slog("file dialog (%s): the system's failed (%s): the app's own", purpose, err[0] ? err : "?");
        dlg_broken = 1;
        in_app(a, purpose);
        return;
    }
    if (cancelled) { slog("file dialog (%s): cancelled", purpose); return; }
    slog("file dialog (%s): %s", purpose, path);
    if (!strcmp(purpose, "open")) app_open(a, path);
    else if (!strcmp(purpose, "link_model")) link_path(a, path, 0);
    else if (!strcmp(purpose, "link_folder")) link_path(a, path, 1);
    else if (!strcmp(purpose, "save_as")) {
        char p[1420];
        snprintf(p, sizeof p, "%s%s", path, strstr(path, ".bimble") ? "" : ".bimble");
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"path\":"), json_str(&b, p), buf_puts(&b, "}");
        core_action(a, "save_as", buf_cstr(&b));
        buf_free(&b);
    }
}

/* ---- a file or folder dropped on the window: a folder is linked (a project open), a file opened; with Shift an IFC is
 * linked into the open project ---- */
void desk_drop(App *a, const char *path) {
    struct stat st;
    if (!path || stat(path, &st) != 0) { slog("drop: %s: not found", path ? path : "(none)"); return; }
    const char *dot = strrchr(path, '.');
    int ifc = dot && !strcasecmp(dot, ".ifc"), proj = a->screen_kind == SCR_MAIN && a->ses->open;
    int shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    if (S_ISDIR(st.st_mode)) {
        if (!proj) { snprintf(a->notice, sizeof a->notice, "%s", T(a, "native.dropFolder")), a->notice_t = app_now(); slog("drop: %s: a folder, no project open", path); return; }
        slog("drop: %s: a folder, linked", path);
        link_path(a, path, 1);
        return;
    }
    if (ifc && proj && shift) { slog("drop: %s: linked (Shift)", path); link_path(a, path, 0); return; }
    slog("drop: %s: opened", path);
    app_open(a, path);
}

/* ---- the wheel and the touchpad: Ctrl + wheel and a mouse's wheel zoom; a touchpad's two-finger scroll (fractional
 * steps) pans (native.json touchpad_pan, on); a pinch zooms ---- */
int desk_wheel(App *a, const SDL_MouseWheelEvent *w, float mx, float my) {
    int ctrl = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
    float x = w->x, y = w->y;
    if (w->direction == SDL_MOUSEWHEEL_FLIPPED) x = -x, y = -y;
    int smooth = fabsf(y - roundf(y)) > 1e-3f || fabsf(x - roundf(x)) > 1e-3f || (x != 0 && y == 0);
    static int said = -1; /* the session log says how the wheel was read (a free-spinning mouse wheel sends fractions too:
                           * touchpad_pan off then) */
    if (said != smooth) said = smooth, slog("wheel: %s (dx %.3f dy %.3f%s)", smooth ? "fractional steps: a touchpad's scroll, pans (touchpad_pan)" : "whole steps: a mouse's wheel, zooms", x, y, ctrl ? ", Ctrl" : "");
    if (!ctrl && smooth && nset_num("touchpad_pan", 1) != 0) {
        nv_cam_pan(&a->cam, -x * 24 * a->scale, y * 24 * a->scale, a->vh);
        return 1;
    }
    nv_cam_zoom_at(&a->cam, ctrl && smooth ? y : (y > 0 ? 1 : y < 0 ? -1 : 0), mx - a->vx, my - a->vy, a->vw, a->vh);
    return 1;
}
void desk_pinch(App *a, float scale) {
    if (!(scale > 0.01f && scale < 100)) return;
    float mx, my;
    SDL_GetMouseState(&mx, &my);
    nv_cam_zoom_at(&a->cam, log(scale) / -log(0.85), mx - a->vx, my - a->vy, a->vw, a->vh); /* zoom_at: 0.85^steps */
}

/* ---- the section box by numbers (the lab's small sheet shows its top and bottom; here all six faces, metres in the
 * project's frame, written with Enter): box_set ---- */
int desk_box_set(App *a, const double v[6]);
void panel_section_box(App *a) {
    if (!a->box_on || a->show != SHOW_VIEW || a->view_name[0] || a->screen_kind != SCR_MAIN || !nset_num("box_panel", 1)) return;
    float s = a->scale;
    igSetNextWindowPos(V2(a->vx + 12 * s, a->vy + a->vh - 12 * s), ImGuiCond_Always, V2(0, 1));
    igSetNextWindowBgAlpha(0.92f);
    igBegin("##boxpanel", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    igTextUnformatted(T(a, "small.sectionBox"), NULL);
    static const char *AX[3] = {"x", "y", "z"};
    double v[6];
    for (int i = 0; i < 3; i++) /* the app's frame to the project's (origin0), metres */
        v[i] = (a->box_min[i] + a->origin0[i]) / 1000.0, v[3 + i] = (a->box_max[i] + a->origin0[i]) / 1000.0;
    int changed = 0;
    for (int i = 0; i < 3; i++) {
        char lab[24];
        igSetNextItemWidth(130 * s);
        snprintf(lab, sizeof lab, "%s min##b%d", AX[i], i);
        if (igInputDouble(lab, &v[i], 0, 0, "%.3f m", ImGuiInputTextFlags_EnterReturnsTrue)) changed = 1;
        igSameLine(0, 8 * s);
        igSetNextItemWidth(130 * s);
        snprintf(lab, sizeof lab, "%s max##b%d", AX[i], i);
        if (igInputDouble(lab, &v[3 + i], 0, 0, "%.3f m", ImGuiInputTextFlags_EnterReturnsTrue)) changed = 1;
    }
    igTextDisabled("%s %.2f m  ·  %s %.2f m", T(a, "small.top"), v[5], T(a, "small.bottom"), v[2]);
    igEnd();
    if (changed) desk_box_set(a, v);
}

/* the box's faces (metres in the project's frame: min x y z, max x y z) to the core (box_set, mm in the box's frame) */
int desk_box_set(App *a, const double v[6]) {
    char body[400];
    double lo[3], hi[3];
    for (int i = 0; i < 3; i++) {
        double x0 = v[i] * 1000.0 - a->box_org[i], x1 = v[3 + i] * 1000.0 - a->box_org[i];
        lo[i] = x0 < x1 ? x0 : x1, hi[i] = x0 < x1 ? x1 : x0;
    }
    snprintf(body, sizeof body, "{\"min\":[%.3f,%.3f,%.3f],\"max\":[%.3f,%.3f,%.3f]}", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    Buf o;
    buf_init(&o);
    int st = core_call(a, "tool", "box_set", body, &o);
    slog("section box by numbers: %s -> %d %.*s", body, st, (int)(o.len < 160 ? o.len : 160), o.p ? o.p : "");
    if (st != CC_OK) snprintf(a->notice, sizeof a->notice, "%.*s", (int)(o.len < 300 ? o.len : 300), o.p ? o.p : ""), a->notice_t = app_now();
    buf_free(&o);
    a->vis_dirty = 1, a->dirty = 1;
    return st == CC_OK ? 0 : -1;
}

/* ---- the command palette (Ctrl+K): every ribbon tool and the File menu's actions by name, typed to filter, Enter runs
 * the first ---- */
static int palette_open;
void palette_toggle(App *a) { palette_open = !palette_open, a->dirty = 1; }
int palette_shown(void) { return palette_open; }
static int contains_ci(const char *h, const char *n) {
    if (!*n) return 1;
    for (; *h; h++) {
        size_t k = 0;
        while (n[k] && h[k] && (h[k] | 32) == (n[k] | 32)) k++;
        if (!n[k]) return 1;
    }
    return 0;
}
void palette_ui(App *a) {
    if (!palette_open || a->screen_kind != SCR_MAIN) return;
    float s = a->scale;
    static char q[96];
    igSetNextWindowPos(V2(a->lw / 2.0f, 90 * s), ImGuiCond_Always, V2(0.5f, 0));
    igSetNextWindowSize(V2(520 * s, 0), ImGuiCond_Always);
    igBegin("##palette", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    if (igIsWindowAppearing()) igSetKeyboardFocusHere(0);
    igSetNextItemWidth(-1);
    int enter = igInputTextWithHint("##q", T(a, "native.palette"), q, sizeof q, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
    const char *run = NULL, *first = NULL, *seen[16];
    int shown = 0;
    static const char *const FILE_ACTIONS[][2] = {{"open", "filemenu.open"}, {"new", "filemenu.new"}, {"save", "filemenu.save"}, {"link_model", "files.go.link"},
                                                  {"link_folder", "files.go.linkFolder"}, {"settings", "settings.title"}, {"fit", "native.fit"}};
    for (size_t k = 0; k < sizeof FILE_ACTIONS / sizeof *FILE_ACTIONS && shown < 14; k++) {
        const char *label = T(a, FILE_ACTIONS[k][1]);
        if (!contains_ci(label, q) && !contains_ci(FILE_ACTIONS[k][0], q)) continue;
        if (!first) first = FILE_ACTIONS[k][0];
        if (igSelectable_Bool(label, shown == 0, 0, V2(0, 0))) run = FILE_ACTIONS[k][0];
        seen[shown++] = FILE_ACTIONS[k][0];
    }
    for (int t = 0; t < a->ntabs && shown < 14; t++)
        for (int g = 0; g < a->tabs[t].ngroups && shown < 14; g++)
            for (int i = 0; i < a->tabs[t].groups[g].ntools && shown < 14; i++) {
                const RTool *tl = &a->tabs[t].groups[g].tools[i];
                if (!tl->action || tl->page) continue;
                const JrVal *row = action_row(a, tl->action);
                if (!row || !jb(row, "enabled")) continue;
                const char *label = T(a, tl->key);
                if (!contains_ci(label, q) && !contains_ci(tl->action, q)) continue;
                int dup = 0;
                for (int k = 0; k < shown && !dup; k++) dup = !strcmp(seen[k], tl->action);
                if (dup) continue;
                if (!first) first = tl->action;
                char lab[200];
                snprintf(lab, sizeof lab, "%s##%s%d%d", label, tl->action, t, g);
                if (igSelectable_Bool(lab, shown == 0, 0, V2(0, 0))) run = tl->action;
                seen[shown++] = tl->action;
            }
    if (enter && first) run = first;
    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) palette_open = 0;
    igEnd();
    if (run) {
        slog("palette: %s", run);
        palette_open = 0, q[0] = 0;
        tool_run_action(a, run);
    }
}
