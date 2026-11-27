/* The native app's core calls (ticket NUI1): the session in-process (cc_call, as the host's POST
 * /<op>/<name> and the page's bimble.ts `call`), each under the session's lock (loader.h), the JSON
 * answers, and the data shared with the web UI (data/ JSON files, embedded: embed.h). */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "embed.h"
#include "calls.h"
#include "links.h"
#include "machine.h"

int core_call(App *a, const char *op, const char *name, const char *body, Buf *out) {
    if (!body) body = "";
    buf_clear(out);
    nv_ses_wr(a->loader);
    int st = cc_call(a->ses, op, name ? name : "", body, strlen(body), out);
    nv_ses_unwr(a->loader);
    if (strcmp(op, "query")) a->core_rev++; /* anything but a query may change what the queries answer */
    return st;
}

void json_free(Json *j) {
    if (j->root || j->arena.head) arena_free(&j->arena);
    memset(j, 0, sizeof *j);
}

int json_parse(Json *j, const char *text, size_t n) {
    json_free(j);
    arena_init(&j->arena, 0);
    JrError e;
    j->root = jr_parse(&j->arena, text, n, &e);
    if (!j->root) snprintf(j->err, sizeof j->err, "%s", e.msg);
    return j->root ? 0 : -1;
}

int core_query(App *a, const char *name, const char *body, Json *j) {
    Buf out;
    buf_init(&out);
    int st = core_call(a, "query", name, body, &out);
    int rc = -1;
    if (st == CC_OK) rc = json_parse(j, out.p ? out.p : "null", out.len ? out.len : 4);
    else {
        json_free(j);
        snprintf(j->err, sizeof j->err, "%.*s", (int)(out.len < 190 ? out.len : 190), out.p ? out.p : "");
    }
    buf_free(&out);
    return rc;
}

void json_str(Buf *b, const char *s) {
    buf_putc(b, '"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') buf_putc(b, '\\'), buf_putc(b, (char)c);
        else if (c < 0x20) {
            char e[8];
            snprintf(e, sizeof e, "\\u%04x", c);
            buf_puts(b, e);
        } else buf_putc(b, (char)c);
    }
    buf_putc(b, '"');
}

const char *js(const JrVal *v, const char *key) {
    const JrVal *x = v && v->type == JR_OBJ ? jr_get(v, key) : NULL;
    return x && x->type == JR_STR ? x->s : "";
}
double jnum(const JrVal *v, const char *key, double dflt) {
    const JrVal *x = v && v->type == JR_OBJ ? jr_get(v, key) : NULL;
    return x && x->type == JR_NUM ? x->num : dflt;
}
int jb(const JrVal *v, const char *key) {
    const JrVal *x = v && v->type == JR_OBJ ? jr_get(v, key) : NULL;
    return x && x->type == JR_BOOL && x->b;
}

const JrVal *action_row(App *a, const char *name) {
    const JrVal *r = a->actions.root;
    if (!r || r->type != JR_ARR || !name) return NULL;
    for (size_t k = 0; k < r->n; k++)
        if (!strcmp(js(&r->items[k], "name"), name)) return &r->items[k];
    return NULL;
}

int cap_has(App *a, const char *name) {
    const JrVal *c = a->caps.root;
    if (!c) return 0; /* no answer: nothing is served (the page's rule without a core) */
    return jb(c, name);
}

/* capability.ts notYetAction: no action, or an action of a capability the core does not serve. */
int action_not_yet(App *a, const char *name) {
    if (!name || !*name) return 1;
    const char *c = js(a->action_cap, name);
    return *c && !cap_has(a, c);
}

/* The pages and dialogs the native shell has built (the rest: "not yet (native)", with "Open in the
 * web lab"). A page or dialog name of ribbon.json / machine.json. */
static const char *const NATIVE_BUILT[] = {"quantities", "settings", "performance", "links", "link_model", "manage_links", "keys", NULL};
int native_not_yet(const char *what) {
    if (!what) return 0;
    for (int i = 0; NATIVE_BUILT[i]; i++) if (!strcmp(NATIVE_BUILT[i], what)) return 0;
    return 1;
}

/* The session's own work: a finished conversion taken, the next started, decoded link meshes past the
 * budget dropped (the host's poll loop does the same every 100 ms). */
