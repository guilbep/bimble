/* The native app's 3D scene (ticket NUI1): the open model as one renderer part, each link as its
 * proxies at once (its cache's HLOD: the first picture) then its detail (decoded by the loader, one
 * link per thread at a time, the largest on screen first), placed as the session places it. Every
 * read of the session holds its lock (loader.h). */
#define _GNU_SOURCE
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "links.h"
#include "mesh.h"
#include "xshell.h"

/* ---- the global element numbers: the open model's part, then each link part's, never reused while
 * the model is open (a link reloaded gets new numbers; the colour table grows by 4 bytes an element) ---- */
static uint32_t take_elems(App *a, uint32_t n) {
    uint32_t e = a->next_elem;
    a->next_elem += n;
    return e;
}

void scene_reset(App *a) {
    nv_loader_cancel(a->loader, 0);
    while (nv_loader_busy(a->loader)) SDL_Delay(1);
    for (nv_job *j; (j = nv_loader_take(a->loader));) nv_job_free(a->dev, j);
    for (int i = 0; i < a->nlinks; i++) {
        if (a->links_s[i].proxy) nv_render_remove(a->r, a->links_s[i].proxy);
        if (a->links_s[i].detail) nv_render_remove(a->r, a->links_s[i].detail);
        if (a->links_s[i].ts) nv_render_remove_tileset(a->r, a->links_s[i].ts), nv_tileset_unref(a->dev, a->links_s[i].ts);
        free(a->links_s[i].ids);
    }
    a->tile_jobs = 0;
    memset(a->links_s, 0, sizeof a->links_s);
    a->nlinks = 0;
    if (a->model_part) nv_render_remove(a->r, a->model_part), a->model_part = NULL;
    if (a->model_ts) nv_render_remove_tileset(a->r, a->model_ts), nv_tileset_unref(a->dev, a->model_ts), a->model_ts = NULL;
    a->model_tiles = 0, a->model_ident = NULL;
    free(a->model_ids), a->model_ids = NULL, a->model_nE = 0;
    a->have_origin = 0, a->origin_links = 0, a->next_elem = NV_ELEM0, a->draw_key = -1, a->fitted = 0;
    a->selected = 0;
    a->ids_dirty = 1;
    a->t_complete = 0, a->t_first_picture = 0;
    a->dirty = 1;
}

/* The open model's part again (its draw key changed: an edit, an open). Form 6: the open model's BMSH
 * version 3 and the links' heads only (no link decoded). */
static void model_part(App *a) {
    const char *p;
    size_t n;
    uint32_t rev;
    double bm;
    nv_ses_wr(a->loader);
    int st = a->ses->open ? cc_mesh_get_form(a->ses, CC_MESH_FORM_LAZY, &p, &n, &rev, &bm) : 404;
    nv_scene sc;
    if (st == 200 && n >= 52 && !a->have_origin && ((const uint32_t *)p)[3] > 0) { /* its origin, when it has elements */
        memcpy(a->origin0, p + 28, 24);
        a->have_origin = 1;
    }
    char err[256] = "";
    int ok = 0;
    if (st == 200) {
        nv_load_opts lo = {.min_copies = 4, .c = 1, .s = 0, .colour = -1};
        double org[3];
        memcpy(org, p + 28, 24);
        memcpy(a->core_origin, org, sizeof org);
        if (!a->have_origin) memcpy(a->origin0, org, sizeof org);
        for (int i = 0; i < 3; i++) lo.off[i] = org[i] - a->origin0[i];
        uint32_t nE = ((const uint32_t *)p)[3];
        nv_scene_init(&sc, take_elems(a, nE), a->origin0);
        ok = nv_load_bytes(&sc, p, n, &lo, err, sizeof err) == 0 && nv_finish(&sc) == 0;
        memcpy(a->model_off, lo.off, sizeof a->model_off);
    }
    const void *scene_now = a->ses->open || a->ses->view ? (const void *)cc_session_scene(a->ses) : NULL;
    if (st != 200) slog("the open model's mesh: %d %.*s", st, (int)(n < 200 ? n : 200), st != 404 && p ? p : "");
    else slog("the open model's mesh: %u elements", n >= 52 ? ((const uint32_t *)p)[3] : 0);
    free(a->model_ids), a->model_ids = NULL, a->model_nE = 0;
    const cc_scene *vs = st != 200 && a->use_tiles && scene_now ? cc_session_scene(a->ses) : NULL;
    if (vs && vs->nelements) { /* NUI2: a view file opened as it is (no mesh answer): drawn by tiles alone, its ids and colours here */
        uint32_t nE = (uint32_t)vs->nelements;
        uint32_t *col = malloc(nE * 4ull);
        a->model_ids = malloc(nE * 22ull);
        if (col && a->model_ids) {
            if (!a->have_origin) memcpy(a->origin0, vs->origin, sizeof a->origin0), a->have_origin = 1;
            memcpy(a->core_origin, vs->origin, sizeof a->core_origin);
            for (int i = 0; i < 3; i++) a->model_off[i] = vs->origin[i] - a->origin0[i];
            a->model_elem0 = take_elems(a, nE), a->model_nE = nE;
            for (uint32_t i = 0; i < nE; i++) {
                memcpy(a->model_ids[i], vs->elements[i].id.c, 22);
                col[i] = nv_category_colour((int)vs->elements[i].category < 8 ? (int)vs->elements[i].category : 7);
            }
            nv_render_add_colours(a->r, a->model_elem0, nE, col);
        } else free(a->model_ids), a->model_ids = NULL;
        free(col);
    }
    nv_ses_unwr(a->loader);
    if (a->model_part) nv_render_remove(a->r, a->model_part), a->model_part = NULL;
    if (a->model_ts) nv_render_remove_tileset(a->r, a->model_ts), nv_tileset_unref(a->dev, a->model_ts), a->model_ts = NULL;
    a->model_tiles = 0, a->model_ident = NULL;
    if (((ok && sc.nelements) || a->model_ids) && a->use_tiles && scene_now) { /* NUI2: the open model by tiles too: its pyramid built by a loader */
        nv_job *j = calloc(1, sizeof *j);
        if (j) {
            j->kind = NV_JOB_MODEL_TILES, j->ses = a->ses, j->uid = 0xFFFFFFFFu, j->ident = scene_now;
            a->model_tiles = 1, a->model_ident = scene_now;
            nv_loader_submit(a->loader, j);
        }
    }
    if (ok && sc.nelements) {
        a->model_part = nv_part_upload(a->dev, &sc, err, sizeof err);
        if (a->model_part) nv_render_add(a->r, a->model_part);
    } else if (st == 200) nv_scene_free(&sc);
    if (!ok && st == 200) snprintf(a->notice, sizeof a->notice, "%s", err), a->notice_t = app_now();
    a->ids_dirty = 1, a->dirty = 1;
}

