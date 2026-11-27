/* The native app's Home screen and dialogs (ticket NUI1): Home (Home.vue: recent files from the same
 * settings file as the web lab, New, Open), the file dialog (FileBrowser.vue: the core's own folder
 * lists, `fs_list`, under the same roots rule; Open, Save as, Link IFC… with several files, Link this
 * folder), Settings (the settings the core stores: query settings, action set_settings), Performance
 * (Performance.vue's report, same shape; renderer "native (SDL GPU / Vulkan)"), the key table, and the
 * unsaved-changes question. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ui.h"
#include "calls.h"
#include "fs.h"
#include "settings.h"

static const float BIG = 1e9f;

static void modal_begin(App *a, const char *title, float w, float h, bool *open) {
    float s = a->scale;
    igSetNextWindowPos(V2(a->lw / 2.0f, a->lh / 2.0f), ImGuiCond_Appearing, V2(0.5f, 0.5f));
    igSetNextWindowSize(V2(w * s, h * s), ImGuiCond_Appearing);
    igPushStyleColor_U32(ImGuiCol_WindowBg, C_SHEET);
    igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 16 * s);
    igBegin(title, open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
}
static void modal_end(void) {
    igEnd();
    igPopStyleVar(1);
    igPopStyleColor(1);
}

/* ---- Home ---- */
void home_screen(App *a) {
    float s = a->scale, W = (float)a->lw, H = (float)a->lh;
    igSetNextWindowPos(V2(0, 0), 0, V2(0, 0));
    igSetNextWindowSize(V2(W, H), 0);
    igBegin("##home", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList *dl = igGetWindowDrawList();
    float x = W / 2 - 320 * s, y = 70 * s;
    ImDrawList_AddCircleFilled(dl, V2(x + 26 * s, y + 26 * s), 26 * s, C_CLAY, 0);
    ImDrawList_AddText_FontPtr(dl, a->f_disp, 34 * s, V2(x + 16 * s, y + 5 * s), C_CREAM, "b", NULL, 0, NULL);
    char eb[128];
    ImDrawList_AddText_FontPtr(dl, a->f_text, 12 * s, V2(x + 70 * s, y + 6 * s), C_STONE, Tf(a, eb, sizeof eb, "home.eyebrow", "{version}", CC_RELEASE_VERSION " native"), NULL, 0, NULL);
    ImDrawList_AddText_FontPtr(dl, a->f_disp, 28 * s, V2(x + 70 * s, y + 20 * s), C_INK, T(a, "home.noModel"), NULL, 0, NULL);
    ImDrawList_AddText_FontPtr(dl, a->f_text, 16 * s, V2(x, y + 70 * s), C_STONE, T(a, "home.lead"), NULL, 0, NULL);
    igSetCursorScreenPos(V2(x, y + 104 * s));
    if (text_button(a, "hopen", T(a, "home.open"), 1, 0)) tool_run_action(a, "open");
    igSameLine(0, 8 * s);
    if (text_button(a, "hnew", T(a, "home.new"), 0, 0)) app_new(a);
    igSameLine(0, 8 * s);
    if (text_button(a, "hset", T(a, "filemenu.settings"), 0, 0)) tool_run_action(a, "settings");
    igSameLine(0, 8 * s);
    if (text_button(a, "hlang", a->lang_en ? "FR" : "EN", 0, 0)) a->lang_en = !a->lang_en, a->strings = a->lang_en ? a->strings_en : a->strings_fr;
    if (a->screen_kind == SCR_OPENING) { /* the open runs on its own thread: its progress within 200 ms */
        static const char SPIN[] = "|/-\\";
        char t[1200];
        snprintf(t, sizeof t, "%c  %s %s  (%.1f s)", SPIN[(int)(app_now() / 120) % 4], T(a, "native.opening"), a->model_path, (app_now() - a->t_open) / 1000);
        ImDrawList_AddText_FontPtr(dl, a->f_semi, 15 * s, V2(x, y + 150 * s), C_INK, t, NULL, 640 * s, NULL);
    } else if (a->open_err[0]) ImDrawList_AddText_FontPtr(dl, a->f_text, 14 * s, V2(x, y + 150 * s), C_CLAY, a->open_err, NULL, 640 * s, NULL);
    /* the recent files (query recent_files: the settings file the web lab writes too) */
    igSetCursorScreenPos(V2(x, y + 190 * s));
    igPushFont(a->f_disp, 16);
    igTextUnformatted(T(a, "home.recent"), NULL);
    igPopFont();
    const JrVal *r = a->recent.root;
    if (r && r->type == JR_OBJ) r = jr_get(r, "files");
    igSetCursorScreenPos(V2(x, y + 220 * s));
    if (igBeginChild_Str("recent", V2(640 * s, H - y - 260 * s), 0, 0)) {
        if (!r || r->type != JR_ARR || !r->n) igTextColored(col4(C_STONE), "%s", T(a, "na.instead.recent"));
        for (size_t k = 0; r && r->type == JR_ARR && k < r->n && a->screen_kind == SCR_HOME; k++) {
            const JrVal *f = &r->items[k];
            const char *path = f->type == JR_STR ? f->s : js(f, "path");
            const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
            int exists = f->type == JR_STR || !jr_get(f, "exists") || jb(f, "exists");
            char lab[1200];
            snprintf(lab, sizeof lab, "%s##r%zu", name, k);
            if (igSelectable_Bool(lab, false, exists ? 0 : ImGuiSelectableFlags_Disabled, V2(0, 34 * s)) && exists) app_open(a, path);
            ImVec2 p = igGetItemRectMin();
            ImDrawList_AddText_FontPtr(igGetWindowDrawList(), a->f_text, 12 * s, V2(p.x + 4 * s, p.y + 18 * s), C_STONE, exists ? path : T(a, "home.notFound"), NULL, 0, NULL);
        }
    }
    igEndChild();
    igEnd();
}

/* ---- the file dialog: fs_list (host/fs.c, linked in), its roots rule ---- */
static const char *exts_for(const char *purpose) {
    if (!strcmp(purpose, "open")) return "[\".bimble\",\".ifc\",\".bimbleview\",\".json\"]";
    if (!strcmp(purpose, "link_model") || !strcmp(purpose, "link_folder")) return "[\".ifc\",\".bimble\",\".bimbleview\"]";
    if (!strcmp(purpose, "save_as")) return "[\".bimble\"]";
    return NULL;
}

void fs_open(App *a, const char *dir, const char *purpose) {
    snprintf(a->fs_purpose, sizeof a->fs_purpose, "%s", purpose);
    Buf body, out;
    buf_init(&body), buf_init(&out);
    char home[1100] = "";
    if (!dir) { /* the folder this purpose listed last (fs_home), else the model's, else home */
        buf_puts(&body, "{\"purpose\":"), json_str(&body, purpose), buf_puts(&body, "}");
        if (cc_host_fs_query("fs_home", body.p, body.len, &out) == 200 || 1) {
            Json j = {0};
            if (out.len && json_parse(&j, out.p, out.len) == 0) {
                const char *last = js(j.root, "last"), *h = js(j.root, "path");
                snprintf(home, sizeof home, "%s", *last ? last : h);
            }
            json_free(&j);
        }
        if (!home[0]) snprintf(home, sizeof home, "%s", getenv("HOME") ? getenv("HOME") : "/");
        dir = home;
        buf_clear(&body), buf_clear(&out);
    }
    buf_puts(&body, "{\"dir\":"), json_str(&body, dir), buf_puts(&body, ",\"purpose\":"), json_str(&body, purpose);
    const char *ex = exts_for(purpose);
    if (ex) buf_puts(&body, ",\"ext\":"), buf_puts(&body, ex);
    buf_putc(&body, '}');
    int st = cc_host_fs_query("fs_list", body.p, body.len, &out);
    if (st == 200 || st == CC_OK) {
        Json j = {0};
        if (json_parse(&j, out.p ? out.p : "null", out.len ? out.len : 4) == 0) {
            json_free(&a->fs);
            a->fs = j;
            snprintf(a->fs_dir, sizeof a->fs_dir, "%s", js(a->fs.root, "dir"));
            memset(a->fs_sel, 0, sizeof a->fs_sel);
        }
    } else {
        char t[400];
        snprintf(a->notice, sizeof a->notice, "%s", Tf(a, t, sizeof t, "files.cannotList", "{msg}", out.p ? out.p : "?")), a->notice_t = app_now();
        if (!a->fs.root) snprintf(a->fs.err, sizeof a->fs.err, "%.*s", (int)(out.len < 190 ? out.len : 190), out.p ? out.p : "");
    }
    buf_free(&body), buf_free(&out);
}

static void path_join(char *out, size_t n, const char *dir, const char *name) {
    size_t l = strlen(dir);
    snprintf(out, n, "%s%s%s", dir, l && dir[l - 1] == '/' ? "" : "/", name);
}

static void file_dialog(App *a) {
    float s = a->scale;
    const char *purpose = a->fs_purpose;
    char key[80];
    snprintf(key, sizeof key, "files.title.%s", purpose);
    bool open = true;
    modal_begin(a, T(a, key), 720, 540, &open);
    int link = !strcmp(purpose, "link_model") || !strcmp(purpose, "link_folder");
    igTextColored(col4(C_STONE), "%s", a->fs_dir);
    if (igButton(T(a, "files.up"), V2(0, 0))) {
        const char *par = js(a->fs.root, "parent");
        if (*par) { char d[1100]; snprintf(d, sizeof d, "%s", par); fs_open(a, d, purpose); }
    }
    igSameLine(0, 6 * s);
    if (igButton(T(a, "files.home"), V2(0, 0))) fs_open(a, getenv("HOME") ? getenv("HOME") : "/", purpose);
    const JrVal *en = a->fs.root ? jr_get(a->fs.root, "entries") : NULL;
    int nen = en && en->type == JR_ARR ? (int)en->n : 0;
    float foot = 46 * s;
    char go[1100] = "", pick[1100] = "";
    if (igBeginChild_Str("fs", V2(0, -foot), ImGuiChildFlags_Borders, 0)) {
        if (!nen) igTextColored(col4(C_STONE), "%s", a->fs.err[0] ? a->fs.err : T(a, "files.empty"));
        ImGuiListClipper *clip = ImGuiListClipper_ImGuiListClipper();
        ImGuiListClipper_Begin(clip, nen, -1);
        while (ImGuiListClipper_Step(clip))
            for (int k = clip->DisplayStart; k < clip->DisplayEnd; k++) {
                const JrVal *E = &en->items[k];
                int dir = !strcmp(js(E, "kind"), "dir");
                char lab[400];
                snprintf(lab, sizeof lab, "%s%s##f%d", js(E, "name"), dir ? "/" : "", k);
                int sel = k < 5000 && a->fs_sel[k];
                if (igSelectable_Bool(lab, sel, ImGuiSelectableFlags_AllowDoubleClick, V2(0, 0))) {
                    if (dir) path_join(go, sizeof go, a->fs_dir, js(E, "name"));
                    else if (k < 5000) {
                        if (!link || !(igGetIO_Nil()->KeyCtrl)) memset(a->fs_sel, 0, sizeof a->fs_sel);
                        a->fs_sel[k] = !sel || !link;
                        if (!strcmp(purpose, "save_as")) snprintf(a->fs_name, sizeof a->fs_name, "%s", js(E, "name"));
                        if (igIsMouseDoubleClicked_Nil(0) && !link) path_join(pick, sizeof pick, a->fs_dir, js(E, "name"));
                    }
                }
                if (!dir) {
                    char sz[48];
                    double b = jnum(E, "size", 0);
                    snprintf(sz, sizeof sz, b > 1e6 ? "%.1f MB" : "%.0f kB", b > 1e6 ? b / 1e6 : b / 1e3);
                    igSameLine(igGetContentRegionAvail().x - 60 * s + igGetCursorPosX(), 0);
                    igTextColored(col4(C_STONE), "%s", sz);
                }
            }
        ImGuiListClipper_destroy(clip);
    }
    igEndChild();
    int nsel = 0, first = -1;
    for (int k = 0; k < nen && k < 5000; k++) if (a->fs_sel[k]) nsel++, first = first < 0 ? k : first;
    if (!strcmp(purpose, "save_as")) {
        igSetNextItemWidth(320 * s);
        igInputText(T(a, "files.name"), a->fs_name, sizeof a->fs_name, 0, NULL, NULL);
        igSameLine(0, 8 * s);
        if (text_button(a, "go", T(a, "files.go.save"), 1, 0) && a->fs_name[0]) {
            char p[1400], n2[300];
            snprintf(n2, sizeof n2, "%s%s", a->fs_name, strstr(a->fs_name, ".bimble") ? "" : ".bimble");
            path_join(p, sizeof p, a->fs_dir, n2);
            Buf b;
            buf_init(&b);
            buf_puts(&b, "{\"path\":"), json_str(&b, p), buf_puts(&b, "}");
            if (core_action(a, "save_as", buf_cstr(&b)) == CC_OK) a->dialog = DLG_NONE;
            buf_free(&b);
        }
    } else if (link) {
        char t[80], nb[16];
        snprintf(nb, sizeof nb, "%d", nsel);
        if (nsel && text_button(a, "go", nsel > 1 ? Tf(a, t, sizeof t, "files.go.linkN", "{n}", nb) : T(a, "files.go.link"), 1, 0)) {
            Buf b;
            buf_init(&b);
            buf_puts(&b, "{\"paths\":[");
            int c = 0;
            for (int k = 0; k < nen && k < 5000; k++)
                if (a->fs_sel[k]) {
                    char p[1400];
                    path_join(p, sizeof p, a->fs_dir, js(&en->items[k], "name"));
                    if (c++) buf_putc(&b, ',');
                    json_str(&b, p);
                }
            buf_puts(&b, "]}");
            if (core_action(a, "link_model", buf_cstr(&b)) == CC_OK) a->dialog = DLG_NONE, a->links_synced = -1;
            buf_free(&b);
        }
        if (nsel) igSameLine(0, 8 * s);
        if (text_button(a, "gofolder", T(a, "files.go.linkFolder"), !nsel, 0)) {
            Buf b;
            buf_init(&b);
            buf_puts(&b, "{\"path\":"), json_str(&b, a->fs_dir), buf_puts(&b, "}");
            if (core_action(a, "link_folder", buf_cstr(&b)) == CC_OK) a->dialog = DLG_NONE, a->links_synced = -1;
            buf_free(&b);
        }
    } else if (nsel && text_button(a, "go", T(a, "files.go.open"), 1, 0)) path_join(pick, sizeof pick, a->fs_dir, js(&en->items[first], "name"));
    modal_end();
    if (go[0]) fs_open(a, go, purpose);
    if (pick[0]) a->dialog = DLG_NONE, app_open(a, pick);
    if (!open) a->dialog = DLG_NONE;
    (void)BIG;
}

/* ---- Settings: what the core stores (query settings values), written with set_settings ---- */
static void set_setting(App *a, const char *field, const char *json_value) {
    char b[600];
    snprintf(b, sizeof b, "{\"%s\":%s}", field, json_value);
    core_action(a, "set_settings", b);
    core_query(a, "settings", NULL, &a->settings);
}

static void settings_dialog(App *a) {
    bool open = true;
    float s = a->scale;
    modal_begin(a, T(a, "settings.title"), 560, 520, &open);
    const JrVal *v = a->settings.root ? jr_get(a->settings.root, "values") : NULL;
    if (!v) igTextWrapped("%s", a->settings.err[0] ? a->settings.err : T(a, "settings.unavailable"));
    else {
        igTextColored(col4(C_STONE), "%s", T(a, "settings.appearance"));
        const char *theme = js(v, "theme");
        char key[80];
        snprintf(key, sizeof key, "settings.theme.%s", theme);
        if (igBeginCombo(T(a, "settings.theme"), T(a, key), 0)) {
            for (int k = 0; k < theme_count(); k++) {
                snprintf(key, sizeof key, "settings.theme.%s", theme_id(k));
                if (igSelectable_Bool(T(a, key), !strcmp(theme, theme_id(k)), 0, V2(0, 0))) {
                    char q[64];
                    snprintf(q, sizeof q, "\"%s\"", theme_id(k));
                    set_setting(a, "theme", q);
                    theme_set(a, k);
                }
            }
            igEndCombo();
        }
        igDummy(V2(1, 6 * s));
        optimisations_ui(a);
        igDummy(V2(1, 6 * s));
        { /* the local session log (native.json session_log; on by default, off: nothing written) */
            bool on = nset_num("session_log", 1) != 0;
            if (igCheckbox(T(a, "native.sessionLog"), &on)) nset_set("session_log", on ? "true" : "false", 0);
            igTextWrapped("%s", T(a, "native.sessionLogText"));
            if (slog_dir()[0]) {
                igPushFont(a->f_mono, 12), igTextWrapped("%s", slog_dir()), igPopFont();
                if (text_button(a, "logdir", T(a, "native.openFolder"), 0, 0)) {
                    char u[1200];
                    snprintf(u, sizeof u, "file://%s", slog_dir());
                    if (!SDL_OpenURL(u)) snprintf(a->notice, sizeof a->notice, "%s", SDL_GetError()), a->notice_t = app_now();
                }
            } else igTextColored(col4(C_STONE), "%s", T(a, "native.sessionLogOff"));
        }
        igDummy(V2(1, 6 * s));
        igTextColored(col4(C_STONE), "%s", T(a, "settings.you"));
        static char name[CC_SETTINGS_LINE_BYTES], email[CC_SETTINGS_LINE_BYTES];
        static int loaded;
        if (!loaded) snprintf(name, sizeof name, "%s", js(v, "user_name")), snprintf(email, sizeof email, "%s", js(v, "user_email")), loaded = 1;
        igInputText(T(a, "settings.name"), name, sizeof name, 0, NULL, NULL);
        if (igIsItemDeactivatedAfterEdit()) { Buf q; buf_init(&q); json_str(&q, name); set_setting(a, "user_name", buf_cstr(&q)); buf_free(&q); }
        igInputText(T(a, "settings.email"), email, sizeof email, 0, NULL, NULL);
        if (igIsItemDeactivatedAfterEdit()) { Buf q; buf_init(&q); json_str(&q, email); set_setting(a, "user_email", buf_cstr(&q)); buf_free(&q); }
        char ft[1200];
        igTextColored(col4(C_STONE), "%s", Tf(a, ft, sizeof ft, "settings.file", "{file}", js(a->settings.root, "file")));
    }
    modal_end();
    if (!open) a->dialog = DLG_NONE;
}


/* ---- the optimisations (the owner's rule, 27 Nov 2026: each one a setting with a good default and an
 * off value, shown in Settings and Performance and in the report). The SAME keys as the lab's settings
 * (session/settings.h); only what this renderer has:
 *   moving_fps      the moving budget: while the view moves, small elements are left out until a frame fits
 *                   1/fps (default 30; 0: off, every moving frame whole)
 *   moving_proxies  far parts drawn as their proxies (the links' caches' HLOD; default true; false: off,
 *                   every link's detail cell drawn)
 *   link_gpu_mb     the links' detail on the GPU (default 768 MB; 65536: off, no limit): past it the other
 *                   links keep their proxies
 *   edges           (the View tab's Edges, the session's `edges` action, not a setting) screen-space edges
 *                   (default on; off: faces only)
 * The cull pass (frustum and screen-size cut of every element on the GPU) is how this renderer draws: no
 * setting, always on (the lab's gpu_cull is WebGPU's culling of copies, not this). ---- */
#define LINK_GPU_OFF 65536
/* NUI2: an optimisation's value: the session's settings when they know the key (the web lab's: VIS1 adds draw_visible,
 * still_px, cull_back), else the app's own (native.json, a run's --set), else the default */
static double opt_num(App *a, const char *key, double dflt) {
    const JrVal *v = a->settings.root ? jr_get(a->settings.root, "values") : NULL, *x = v ? jr_get(v, key) : NULL;
    double run = nset_num(key, NAN);
    const char *nv = nset_get(key);
    if (!isnan(run) && nv) return run; /* a run's --set or the app's own file first: the A/B runs set any key */
    if (x && x->type == JR_NUM) return x->num;
    if (x && x->type == JR_BOOL) return x->b;
    return dflt;
}
static int opt_str_is(const char *key, const char *val, int dflt) {
    const char *v = nset_get(key);
    if (!v) return dflt;
    size_t n = strlen(val);
    return v[0] == '"' && !strncmp(v + 1, val, n) && v[n + 1] == '"';
}
/* the defaults (ADR 0084 amendment, NUI2: from the A/B tables) */
#define D_ERROR_PX 1.0
#define D_STILL_PX 0.0
#define D_CULL_BACK 0
#define D_OCCLUSION 1
#define D_TILES 0
void optimisations_apply(App *a) {
    const JrVal *v = a->settings.root ? jr_get(a->settings.root, "values") : NULL;
    a->moving_fps = (int)opt_num(a, "moving_fps", 30);
    a->adaptive = a->moving_fps > 0;
    a->hlod = !v || !jr_get(v, "moving_proxies") || jb(v, "moving_proxies");
    if (nset_get("moving_proxies")) a->hlod = nset_num("moving_proxies", 1) != 0;
    nv_render_set_hlod(a->r, a->hlod, 0);
    const char *g = getenv("BIMBLE_LINK_GPU_MB"); /* a run's own */
    double mb = g && *g ? atof(g) : opt_num(a, "link_gpu_mb", 768);
    a->gpu_budget = mb >= LINK_GPU_OFF ? 0 : (uint64_t)(mb * 1048576.0);
    int tiles = opt_str_is("renderer_path", "tiles", D_TILES); /* parts until the tiles win everywhere (the ticket's rule) */
    if (tiles != a->use_tiles) a->use_tiles = tiles, a->links_synced = -1;
    a->topts.occlusion = opt_num(a, "draw_visible", D_OCCLUSION) != 0;
    a->topts.error_px = opt_num(a, "error_px", D_ERROR_PX);
    a->topts.lod = opt_num(a, "lod", 1) != 0 && a->topts.error_px > 0;
    a->topts.cull_back = opt_num(a, "cull_back", D_CULL_BACK) != 0;
    a->topts.groups = opt_num(a, "occlusion_groups", 1) != 0;
    a->topts.clip_planes = opt_num(a, "section_clip_planes", 1) != 0;
    a->topts.budget = a->gpu_budget;
    a->still_cut = (float)opt_num(a, "still_px", D_STILL_PX);
    if (a->still_cut < 0) a->still_cut = 0;
    a->dirty = 1;
}

/* a setting changed from the panel: the session's when it knows the key, else the app's own */
static void set_opt(App *a, const char *key, const char *json) {
    char b[200];
    snprintf(b, sizeof b, "{\"%s\":%s}", key, json);
    Buf o;
    buf_init(&o);
    int st = core_call(a, "action", "set_settings", b, &o);
    buf_free(&o);
    if (st == CC_OK) core_query(a, "settings", NULL, &a->settings);
    nset_set(key, json, 0);
    optimisations_apply(a);
}

void optimisations_ui(App *a) {
    float s = a->scale;
    if (!a->settings.root) core_query(a, "settings", NULL, &a->settings);
    const JrVal *v = a->settings.root ? jr_get(a->settings.root, "values") : NULL;
    igPushFont(a->f_semi, 14), igTextUnformatted(T(a, "native.optimisations"), NULL), igPopFont();
    igTextColored(col4(C_STONE), "%s", T(a, "settings.moving"));
    int fps = (int)jnum(v, "moving_fps", 30);
    static const int FPS[] = {0, 15, 30, 60};
    char lab[64], nb[16];
    for (int k = 0; k < 4; k++) {
        snprintf(nb, sizeof nb, "%d", FPS[k]);
        const char *l = FPS[k] ? Tf(a, lab, sizeof lab, "settings.moving.fps", "{fps}", nb) : T(a, "settings.moving.0");
        if (k) igSameLine(0, 10 * s);
        if (igRadioButton_Bool(l, fps == FPS[k])) set_setting(a, "moving_fps", nb), optimisations_apply(a);
    }
    bool px = !v || !jr_get(v, "moving_proxies") || jb(v, "moving_proxies");
    if (igCheckbox(T(a, "settings.proxies"), &px)) set_setting(a, "moving_proxies", px ? "true" : "false"), optimisations_apply(a);
    int mbv = (int)jnum(v, "link_gpu_mb", 768);
    bool lim = mbv < LINK_GPU_OFF;
    if (igCheckbox(T(a, "settings.linkGpu"), &lim)) {
        set_setting(a, "link_gpu_mb", lim ? "768" : "65536"), optimisations_apply(a);
        for (int i = 0; i < a->nlinks; i++) if (a->links_s[i].detail_state == 4) a->links_s[i].detail_state = 0; /* asked again */
    }
    if (lim) {
        igSameLine(0, 8 * s);
        igSetNextItemWidth(200 * s);
        igSliderInt("##linkgpu", &mbv, 64, 16384, "%d MB", ImGuiSliderFlags_Logarithmic);
        if (igIsItemDeactivatedAfterEdit()) {
            char q[32];
            snprintf(q, sizeof q, "%d", mbv);
            set_setting(a, "link_gpu_mb", q), optimisations_apply(a);
            for (int i = 0; i < a->nlinks; i++) if (a->links_s[i].detail_state == 4) a->links_s[i].detail_state = 0;
        }
    }
    bool ed = nv_render_edges(a->r) != NV_EDGES_NONE;
    if (igCheckbox(T(a, "native.edgesScreen"), &ed)) tool_run_action(a, "edges");
    /* NUI2: the tile path's */
    bool tl = a->use_tiles;
    if (igCheckbox(T(a, "native.opt.tiles"), &tl)) set_opt(a, "renderer_path", tl ? "\"tiles\"" : "\"parts\"");
    if (a->use_tiles) {
        bool oc = a->topts.occlusion, lod = a->topts.lod, cb = a->topts.cull_back;
        if (igCheckbox(T(a, "native.opt.occlusion"), &oc)) set_opt(a, "draw_visible", oc ? "true" : "false");
        bool gr = a->topts.groups;
        if (oc && igCheckbox(T(a, "native.opt.groups"), &gr)) set_opt(a, "occlusion_groups", gr ? "true" : "false");
        if (igCheckbox(T(a, "native.opt.lod"), &lod)) set_opt(a, "lod", lod ? "true" : "false");
        if (igCheckbox(T(a, "native.opt.cullBack"), &cb)) set_opt(a, "cull_back", cb ? "true" : "false");
        float e = (float)a->topts.error_px, m = a->still_cut;
        igSetNextItemWidth(200 * s);
        igSliderFloat(T(a, "native.opt.errorPx"), &e, 0, 8, "%.1f px", 0);
        if (igIsItemDeactivatedAfterEdit()) { char q[32]; snprintf(q, sizeof q, "%.2f", e); set_opt(a, "error_px", q); }
        igSetNextItemWidth(200 * s);
        igSliderFloat(T(a, "native.opt.stillPx"), &m, 0, 8, "%.1f px", 0);
        if (igIsItemDeactivatedAfterEdit()) { char q[32]; snprintf(q, sizeof q, "%.2f", m); set_opt(a, "still_px", q); }
    }
    igTextColored(col4(C_STONE), "%s", T(a, "native.cullAlways"));
}

static void optimisations_report(App *a, Buf *b) {
    char t[900];
    snprintf(t, sizeof t, "optimisations: moving budget (moving_fps) %s; far parts as proxies (moving_proxies) %s; links' GPU budget (link_gpu_mb) %s; edges (action edges) %s; cull pass on the GPU (no setting) on\n",
             a->moving_fps > 0 ? (snprintf(t + 300, 40, "%d fps", a->moving_fps), t + 300) : "off", a->hlod ? "on" : "off",
             a->gpu_budget ? (snprintf(t + 340, 40, "%llu MB", (unsigned long long)(a->gpu_budget >> 20)), t + 340) : "off (no limit)",
             nv_render_edges(a->r) == NV_EDGES_SCREEN ? "screen" : nv_render_edges(a->r) == NV_EDGES_LINES ? "lines" : "off");
    t[strcspn(t, "\n")] = 0;
    size_t k = strlen(t);
    if (a->use_tiles)
        snprintf(t + k, sizeof t - k, "; tiles (renderer_path) on: occlusion (draw_visible) %s%s, error (error_px) %.2g px, levels of detail (lod) %s, small elements (still_px) %.2g px, back faces (cull_back) %s\n",
                 a->topts.occlusion ? "on" : "off", a->topts.occlusion ? (a->topts.groups ? " to groups of 8 elements (occlusion_groups)" : " by tile (occlusion_groups off)") : "", a->topts.error_px, a->topts.lod ? "on" : "off", a->still_cut, a->topts.cull_back ? "culled" : "drawn");
    else snprintf(t + k, sizeof t - k, "; tiles (renderer_path) off: parts\n");
    buf_puts(b, t);
    snprintf(a->settings_line, sizeof a->settings_line, "%.*s", (int)strcspn(t + 15, "\n"), t + 15);
}

/* ---- Performance (perf.ts perfReport's shape) ---- */
typedef struct { int n; double last, median, p95, max; } Stats;
static int cmpf(const void *x, const void *y) { float a = *(const float *)x, b = *(const float *)y; return a < b ? -1 : a > b; }
static Stats stats(const float *ring, int count, int from) {
    Stats s = {0};
    int n = count - from;
    if (n > 2048) n = 2048, from = count - 2048;
    if (n <= 0) return s;
    float *v = malloc((size_t)n * sizeof *v);
    if (!v) return s;
    for (int k = 0; k < n; k++) v[k] = ring[(from + k) % 2048];
    s.n = n, s.last = v[n - 1];
    qsort(v, (size_t)n, sizeof *v, cmpf);
    s.median = v[n / 2], s.p95 = v[(int)(n * 0.95) < n ? (int)(n * 0.95) : n - 1], s.max = v[n - 1];
    free(v);
    return s;
}
static void msf(char *o, size_t n, double x) {
    if (!isfinite(x) || x < 0) snprintf(o, n, "-");
    else snprintf(o, n, x < 10 ? "%.1f ms" : "%.0f ms", x);
}
static void fline(Buf *b, const char *head, Stats s) {
    char l[64], m[64], p[64], x[64], t[300];
    if (!s.n) { snprintf(t, sizeof t, "%snone\n", head); buf_puts(b, t); return; }
    msf(l, sizeof l, s.last), msf(m, sizeof m, s.median), msf(p, sizeof p, s.p95), msf(x, sizeof x, s.max);
    snprintf(t, sizeof t, "%sn %d, last %s, median %s, p95 %s, max %s\n", head, s.n, l, m, p, x);
    buf_puts(b, t);
}

void perf_report(App *a, Buf *b) {
    char t[1024], m1[64], m2[64];
    time_t now = time(NULL);
    struct tm g;
    gmtime_r(&now, &g);
    strftime(m1, sizeof m1, "%Y-%m-%dT%H:%M:%SZ", &g);
    buf_puts(b, "Bimble performance report\n");
    snprintf(t, sizeof t, "date: %s; page: native app (bimble %s); system: Linux\n", m1, CC_RELEASE_VERSION), buf_puts(b, t);
    buf_puts(b, "renderer: native (SDL GPU / Vulkan)\n");
    optimisations_report(a, b);
    snprintf(t, sizeof t, "adapter: %s (%s)\n", a->gpu_name ? a->gpu_name : "unknown", a->gpu_driver ? a->gpu_driver : "?"), buf_puts(b, t);
    snprintf(t, sizeof t, "canvas: %u x %u px, device pixel ratio %.2g\n", a->vpw, a->vph, a->offscreen ? 1.0 : SDL_GetWindowPixelDensity(a->win)), buf_puts(b, t);
    buf_puts(b, "frame time (draw to pixels: record to the GPU's fence), last 2048 frames:\n");
    fline(b, "  moving: ", stats(a->ring_move, a->nmove, 0));
    fline(b, "  still:  ", stats(a->ring_still, a->nstill, 0));
    nv_render_stats st = nv_render_get_stats(a->r);
    snprintf(t, sizeof t, "last frame: %llu triangles, %u draw calls; parts %u, commands %u; proxies drawn %u\n", (unsigned long long)st.tris, st.draws,
             st.parts, st.commands, st.proxies), buf_puts(b, t);
    uint64_t peak = 0, rss = app_rss(&peak);
    snprintf(t, sizeof t, "memory: GPU buffers %.1f MB, process %.1f MB (peak %.1f MB)\n", st.gpu_bytes / 1048576.0, rss / 1048576.0, peak / 1048576.0), buf_puts(b, t);
    uint32_t nel = 0;
    uint64_t tris = 0;
    for (uint32_t k = 0; k < nv_render_nparts(a->r); k++) nel += nv_part_scene(nv_render_part(a->r, k))->nelements, tris += nv_part_scene(nv_render_part(a->r, k))->tris_total;
    msf(m1, sizeof m1, a->t_first_picture ? a->t_first_picture - a->t_open : -1), msf(m2, sizeof m2, a->t_complete ? a->t_complete - a->t_open : -1);
    snprintf(t, sizeof t, "model: %u elements, %llu triangles; open -> first frame %s, -> complete picture %s\n", nel, (unsigned long long)tris, m1, m2), buf_puts(b, t);
    if (a->nlinks) {
        snprintf(t, sizeof t, "links (%d):\n", a->nlinks), buf_puts(b, t);
        for (int k = 0; k < a->nlinks; k++) {
            const SLink *L = &a->links_s[k];
            if (L->ts && nv_tileset_toc(L->ts)->ntiles)
                snprintf(t, sizeof t, "  link %d: %u elements, %llu triangles, %s, detail loaded by tiles (%u of %u tiles on the GPU, %.1f MB)\n", k + 1, L->nelem,
                         (unsigned long long)L->tris, L->visible ? "shown" : "hidden", nv_tileset_resident(L->ts), nv_tileset_toc(L->ts)->ntiles, nv_tileset_gpu_bytes(L->ts) / 1048576.0);
            else
            snprintf(t, sizeof t, "  link %d: %u elements, %llu triangles, %s%s (%.1f MB)\n", k + 1, L->nelem, (unsigned long long)L->tris, L->visible ? "shown" : "hidden",
                     L->detail ? ", detail loaded" : L->proxy ? ", proxies only" : "", (L->detail ? nv_part_gpu_bytes(L->detail) : L->proxy ? nv_part_gpu_bytes(L->proxy) : 0) / 1048576.0);
            buf_puts(b, t);
        }
    }
    buf_puts(b, "slowest input -> frame (this session):\n");
    if (!a->nslow) buf_puts(b, "  none yet\n");
    for (int k = 0; k < a->nslow; k++) msf(m1, sizeof m1, a->slow[k].ms), snprintf(t, sizeof t, "  %d. %s %s\n", k + 1, m1, a->slow[k].what), buf_puts(b, t);
    if (a->rec_done) {
        snprintf(t, sizeof t, "recording (%.0f s):\n", a->rec_seconds), buf_puts(b, t);
        fline(b, "  moving: ", stats(a->ring_move, a->nmove, a->rec_move0));
        fline(b, "  still:  ", stats(a->ring_still, a->nstill, a->rec_still0));
        msf(m1, sizeof m1, a->rec_slowest);
        snprintf(t, sizeof t, "  inputs: %d, slowest %s\n", a->ninputs - a->rec_inputs0, m1), buf_puts(b, t);
    }
}

/* Copy: the report to the clipboard, checked by reading it back, and ALWAYS to a file too (the session log's folder),
 * its path in the notice; a failure says why (SDL's error). */
void perf_copy(App *a) {
    Buf b;
    buf_init(&b);
    perf_report(a, &b);
    int lines = 0;
    for (size_t k = 0; k < b.len; k++) lines += b.p[k] == '\n';
    char n[16], t[256], path[1200] = "";
    snprintf(n, sizeof n, "%d", lines);
    int saved = slog_save_report(a, path, sizeof path) == 0;
    int ok = SDL_SetClipboardText(buf_cstr(&b));
    char why[256] = "";
    if (!ok) snprintf(why, sizeof why, "%s", SDL_GetError());
    else {
        char *back = SDL_GetClipboardText(); /* what the clipboard holds now (this process serves it) */
        if (!back || strcmp(back, buf_cstr(&b))) ok = 0, snprintf(why, sizeof why, "the clipboard did not keep it (%s)", SDL_GetError());
        SDL_free(back);
    }
    if (ok) snprintf(a->notice, sizeof a->notice, "%s", Tf(a, t, sizeof t, "perf.copied", "{n}", n));
    else snprintf(a->notice, sizeof a->notice, "%s: %s", T(a, "perf.copyFailed"), why[0] ? why : "?");
    if (saved) {
        size_t k = strlen(a->notice);
        snprintf(a->notice + k, sizeof a->notice - k, " %s %s", T(a, "native.savedTo"), path);
        snprintf(a->notice_path, sizeof a->notice_path, "%s", path);
    }
    slog("Copy report: clipboard %s%s%s; file %s", ok ? "ok" : "failed: ", ok ? "" : why, "", saved ? path : "not written");
    a->notice_t = app_now();
    buf_free(&b);
}

/* Save report…: the system's save dialog (a portal on Wayland; its answer arrives on another thread) */
static char save_text_path[1200];
static SDL_AtomicInt save_done;
static char *save_text;
static void SDLCALL save_cb(void *ud, const char *const *files, int filter) {
    (void)ud, (void)filter;
    if (files && files[0] && save_text) {
        FILE *f = fopen(files[0], "w");
        int ok = f && fputs(save_text, f) >= 0;
        if (f) ok = fclose(f) == 0 && ok;
        snprintf(save_text_path, sizeof save_text_path, "%s%s", ok ? "" : "!", files[0]);
    } else snprintf(save_text_path, sizeof save_text_path, "%s", files ? "" : "!");
    SDL_SetAtomicInt(&save_done, 1);
}
void perf_save_as(App *a) {
    Buf b;
    buf_init(&b);
    perf_report(a, &b);
    free(save_text);
    save_text = strdup(buf_cstr(&b));
    buf_free(&b);
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS), *home = getenv("HOME"); /* the session's folder, else Documents, never the home's root */
    char def[1300];
    if (slog_dir()[0]) snprintf(def, sizeof def, "%s/bimble-performance-report.txt", slog_dir());
    else if (docs) snprintf(def, sizeof def, "%sbimble-performance-report.txt", docs);
    else snprintf(def, sizeof def, "%s/bimble-performance-report.txt", home ? home : ".");
    static const SDL_DialogFileFilter flt[] = {{"Text", "txt"}};
    SDL_SetAtomicInt(&save_done, 0);
    SDL_ShowSaveFileDialog(save_cb, NULL, a->win, flt, 1, def);
}
static void perf_save_poll(App *a) {
    if (!SDL_GetAtomicInt(&save_done)) return;
    SDL_SetAtomicInt(&save_done, 0);
    if (!save_text_path[0]) return; /* cancelled */
    if (save_text_path[0] == '!') snprintf(a->notice, sizeof a->notice, "%s %s", T(a, "native.saveFailed"), save_text_path[1] ? save_text_path + 1 : SDL_GetError());
    else snprintf(a->notice, sizeof a->notice, "%s %s", T(a, "native.savedTo"), save_text_path);
    slog("Save report: %s", a->notice);
    a->notice_t = app_now();
}