void core_poll(App *a) {
    if (a->screen_kind != SCR_MAIN) return;
    nv_ses_wr(a->loader);
    int busy = cc_links_wants_tick(a->ses), changed = busy && cc_links_tick(a->ses);
    a->links_busy = cc_links_busy(a->ses);
    nv_ses_unwr(a->loader);
    if (changed) a->core_rev++;
    double now = app_now();
    if (a->links_busy && now - a->links_polled > 250) { /* the panel's progress (the page polls links_panel so) */
        a->links_polled = now;
        core_query(a, "links_panel", NULL, &a->links);
    }
}

void core_refresh(App *a) {
    if (a->asked_rev == a->core_rev || a->screen_kind != SCR_MAIN) return;
    int rev = a->core_rev;
    core_query(a, "actions", NULL, &a->actions);
    core_query(a, "capabilities", NULL, &a->caps);
    core_query(a, "state", NULL, &a->state);
    core_query(a, "open_views", NULL, &a->views); /* the views the viewer shows: storeys' plans and ceilings, links' plans (M1LINKS) */
    core_query(a, "links_panel", NULL, &a->links);
    core_query(a, "status", NULL, &a->status);
    core_query(a, "link_notes", NULL, &a->notes);
    if (core_query(a, "mesh_key", NULL, &a->mesh_key) == 0) {
        int d = (int)jnum(a->mesh_key.root, "draw", 0), l = (int)jnum(a->mesh_key.root, "links", 0);
        a->mesh_draw_key = d;
        if (l != a->links_rev) a->links_rev = l;
    }
    a->links_synced = -1; /* any change: the links compared again (an eye, a colour, a move, a reload) */
    a->vis_dirty = 1;
    a->core_rev = a->asked_rev = rev; /* the queries above are not changes */
}

int core_action(App *a, const char *name, const char *body) {
    Buf out;
    buf_init(&out);
    int st = core_call(a, "action", name, body, &out);
    if (st != CC_OK) snprintf(a->notice, sizeof a->notice, "%s: %.*s", name, (int)(out.len < 300 ? out.len : 300), out.p ? out.p : "");
    a->notice_t = app_now();
    buf_free(&out);
    a->dirty = 1;
    return st;
}

/* The selection, through the core (select: a JSON array of GlobalIds; a link's: {"link","ids"}), and the
 * properties (query element, or element {id, link}: bimble.ts selectLinked, linkedElement). */
void core_select(App *a, uint32_t elem1) {
    a->selected = 0, a->sel_id[0] = 0, a->sel_link[0] = 0;
    Buf out, body;
    buf_init(&out), buf_init(&body);
    if (elem1 > NV_ELEM0 && scene_elem(a, elem1 - 1, a->sel_id, a->sel_link, sizeof a->sel_link)) {
        a->selected = elem1;
        if (!a->sel_link[0]) {
            buf_puts(&body, "[\""), buf_puts(&body, a->sel_id), buf_puts(&body, "\"]");
            core_call(a, "select", NULL, buf_cstr(&body), &out);
            buf_clear(&body);
            buf_puts(&body, "{\"id\":\""), buf_puts(&body, a->sel_id), buf_puts(&body, "\"}");
            if (core_query(a, "element", buf_cstr(&body), &a->sel_el) != 0) core_query(a, "scene_element", buf_cstr(&body), &a->sel_el);
        } else {
            buf_puts(&body, "{\"link\":"), json_str(&body, a->sel_link), buf_puts(&body, ",\"ids\":[\""), buf_puts(&body, a->sel_id),
                buf_puts(&body, "\"]}");
            core_call(a, "select", NULL, buf_cstr(&body), &out);
            buf_clear(&body);
            buf_puts(&body, "{\"id\":\""), buf_puts(&body, a->sel_id), buf_puts(&body, "\",\"link\":"), json_str(&body, a->sel_link),
                buf_puts(&body, "}");
            core_query(a, "element", buf_cstr(&body), &a->sel_el);
        }
    } else {
        core_call(a, "select", NULL, "[]", &out);
        json_free(&a->sel_el);
    }
    buf_free(&out), buf_free(&body);
    a->vis_dirty = 1;
    a->dirty = 1;
}