/* NUI2: a link drawn by tiles: its cache read in place holds a section TILE */
static int can_tile(const cc_link *l) {
    const cc_viewfile *v = l->vf;
    return v && v->has_tiles && v->lazy && v->base && v->tiles_off <= v->base_len && v->tiles_len >= 48 && v->tiles_len <= v->base_len - v->tiles_off;
}
/* its tile set (under the session's lock): the table of contents and occluders copied, its elements' ids and colours */
static void tile_slot(App *a, SLink *L, const cc_link *l) {
    const cc_viewfile *v = l->vf;
    const uint8_t *sec = v->base + v->tiles_off;
    uint32_t toc, nocc;
    memcpy(&toc, sec + 24, 4), memcpy(&nocc, sec + 20, 4);
    if (toc < 48 || toc > v->tiles_len || (uint64_t)nocc * 36 > v->tiles_len - toc) { slog("link %s: its section TILE is damaged (the table's size)", L->name); return; }
    const cc_scene *rows = cc_link_rows(l);
    uint32_t nE = (uint32_t)rows->nelements;
    char err[256];
    uint32_t lc = L->colour >= 0 ? nv_link_colour(L->colour) : 0;
    nv_tileset *ts = nv_tileset_new(sec, toc, nE, nocc ? (const void *)(sec + toc) : NULL, nocc, L->elem0, L->c, L->s, L->off, lc, err, sizeof err);
    if (!ts) { slog("link %s: drawn as parts: %s", L->name, err); return; }
    uint32_t *col = malloc((nE + 1ull) * 4);
    L->ids = malloc((nE + 1ull) * 22);
    if (!col || !L->ids) { free(col), free(L->ids), L->ids = NULL; nv_tileset_unref(a->dev, ts); return; }
    for (uint32_t i = 0; i < nE; i++) {
        cc_link_id(l, i, L->ids[i]);
        int cat = (int)rows->elements[i].category < 8 ? (int)rows->elements[i].category : 7;
        col[i] = L->colour >= 0 ? nv_link_colour(L->colour) : nv_category_colour(cat);
    }
    nv_render_add_colours(a->r, L->elem0, nE, col);
    free(col);
    nv_tileset_set_shown(ts, L->visible);
    nv_render_add_tileset(a->r, ts);
    L->ts = ts, L->proxy_state = 2, L->detail_state = 2;
    a->ids_dirty = 1, a->dirty = 1, a->vis_dirty = 1;
    slog("link %s: drawn by tiles (%u tiles, %u elements, %u occluder triangles)", L->name, nv_tileset_toc(ts)->ntiles, nE, nocc);
}

/* The link slot of uid (-1 none). */
static int slot_of(App *a, uint32_t uid) {
    for (int i = 0; i < a->nlinks; i++) if (a->links_s[i].uid == uid) return i;
    return -1;
}
static void slot_drop(App *a, int i) {
    SLink *L = &a->links_s[i];
    nv_loader_cancel(a->loader, L->uid);
    if (L->proxy) nv_render_remove(a->r, L->proxy);
    if (L->detail) nv_render_remove(a->r, L->detail);
    if (L->ts) nv_render_remove_tileset(a->r, L->ts), nv_tileset_unref(a->dev, L->ts); /* a job of it still running holds its own reference */
    a->tile_jobs -= L->tiles_inflight;
    free(L->ids);
    memmove(&a->links_s[i], &a->links_s[i + 1], (size_t)(a->nlinks - i - 1) * sizeof *a->links_s);
    a->nlinks--;
    a->ids_dirty = 1, a->dirty = 1;
}

