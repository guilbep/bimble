/* Prototype I (spike NAT2): one page, Quantities (pages/Quantities.vue): the saved lists
 * (`query schedules`) on the left, the chosen list's rows on the right (`query schedule_rows`
 * {name, offset, limit}: a window of the rows, as QTY1 asks them; the table virtualised by
 * ImGuiListClipper, so a list of 100,000 rows costs the rows on screen). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include "ui.h"

static Json lists, win;
static char current[256];
static int win_off = -1, win_n;

void page_quantities(App *a, float x, float y, float w, float h) {
    float s = a->scale;
    igSetNextWindowPos(V2(x, y), 0, V2(0, 0));
    igSetNextWindowSize(V2(w, h), 0);
    igPushStyleColor_U32(ImGuiCol_WindowBg, C_SHEET);
    igBegin("##quantities", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    igPushFont(a->f_disp, 20);
    igTextUnformatted(T(a, "na.screen.quantities"), NULL);
    igPopFont();
    if (!lists.root || a->asked_rev != a->core_rev) core_query(a, "schedules", NULL, &lists);
    const JrVal *L = lists.root;
    igBeginChild_Str("lists", V2(220 * s, 0), ImGuiChildFlags_Borders, 0);
    for (size_t k = 0; L && L->type == JR_ARR && k < L->n; k++) {
        const char *name = js(&L->items[k], "name");
        if (igSelectable_Bool(name, !strcmp(name, current), 0, V2(0, 0))) snprintf(current, sizeof current, "%s", name), win_off = -1;
    }
    if (!L || !L->n) igTextWrapped("%s", T(a, "qty1.new.all"));
    igEndChild();
    igSameLine(0, 12 * s);
    igBeginChild_Str("rows", V2(0, 0), 0, 0);
    if (!current[0] && L && L->type == JR_ARR && L->n) snprintf(current, sizeof current, "%s", js(&L->items[0], "name"));
    if (current[0]) {
        /* the first window tells the columns and the row count */
        if (win_off < 0) {
            char body[400];
            snprintf(body, sizeof body, "{\"name\":\"%s\",\"offset\":0,\"limit\":200}", current);
            core_query(a, "schedule_rows", body, &win);
            win_off = 0, win_n = 200;
        }
        const JrVal *W = win.root, *cols = jr_get(W, "columns"), *groups = jr_get(W, "groups");
        int ncol = cols && cols->type == JR_ARR ? (int)cols->n : 0;
        int nrows = (int)jnum(W, "row_count", jnum(W, "count", 0));
        igTextColored(col4(C_STONE), "%s · %d elements", current, (int)jnum(W, "count", nrows));
        if (ncol > 0 && ncol < 64 && igBeginTable("q", ncol, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, V2(0, 0), 0)) {
            for (int c = 0; c < ncol; c++) {
                char key[160];
                snprintf(key, sizeof key, "quantities.col.%s", cols->items[c].s);
                igTableSetupColumn(T(a, key), 0, 0, 0);
            }
            igTableSetupScrollFreeze(0, 1);
            igTableHeadersRow();
            /* the lines of the window: each group's rows, then its subtotal (semibold), the total last;
             * one flat list, virtualised by the clipper */
            const JrVal *lines[4096];
            int bold[4096], nl = 0;
            for (size_t g = 0; groups && groups->type == JR_ARR && g < groups->n && nl < 4090; g++) {
                const JrVal *R = jr_get(&groups->items[g], "rows"), *sub = jr_get(&groups->items[g], "subtotal");
                for (size_t r = 0; R && R->type == JR_ARR && r < R->n && nl < 4090; r++) lines[nl] = &R->items[r], bold[nl++] = 0;
                if (sub && sub->type == JR_ARR) lines[nl] = sub, bold[nl++] = 1;
            }
            const JrVal *tot = jr_get(W, "total");
            if (tot && tot->type == JR_ARR) lines[nl] = tot, bold[nl++] = 2;
            ImGuiListClipper *clip = ImGuiListClipper_ImGuiListClipper();
            ImGuiListClipper_Begin(clip, nl, -1);
            while (ImGuiListClipper_Step(clip))
                for (int i = clip->DisplayStart; i < clip->DisplayEnd; i++) {
                    igTableNextRow(0, 0);
                    if (bold[i]) igPushFont(a->f_semi, 0);
                    if (bold[i] == 2) igTableSetBgColor(ImGuiTableBgTarget_RowBg0, C_SUNK, -1);
                    for (int c = 0; c < ncol && (size_t)c < lines[i]->n; c++) {
                        igTableNextColumn();
                        const JrVal *x = &lines[i]->items[c];
                        igTextUnformatted(x->type == JR_STR ? x->s : "", NULL);
                    }
                    if (bold[i]) igPopFont();
                }
            ImGuiListClipper_destroy(clip);
            igEndTable();
        }
    }
    igEndChild();
    igEnd();
    igPopStyleColor(1);
}