static void perf_dialog(App *a) {
    perf_save_poll(a);
    bool open = true;
    float s = a->scale;
    modal_begin(a, T(a, "perf.title"), 620, 560, &open);
    static char shown[8192];
    if (a->rec_until && app_now() >= a->rec_until) a->rec_until = 0, a->rec_done = 1;
    if (text_button(a, "copy", T(a, "perf.copy"), 1, 0)) { /* on the click itself, the main thread (Wayland wants the input's serial) */
        perf_copy(a);
        snprintf(shown, sizeof shown, "%s", a->notice);
    }
    igSameLine(0, 8 * s);
    if (text_button(a, "save", T(a, "native.saveReport"), 0, 0)) perf_save_as(a);
    igSameLine(0, 8 * s);
    if (text_button(a, "rec", a->rec_until ? T(a, "perf.recording") : T(a, "perf.record"), 0, a->rec_until != 0) && !a->rec_until) {
        a->rec_until = app_now() + 10000, a->rec_done = 0, a->rec_seconds = 10;
        a->rec_move0 = a->nmove, a->rec_still0 = a->nstill, a->rec_inputs0 = a->ninputs, a->rec_slowest = 0;
    }
    optimisations_ui(a);
    Buf b;
    buf_init(&b);
    perf_report(a, &b);
    igPushFont(a->f_mono, 12.5f);
    igBeginChild_Str("rep", V2(0, 0), ImGuiChildFlags_Borders, 0);
    igTextUnformatted(shown[0] ? shown : buf_cstr(&b), NULL);
    igEndChild();
    igPopFont();
    buf_free(&b);
    igTextWrapped("%s", T(a, "perf.private"));
    modal_end();
    if (!open) a->dialog = DLG_NONE, shown[0] = 0;
}