static void submit(App *a, SLink *L, int kind) {
    nv_job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = kind, j->ses = a->ses, j->uid = L->uid, j->ident = L->ident;
    j->elem0 = kind == NV_JOB_DETAIL ? L->elem0 : 0;
    memcpy(j->origin0, a->origin0, sizeof j->origin0);
    j->lo = (nv_load_opts){.min_copies = 4, .c = L->c, .s = L->s, .colour = L->colour};
    memcpy(j->lo.off, L->off, sizeof L->off);
    j->with_proxies = a->hlod;
    j->tag = (int)L->gen;
    if (kind == NV_JOB_PROXIES) L->proxy_state = 1;
    else L->detail_state = 1;
    nv_loader_submit(a->loader, j);
}

/* The session's links against the slots (under the lock): a new or changed link (another file read, a
 * move, a colour) gets new jobs; a link gone or not ready loses its parts. */
static void links_sync(App *a) {
    cc_session *s = a->ses;
    nv_ses_wr(a->loader);
    if (!a->have_origin || a->origin_links) { /* a project of links alone: the scene's origin from the links (NUI3) */
        /* the ready links' placed origins; the origin is the one with the most triangles (else elements) of ready links within 1 km of it
         * (a georeferenced project with one link in a local frame: the many, not the first ready; f32 positions step by
         * distance / 2^24: 150 mm at 2,500 km). When a better one comes later (more links ready) the scene moves to it
         * (every link re-placed, the camera shifted: the picture does not move). */
        double O[64][3], W[64];
        int no = 0;
        for (size_t i = 0; i < s->nlinks && no < 64; i++) {
            if (!nv_link_ident(s, (int)i)) continue;
            double c, sn, t[3];
            cc_link_placement(s, &s->links[i], &c, &sn, t);
            const double *o = cc_link_rows(&s->links[i])->origin;
            O[no][0] = c * o[0] - sn * o[1] + t[0], O[no][1] = sn * o[0] + c * o[1] + t[1], O[no][2] = o[2] + t[2];
            double tr = (double)cc_link_triangles(&s->links[i]), ne = (double)cc_link_rows(&s->links[i])->nelements;
            W[no++] = (tr > ne ? tr : ne) + 1; /* triangles, else elements (a link's triangles may be counted later) */
        }
        int best = -1;
        double bw = 0, cur = 0;
        for (int i = 0; i < no; i++) {
            double w = 0, wc = 0;
            for (int k = 0; k < no; k++) {
                double d0 = O[k][0] - O[i][0], d1 = O[k][1] - O[i][1], d2 = O[k][2] - O[i][2];
                if (d0 * d0 + d1 * d1 + d2 * d2 < 1e12) w += W[k];
                d0 = O[k][0] - a->origin0[0], d1 = O[k][1] - a->origin0[1], d2 = O[k][2] - a->origin0[2];
                if (d0 * d0 + d1 * d1 + d2 * d2 < 1e12) wc += W[k];
            }
            if (w > bw) bw = w, best = i;
            cur = wc;
        }
        if (getenv("BIMBLE_ORIGIN_DEBUG")) slog("origin: %d ready, best %d weight %.0f, here %.0f", no, best, bw, cur);
        if (best >= 0 && !a->have_origin) {
            memcpy(a->origin0, O[best], sizeof a->origin0), a->have_origin = 1, a->origin_links = 1;
            slog("the scene origin: a link's placed origin (%.0f, %.0f, %.0f) mm", a->origin0[0], a->origin0[1], a->origin0[2]);
        } else if (best >= 0 && bw > cur) { /* most of the model is elsewhere: move the scene's origin there */
            double d[3];
            for (int k = 0; k < 3; k++) d[k] = a->origin0[k] - O[best][k];
            slog("the scene origin moved by (%.0f, %.0f, %.0f) mm to where most triangles are", -d[0], -d[1], -d[2]);
            for (int i = a->nlinks - 1; i >= 0; i--) slot_drop(a, i);
            memcpy(a->origin0, O[best], sizeof a->origin0);
            for (int k = 0; k < 3; k++) a->cam.target[k] += d[k];
        }
    }
    for (int i = a->nlinks - 1; i >= 0; i--) { /* gone */
        int found = 0;
        for (size_t k = 0; k < s->nlinks; k++) if (s->links[k].uid == a->links_s[i].uid) found = 1;
        if (!found) slot_drop(a, i);
    }
    for (size_t k = 0; k < s->nlinks; k++) {
        cc_link *l = &s->links[k];
        const void *id = nv_link_ident(s, (int)k);
        int i = slot_of(a, l->uid);
        if (!id) { /* not ready (queued, converting, failed, unloaded, missing): nothing drawn */
            if (i >= 0) slot_drop(a, i);
            continue;
        }
        double c, sn, t[3];
        cc_link_placement(s, l, &c, &sn, t);
        const double *o = cc_link_rows(l)->origin;
        double off[3] = {c * o[0] - sn * o[1] + t[0] - a->origin0[0], sn * o[0] + c * o[1] + t[1] - a->origin0[1], o[2] + t[2] - a->origin0[2]};
        if (i >= 0) {
            SLink *L = &a->links_s[i];
            int same = L->ident == id && L->c == c && L->s == sn && !memcmp(L->off, off, sizeof off) && L->colour == l->colour &&
                       L->proxies_gen == l->proxies_gen && (L->ts != NULL) == (a->use_tiles && can_tile(l));
            L->visible = l->visible;
            if (same) continue;
            slot_drop(a, i);
        }
        if (a->nlinks >= (int)(sizeof a->links_s / sizeof *a->links_s)) continue;
        SLink *L = &a->links_s[a->nlinks++];
        memset(L, 0, sizeof *L);
        L->uid = l->uid, L->ident = id, L->c = c, L->s = sn, L->colour = l->colour, L->visible = l->visible;
        L->proxies_gen = l->proxies_gen;
        memcpy(L->off, off, sizeof off);
        if (fabs(off[0]) > 1e6 || fabs(off[1]) > 1e6 || fabs(off[2]) > 1e6) /* NUI3: f32 positions there step by off / 2^24 */
            slog("link %zu: %.0f km from the scene origin: its f32 positions step by %.0f mm", k, sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]) / 1e6,
                 sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]) / 16777216.0);
        snprintf(L->name, sizeof L->name, "%s", l->name ? l->name : "");
        L->nelem = (uint32_t)cc_link_rows(l)->nelements;
        L->elem0 = take_elems(a, L->nelem);
        L->tris = cc_link_triangles(l);
        L->gen = ++a->link_gen;
        L->has_box = 0;
        double lo[3], hi[3];
        if (cc_link_box_lazy(l, a->origin0, lo, hi)) { /* in the open scene's frame: its origin is ours when the model is empty */
            const double *so = a->ses->open ? NULL : NULL;
            (void)so;
            memcpy(L->lo, lo, sizeof lo), memcpy(L->hi, hi, sizeof hi), L->has_box = 1;
        }
        L->t_ready = app_now();
        if (a->use_tiles && can_tile(l)) tile_slot(a, L, l);
    }
    nv_ses_unwr(a->loader);
    for (int i = 0; i < a->nlinks; i++) {
        SLink *L = &a->links_s[i];
        if (!L->ts && !L->proxy_state && !L->detail_state) submit(a, L, NV_JOB_PROXIES); /* the proxies first: small, at once */
    }
}