/* ---- the shared data (embedded) ---- */
static JrVal *load_json(App *a, const char *name) {
    const nui_blob *b = nui_blob_get(name);
    if (!b) { fprintf(stderr, "bimble: %s is not in this build\n", name); return NULL; }
    JrError e;
    JrVal *v = jr_parse(&a->data, (const char *)b->p, b->n, &e);
    if (!v) fprintf(stderr, "bimble: %s: %s at %zu\n", name, e.msg, e.offset);
    return v;
}

/* The native app's own strings (what the web UI has no key for), French and English. */
static const char *const NATIVE_STRINGS[][3] = {
    {"native.notYet", "Cette page n'est pas encore dans l'application native : elle est dans le labo web.", "This page is not in the native app yet: the web lab has it."},
    {"native.notYetShort", "pas encore (natif)", "not yet (native)"},
    {"native.tooltip", "{name} : pas encore dans l'application native (le labo web l'a : Fichier › Ouvrir dans le labo web)", "{name}: not yet in the native app (the web lab has it: File › Open in the web lab)"},
    {"native.openInLab", "Ouvrir dans le labo web", "Open in the web lab"},
    {"native.opening", "Ouverture de", "Opening"},
    {"native.loading", "chargement", "loading"},
    {"native.keys", "Raccourcis clavier", "Keyboard shortcuts"},
    {"native.notes", "Notes", "Notes"},
    {"native.unload", "Décharger", "Unload"},
    {"native.unpinned", "Libre", "Unpinned"},
    {"native.discard", "Le modèle a des modifications non enregistrées. Les abandonner ?", "The model has unsaved changes. Discard them?"},
    {"native.discardGo", "Abandonner les modifications", "Discard the changes"},
    {"native.optimisations", "Optimisations", "Optimisations"},
    {"native.edgesScreen", "Arêtes (à l'écran)", "Edges (screen space)"},
    {"native.cullAlways", "Tri des éléments sur la carte graphique : toujours actif (c'est ainsi que l'application native dessine)", "Culling on the graphics card: always on (how the native app draws)"},
    {"native.saveReport", "Enregistrer le rapport…", "Save report…"},
    {"native.savedTo", "Enregistré aussi dans", "Also saved to"},
    {"native.saveFailed", "Rapport non enregistré :", "Report not saved:"},
    {"native.sessionLog", "Journal de session local", "Local session log"},
    {"native.sessionLogText", "Chaque session écrit un journal sur cet ordinateur seulement (versions, carte graphique, tailles de fenêtre, fichiers ouverts et leurs temps, erreurs, une ligne de mesures par seconde, le rapport de performance) : rien ne quitte la machine. Les 20 dernières sessions sont gardées.", "Each session writes a log on this computer only (versions, graphics card, window sizes, files opened and their times, errors, one line of measures a second, the performance report): nothing leaves the machine. The last 20 sessions are kept."},
    {"native.sessionLogOff", "Pas de journal pour cette session (activé : dès la prochaine).", "No log for this session (turned on: from the next one)."},
    {"native.openFolder", "Ouvrir le dossier", "Open folder"},
    {"native.cube.top", "Dessus", "Top"},
    {"native.cube.bottom", "Dessous", "Bottom"},
    {"native.cube.front", "Avant", "Front"},
    {"native.cube.back", "Arrière", "Back"},
    {"native.cube.left", "Gauche", "Left"},
    {"native.cube.right", "Droite", "Right"},
};

const char *T(App *a, const char *key) {
    if (!strncmp(key, "native.", 7))
        for (size_t k = 0; k < sizeof NATIVE_STRINGS / sizeof *NATIVE_STRINGS; k++)
            if (!strcmp(NATIVE_STRINGS[k][0], key)) return NATIVE_STRINGS[k][a->lang_en ? 2 : 1];
    const char *s = js(a->strings, key);
    if (!*s && a->strings != a->strings_en) s = js(a->strings_en, key);
    return *s ? s : key;
}

