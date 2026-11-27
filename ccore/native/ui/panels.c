/* The native app's panels (ticket NUI1): the model navigator (Navigator.vue: views, links with their
 * eyes, a link's objects tree, notes), the properties of the selected element (Properties.vue: `element`,
 * or `element {id, link}` for a linked one, with its property sets read lazily from the link's cache),
 * and the Links panel (LinksPanel.vue: each link's state and progress, eye, pin, colour, positioning,
 * Reload, Unload, Report; Link IFC… and Link a folder… through the file dialog). Every row is a query. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "calls.h"

/* One tree row: a TreeNode whose label is drawn by us (the count right-aligned in DM Mono). */
static int tree_row(App *a, const char *id, const char *label, const char *count, int leaf, int open_default) {
    char lab[300];
    snprintf(lab, sizeof lab, "##%s", id);
    ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_SpanAvailWidth | (leaf ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen : 0) |
                           (open_default ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImVec2 p = igGetCursorScreenPos();
    float right = p.x + igGetContentRegionAvail().x;
    int open = igTreeNodeEx_Str(lab, f);
    float s = a->scale, x = p.x + igGetTreeNodeToLabelSpacing();
    ImDrawList *dl = igGetWindowDrawList();
    ImDrawList_AddText_FontPtr(dl, leaf ? a->f_text : a->f_semi, 14 * s, V2(x, p.y + 1 * s), C_INK, label, NULL, right - x - 50 * s, NULL);
    if (count && *count) {
        ImVec2 cs = igCalcTextSize(count, NULL, false, -1);
        ImDrawList_AddText_FontPtr(dl, a->f_mono, 13 * s, V2(right - cs.x * 13 / 14, p.y + 2 * s), C_STONE, count, NULL, 0, NULL);
    }
    return open && !leaf;
}

static void link_op(App *a, const char *op, const char *name, const char *extra) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"op\":"), json_str(&b, op), buf_puts(&b, ",\"name\":"), json_str(&b, name);
    if (extra) buf_putc(&b, ','), buf_puts(&b, extra);
    buf_putc(&b, '}');
    core_action(a, "manage_links", buf_cstr(&b));
    buf_free(&b);
    a->links_synced = -1;
}

static int eye(App *a, const char *id, int on) {
    float s = a->scale;
    igPushID_Str(id);
    ImVec2 ep = igGetCursorScreenPos();
    int hit = igInvisibleButton("##eye", V2(20 * s, 20 * s), 0);
    icon_draw(a, igGetWindowDrawList(), on ? icon_for(a, "reveal_hidden") : icon_for(a, "hide_now"), ep.x, ep.y, 20 * s, on ? C_INK : C_MUTED);
    if (igIsItemHovered(0)) igSetTooltip("%s", T(a, on ? "links.hide" : "links.show"));
    igPopID();
    return hit;
}

static void link_count(App *a, const JrVal *L, char *cnt, size_t n) {
    const char *st = js(L, "state");
    if (!strcmp(st, "converting")) snprintf(cnt, n, "%.0f %%", jnum(L, "progress", 0));
    else if (!strcmp(st, "ready")) {
        snprintf(cnt, n, "%.0f", jnum(L, "elements", 0));
        for (int i = 0; i < a->nlinks; i++) /* NUI1: proxies until its detail is drawn */
            if (!strcmp(a->links_s[i].name, js(L, "name")) && !a->links_s[i].detail && a->links_s[i].detail_state != 3 && a->links_s[i].nelem)
                snprintf(cnt, n, "%s", a->links_s[i].proxy ? T(a, "links.proxiesOnly") : T(a, "native.loading"));
    } else {
        char key[64];
        snprintf(key, sizeof key, "links.state.%s", st);
        snprintf(cnt, n, "%s", T(a, key));
    }
}