/* The screen size of a link (its box projected), for the order of the detail jobs. */
static double screen_size(App *a, const SLink *L) {
    double lo[3], hi[3];
    if (L->proxy) {
        const nv_scene *s = nv_part_scene(L->proxy);
        memcpy(lo, s->bmin, sizeof lo), memcpy(hi, s->bmax, sizeof hi);
    } else if (L->has_box) memcpy(lo, L->lo, sizeof lo), memcpy(hi, L->hi, sizeof hi);
    else return 1; /* unknown: after the known ones */
    if (lo[0] > hi[0]) return 1;
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    int any = 0;
    for (int k = 0; k < 8; k++) {
        double p[3] = {k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2]}, q[2];
        if (!nv_cam_project(&a->cam, p, a->vw, a->vh, q)) return 1e12; /* around the eye: first */
        any = 1;
        x0 = fmin(x0, q[0]), y0 = fmin(y0, q[1]), x1 = fmax(x1, q[0]), y1 = fmax(y1, q[1]);
    }
    if (!any) return 1;
    x0 = fmax(x0, 0), y0 = fmax(y0, 0), x1 = fmin(x1, a->vw), y1 = fmin(y1, a->vh);
    double area = (x1 - x0) * (y1 - y0);
    return area > 0 ? 2 + area : 1.5; /* off screen: after the ones on it */
}

