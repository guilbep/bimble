/* The native app's shell (ticket NUI1): the title row with the File menu (QuickBar.vue, FileMenu), the
 * tab bar, the ribbon (every tab, from data/ribbon.json: the same RIBBON the Vue Ribbon.vue draws), the
 * status bar and the right-click menu (ContextMenu.vue). The ribbon tool's rules are RibbonTool.vue's:
 * "not available yet" from capability.ts (no action, or a capability not served), off from `query
 * actions` `enabled`, on from `pressed` or the page shown; NUI1 adds "not yet (native)": an action
 * whose page, dialog or drawing the native shell has not built (tool_native below). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ui.h"
#include "calls.h"

int text_button(App *a, const char *id, const char *label, int primary, int on) {
    ImVec2 sz = igCalcTextSize(label, NULL, false, -1);
    float s = a->scale, h = 30 * s, w = sz.x + 24 * s;
    ImVec2 p = igGetCursorScreenPos();
    int hit = igInvisibleButton(id, V2(w, h), 0);
    int hov = igIsItemHovered(0);
    ImDrawList *dl = igGetWindowDrawList();
    ImU32 bg = primary ? C_CLAY : on ? C_INK : hov ? C_SUNK : 0;
    if (bg) ImDrawList_AddRectFilled(dl, p, V2(p.x + w, p.y + h), bg, h / 2, 0);
    ImDrawList_AddText_FontPtr(dl, primary ? a->f_semi : a->f_text, 14 * s, V2(p.x + 12 * s, p.y + (h - sz.y) / 2),
                               primary || on ? C_CREAM : C_INK, label, NULL, 0, NULL);
    return hit;
}

/* The actions the native shell runs (their effect drawn natively). Any other action of a ribbon
 * tool: "not yet (native)" (its ghost, page, dialog or 2D drawing is the web lab's for now). */
static const char *const NATIVE_ACTIONS[] = {
    "fit", "top", "home", "orthographic", "edges", "measure", "section_box", "box_to_selection", "hide_now", "isolate_now",
    "hide_in_view", "isolate_in_view", "unhide", "reveal_hidden", "clear_temporary", "show_all_in_view", "show_all", "show_previous",
    "undo", "redo", "save", "new", "open", "link_model", "link_folder", "manage_links", "settings", NULL};
static int native_action(const char *action) {
    for (int i = 0; action && NATIVE_ACTIONS[i]; i++) if (!strcmp(NATIVE_ACTIONS[i], action)) return 1;
    return 0;
}
/* 1: the tool is "not yet (native)" (the core serves it, the native shell has not built it). */
static int tool_native_not_yet(const RTool *t) {
    if (t->page) return native_not_yet(t->page);
    if (t->dialog) return native_not_yet(t->dialog);
    if (t->panel) return native_not_yet(t->panel);
    return t->action && !native_action(t->action);
}

/* The web lab on the same file, beside this app (bimble-ccore open <file>: the browser opens it). */
static void open_web_lab(App *a) {
    char exe[1024], cmd[4096];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[k > 0 ? k : 0] = 0;
    char *sl = strrchr(exe, '/');
    if (sl) sl[1] = 0;
    const char *file = jb(a->state.root, "editable") || *js(a->state.root, "file") ? a->ses->path : NULL;
    Buf q;
    buf_init(&q);
    if (file && *file) { /* single quotes, each ' as '\'' */
        buf_puts(&q, " '");
        for (const char *c = file; *c; c++) *c == '\'' ? buf_puts(&q, "'\\''") : buf_putc(&q, *c);
        buf_putc(&q, '\'');
    }
    snprintf(cmd, sizeof cmd, "'%sbimble-ccore' open%s >/dev/null 2>&1 &", exe, q.p ? q.p : "");
    if (access(cmd + 1, X_OK) && 0) {}
    int rc = system(cmd);
    snprintf(a->notice, sizeof a->notice, "%s: bimble-ccore open%s (%d)", T(a, "native.openInLab"), q.p ? q.p : "", rc);
    a->notice_t = app_now();
    buf_free(&q);
}