void panel_navigator(App *a, float x, float y, float w, float h) {
    float s = a->scale;
    igSetNextWindowPos(V2(x, y), 0, V2(0, 0));
    igSetNextWindowSize(V2(w, h), 0);
    igBegin("##nav", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList_AddLine(igGetWindowDrawList(), V2(x + w - 1, y), V2(x + w - 1, y + h), C_LINE, 1 * s, 0);
    igPushFont(a->f_disp, 15);
    igTextUnformatted(T(a, "nav.title"), NULL);
    igPopFont();
    igDummy(V2(1, 4 * s));
    char n[64];
    /* Views (query views): a click shows it (3D, a plan or a section: an orthographic camera) */
    const JrVal *v = a->views.root;
    if (v && v->type == JR_OBJ) v = jr_get(v, "views");
    if (tree_row(a, "views", T(a, "nav.views"), "", 0, 1)) {
        igPushID_Str("v3d");
        if (igSelectable_Bool(T(a, "view.three_d"), !a->view_name[0], 0, V2(0, 0))) view_set(a, NULL);
        igPopID();
        for (size_t k = 0; v && v->type == JR_ARR && k < v->n; k++) {
            const char *name = js(&v->items[k], "name"), *kind = js(&v->items[k], "kind");
            char lab[300];
            snprintf(lab, sizeof lab, "%s  ·  %s##view%zu", name, kind, k);
            if (igSelectable_Bool(lab, !strcmp(a->view_name, name), 0, V2(0, 0))) view_set(a, name);
        }
        igTreePop();
    }
    /* Links (query links_panel models), each with its eye */
    const JrVal *lm = a->links.root ? jr_get(a->links.root, "models") : NULL;
    snprintf(n, sizeof n, "%zu", lm && lm->type == JR_ARR ? lm->n : 0);
    if (tree_row(a, "links", T(a, "nav.links"), n, 0, 1)) {
        for (size_t k = 0; lm && lm->type == JR_ARR && k < lm->n; k++) {
            const JrVal *L = &lm->items[k];
            const char *name = js(L, "name");
            char id[64], cnt[64];
            snprintf(id, sizeof id, "eye%zu", k);
            if (eye(a, id, jb(L, "visible"))) link_op(a, jb(L, "visible") ? "hide" : "show", name, NULL);
            igSameLine(0, 4 * s);
            link_count(a, L, cnt, sizeof cnt);
            snprintf(id, sizeof id, "link%zu", k);
            if (tree_row(a, id, name, cnt, 0, 0)) {
                Json t = {0}; /* a linked model's objects tree: query scene_tree {link} */
                Buf body;
                buf_init(&body);
                buf_puts(&body, "{\"link\":"), json_str(&body, name), buf_puts(&body, "}");
                if (core_query(a, "scene_tree", buf_cstr(&body), &t) == 0) {
                    const JrVal *ss = jr_get(t.root, "storeys");
                    for (size_t i = 0; ss && ss->type == JR_ARR && i < ss->n; i++) {
                        char sid[80], sc[32];
                        snprintf(sid, sizeof sid, "l%zus%zu", k, i);
                        snprintf(sc, sizeof sc, "%.0f", jnum(&ss->items[i], "count", 0));
                        if (tree_row(a, sid, *js(&ss->items[i], "name") ? js(&ss->items[i], "name") : "—", sc, 0, 0)) {
                            const JrVal *cl = jr_get(&ss->items[i], "classes");
                            for (size_t c = 0; cl && cl->type == JR_ARR && c < cl->n; c++) {
                                snprintf(sid, sizeof sid, "l%zus%zuc%zu", k, i, c);
                                snprintf(sc, sizeof sc, "%.0f", jnum(&cl->items[c], "count", 0));
                                tree_row(a, sid, js(&cl->items[c], "class"), sc, 1, 0);
                            }
                            igTreePop();
                        }
                    }
                }
                buf_free(&body);
                json_free(&t);
                igTreePop();
            }
        }
        igTreePop();
    }
    /* Notes (query link_notes: M1LINKS) */
    const JrVal *nt = a->notes.root;
    if (nt && nt->type == JR_OBJ) nt = jr_get(nt, "notes");
    snprintf(n, sizeof n, "%zu", nt && nt->type == JR_ARR ? nt->n : 0);
    if (tree_row(a, "notes", T(a, "native.notes"), n, 0, 0)) {
        for (size_t k = 0; nt && nt->type == JR_ARR && k < nt->n; k++) {
            char id[32];
            snprintf(id, sizeof id, "note%zu", k);
            const char *txt = js(&nt->items[k], "text");
            tree_row(a, id, *txt ? txt : js(&nt->items[k], "title"), js(&nt->items[k], "link"), 1, 0);
        }
        igTreePop();
    }
    igEnd();
}

/* A JSON value as rows: scalars as "key  value", objects and arrays as tree nodes (depth-capped). */
static void json_rows(App *a, const char *key, const JrVal *v, int depth, int *row) {
    char id[32];
    snprintf(id, sizeof id, "r%d", (*row)++);
    if (!v || depth > 6 || *row > 3000) return;
    if (v->type == JR_OBJ || v->type == JR_ARR) {
        if (!v->n) return;
        char cnt[16];
        snprintf(cnt, sizeof cnt, "%zu", v->n);
        if (depth == 0 || tree_row(a, id, key, v->type == JR_ARR ? cnt : "", 0, depth < 2)) {
            for (size_t k = 0; k < v->n; k++) {
                char ik[24];
                if (v->type == JR_ARR) snprintf(ik, sizeof ik, "%zu", k + 1);
                const JrVal *x = v->type == JR_OBJ ? &v->members[k].val : &v->items[k];
                const char *xk = v->type == JR_OBJ ? v->members[k].key : ik;
                if (v->type == JR_ARR && x->type == JR_OBJ && *js(x, "name")) xk = js(x, "name"); /* a property set: its name */
                json_rows(a, xk, x, depth + 1, row);
            }
            if (depth) igTreePop();
        }
        return;
    }
    char val[256];
    if (v->type == JR_STR) snprintf(val, sizeof val, "%s", v->s);
    else if (v->type == JR_NUM) snprintf(val, sizeof val, "%.*s", (int)(v->n < 60 ? v->n : 60), v->s);
    else if (v->type == JR_BOOL) snprintf(val, sizeof val, "%s", v->b ? "true" : "false");
    else snprintf(val, sizeof val, "—");
    float s = a->scale;
    ImVec2 p = igGetCursorScreenPos();
    float w = igGetContentRegionAvail().x;
    ImDrawList *dl = igGetWindowDrawList();
    ImDrawList_AddText_FontPtr(dl, a->f_text, 13.5f * s, p, C_STONE, key, NULL, w * 0.4f, NULL);
    ImDrawList_AddText_FontPtr(dl, v->type == JR_NUM ? a->f_mono : a->f_text, 13.5f * s, V2(p.x + w * 0.42f, p.y), C_INK, val, NULL, w * 0.58f, NULL);
    ImVec2 vs = igCalcTextSize(val, NULL, false, w * 0.58f);
    igDummy(V2(w, (vs.y > 18 * s ? vs.y : 18 * s) + 2 * s));
}

void panel_properties(App *a, float x, float y, float w, float h) {
    float s = a->scale;
    igSetNextWindowPos(V2(x, y), 0, V2(0, 0));
    igSetNextWindowSize(V2(w, h), 0);
    igBegin("##props", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList_AddLine(igGetWindowDrawList(), V2(x, y), V2(x, y + h), C_LINE, 1 * s, 0);
    igPushFont(a->f_disp, 15);
    igTextUnformatted(T(a, "props.title"), NULL);
    igPopFont();
    igDummy(V2(1, 4 * s));
    char t[400];
    if (a->sel_link[0]) igTextColored(col4(C_SLATE), "%s", Tf(a, t, sizeof t, "props.linkedReadOnly", "{name}", a->sel_link));
    if (a->selected) igTextColored(col4(C_STONE), "%s", a->sel_id);
    if (!a->selected) igTextWrapped("%s", T(a, "props.empty"));
    else if (!a->sel_el.root) igTextWrapped("%s", a->sel_el.err);
    else {
        int row = 0;
        json_rows(a, "", a->sel_el.root, 0, &row);
    }
    igEnd();
}

/* ---- the Links panel ---- */
static const char *const ORIGINS[] = {"file", "origin", "base_point", "survey_point"};

void panel_links(App *a) {
    if (!a->links_panel) return;
    float s = a->scale;
    igSetNextWindowPos(V2(a->lw / 2.0f, a->lh / 2.0f), ImGuiCond_Appearing, V2(0.5f, 0.5f));
    igSetNextWindowSize(V2(760 * s, 520 * s), ImGuiCond_Appearing);
    igPushStyleColor_U32(ImGuiCol_WindowBg, C_SHEET);
    igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 16 * s);
    bool open = true;
    igBegin(T(a, "nav.links"), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    if (text_button(a, "addm", T(a, "links.addModel"), 1, 0)) tool_run_action(a, "link_model");
    igSameLine(0, 6 * s);
    if (text_button(a, "addf", T(a, "links.addFolder"), 0, 0)) tool_run_action(a, "link_folder");
    igSameLine(0, 6 * s);
    if (text_button(a, "rall", T(a, "links.reloadAll"), 0, 0)) core_action(a, "manage_links", "{\"op\":\"reload_all\"}"), a->links_synced = -1;
    const JrVal *lm = a->links.root ? jr_get(a->links.root, "models") : NULL;
    if (!lm || !lm->n) igTextUnformatted(T(a, "links.none"), NULL);
    static int report = -1;
    static Json log;
    if (lm && lm->n && igBeginTable("links", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY, V2(0, report >= 0 ? 220 * s : 0), 0)) {
        igTableSetupColumn("eye", ImGuiTableColumnFlags_WidthFixed, 24 * s, 0);
        igTableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0, 0);
        igTableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, 120 * s, 0);
        igTableSetupColumn("n", ImGuiTableColumnFlags_WidthFixed, 110 * s, 0);
        igTableSetupColumn("pos", ImGuiTableColumnFlags_WidthFixed, 150 * s, 0);
        igTableSetupColumn("ops", ImGuiTableColumnFlags_WidthFixed, 210 * s, 0);
        for (size_t k = 0; k < lm->n; k++) {
            const JrVal *L = &lm->items[k];
            const char *name = js(L, "name"), *st = js(L, "state");
            igPushID_Int((int)k);
            igTableNextRow(0, 0);
            igTableNextColumn();
            if (eye(a, "e", jb(L, "visible"))) link_op(a, jb(L, "visible") ? "hide" : "show", name, NULL);
            igTableNextColumn(), igTextUnformatted(name, NULL);
            if (igIsItemHovered(0)) igSetTooltip("%s\n%s", js(L, "path"), js(L, "message"));
            igTableNextColumn();
            if (!strcmp(st, "converting")) igProgressBar((float)jnum(L, "progress", 0) / 100, V2(-1, 0), NULL);
            else {
                char cnt[64];
                link_count(a, L, cnt, sizeof cnt);
                if (!strcmp(st, "ready")) igTextColored(col4(C_MOSS), "%s", jnum(L, "elements", 0) > 0 && strcmp(cnt, T(a, "native.loading")) && strcmp(cnt, T(a, "links.proxiesOnly")) ? T(a, "links.state.ready") : cnt);
                else igTextColored(col4(C_OCHRE), "%s", cnt);
            }
            igTableNextColumn(), igText("%.2f M tri", jnum(L, "triangles", 0) / 1e6);
            igTableNextColumn();
            { /* positioning (Revit's Link IFC options): the core refuses it while pinned */
                const char *org = js(L, "origin");
                char key[64];
                snprintf(key, sizeof key, "links.origin.%s", org);
                igSetNextItemWidth(-1);
                if (igBeginCombo("##o", T(a, key), 0)) {
                    for (int o = 0; o < 4; o++) {
                        snprintf(key, sizeof key, "links.origin.%s", ORIGINS[o]);
                        if (igSelectable_Bool(T(a, key), !strcmp(org, ORIGINS[o]), 0, V2(0, 0))) {
                            char ex[64];
                            snprintf(ex, sizeof ex, "\"origin\":\"%s\"", ORIGINS[o]);
                            link_op(a, "place", name, ex);
                        }
                    }
                    igEndCombo();
                }
            }
            igTableNextColumn();
            if (igSmallButton(jb(L, "pinned") ? T(a, "links.pinned") : T(a, "native.unpinned"))) link_op(a, jb(L, "pinned") ? "unpin" : "pin", name, NULL);
            if (igIsItemHovered(0)) igSetTooltip("%s", T(a, jb(L, "pinned") ? "links.pinnedTip" : "links.pinTip"));
            igSameLine(0, 4 * s);
            { /* colour: its own, or one of the 12 */
                const JrVal *c = jr_get(L, "colour");
                int ci = c && c->type == JR_NUM ? (int)c->num : -1;
                ImU32 cc = ci >= 0 ? (nv_link_colour(ci)) : C_LINE;
                ImVec2 p = igGetCursorScreenPos();
                if (igInvisibleButton("col", V2(18 * s, 18 * s), 0)) {
                    char ex[32];
                    int next = ci + 1 > 11 ? -1 : ci + 1;
                    if (next < 0) snprintf(ex, sizeof ex, "\"colour\":null");
                    else snprintf(ex, sizeof ex, "\"colour\":%d", next);
                    link_op(a, "colour", name, ex);
                }
                ImDrawList_AddRectFilled(igGetWindowDrawList(), p, V2(p.x + 18 * s, p.y + 18 * s), cc, 4 * s, 0);
                ImDrawList_AddRect(igGetWindowDrawList(), p, V2(p.x + 18 * s, p.y + 18 * s), C_STONE, 4 * s, 1, 0);
                if (igIsItemHovered(0)) igSetTooltip("%s", T(a, "links.colour"));
            }
            igSameLine(0, 4 * s);
            if (igSmallButton(T(a, "links.reload"))) link_op(a, "reload", name, NULL);
            igSameLine(0, 4 * s);
            if (!strcmp(st, "unloaded")) { if (igSmallButton(T(a, "links.load"))) link_op(a, "load", name, NULL); }
            else if (!strcmp(st, "converting") || !strcmp(st, "queued")) { if (igSmallButton(T(a, "links.cancel"))) link_op(a, "cancel", name, NULL); }
            else if (igSmallButton(T(a, "native.unload"))) link_op(a, "unload", name, NULL);
            igSameLine(0, 4 * s);
            if (igSmallButton(T(a, "links.log"))) {
                Buf b;
                buf_init(&b);
                buf_puts(&b, "{\"name\":"), json_str(&b, name), buf_puts(&b, "}");
                core_query(a, "link_log", buf_cstr(&b), &log);
                buf_free(&b);
                report = (int)k;
            }
            igSameLine(0, 4 * s);
            if (igSmallButton("×")) link_op(a, "remove", name, NULL);
            if (igIsItemHovered(0)) igSetTooltip("%s", T(a, "links.remove"));
            igPopID();
        }
        igEndTable();
    }
    if (report >= 0) {
        igSeparator();
        if (igSmallButton(T(a, "files.close"))) report = -1;
        if (igBeginChild_Str("log", V2(0, 0), ImGuiChildFlags_Borders, 0)) {
            igPushFont(a->f_mono, 12.5f);
            igTextUnformatted(log.root ? js(log.root, "text") : log.err, NULL);
            igPopFont();
        }
        igEndChild();
    }
    igEnd();
    igPopStyleVar(1);
    igPopStyleColor(1);
    if (!open) a->links_panel = 0;
}