/* Each frame: finished jobs into the renderer, the next detail job to an idle loader. */
void scene_tick(App *a) {
    if (!a->r) return;
    int key = a->mesh_draw_key;
    if (key != a->draw_key) a->draw_key = key, model_part(a);
    if (a->links_rev != a->links_synced) a->links_synced = a->links_rev, links_sync(a);
    if (a->use_tiles && a->topts.priority && a->nlinks > 1 && app_now() - a->xshell_t > 500) { /* NUI3: the cross-link shell */
        a->xshell_t = app_now();
        nv_ses_wr(a->loader);
        int st = cc_xshell_poll(a->ses);
        if (st != a->xshell_state) slog("cross-link shell: %s", st == 2 ? "ready" : st == 1 ? "computing" : st == 0 ? "none" : "failed"), a->xshell_state = st;
        for (size_t k = 0; st == 2 && k < a->ses->nlinks; k++) {
            int i = slot_of(a, a->ses->links[k].uid);
            if (i < 0 || !a->links_s[i].ts) continue;
            uint32_t n = 0;
            const uint8_t *m = cc_xshell_marks(a->ses, &a->ses->links[k], &n);
            if (!m || m == a->links_s[i].xmarks) continue;
            if (nv_tileset_set_xmask(a->links_s[i].ts, m, n) == 0) {
                uint64_t ext = 0;
                for (uint32_t e = 0; e < n; e++) ext += m[e] != 0;
                slog("link %s: cross-link shell attached (%llu of %u elements exterior)", a->links_s[i].name, (unsigned long long)ext, n);
            }
            a->links_s[i].xmarks = m;
        }
        nv_ses_unwr(a->loader);
    }
    for (nv_job *j; (j = nv_loader_take(a->loader));) {
        int i = slot_of(a, j->uid);
        SLink *L = i >= 0 ? &a->links_s[i] : NULL;
        if (j->kind == NV_JOB_MODEL_TILES) { /* the open model's pyramid: drawn by tiles from now (its part kept for its ids) */
            if (j->ident == a->model_ident && a->model_tiles == 1 && (a->model_part || a->model_ids) && j->section && j->section_n >= 48) {
                uint32_t toc, nocc;
                memcpy(&toc, j->section + 24, 4), memcpy(&nocc, j->section + 20, 4);
                char err[256] = "";
                uint32_t me0 = a->model_part ? nv_part_scene(a->model_part)->elem0 : a->model_elem0;
                uint32_t mne = a->model_part ? nv_part_scene(a->model_part)->nelements : a->model_nE;
                nv_tileset *ts = toc <= j->section_n && (uint64_t)nocc * 36 <= j->section_n - toc
                                     ? nv_tileset_new(j->section, toc, j->nelements, nocc ? (const void *)(j->section + toc) : NULL, nocc, me0, 1, 0, a->model_off, 0, err, sizeof err)
                                     : NULL;
                if (ts && j->nelements == mne) {
                    nv_tileset_keep_section(ts, j->section, j->section_n), j->section = NULL;
                    nv_render_add_tileset(a->r, ts);
                    a->model_ts = ts, a->model_tiles = 2;
                    if (a->model_part) nv_part_set_shown(a->model_part, 0);
                    a->ids_dirty = 1, a->fitted = 0;
                    slog("the open model: drawn by tiles (%u tiles, built in %.0f ms)", nv_tileset_toc(ts)->ntiles, j->ms);
                } else {
                    if (ts) nv_tileset_unref(a->dev, ts);
                    a->model_tiles = 3;
                    slog("the open model: drawn as a part (its tiles: %s)", err[0] ? err : "elements differ");
                }
            } else if (j->ident == a->model_ident && a->model_tiles == 1) a->model_tiles = 3, slog("the open model: drawn as a part (%s)", j->err[0] ? j->err : "no tiles");
            a->dirty = 1;
            nv_job_free(a->dev, j);
            continue;
        }
        if (j->kind == NV_JOB_TILE && j->model) {
            a->tile_jobs--;
            if (j->ts == a->model_ts) {
                if (!j->gt && !strncmp(j->err, "no room", 7)) nv_tileset_queued(j->ts, j->tile, 0), nv_render_tiles_squeeze(a->r);
                else if (j->gt || !j->cancelled) nv_tileset_attach(a->r, j->ts, j->tile, j->gt), j->gt = NULL;
                else nv_tileset_queued(j->ts, j->tile, 0);
                a->dirty_tiles = 1;
            }
            nv_job_free(a->dev, j);
            continue;
        }
        if (j->kind == NV_JOB_TILE) { /* a tile: into its set when the set is still drawn */
            if (L && L->ts == j->ts) {
                L->tiles_inflight--, a->tile_jobs--;
                if (!j->gt && !strncmp(j->err, "no room", 7)) nv_tileset_queued(j->ts, j->tile, 0), nv_render_tiles_squeeze(a->r); /* asked again later */
                else if (j->gt || !j->cancelled) {
                    if (!j->gt && j->err[0]) slog("link %s: tile %u: %s", L->name, j->tile, j->err);
                    nv_tileset_attach(a->r, j->ts, j->tile, j->gt), j->gt = NULL;
                } else nv_tileset_queued(j->ts, j->tile, 0);
                a->dirty_tiles = 1;
            }
            a->load_ms_total += j->ms;
            nv_job_free(a->dev, j);
            continue;
        }
        if (L && j->tag != (int)L->gen) L = NULL; /* an older load of that link */
        a->load_ms_total += j->ms;
        if (j->peak_bytes > a->load_peak_bytes) a->load_peak_bytes = j->peak_bytes;
        if (L && j->kind == NV_JOB_PROXIES) {
            L->proxy_state = j->part ? 2 : 3;
            if (j->part && !L->detail) {
                L->proxy = j->part, j->part = NULL;
                nv_part_set_shown(L->proxy, L->visible);
                nv_render_add(a->r, L->proxy);
                a->dirty = 1;
            }
            L->t_proxy = app_now();
        } else if (L && j->kind == NV_JOB_DETAIL) {
            L->detail_state = j->part ? 2 : (j->cancelled ? 0 : 3);
            if (j->part) {
                if (L->proxy) nv_render_remove(a->r, L->proxy), L->proxy = NULL;
                L->detail = j->part, j->part = NULL;
                nv_part_set_shown(L->detail, L->visible);
                nv_render_add(a->r, L->detail);
                a->ids_dirty = 1, a->dirty = 1, a->vis_dirty = 1;
                L->t_detail = app_now();
                L->load_ms = j->ms;
            } else if (j->err[0] && !j->cancelled) snprintf(a->notice, sizeof a->notice, "%s: %s", L->name, j->err), a->notice_t = app_now();
        }
        nv_job_free(a->dev, j);
    }
    /* NUI2: the tiles the last frame wanted (seen, not loaded), nearest first; two a loader thread on their way */
    if (a->use_tiles) {
        nv_tile_want w[64];
        int room = 2 * (nv_loader_threads(a->loader) > 4 ? nv_loader_threads(a->loader) : 4) - a->tile_jobs; /* NUI3: two a loader, 8 at least */
        uint32_t nw = room > 0 ? nv_render_tiles_want(a->r, w, (uint32_t)room < 64 ? (uint32_t)room : 64) : 0;
        for (uint32_t k = 0; k < nw; k++) {
            SLink *L = NULL;
            for (int i = 0; i < a->nlinks; i++) if (a->links_s[i].ts == w[k].ts) L = &a->links_s[i];
            if (!L && w[k].ts == a->model_ts && a->model_ts) { /* the open model's tile */
                nv_job *j = calloc(1, sizeof *j);
                if (!j) break;
                j->kind = NV_JOB_TILE, j->model = 1, j->ses = a->ses, j->uid = 0xFFFFFFFFu, j->ident = a->model_ident, j->ts = w[k].ts, j->tile = w[k].tile;
                nv_tileset_ref(j->ts);
                nv_tileset_queued(j->ts, j->tile, 1);
                a->tile_jobs++;
                nv_loader_submit(a->loader, j);
                continue;
            }
            if (!L) continue;
            nv_job *j = calloc(1, sizeof *j);
            if (!j) break;
            j->kind = NV_JOB_TILE, j->ses = a->ses, j->uid = L->uid, j->ident = L->ident, j->ts = w[k].ts, j->tile = w[k].tile, j->tag = (int)L->gen;
            nv_tileset_ref(j->ts);
            nv_tileset_queued(j->ts, j->tile, 1);
            L->tiles_inflight++, a->tile_jobs++;
            nv_loader_submit(a->loader, j);
        }
        if (nw) a->dirty_tiles = 1;
        /* a link whose cache gets its section TILE later (its indexing job): drawn by tiles once it is there */
        if (app_now() - a->tiles_checked > 1000) {
            a->tiles_checked = app_now();
            for (int i = 0; i < a->nlinks; i++) if (!a->links_s[i].ts) a->links_synced = -1;
        }
    }
    /* the next detail: the largest on screen first, once every link's proxies are in (or failed) */
    int proxies_pending = 0;
    for (int i = 0; i < a->nlinks; i++) if (a->links_s[i].proxy_state == 1) proxies_pending = 1;
    /* the links' GPU budget (settings link_gpu_mb, as the lab's: SIZE3): once the details drawn (and those
     * on their way, by their triangles) fill it, the other links keep their proxies (state 4) */
    uint64_t used = 0;
    for (int i = 0; i < a->nlinks; i++) {
        const SLink *L = &a->links_s[i];
        if (L->detail) used += nv_part_gpu_bytes(L->detail);
        else if (L->detail_state == 1) used += L->tris * 40;
    }
    while (!proxies_pending && nv_loader_idle(a->loader) > 0) {
        if (a->gpu_budget && used >= a->gpu_budget) {
            for (int i = 0; i < a->nlinks; i++)
                if (!a->links_s[i].detail_state && a->links_s[i].visible) a->links_s[i].detail_state = 4;
            break;
        }
        int best = -1;
        double bs = 0;
        for (int i = 0; i < a->nlinks; i++) {
            SLink *L = &a->links_s[i];
            if (L->detail_state || !L->visible) continue;
            double sz = screen_size(a, L);
            if (best < 0 || sz > bs) best = i, bs = sz;
        }
        if (best < 0) break;
        submit(a, &a->links_s[best], NV_JOB_DETAIL);
        used += a->links_s[best].tris * 40;
        if (nv_loader_idle(a->loader) <= 1) break; /* one per idle thread: the queue stays empty */
    }
    /* the visibility of each part: its link's eye */
    for (int i = 0; i < a->nlinks; i++) {
        SLink *L = &a->links_s[i];
        if (L->ts) nv_tileset_set_shown(L->ts, L->visible);
        if (L->proxy && nv_part_shown(L->proxy) != L->visible) nv_part_set_shown(L->proxy, L->visible), a->dirty = 1;
        if (L->detail && nv_part_shown(L->detail) != L->visible) nv_part_set_shown(L->detail, L->visible), a->dirty = 1;
    }
    if (!a->fitted && (a->model_part || a->model_ts || a->nlinks)) {
        int any = a->model_part != NULL || a->model_ts != NULL;
        for (int i = 0; i < a->nlinks; i++) any |= a->links_s[i].proxy || a->links_s[i].detail || a->links_s[i].ts;
        if (any) app_fit(a), a->fitted = 1;
    }
    { /* NUI2: the links' conversions done: the heap they grew given back (RSS 1.39 GB at exit after a cache rebuild) */
        static int was_busy;
        if (was_busy && !a->links_busy) malloc_trim(0), slog("links converted: memory given back (RSS %.0f MB)", app_rss(NULL) / 1048576.0);
        was_busy = a->links_busy;
    }
    if (a->ids_dirty) scene_ids(a);
    if (a->vis_dirty) a->vis_dirty = 0, core_visibility(a);
}