/* An action of the ribbon, a key, the File menu or the right-click menu. */
void tool_run_action(App *a, const char *action) {
    slog("action %s", action ? action : "(none)"); /* every action in the session log (the owner's sessions read without asking) */
    if (!action) return;
    Buf o;
    buf_init(&o);
    if (!strcmp(action, ":weblab")) open_web_lab(a);
    else if (!strcmp(action, "fit")) app_fit(a);
    else if (!strcmp(action, "top")) view_top(a);
    else if (!strcmp(action, "home")) app_close(a);
    else if (!strcmp(action, "undo") || !strcmp(action, "redo")) {
        int st = core_call(a, action, NULL, "", &o);
        if (st != CC_OK) snprintf(a->notice, sizeof a->notice, "%.*s", (int)(o.len < 300 ? o.len : 300), o.p ? o.p : ""), a->notice_t = app_now();
    } else if (!strcmp(action, "new")) app_new(a);
    else if (!strcmp(action, "open")) fs_open(a, NULL, "open"), a->dialog = DLG_OPEN;
    else if (!strcmp(action, "link_model")) fs_open(a, NULL, "link_model"), a->dialog = DLG_LINK;
    else if (!strcmp(action, "link_folder")) fs_open(a, NULL, "link_folder"), a->dialog = DLG_LINK;
    else if (!strcmp(action, "manage_links")) a->links_panel = !a->links_panel;
    else if (!strcmp(action, "settings")) core_query(a, "settings", NULL, &a->settings), a->dialog = DLG_SETTINGS;
    else if (!strcmp(action, "save")) {
        if (core_action(a, "save", NULL) != CC_OK && strstr(a->notice, "save_as")) fs_open(a, NULL, "save_as"), a->dialog = DLG_SAVEAS, a->notice[0] = 0;
    } else if (!strcmp(action, "orthographic")) {
        core_action(a, action, NULL);
        core_refresh(a);
        a->cam.ortho = jb(action_row(a, "orthographic"), "pressed");
        if (a->cam.ortho) a->cam.ortho_h = a->cam.dist * 0.315; /* the same view: tan(35°/2) of the distance */
        a->dirty = 1;
    } else if (!strcmp(action, "edges")) {
        core_action(a, action, NULL);
        core_refresh(a);
        nv_render_set_edges(a->r, jb(action_row(a, "edges"), "pressed") ? NV_EDGES_SCREEN : NV_EDGES_NONE);
        a->dirty = 1;
    } else {
        core_action(a, action, NULL);
        a->vis_dirty = 1;
        if (!strcmp(action, "measure")) json_free(&a->measure);
    }
    buf_free(&o);
}

static void tool_run(App *a, const RTool *t) {
    if (t->page) {
        if (a->show == SHOW_PAGE && !strcmp(a->page, t->page)) a->show = SHOW_VIEW; /* the page's tool again: back */
        else {
            if (t->action) core_action(a, t->action, NULL);
            snprintf(a->page, sizeof a->page, "%s", t->page), a->show = SHOW_PAGE;
        }
        return;
    }
    tool_run_action(a, t->action);
}