static void keys_dialog(App *a) {
    bool open = true;
    modal_begin(a, T(a, "native.keys"), 520, 460, &open);
    const JrVal *K = a->keys;
    if (igBeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH, V2(0, 0), 0)) {
        for (size_t i = 0; K && K->type == JR_ARR && i < K->n; i++) {
            const JrVal *r = &K->items[i];
            char k[64];
            snprintf(k, sizeof k, "%s%s%s", jb(r, "ctrl") ? "Ctrl " : "", jb(r, "shift") ? "Shift " : "", js(r, "key"));
            igTableNextRow(0, 0);
            igTableNextColumn();
            igPushFont(a->f_mono, 13), igTextUnformatted(k, NULL), igPopFont();
            igTableNextColumn();
            const char *act = js(r, "action");
            char key[96];
            snprintf(key, sizeof key, "action.%s", act);
            igTextUnformatted(*act ? T(a, key) : js(r, "effect"), NULL);
        }
        igEndTable();
    }
    modal_end();
    if (!open) a->dialog = DLG_NONE;
}

static void discard_dialog(App *a) {
    bool open = true;
    modal_begin(a, T(a, "app.notSaved"), 420, 150, &open);
    igTextWrapped("%s", T(a, "native.discard"));
    if (text_button(a, "disc", T(a, "native.discardGo"), 1, 0)) {
        char p[1024];
        snprintf(p, sizeof p, "%s", a->pending_open);
        a->dialog = DLG_NONE;
        Buf o;
        buf_init(&o);
        if (!strcmp(p, ":close")) {
            scene_reset(a);
            core_call(a, "action", "close", "{\"discard\":true}", &o);
            a->screen_kind = SCR_HOME;
            core_query(a, "recent_files", NULL, &a->recent);
        } else {
            snprintf(a->model_path, sizeof a->model_path, "%s", p);
            a->screen_kind = SCR_OPENING, a->t_open = app_now(), a->open_err[0] = 0;
        }
        buf_free(&o);
    }
    igSameLine(0, 8);
    if (text_button(a, "keep", T(a, "links.cancel"), 0, 0)) a->dialog = DLG_NONE;
    modal_end();
    if (!open) a->dialog = DLG_NONE;
}

void dialogs(App *a) {
    switch (a->dialog) {
    case DLG_OPEN: case DLG_LINK: case DLG_SAVEAS: file_dialog(a); break;
    case DLG_SETTINGS: settings_dialog(a); break;
    case DLG_PERF: perf_dialog(a); break;
    case DLG_KEYS: keys_dialog(a); break;
    case DLG_DISCARD: discard_dialog(a); break;
    default: break;
    }
}