/* 1 when every ready link is drawn in detail (or failed) and nothing is converting. */
int scene_complete(App *a) {
    if (nv_loader_busy(a->loader) || a->tile_jobs > 0 || a->model_tiles == 1) return 0;
    if (a->use_tiles && nv_render_get_stats(a->r).wanted > 0) return 0;
    for (int i = 0; i < a->nlinks; i++) {
        const SLink *L = &a->links_s[i];
        if (L->visible && L->detail_state != 2 && L->detail_state != 3 && L->detail_state != 4) return 0;
    }
    return !a->links_busy;
}

/* The box of what is drawn (scene mm), the model part and every link: 0 when nothing. A link far
 * from the rest (no georeference: thousands of km) is left out of the fit. */
int scene_box(App *a, double lo[3], double hi[3]) {
    double b[65][6];
    int n = 0;
    if (a->model_ts) nv_tileset_box(a->model_ts, b[n], b[n] + 3), n += b[n][0] <= b[n][3];
    else if (a->model_part) {
        const nv_scene *s = nv_part_scene(a->model_part);
        if (s->bmin[0] <= s->bmax[0]) memcpy(b[n], s->bmin, 24), memcpy(b[n] + 3, s->bmax, 24), n++;
    }
    for (int i = 0; i < a->nlinks && n < 65; i++) {
        const SLink *L = &a->links_s[i];
        if (!L->visible) continue;
        const nv_part *p = L->detail ? L->detail : L->proxy;
        if (L->ts) nv_tileset_box(L->ts, b[n], b[n] + 3), n += b[n][0] <= b[n][3];
        else if (p) {
            const nv_scene *s = nv_part_scene(p);
            if (s->bmin[0] <= s->bmax[0]) memcpy(b[n], s->bmin, 24), memcpy(b[n] + 3, s->bmax, 24), n++;
        } else if (L->has_box) memcpy(b[n], L->lo, 24), memcpy(b[n] + 3, L->hi, 24), n++;
    }
    if (!n) return 0;
    int best = 0; /* the box nearest to all others (NAT1's rule) */
    double bestd = 1e300;
    for (int x = 0; x < n; x++) {
        double d = 0;
        for (int y = 0; y < n; y++)
            for (int i = 0; i < 3; i++) d += fabs((b[x][i] + b[x][3 + i]) - (b[y][i] + b[y][3 + i])) / 2;
        if (d < bestd) bestd = d, best = x;
    }
    for (int i = 0; i < 3; i++) lo[i] = 1e300, hi[i] = -1e300;
    for (int x = 0; x < n; x++) {
        double d = 0;
        for (int i = 0; i < 3; i++) d += fabs((b[x][i] + b[x][3 + i]) - (b[best][i] + b[best][3 + i])) / 2;
        if (d > 2e7) continue;
        for (int i = 0; i < 3; i++) lo[i] = fmin(lo[i], b[x][i]), hi[i] = fmax(hi[i], b[x][3 + i]);
    }
    return lo[0] <= hi[0];
}