static void ribbon_tool(App *a, const RTool *t) {
    float s = a->scale;
    const char *label = T(a, t->key);
    const JrVal *row = action_row(a, t->action);
    int not_yet = t->page ? (t->action ? action_not_yet(a, t->action) : 1) : action_not_yet(a, t->action) || (!t->action && !t->dialog);
    if (t->page && !strcmp(t->page, "quantities")) not_yet = 0;
    int not_native = !not_yet && tool_native_not_yet(t);
    int on = !not_yet && (t->page ? a->show == SHOW_PAGE && !strcmp(a->page, t->page) : jb(row, "pressed"));
    if (t->action && !strcmp(t->action, "manage_links")) on = a->links_panel;
    int off = !not_yet && !not_native && t->action && row && !jb(row, "enabled");
    /* RibbonTool.vue: min-width 56 px, padding 6 8, icon 22, the label under it (two lines at most) */
    ImVec2 ts = igCalcTextSize(label, NULL, false, 90 * s);
    float w = ts.x + 16 * s, h = 64 * s;
    if (w < 56 * s) w = 56 * s;
    ImVec2 p = igGetCursorScreenPos();
    igPushID_Str(t->key);
    int hit = igInvisibleButton("t", V2(w, h), 0);
    int hov = igIsItemHovered(0);
    igPopID();
    ImDrawList *dl = igGetWindowDrawList();
    if (on) ImDrawList_AddRectFilled(dl, p, V2(p.x + w, p.y + h - 4 * s), C_SUNK, 8 * s, 0), ImDrawList_AddRect(dl, p, V2(p.x + w, p.y + h - 4 * s), C_INK, 8 * s, 1.5f * s, 0);
    else if (hov && !not_yet && !off) ImDrawList_AddRectFilled(dl, p, V2(p.x + w, p.y + h - 4 * s), C_SUNK, 8 * s, 0);
    ImU32 fg = not_yet || off || not_native ? C_MUTED : C_INK;
    icon_draw(a, dl, t->icon, p.x + (w - 22 * s) / 2, p.y + 6 * s, 22 * s, fg);
    ImDrawList_AddText_FontPtr(dl, a->f_text, 13 * s, V2(p.x + (w - ts.x) / 2, p.y + 32 * s), fg, label, NULL, 90 * s, NULL);
    if (not_native) ImDrawList_AddCircleFilled(dl, V2(p.x + w - 7 * s, p.y + 7 * s), 3 * s, C_SLATE, 0); /* a dot: the web lab has it */
    char tip[300];
    if (hov && not_yet) igSetTooltip("%s", Tf(a, tip, sizeof tip, "na.tooltip", "{name}", label));
    else if (hov && not_native) igSetTooltip("%s", Tf(a, tip, sizeof tip, "native.tooltip", "{name}", label));
    else if (hov && row && *js(row, "shortcut")) igSetTooltip("%s (%s)", label, js(row, "shortcut"));
    if (hit && not_native) {
        snprintf(a->notice, sizeof a->notice, "%s", Tf(a, tip, sizeof tip, "native.tooltip", "{name}", label));
        a->notice_t = app_now();
    } else if (hit && !not_yet && !off) tool_run(a, t);
    igSameLine(0, 2 * s);
}

/* ---- the File menu (FileMenu.vue's rows the native app has; the rest: not yet (native)) ---- */
static int menu_row(App *a, const char *label, const char *shortcut, int enabled, int native) {
    char lab[300];
    snprintf(lab, sizeof lab, "%s%s", label, native ? "" : "  ·");
    int hit = igMenuItem_Bool(lab, shortcut, false, enabled);
    if (!native && igIsItemHovered(0)) {
        char tip[300];
        igSetTooltip("%s", Tf(a, tip, sizeof tip, "native.tooltip", "{name}", label));
    }
    if (hit && !native) {
        char tip[300];
        snprintf(a->notice, sizeof a->notice, "%s", Tf(a, tip, sizeof tip, "native.tooltip", "{name}", label)), a->notice_t = app_now();
        return 0;
    }
    return hit;
}

