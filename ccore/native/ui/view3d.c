/* The native app's 3D scene (ticket NUI1): the open model as one renderer part, each link as its
 * proxies at once (its cache's HLOD: the first picture) then its detail (decoded by the loader, one
 * link per thread at a time, the largest on screen first), placed as the session places it. Every
 * read of the session holds its lock (loader.h). */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "links.h"
#include "mesh.h"

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
    }
    memset(a->links_s, 0, sizeof a->links_s);
    a->nlinks = 0;
    if (a->model_part) nv_render_remove(a->r, a->model_part), a->model_part = NULL;
    a->have_origin = 0, a->next_elem = NV_ELEM0, a->draw_key = -1, a->fitted = 0;
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
    }
    nv_ses_unwr(a->loader);
    if (a->model_part) nv_render_remove(a->r, a->model_part), a->model_part = NULL;
    if (ok && sc.nelements) {
        a->model_part = nv_part_upload(a->dev, &sc, err, sizeof err);
        if (a->model_part) nv_render_add(a->r, a->model_part);
    } else if (st == 200) nv_scene_free(&sc);
    if (!ok && st == 200) snprintf(a->notice, sizeof a->notice, "%s", err), a->notice_t = app_now();
    a->ids_dirty = 1, a->dirty = 1;
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
    if (!a->have_origin) /* a project of links alone: the first ready link's placed origin is the scene's */
        for (size_t i = 0; i < s->nlinks && !a->have_origin; i++) {
            if (!nv_link_ident(s, (int)i)) continue;
            double c, sn, t[3];
            cc_link_placement(s, &s->links[i], &c, &sn, t);
            const double *o = cc_link_rows(&s->links[i])->origin;
            a->origin0[0] = c * o[0] - sn * o[1] + t[0], a->origin0[1] = sn * o[0] + c * o[1] + t[1], a->origin0[2] = o[2] + t[2];
            a->have_origin = 1;
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
                       L->proxies_gen == l->proxies_gen;
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
    }
    nv_ses_unwr(a->loader);
    for (int i = 0; i < a->nlinks; i++) {
        SLink *L = &a->links_s[i];
        if (!L->proxy_state && !L->detail_state) submit(a, L, NV_JOB_PROXIES); /* the proxies first: small, at once */
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
    for (nv_job *j; (j = nv_loader_take(a->loader));) {
        int i = slot_of(a, j->uid);
        SLink *L = i >= 0 ? &a->links_s[i] : NULL;
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
        if (L->proxy && nv_part_shown(L->proxy) != L->visible) nv_part_set_shown(L->proxy, L->visible), a->dirty = 1;
        if (L->detail && nv_part_shown(L->detail) != L->visible) nv_part_set_shown(L->detail, L->visible), a->dirty = 1;
    }
    if (!a->fitted && (a->model_part || a->nlinks)) {
        int any = a->model_part != NULL;
        for (int i = 0; i < a->nlinks; i++) any |= a->links_s[i].proxy || a->links_s[i].detail;
        if (any) app_fit(a), a->fitted = 1;
    }
    if (a->ids_dirty) scene_ids(a);
    if (a->vis_dirty) a->vis_dirty = 0, core_visibility(a);
}

/* 1 when every ready link is drawn in detail (or failed) and nothing is converting. */
int scene_complete(App *a) {
    if (nv_loader_busy(a->loader)) return 0;
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
    if (a->model_part) {
        const nv_scene *s = nv_part_scene(a->model_part);
        if (s->bmin[0] <= s->bmax[0]) memcpy(b[n], s->bmin, 24), memcpy(b[n] + 3, s->bmax, 24), n++;
    }
    for (int i = 0; i < a->nlinks && n < 65; i++) {
        const SLink *L = &a->links_s[i];
        if (!L->visible) continue;
        const nv_part *p = L->detail ? L->detail : L->proxy;
        if (p) {
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
}
/* The global element of a GlobalId (+1; 0 none). Several parts may hold one id (a link of the same
 * file twice): the first. */
uint32_t scene_find(App *a, const char *id, size_t n) {
    if (n != 22 || !a->id_cap) return 0;
    for (size_t h = id_hash(id) & (a->id_cap - 1); a->id_slot[h]; h = (h + 1) & (a->id_cap - 1)) {
        uint32_t e = a->id_slot[h] - 1, loc;
        nv_part *p = nv_render_part_of(a->r, e, &loc);
        if (p && !memcmp(((const char(*)[22])nv_part_scene(p)->ids.p)[loc], id, 22)) return e + 1;
    }
    return 0;
}
/* The id and link of a global element: 1 found (`link` "" for the open model's). */
int scene_elem(App *a, uint32_t elem, char id[23], char *link, size_t nlink) {
    uint32_t loc;
    nv_part *p = nv_render_part_of(a->r, elem, &loc);
    if (!p) return 0;
    memcpy(id, ((const char(*)[22])nv_part_scene(p)->ids.p)[loc], 22), id[22] = 0;
    link[0] = 0;
    for (int i = 0; i < a->nlinks; i++)
        if (a->links_s[i].detail == p) snprintf(link, nlink, "%s", a->links_s[i].name);
    return 1;
}