/* T(key) with "{var}" replaced by `val`. */
const char *Tf(App *a, char *out, size_t n, const char *key, const char *var, const char *val) {
    const char *fmt = T(a, key), *at = strstr(fmt, var);
    if (!at) snprintf(out, n, "%s", fmt);
    else snprintf(out, n, "%.*s%s%s", (int)(at - fmt), fmt, val, at + strlen(var));
    return out;
}

int data_load(App *a) {
    arena_init(&a->data, 0);
    JrVal *rib = load_json(a, "ribbon.json");
    a->strings_fr = load_json(a, "strings.fr.json");
    a->strings_en = load_json(a, "strings.en.json");
    a->keys = load_json(a, "keys.json");
    a->machine = load_json(a, "machine.json");
    if (!rib || !a->strings_fr || !a->strings_en) return -1;
    a->strings = a->lang_en ? a->strings_en : a->strings_fr;
    a->action_cap = (JrVal *)jr_get(rib, "actionCapability");
    const JrVal *tabs = jr_get(rib, "tabs"), *ribbon = jr_get(rib, "ribbon");
    if (!tabs || tabs->type != JR_ARR || !ribbon || ribbon->type != JR_OBJ) return -1;
    a->ntabs = (int)tabs->n;
    a->tabs = arena_alloc_n(&a->data, tabs->n, sizeof(RTab), 8);
    for (size_t t = 0; t < tabs->n; t++) {
        RTab *tb = &a->tabs[t];
        tb->id = js(&tabs->items[t], "id"), tb->group = js(&tabs->items[t], "group");
        const JrVal *gs = jr_get(ribbon, tb->id);
        tb->ngroups = gs && gs->type == JR_ARR ? (int)gs->n : 0;
        tb->groups = arena_alloc_n(&a->data, tb->ngroups + 1, sizeof(RGroup), 8);
        for (int g = 0; g < tb->ngroups; g++) {
            const JrVal *G = &gs->items[g];
            RGroup *rg = &tb->groups[g];
            rg->key = js(G, "key");
            rg->has_box = jr_get(G, "box") != NULL;
            const JrVal *tl = jr_get(G, "tools");
            rg->ntools = tl && tl->type == JR_ARR ? (int)tl->n : 0;
            rg->tools = arena_alloc_n(&a->data, rg->ntools + 1, sizeof(RTool), 8);
            for (int k = 0; k < rg->ntools; k++) {
                const JrVal *X = &tl->items[k];
                RTool *rt = &rg->tools[k];
                const char *s;
                rt->key = js(X, "key");
                rt->action = *(s = js(X, "action")) ? s : NULL;
                rt->page = *(s = js(X, "page")) ? s : NULL;
                rt->dialog = *(s = js(X, "dialog")) ? s : NULL;
                rt->panel = *(s = js(X, "panel")) ? s : NULL;
                rt->icon = -1;
                const char *svg = js(X, "icon");
                if (*svg) { /* the icon atlas: each different SVG once */
                    int found = -1;
                    for (int i = 0; i < a->icon_n; i++) if (!strcmp(a->icon_src[i], svg)) found = i;
                    if (found < 0 && a->icon_n < 256) {
                        if (!a->icon_src) a->icon_src = calloc(256, sizeof *a->icon_src);
                        if (a->icon_src) a->icon_src[found = a->icon_n++] = svg;
                    }
                    rt->icon = found;
                }
            }
        }
    }
    for (int t = 0; t < a->ntabs; t++) if (!strcmp(a->tabs[t].id, "view")) a->tab = t; /* tabs.ts: activeTab "view" */
    return 0;
}

/* ---- the core's visibility, drawn natively: hide / isolate (query visibility_sets: the whole sets, by
 * GlobalId, of the open model and its links) and the section box (query visibility: scene mm) ---- */