static void file_menu(App *a) {
    if (!a->file_menu) return;
    float s = a->scale;
    igSetNextWindowPos(V2(52 * s, 42 * s), 0, V2(0, 0));
    igSetNextWindowSize(V2(300 * s, 0), 0);
    igPushStyleColor_U32(ImGuiCol_WindowBg, C_SHEET);
    igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 8 * s);
    igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 1);
    igBegin("##filemenu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    int loaded = a->ses->open;
    igTextColored(col4(C_STONE), "%s", T(a, "filemenu.files"));
    if (menu_row(a, T(a, "filemenu.new"), "Ctrl N", 1, 1)) tool_run_action(a, "new"), a->file_menu = 0;
    if (menu_row(a, T(a, "filemenu.open"), "Ctrl O", 1, 1)) tool_run_action(a, "open"), a->file_menu = 0;
    menu_row(a, T(a, "filemenu.importIfc"), NULL, 1, 0);
    if (menu_row(a, T(a, "filemenu.save"), "Ctrl S", loaded && jb(a->state.root, "editable"), 1)) tool_run_action(a, "save"), a->file_menu = 0;
    if (menu_row(a, T(a, "filemenu.saveAs"), NULL, loaded && jb(a->state.root, "editable"), 1)) fs_open(a, NULL, "save_as"), a->dialog = DLG_SAVEAS, a->file_menu = 0;
    if (menu_row(a, T(a, "filemenu.close"), NULL, loaded, 1)) app_close(a), a->file_menu = 0;
    igSeparator();
    igTextColored(col4(C_STONE), "%s", T(a, "nav.links"));
    if (menu_row(a, T(a, "links.addModel"), NULL, loaded, 1)) tool_run_action(a, "link_model"), a->file_menu = 0;
    if (menu_row(a, T(a, "links.addFolder"), NULL, loaded, 1)) tool_run_action(a, "link_folder"), a->file_menu = 0;
    igSeparator();
    igTextColored(col4(C_STONE), "%s", T(a, "filemenu.share"));
    menu_row(a, T(a, "filemenu.shareView"), NULL, loaded, 0);
    menu_row(a, T(a, "filemenu.ifc4"), NULL, loaded, 0);
    menu_row(a, T(a, "filemenu.ifc2x3"), NULL, loaded, 0);
    menu_row(a, T(a, "filemenu.print"), NULL, loaded, 0);
    igSeparator();
    igTextColored(col4(C_STONE), "%s", T(a, "filemenu.project"));
    menu_row(a, T(a, "filemenu.projectInfo"), NULL, loaded, 0);
    menu_row(a, T(a, "filemenu.history"), NULL, loaded, 0);
    if (menu_row(a, T(a, "filemenu.performance"), NULL, 1, 1)) a->dialog = DLG_PERF, a->file_menu = 0;
    if (menu_row(a, T(a, "filemenu.settings"), NULL, 1, 1)) tool_run_action(a, "settings"), a->file_menu = 0;
    if (menu_row(a, T(a, "native.keys"), NULL, 1, 1)) a->dialog = DLG_KEYS, a->file_menu = 0;
    if (menu_row(a, T(a, "native.openInLab"), NULL, 1, 1)) open_web_lab(a), a->file_menu = 0;
    menu_row(a, T(a, "filemenu.about"), NULL, 1, 0);
    if (menu_row(a, T(a, "filemenu.quit"), "Ctrl Q", 1, 1)) a->quit = 1;
    if (!igIsWindowHovered(ImGuiHoveredFlags_ChildWindows) && igIsMouseClicked_Bool(0, false) && igGetIO_Nil()->MousePos.y > 44 * s) a->file_menu = 0;
    igEnd();
    igPopStyleVar(2);
    igPopStyleColor(1);
}