/* ---- ids: every drawn element's GlobalId, hashed (hide, isolate and the selection go by id) ---- */
static uint64_t id_hash(const char *id) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 22; i++) h = (h ^ (uint8_t)id[i]) * 1099511628211ull;
    return h;
}
void scene_ids(App *a) {
    a->ids_dirty = 0;
    free(a->id_slot), a->id_slot = NULL, a->id_cap = 0;
    size_t total = 0;
    for (uint32_t k = 0; k < nv_render_nparts(a->r); k++) total += nv_part_scene(nv_render_part(a->r, k))->nelements;
    for (int i = 0; i < a->nlinks; i++) if (a->links_s[i].ids) total += a->links_s[i].nelem;
    total += a->model_nE;
    size_t cap = 64;
    while (cap < 2 * total + 2) cap *= 2;
    a->id_slot = calloc(cap, sizeof *a->id_slot);
    if (!a->id_slot) return;
    a->id_cap = cap;
    for (uint32_t k = 0; k < nv_render_nparts(a->r); k++) {
        const nv_scene *s = nv_part_scene(nv_render_part(a->r, k));
        const char(*ids)[22] = s->ids.p;
        for (uint32_t e = 0; e < s->nelements; e++)
            for (size_t h = id_hash(ids[e]) & (cap - 1);; h = (h + 1) & (cap - 1))
                if (!a->id_slot[h]) { a->id_slot[h] = s->elem0 + e + 1; break; }
    }
    for (uint32_t e = 0; a->model_ids && e < a->model_nE; e++)
        for (size_t h = id_hash(a->model_ids[e]) & (cap - 1);; h = (h + 1) & (cap - 1))
            if (!a->id_slot[h]) { a->id_slot[h] = a->model_elem0 + e + 1; break; }
    for (int i = 0; i < a->nlinks; i++) {
        const SLink *L = &a->links_s[i];
        for (uint32_t e = 0; L->ids && e < L->nelem; e++)
            for (size_t h = id_hash(L->ids[e]) & (cap - 1);; h = (h + 1) & (cap - 1))
                if (!a->id_slot[h]) { a->id_slot[h] = L->elem0 + e + 1; break; }
    }
}
/* a view file drawn by tiles alone: its element's id (NULL none) */
static const char *model_id(App *a, uint32_t e) { return a->model_ids && e >= a->model_elem0 && e < a->model_elem0 + a->model_nE ? a->model_ids[e - a->model_elem0] : NULL; }
/* a tile link's element (global e): its slot, its id */
static const SLink *tile_link_of(App *a, uint32_t e) {
    for (int i = 0; i < a->nlinks; i++)
        if (a->links_s[i].ids && e >= a->links_s[i].elem0 && e < a->links_s[i].elem0 + a->links_s[i].nelem) return &a->links_s[i];
    return NULL;
}
/* The global element of a GlobalId (+1; 0 none). Several parts may hold one id (a link of the same
 * file twice): the first. */