void core_visibility(App *a) {
    if (!a->r) return;
    Json v = {0}, b = {0};
    nv_render_clear_states(a->r);
    uint32_t total = a->next_elem;
    if (core_query(a, "visibility_sets", NULL, &v) == 0 && total > NV_ELEM0) {
        uint8_t *mark = calloc(total, 1); /* 1 hidden, 2 isolated */
        const JrVal *now = jr_get(v.root, "now"), *hid = jr_get(v.root, "hidden_in_view"), *iso = jr_get(v.root, "isolated_in_view");
        const JrVal *byv = jr_get(v.root, "by_view");
        const JrVal *nids = now && now->type == JR_OBJ ? jr_get(now, "ids") : NULL;
        int isolate = now && now->type == JR_OBJ && jb(now, "isolate"), any_iso = 0, reveal = jb(v.root, "reveal");
        const JrVal *lists[4] = {hid, nids, iso, byv};
        for (int L = 0; L < 4 && mark && !reveal; L++) {
            const JrVal *x = lists[L];
            int is_iso = L == 2 || (L == 1 && isolate);
            for (size_t k = 0; x && x->type == JR_ARR && k < x->n; k++) {
                uint32_t e = x->items[k].type == JR_STR ? scene_find(a, x->items[k].s, x->items[k].n) : 0;
                if (e && e - 1 < total) mark[e - 1] |= is_iso ? 2 : 1;
                any_iso |= is_iso;
            }
        }
        for (uint32_t k = NV_ELEM0; mark && k < total; k++)
            if ((mark[k] & 1) || (any_iso && !(mark[k] & 2))) nv_render_set_state(a->r, k, 2);
        if (mark && a->selected > NV_ELEM0 && a->selected - 1 < total && (mark[a->selected - 1] & 1)) a->selected = 0;
        free(mark);
    }
    if (a->selected > NV_ELEM0) nv_render_set_state(a->r, a->selected - 1, 1);
    if (core_query(a, "visibility", NULL, &b) == 0) {
        const JrVal *sb = jr_get(b.root, "section_box"), *box = sb ? jr_get(sb, "box") : NULL;
        const JrVal *mn = box && box->type == JR_OBJ ? jr_get(box, "min") : NULL, *mx = box && box->type == JR_OBJ ? jr_get(box, "max") : NULL;
        double lo[3] = {0}, hi[3] = {0};
        int on = jb(sb, "on") && mn && mx && mn->type == JR_ARR && mx->type == JR_ARR && mn->n == 3 && mx->n == 3;
        for (int i = 0; on && i < 3; i++) lo[i] = mn->items[i].num, hi[i] = mx->items[i].num;
        /* the core's box is in the open scene's frame (its origin); ours may be a link's */
        double org[3];
        memcpy(org, a->core_origin, sizeof org);
        const JrVal *o = sb ? jr_get(sb, "origin_mm") : NULL;
        if (o && o->type == JR_ARR && o->n == 3) for (int i = 0; i < 3; i++) org[i] = o->items[i].num;
        for (int i = 0; on && i < 3; i++) lo[i] += org[i] - a->origin0[i], hi[i] += org[i] - a->origin0[i];
        if (a->view_name[0] && a->view_clip) nv_render_set_clip(a->r, 1, a->view_lo, a->view_hi); /* a plan or section: its own box */
        else nv_render_set_clip(a->r, on, lo, hi);
        a->box_on = on;
        memcpy(a->box_min, lo, sizeof lo), memcpy(a->box_max, hi, sizeof hi);
    }
    json_free(&v), json_free(&b);
    a->dirty = 1;
}

/* ---- the key table (data/keys.json = shell/keys.ts KEYS): one row per shortcut; a row runs only when
 * its action is enabled (`query actions`), never while typing in a field (keys.ts installKeys) ---- */
static int key_name_is(const char *name, SDL_Keycode k) {
    if (!strcmp(name, "delete")) return k == SDLK_DELETE;
    if (!strcmp(name, "escape")) return k == SDLK_ESCAPE;
    if (!strcmp(name, "enter")) return k == SDLK_RETURN;
    return strlen(name) == 1 && (SDL_Keycode)(unsigned char)name[0] == k; /* SDL: lower-case letters are their ASCII */
}