void shell_top(App *a) {
    float s = a->scale, W = (float)a->lw;
    /* ---- the title row (QuickBar.vue): logo, File, undo, redo, the file's name and state, language, Save ---- */
    igSetNextWindowPos(V2(0, 0), 0, V2(0, 0));
    igSetNextWindowSize(V2(W, 44 * s), 0);
    igBegin("##top", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList *dl = igGetWindowDrawList();
    ImDrawList_AddCircleFilled(dl, V2(26 * s, 22 * s), 13 * s, C_CLAY, 0);
    ImDrawList_AddText_FontPtr(dl, a->f_disp, 17 * s, V2(21 * s, 11 * s), C_CREAM, "b", NULL, 0, NULL);
    igSetCursorScreenPos(V2(52 * s, 6 * s));
    if (text_button(a, "file", T(a, "filemenu.label"), 0, a->file_menu)) a->file_menu = !a->file_menu;
    igSameLine(0, 8 * s);
    if (text_button(a, "undo", T(a, "app.undo"), 0, 0)) tool_run_action(a, "undo");
    igSameLine(0, 2 * s);
    if (text_button(a, "redo", T(a, "app.redo"), 0, 0)) tool_run_action(a, "redo");
    const char *file = js(a->state.root, "file");
    char title[512];
    snprintf(title, sizeof title, "%s", *file ? file : T(a, "app.noFile"));
    ImVec2 ts = igCalcTextSize(title, NULL, false, -1);
    float tx = (W - ts.x) / 2;
    ImDrawList_AddText_FontPtr(dl, a->f_semi, 14 * s, V2(tx, 13 * s), C_INK, title, NULL, 0, NULL);
    int unsaved = jb(a->state.root, "unsaved"), editable = jb(a->state.root, "editable");
    const char *chip = !editable ? T(a, "na.chip.readOnly") : unsaved ? T(a, "app.notSaved") : T(a, "app.saved");
    ImVec2 cs = igCalcTextSize(chip, NULL, false, -1);
    float cx = tx + ts.x + 10 * s;
    ImDrawList_AddRectFilled(dl, V2(cx, 11 * s), V2(cx + cs.x + 16 * s, 33 * s), unsaved ? C_OCHRE : C_SUNK, 11 * s, 0);
    ImDrawList_AddText_Vec2(dl, V2(cx + 8 * s, 13 * s), C_INK, chip, NULL);
    igSetCursorScreenPos(V2(W - 90 * s, 7 * s));
    if (text_button(a, "save", T(a, "quick.saveButton"), unsaved || !editable ? 1 : 0, 0)) tool_run_action(a, "save"); /* clay only with something to save */
    igEnd();

    /* ---- the tab bar (TabsBar.vue): three groups split by a line ---- */
    igSetNextWindowPos(V2(0, 44 * s), 0, V2(0, 0));
    igSetNextWindowSize(V2(W, 36 * s), 0);
    igBegin("##tabs", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    dl = igGetWindowDrawList();
    igSetCursorScreenPos(V2(12 * s, 46 * s));
    for (int t = 0; t < a->ntabs; t++) {
        if (t > 0 && strcmp(a->tabs[t].group, a->tabs[t - 1].group)) {
            ImVec2 p = igGetCursorScreenPos();
            ImDrawList_AddLine(dl, V2(p.x + 4 * s, p.y + 7 * s), V2(p.x + 4 * s, p.y + 25 * s), C_LINE, 1 * s, 0);
            igDummy(V2(8 * s, 1)), igSameLine(0, 0);
        }
        char key[64], id[64];
        snprintf(key, sizeof key, "tab.%s", a->tabs[t].id);
        snprintf(id, sizeof id, "tab%d", t);
        if (text_button(a, id, T(a, key), 0, t == a->tab)) a->tab = t;
        igSameLine(0, 0);
    }
    igEnd();

    /* ---- the ribbon (Ribbon.vue): the groups of the tab, the tools, the group's name under them ---- */
    igSetNextWindowPos(V2(0, 80 * s), 0, V2(0, 0));
    igSetNextWindowSize(V2(W, 98 * s), 0);
    igBegin("##ribbon", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    dl = igGetWindowDrawList();
    ImDrawList_AddLine(dl, V2(0, 80 * s), V2(W, 80 * s), C_LINE, 1 * s, 0);
    ImDrawList_AddLine(dl, V2(0, 178 * s - 1), V2(W, 178 * s - 1), C_LINE, 1 * s, 0);
    const RTab *tab = &a->tabs[a->tab];
    float x = 12 * s;
    if (!tab->ngroups) ImDrawList_AddText_Vec2(dl, V2(x, 120 * s), C_STONE, T(a, "na.tabTitle"), NULL);
    for (int g = 0; g < tab->ngroups; g++) {
        const RGroup *G = &tab->groups[g];
        float gx = x;
        igSetCursorScreenPos(V2(x, 88 * s));
        for (int k = 0; k < G->ntools; k++) ribbon_tool(a, &G->tools[k]);
        if (G->has_box) { /* a select or a readout: not built natively, shown as the lab's frame */
            ImVec2 p = igGetCursorScreenPos();
            ImDrawList_AddRect(dl, V2(p.x + 4 * s, p.y + 22 * s), V2(p.x + 154 * s, p.y + 52 * s), C_LINE, 8 * s, 1 * s, 0);
            ImDrawList_AddText_FontPtr(dl, a->f_text, 12 * s, V2(p.x + 12 * s, p.y + 29 * s), C_MUTED, T(a, "native.notYetShort"), NULL, 0, NULL);
            igDummy(V2(160 * s, 60 * s)), igSameLine(0, 0);
        }
        float gw = igGetCursorScreenPos().x - gx;
        const char *gl = T(a, G->key);
        ImVec2 ls = igCalcTextSize(gl, NULL, false, -1);
        ImDrawList_AddText_FontPtr(dl, a->f_text, 12 * s, V2(gx + (gw - ls.x * 12 / 14) / 2, 156 * s), C_STONE, gl, NULL, 0, NULL);
        x = gx + gw + 8 * s;
        ImDrawList_AddLine(dl, V2(x - 4 * s, 90 * s), V2(x - 4 * s, 170 * s), C_LINE, 1 * s, 0);
        x += 4 * s;
    }
    igEnd();
    file_menu(a);
}

void shell_status(App *a) {
    float s = a->scale, W = (float)a->lw, y = a->lh - 32 * s;
    igSetNextWindowPos(V2(0, y), 0, V2(0, 0));
    igSetNextWindowSize(V2(W, 32 * s), 0);
    igBegin("##status", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList *dl = igGetWindowDrawList();
    ImDrawList_AddLine(dl, V2(0, y), V2(W, y), C_LINE, 1 * s, 0);
    char buf[400];
    const char *hint = a->measure_on ? T(a, "action.measure") : a->cam.ortho ? T(a, "status.hintPlan") : T(a, "status.hint");
    if (a->sel_link[0] && a->selected) hint = Tf(a, buf, sizeof buf, "status.linked", "{name}", a->sel_link);
    if (a->notice[0] && app_now() - a->notice_t < 6000) hint = a->notice;
    { /* NUI2: links held simplified by the GPU budget, said, with one click to raise it (the owner looked at coarse
       * stand-ins for 7 links and nothing told him) */
        int held = 0;
        for (int i = 0; i < a->nlinks; i++) held += a->links_s[i].detail_state == 4;
        int tfull = nv_render_get_stats(a->r).budget_full;
        if ((held || tfull) && a->gpu_budget && !(a->notice[0] && app_now() - a->notice_t < 6000)) {
            static char bm[300];
            unsigned mb = (unsigned)(a->gpu_budget >> 20), up = mb * 2 > 65535 ? 65535 : mb * 2;
            if (held) snprintf(bm, sizeof bm, a->lang_en ? "%d links simplified: GPU budget %u MB reached - click to raise it to %u MB" : "%d liens simplifiés : budget graphique de %u Mo atteint - cliquer pour le porter à %u Mo", held, mb, up);
            else snprintf(bm, sizeof bm, a->lang_en ? "Far parts simplified: GPU budget %u MB reached - click to raise it to %u MB" : "Parties lointaines simplifiées : budget graphique de %u Mo atteint - cliquer pour le porter à %u Mo", mb, up);
            hint = bm;
            ImVec2 hs = igCalcTextSize(bm, NULL, false, -1);
            igSetCursorScreenPos(V2(12 * s, y + 4 * s));
            if (igInvisibleButton("budget", V2(hs.x * s + 8, 24 * s), 0)) {
                char q[16];
                snprintf(q, sizeof q, "%u", up);
                char b2[64];
                snprintf(b2, sizeof b2, "{\"link_gpu_mb\":%s}", q);
                core_action(a, "set_settings", b2);
                core_query(a, "settings", NULL, &a->settings);
                nset_set("link_gpu_mb", q, 0);
                optimisations_apply(a);
                for (int i = 0; i < a->nlinks; i++) if (a->links_s[i].detail_state == 4) a->links_s[i].detail_state = 0;
                slog("GPU budget raised to %s MB from the status bar", q);
            }
        }
    }
    ImDrawList_AddText_FontPtr(dl, a->f_text, 13 * s, V2(12 * s, y + 8 * s), C_INK, hint, NULL, 0, NULL);
    /* the counts (StatusBar.vue), and the native frame's own numbers */
    nv_render_stats st = nv_render_get_stats(a->r);
    uint32_t nel = 0;
    for (uint32_t k = 0; k < nv_render_nparts(a->r); k++) nel += nv_part_scene(nv_render_part(a->r, k))->nelements;
    int loading = 0;
    for (int i = 0; i < a->nlinks; i++) loading += a->links_s[i].detail_state != 2 && a->links_s[i].detail_state != 3;
    char perf[300], ld[64] = "";
    if (loading || a->links_busy) snprintf(ld, sizeof ld, " · %s %d", T(a, "native.loading"), loading);
    snprintf(perf, sizeof perf, "%u el · 3D %.1f ms · %.2f M tri · %u draws · GPU %.0f MB%s · %s", nel, a->last3d_ms, st.tris / 1e6, st.draws,
             st.gpu_bytes / 1e6, ld, a->moving ? T(a, "perf.moving") : T(a, "status.idle"));
    igPushFont(a->f_mono, 12.5f);
    ImVec2 ps = igCalcTextSize(perf, NULL, false, -1);
    igPopFont();
    ps.x *= s;
    /* FR | EN (StatusBar.vue's language switch), right */
    float lx = W - 84 * s;
    for (int k = 0; k < 2; k++) {
        float bx = lx + k * 38 * s;
        igSetCursorScreenPos(V2(bx, y + 4 * s));
        igPushID_Int(k);
        if (igInvisibleButton("lang", V2(36 * s, 24 * s), 0)) a->lang_en = k, a->strings = a->lang_en ? a->strings_en : a->strings_fr;
        igPopID();
        int on = a->lang_en == k;
        if (on) ImDrawList_AddRectFilled(dl, V2(bx, y + 4 * s), V2(bx + 36 * s, y + 28 * s), C_INK, 12 * s, 0);
        ImDrawList_AddText_FontPtr(dl, a->f_text, 13 * s, V2(bx + 10 * s, y + 8 * s), on ? C_CREAM : C_INK, k ? "EN" : "FR", NULL, 0, NULL);
    }
    /* Performance (StatusBar.vue's button: the panel) */
    const char *pl = T(a, "perf.open");
    ImVec2 pls = igCalcTextSize(pl, NULL, false, -1);
    float px0 = lx - pls.x * s - 28 * s;
    igSetCursorScreenPos(V2(px0, y + 4 * s));
    if (igInvisibleButton("perfbtn", V2(pls.x * s + 16 * s, 24 * s), 0)) a->dialog = DLG_PERF;
    ImDrawList_AddText_FontPtr(dl, a->f_text, 13 * s, V2(px0 + 8 * s, y + 8 * s), igIsItemHovered(0) ? C_INK : C_STONE, pl, NULL, 0, NULL);
    ImDrawList_AddText_FontPtr(dl, a->f_mono, 12.5f * s, V2(px0 - ps.x - 12 * s, y + 9 * s), C_INK, perf, NULL, 0, NULL);
    if (a->selected) { /* the selection's chip */
        char sel[64];
        snprintf(sel, sizeof sel, "%s", T(a, "status.selectedOne"));
        ImVec2 ss = igCalcTextSize(sel, NULL, false, -1);
        float cx0 = px0 - ps.x - 24 * s - ss.x * s - 16 * s;
        ImDrawList_AddRectFilled(dl, V2(cx0, y + 5 * s), V2(cx0 + ss.x * s + 16 * s, y + 27 * s), C_PINK, 11 * s, 0);
        ImDrawList_AddText_FontPtr(dl, a->f_text, 13 * s, V2(cx0 + 8 * s, y + 8 * s), C_INK, sel, NULL, 0, NULL);
    }
    igEnd();
}

/* ---- the right-click menu (ContextMenu.vue, contextItems.ts: hide, isolate, show all, show previous,
 * box to selection), each row enabled by `query actions` ---- */
void shell_context_menu(App *a) {
    if (!a->ctx_menu) return;
    float s = a->scale;
    igSetNextWindowPos(V2(a->ctx_x, a->ctx_y), 0, V2(0, 0));
    igPushStyleColor_U32(ImGuiCol_WindowBg, C_SHEET);
    igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 8 * s);
    igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 1);
    igBegin("##ctx", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    igTextColored(col4(C_STONE), "%s", T(a, "ctx.menu"));
    static const char *const ROWS[][2] = {{"hide_now", "ctx.hide"}, {"isolate_now", "ctx.isolate"}, {"show_all", "ctx.showAll"},
                                          {"show_previous", "ctx.showPrevious"}, {"box_to_selection", "ctx.boxToSelection"}};
    for (int i = 0; i < 5; i++) {
        const JrVal *row = action_row(a, ROWS[i][0]);
        if (igMenuItem_Bool(T(a, ROWS[i][1]), js(row, "shortcut"), false, row && jb(row, "enabled"))) {
            tool_run_action(a, ROWS[i][0]);
            a->ctx_menu = 0;
        }
    }
    if (!igIsWindowHovered(0) && (igIsMouseClicked_Bool(0, false) || igIsMouseClicked_Bool(1, false))) a->ctx_menu = 0;
    igEnd();
    igPopStyleVar(2);
    igPopStyleColor(1);
}