uint32_t scene_find(App *a, const char *id, size_t n) {
    if (n != 22 || !a->id_cap) return 0;
    for (size_t h = id_hash(id) & (a->id_cap - 1); a->id_slot[h]; h = (h + 1) & (a->id_cap - 1)) {
        uint32_t e = a->id_slot[h] - 1, loc;
        const char *mid = model_id(a, e);
        if (mid) {
            if (!memcmp(mid, id, 22)) return e + 1;
            continue;
        }
        const SLink *TL = tile_link_of(a, e);
        if (TL) {
            if (!memcmp(TL->ids[e - TL->elem0], id, 22)) return e + 1;
            continue;
        }
        nv_part *p = nv_render_part_of(a->r, e, &loc);
        if (p && !memcmp(((const char(*)[22])nv_part_scene(p)->ids.p)[loc], id, 22)) return e + 1;
    }
    return 0;
}
/* The id and link of a global element: 1 found (`link` "" for the open model's). */
int scene_elem(App *a, uint32_t elem, char id[23], char *link, size_t nlink) {
    uint32_t loc;
    const char *mid = model_id(a, elem);
    if (mid) { memcpy(id, mid, 22), id[22] = 0, link[0] = 0; return 1; }
    const SLink *TL = tile_link_of(a, elem);
    if (TL) {
        memcpy(id, TL->ids[elem - TL->elem0], 22), id[22] = 0;
        snprintf(link, nlink, "%s", TL->name);
        return 1;
    }
    nv_part *p = nv_render_part_of(a->r, elem, &loc);
    if (!p) return 0;
    memcpy(id, ((const char(*)[22])nv_part_scene(p)->ids.p)[loc], 22), id[22] = 0;
    link[0] = 0;
    for (int i = 0; i < a->nlinks; i++)
        if (a->links_s[i].detail == p) snprintf(link, nlink, "%s", a->links_s[i].name);
    return 1;
}

/* NUI2: the tile boxes checked against the geometry (drive.c check_tiles): every leaf's TLF1 decoded, each element's
 * placed vertices against its leaf's box and each tile's box against its parent's (link frame, mm, with 1 mm of slack). */
void tiles_check(App *a) {
    long bad_el = 0, bad_tiles = 0, leaves = 0;
    for (int i = 0; i < a->nlinks; i++) {
        SLink *L = &a->links_s[i];
        if (!L->ts) continue;
        const cc_tiles_toc *toc = nv_tileset_toc(L->ts);
        long out_el = 0, out_tiles = 0, nl = 0;
        double worst = 0;
        char worst_id[23] = "";
        nv_ses_wr(a->loader);
        cc_link *l = NULL;
        for (size_t k = 0; k < a->ses->nlinks; k++) if (a->ses->links[k].uid == L->uid) l = &a->ses->links[k];
        for (uint32_t t = 0; l && l->vf && t < toc->ntiles; t++) {
            cc_tile x;
            cc_tiles_get(toc, t, &x);
            if (x.parent != CC_TILES_NONE && x.parent < toc->ntiles) {
                cc_tile p;
                cc_tiles_get(toc, x.parent, &p);
                for (int d = 0; d < 3; d++) if (x.box[d] < p.box[d] - 1 || x.box[3 + d] > p.box[3 + d] + 1) { out_tiles++; break; }
            }
            if (!(x.flags & CC_TILE_LEAF)) continue;
            nl++;
            Buf b;
            buf_init(&b);
            cc_tiles_src src = {NULL, l->vf};
            cc_error e;
            if (cc_tiles_leaf_answer(&src, toc, t, &b, &e) == 0) {
                Arena ar;
                arena_init(&ar, 0);
                cc_mesh m;
                uint32_t *rows = NULL;
                if (cc_tiles_leaf_mesh((const uint8_t *)b.p, b.len, &ar, &m, &rows) == 0) {
                    uint32_t last = UINT32_MAX;
                    for (uint32_t k = 0; k < m.ni; k++) {
                        const float *v = m.pos[m.idx[k]];
                        double o = 0;
                        for (int d = 0; d < 3; d++) o = fmax(o, fmax(x.box[d] - v[d], v[d] - x.box[3 + d]));
                        if (o > 1 && rows[k / 3] != last) {
                            last = rows[k / 3], out_el++;
                            if (o > worst) worst = o, cc_link_id(l, rows[k / 3], worst_id), worst_id[22] = 0;
                        }
                    }
                }
                arena_free(&ar);
            }
            buf_free(&b);
        }
        nv_ses_unwr(a->loader);
        fprintf(stderr, "drive: check_tiles: link %d: %u tiles, %ld leaves, %ld tiles outside their parent, %ld elements outside their leaf's box (worst %.0f mm: %s)\n",
                i + 1, toc->ntiles, nl, out_tiles, out_el, worst, worst_id[0] ? worst_id : "-");
        bad_el += out_el, bad_tiles += out_tiles, leaves += nl;
    }
    fprintf(stderr, "drive: check_tiles: %ld leaves, %ld tiles outside their parent, %ld elements outside their leaf's box\n", leaves, bad_tiles, bad_el);
}