int core_key(App *a, SDL_Keycode k, SDL_Keymod mod, int typing) {
    const JrVal *K = a->keys;
    int ctrl = (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0, shift = (mod & SDL_KMOD_SHIFT) != 0;
    if (ctrl && k == 'o' && !typing) { fs_open(a, NULL, "open"); a->dialog = DLG_OPEN; return 1; } /* the File menu's own keys */
    if (ctrl && k == 's' && !typing && a->screen_kind == SCR_MAIN) { tool_run_action(a, "save"); return 1; }
    for (size_t i = 0; K && K->type == JR_ARR && i < K->n; i++) {
        const JrVal *r = &K->items[i];
        if (!key_name_is(js(r, "key"), k) || jb(r, "ctrl") != ctrl || jb(r, "shift") != shift) continue;
        if (typing && !jb(r, "typing")) return 0;
        const char *ctx = js(r, "context"), *action = js(r, "action"), *effect = js(r, "effect");
        if (a->screen_kind != SCR_MAIN) return 0;
        if (strcmp(ctx, "anywhere") && a->show != SHOW_VIEW) return 0;
        if (!strcmp(ctx, "2d")) return 0; /* the 2D drawing views are the web lab's (not yet native) */
        if (!strcmp(effect, "escape")) {
            if (a->dialog) a->dialog = DLG_NONE;
            else if (a->ctx_menu || a->file_menu) a->ctx_menu = 0, a->file_menu = 0;
            else if (a->links_panel) a->links_panel = 0;
            else if (a->measure_on) { Buf o; buf_init(&o); core_call(a, "tool", "measure_cancel", "", &o); buf_free(&o); json_free(&a->measure); }
            else if (a->selected) core_select(a, 0);
            else if (a->show == SHOW_PAGE) a->show = SHOW_VIEW;
            return 1;
        }
        if (!strcmp(effect, "delete")) { /* keys.ts: the delete verb on the selection (an editable model) */
            Buf o;
            buf_init(&o);
            if (a->selected && !a->sel_link[0]) {
                int st = core_call(a, "verb", "delete", "{}", &o);
                if (st != CC_OK) snprintf(a->notice, sizeof a->notice, "%.*s", (int)(o.len < 300 ? o.len : 300), o.p ? o.p : ""), a->notice_t = app_now();
                else core_select(a, 0);
            }
            buf_free(&o);
            return 1;
        }
        if (!*action) return 1; /* the palette: not built natively */
        const JrVal *row = action_row(a, action);
        if (row && !jb(row, "enabled")) return 1;
        tool_run_action(a, action);
        return 1;
    }
    return 0;
}

/* ---- File: open, new, close: where the lab's shell machine goes (machine.c, machine.ts's table): the
 * unsaved question first (confirmClose: "Discard changes?"), else the open ---- */
static int shell_go(App *a, int ev) {
    int at = a->screen_kind == SCR_HOME ? SK_HOME : a->screen_kind == SCR_OPENING ? SK_OPENING : a->show == SHOW_PAGE ? SK_PAGE : SK_VIEW;
    ShellCore c = {a->screen_kind == SCR_MAIN && a->ses->open, a->screen_kind == SCR_MAIN && jb(a->state.root, "unsaved"), 1};
    return shell_next(at, SK_VIEW, ev, c, 1);
}
void app_open(App *a, const char *path) {
    if (shell_go(a, SE_OPEN) == SK_CONFIRM) {
        snprintf(a->pending_open, sizeof a->pending_open, "%s", path);
        a->dialog = DLG_DISCARD;
        return;
    }
    snprintf(a->model_path, sizeof a->model_path, "%s", path);
    a->screen_kind = SCR_OPENING;
    a->t_open = app_now();
    a->open_err[0] = 0;
}
void app_new(App *a) {
    if (shell_go(a, SE_NEW) == SK_CONFIRM) { snprintf(a->pending_open, sizeof a->pending_open, ":new"); a->dialog = DLG_DISCARD; return; }
    a->model_path[0] = 0;
    snprintf(a->model_path, sizeof a->model_path, ":new");
    a->screen_kind = SCR_OPENING;
    a->t_open = app_now();
}
void app_close(App *a) {
    int to = shell_go(a, SE_CLOSE);
    if (to == SK_REFUSED) return;
    if (to == SK_CONFIRM) { snprintf(a->pending_open, sizeof a->pending_open, ":close"); a->dialog = DLG_DISCARD; return; }
    scene_reset(a);
    Buf o;
    buf_init(&o);
    core_call(a, "action", "close", "{\"discard\":true}", &o);
    buf_free(&o);
    a->screen_kind = SCR_HOME;
    a->show = SHOW_VIEW;
    core_query(a, "recent_files", NULL, &a->recent);
}
