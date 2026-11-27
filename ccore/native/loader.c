/* The native app's loader (loader.h). */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader.h"
#include "hlod.h"
#include "inflate.h"
#include "link_cache.h"
#include "links.h"
#include "meshcodec.h"
#include "session.h"
#include "mesh.h"
#include "viewfile.h"

struct nv_loader {
    SDL_GPUDevice *dev;
    pthread_rwlock_t ses; /* the session's lock: the UI writes, the loaders read */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    nv_job *queue, *done;
    int nthreads, idle, running, stop;
    pthread_t th[8];
    uint32_t cancel_uid[64];
    int ncancel;
};

void nv_ses_wr(nv_loader *L) { pthread_rwlock_wrlock(&L->ses); }
void nv_ses_unwr(nv_loader *L) { pthread_rwlock_unlock(&L->ses); }

const void *nv_link_ident(const cc_session *s, int i) {
    if (i < 0 || (size_t)i >= s->nlinks) return NULL;
    const cc_link *k = &s->links[i];
    if (k->state != CC_LS_READY || !k->kind) return NULL;
    return k->vf ? (const void *)k->vf : k->ifc ? (const void *)k->ifc : (const void *)k->model;
}

/* ---- a job's source: a session link, found again by uid at each lock ---- */
typedef struct {
    nv_loader *L;
    nv_job *j;
    cc_link *k;       /* valid while locked */
    int gone;         /* the link changed or went: cancel */
    int lazy;
    Arena shapes, scratch;
    cc_mesh *shape;   /* lazy: decoded shapes, once each */
    uint8_t *shape_ok; /* 0 not yet, 1 decoded, 2 damaged */
    size_t nshapes;
} Src;

static int find(Src *x) {
    cc_session *s = x->j->ses;
    x->k = NULL;
    for (size_t i = 0; i < s->nlinks; i++)
        if (s->links[i].uid == x->j->uid) {
            if (nv_link_ident(s, (int)i) == x->j->ident) x->k = &s->links[i];
            break;
        }
    if (!x->k) x->gone = 1;
    return x->k != NULL;
}
static int job_cancelled(nv_loader *L, nv_job *j) {
    pthread_mutex_lock(&L->mu);
    int c = 0;
    for (int i = 0; i < L->ncancel; i++) if (L->cancel_uid[i] == j->uid || L->cancel_uid[i] == 0) c = 1;
    pthread_mutex_unlock(&L->mu);
    return c;
}
static void s_lock(void *ctx) {
    Src *x = ctx;
    pthread_rwlock_rdlock(&x->L->ses);
    find(x);
    arena_reset(&x->scratch);
}
static void s_unlock(void *ctx) {
    Src *x = ctx;
    x->k = NULL;
    pthread_rwlock_unlock(&x->L->ses);
}
static int s_cancelled(void *ctx) {
    Src *x = ctx;
    return x->gone || !x->k || job_cancelled(x->L, x->j);
}
static void s_row(void *ctx, size_t i, char id[22], int *cat, int32_t *shape, float m[12]) {
    Src *x = ctx;
    const cc_scene *sc = x->k ? cc_link_rows(x->k) : NULL;
    if (!sc || i >= sc->nelements) { memset(id, '0', 22), *cat = 7, *shape = -1; memset(m, 0, 48); return; }
    const cc_scene_element *e = &sc->elements[i];
    cc_link_id(x->k, i, id);
    *cat = (int)e->category < 8 ? (int)e->category : 7;
    *shape = e->shape;
    const cc_placement32 *p = &e->to_scene;
    for (int c = 0; c < 3; c++) m[c] = p->x[c], m[3 + c] = p->y[c], m[6 + c] = p->z[c], m[9 + c] = p->t[c];
}
static int s_shape_box(void *ctx, size_t s, float bb[6]) {
    Src *x = ctx;
    if (!x->k) return -1;
    if (x->lazy) {
        if (s >= x->k->vf->nshapes || !x->k->vf->shape_refs) return -1;
        const cc_viewfile_ref *r = &x->k->vf->shape_refs[s];
        for (int i = 0; i < 3; i++) bb[i] = r->lo[i], bb[3 + i] = r->hi[i];
        return 0;
    }
    const cc_scene *sc = cc_link_rows(x->k);
    if (s >= sc->nshapes) return -1;
    const cc_mesh *m = &sc->shapes[s];
    for (int i = 0; i < 3; i++) bb[i] = 1e30f, bb[3 + i] = -1e30f;
    for (uint32_t v = 0; m->pos && v < m->nv; v++)
        for (int i = 0; i < 3; i++) {
            if (m->pos[v][i] < bb[i]) bb[i] = m->pos[v][i];
            if (m->pos[v][i] > bb[3 + i]) bb[3 + i] = m->pos[v][i];
        }
    return 0;
}
static int s_shape_mesh(void *ctx, size_t s, cc_mesh *out) {
    Src *x = ctx;
    if (!x->k) return -1;
    if (!x->lazy) {
        const cc_scene *sc = cc_link_rows(x->k);
        if (s >= sc->nshapes) return -1;
        *out = sc->shapes[s];
        return 0;
    }
    if (s >= x->nshapes) return -1;
    if (!x->shape_ok[s]) { /* decoded once into the job's arena (every copy of it uses it) */
        const uint8_t *q;
        size_t qn;
        const char *why;
        x->shape_ok[s] = 2;
        if (cc_viewfile_block(x->k->vf, s, &q, &qn) == 0 && cc_meshcodec_decode(q, qn, &x->shapes, &x->shape[s], &why) == CC_MC_OK)
            x->shape_ok[s] = 1;
    }
    if (x->shape_ok[s] != 1) return -1;
    *out = x->shape[s];
    return 0;
}
static int s_own_mesh(void *ctx, size_t i, cc_mesh *out) {
    Src *x = ctx;
    if (!x->k) return -1;
    if (!x->lazy) {
        const cc_scene *sc = cc_link_rows(x->k);
        if (i >= sc->nelements) return -1;
        *out = sc->elements[i].mesh;
        return 0;
    }
    const cc_viewfile *vf = x->k->vf;
    if (i >= vf->nelements) return -1;
    const uint8_t *q;
    size_t qn;
    const char *why;
    if (cc_viewfile_block(vf, vf->nshapes + i, &q, &qn) != 0) return -1;
    return cc_meshcodec_decode(q, qn, &x->scratch, out, &why) == CC_MC_OK ? 0 : -1;
}

/* The link's proxies (its cache's HLOD section, BPRX), copied under the lock, inflated and checked out
 * of it. 0 with *out (malloc'd); 1 none; -1 damaged (err). */
static int read_bprx(Src *x, Buf *out, uint32_t *nelements, char *err, size_t errn) {
    buf_init(out);
    s_lock(x);
    if (!x->k || !x->k->vf || !x->k->vf->has_hlod || !x->k->vf->base) { s_unlock(x); return 1; }
    const cc_viewfile *vf = x->k->vf;
    *nelements = (uint32_t)cc_link_rows(x->k)->nelements;
    uint64_t off = vf->hlod_off, len = vf->hlod_len;
    if (off + len > vf->base_len || len > ((uint64_t)1 << 31)) { s_unlock(x); snprintf(err, errn, "proxies: out of the cache"); return -1; }
    Buf raw;
    buf_init(&raw);
    buf_append(&raw, (const char *)vf->base + off, (size_t)len);
    s_unlock(x);
    if (raw.oom) { buf_free(&raw); snprintf(err, errn, "out of memory"); return -1; }
    if (raw.len >= 2 && (unsigned char)raw.p[0] == 0x1f && (unsigned char)raw.p[1] == 0x8b) { /* cache format 5 on: gzipped */
        const char *why = NULL;
        size_t cap = (size_t)(len > (CC_LINK_HLOD_MAX * 2) / 64 ? CC_LINK_HLOD_MAX * 2 : len * 64);
        if (cc_gunzip((const unsigned char *)raw.p, raw.len, cap, out, &why)) {
            buf_free(&raw), buf_free(out);
            snprintf(err, errn, "proxies: damaged (%s)", why ? why : "gzip");
            return -1;
        }
        buf_free(&raw);
    } else *out = raw;
    cc_error e;
    if (cc_hlod_bprx_check((const uint8_t *)out->p, out->len, *nelements, &e)) {
        buf_free(out);
        snprintf(err, errn, "proxies: %s", e.msg);
        return -1;
    }
    return 0;
}

static uint64_t scene_bytes(const nv_scene *sc) {
    const nv_vec *v[] = {&sc->wv, &sc->wi, &sc->we, &sc->sv, &sc->si, &sc->se, &sc->plc, &sc->items, &sc->pv, &sc->pi, &sc->pe, &sc->cells};
    uint64_t b = 0;
    for (size_t k = 0; k < sizeof v / sizeof *v; k++) b += (uint64_t)v[k]->cap * v[k]->sz;
    return b;
}

/* NUI2: one tile of a link's pyramid: its bytes taken under the session's read lock (a proxy's TPX1 copied from the
 * mapped cache; a leaf's TLF1 gathered from its elements' blocks), made into GPU buffers out of it. */
static void run_tile(nv_loader *L, nv_job *j) {
    Src x;
    memset(&x, 0, sizeof x);
    x.L = L, x.j = j;
    Buf b;
    buf_init(&b);
    cc_tile t;
    const cc_tiles_toc *toc = nv_tileset_toc(j->ts);
    cc_tiles_get(toc, j->tile, &t);
    int kind = 0;
    pthread_rwlock_rdlock(&L->ses);
    if (j->model) { /* the open model's: its proxies from the copy of the section; a leaf from the session's scene */
        const cc_session *s = j->ses;
        if (t.flags & CC_TILE_PROXY) {
            const uint8_t *sec;
            size_t sn;
            nv_tileset_section(j->ts, &sec, &sn);
            uint64_t at = (uint64_t)toc->blob_at + t.off;
            if (!sec || at > sn || t.len > sn - at) snprintf(j->err, sizeof j->err, "tiles: a proxy out of the section");
            else buf_append(&b, (const char *)sec + at, t.len), kind = 2;
        } else if ((!s->open && !s->view) || s->tiles_scene != j->ident || s->tiles_status != 200) x.gone = 1;
        else {
            cc_tiles_src src = {s->tiles_scene, NULL};
            cc_error e;
            if (cc_tiles_leaf_answer(&src, toc, j->tile, &b, &e)) snprintf(j->err, sizeof j->err, "%s", e.msg);
            else kind = 1;
        }
        pthread_rwlock_unlock(&L->ses);
        goto built;
    }
    find(&x);
    const cc_viewfile *vf = x.k ? x.k->vf : NULL;
    if (!vf || !vf->has_tiles || !vf->lazy || !vf->base || vf->tiles_off > vf->base_len || vf->tiles_len > vf->base_len - vf->tiles_off) {
        if (!x.gone) snprintf(j->err, sizeof j->err, "tiles: the link's cache has none");
    } else if (t.flags & CC_TILE_PROXY) {
        uint64_t at = (uint64_t)toc->blob_at + t.off;
        if (at > vf->tiles_len || t.len > vf->tiles_len - at) snprintf(j->err, sizeof j->err, "tiles: a proxy out of the section");
        else buf_append(&b, (const char *)vf->base + vf->tiles_off + at, t.len), kind = 2;
    } else if (t.flags & CC_TILE_LEAF) {
        cc_tiles_src src = {NULL, vf};
        cc_error e;
        if (cc_tiles_leaf_answer(&src, toc, j->tile, &b, &e)) snprintf(j->err, sizeof j->err, "%s", e.msg);
        else kind = 1;
    } else snprintf(j->err, sizeof j->err, "tiles: tile %u has no content", j->tile);
    pthread_rwlock_unlock(&L->ses);
built:
    if (x.gone) j->cancelled = 1, kind = 0;
    if (b.oom) kind = 0, snprintf(j->err, sizeof j->err, "out of memory");
    j->bytes_in = b.len;
    if (kind == 1) j->gt = nv_gtile_leaf(L->dev, j->ts, j->tile, (const uint8_t *)b.p, b.len, j->err, sizeof j->err);
    else if (kind == 2) j->gt = nv_gtile_proxy(L->dev, j->ts, j->tile, (const uint8_t *)b.p, b.len, j->err, sizeof j->err);
    j->peak_bytes = b.cap;
    buf_free(&b);
}

/* NUI2: the open model's pyramid built in steps (the session's own builder, cc_mesh_tiles: each step under the session's
 * lock as the writer, about 100,000 triangles: the UI waits at most one step), then its section copied out */
static void run_model_tiles(nv_loader *L, nv_job *j) {
    for (int guard = 0; guard < 100000; guard++) {
        if (job_cancelled(L, j)) { j->cancelled = 1; return; }
        const char *p;
        size_t n;
        uint32_t rev;
        pthread_rwlock_wrlock(&L->ses);
        cc_session *s = j->ses;
        int st = s->open || s->view ? cc_mesh_tiles(s, 100000, "toc", 3, &p, &n, &rev) : 404;
        if (st == 200 && s->tiles && s->tiles_scene) {
            if (s->tiles_scene != j->ident) j->cancelled = 1;
            else if ((j->section = malloc(s->tiles->out.len + 1))) {
                memcpy(j->section, s->tiles->out.p, s->tiles->out.len), j->section_n = s->tiles->out.len;
                j->nelements = (uint32_t)s->tiles_scene->nelements;
            } else snprintf(j->err, sizeof j->err, "out of memory");
        } else if (st != 202) snprintf(j->err, sizeof j->err, "the model's tiles: %d %.*s", st, (int)(n < 200 ? n : 200), p ? p : "");
        pthread_rwlock_unlock(&L->ses);
        if (st != 202) return;
    }
}

static void run(nv_loader *L, nv_job *j) {
    double t0 = nv_now_ms();
    if (j->kind == NV_JOB_MODEL_TILES) {
        run_model_tiles(L, j);
        j->ms = nv_now_ms() - t0;
        return;
    }
    if (j->kind == NV_JOB_TILE) {
        if (!job_cancelled(L, j)) run_tile(L, j);
        else j->cancelled = 1;
        j->ms = nv_now_ms() - t0;
        return;
    }
    Src x;
    memset(&x, 0, sizeof x);
    x.L = L, x.j = j;
    arena_init(&x.shapes, 0), arena_init(&x.scratch, 0);
    nv_scene sc;
    nv_scene_init(&sc, j->elem0, j->origin0);
    Buf bprx;
    buf_init(&bprx);
    uint32_t nE = 0;
    nv_src src = {.ctx = &x, .lock = s_lock, .unlock = s_unlock, .cancelled = s_cancelled, .row = s_row,
                  .shape_box = s_shape_box, .shape_mesh = s_shape_mesh, .own_mesh = s_own_mesh};
    int rc = 0;
    if (j->kind == NV_JOB_PROXIES || j->with_proxies) {
        int pr = read_bprx(&x, &bprx, &nE, j->err, sizeof j->err);
        if (pr < 0) rc = j->kind == NV_JOB_PROXIES ? -1 : 0, j->err[0] = j->kind == NV_JOB_PROXIES ? j->err[0] : 0;
        if (pr != 0) buf_free(&bprx);
        if (x.gone) rc = -1, j->cancelled = 1;
    }
    if (!rc && j->kind == NV_JOB_DETAIL) {
        s_lock(&x);
        if (x.k) {
            const cc_scene *rows = cc_link_rows(x.k);
            src.nelements = rows->nelements;
            x.lazy = x.k->vf && x.k->vf->lazy;
            src.nshapes = x.lazy ? x.k->vf->nshapes : rows->nshapes;
            x.nshapes = src.nshapes;
        }
        s_unlock(&x);
        if (!x.gone) {
            x.shape = calloc(x.nshapes + 1, sizeof *x.shape), x.shape_ok = calloc(x.nshapes + 1, 1);
            if (!x.shape || !x.shape_ok) rc = -1, snprintf(j->err, sizeof j->err, "out of memory");
            else rc = nv_load_src(&sc, &src, &j->lo, j->err, sizeof j->err);
            if (rc && (x.gone || !strcmp(j->err, "cancelled"))) j->cancelled = 1;
        } else rc = -1, j->cancelled = 1;
        free(x.shape), free(x.shape_ok);
        arena_free(&x.shapes);
    }
    arena_free(&x.scratch);
    if (!rc && bprx.len) {
        if (j->kind == NV_JOB_PROXIES) sc.proxies_only = 1;
        if (nv_load_bprx(&sc, bprx.p, bprx.len, j->kind == NV_JOB_DETAIL ? nE : 0, &j->lo, j->err, sizeof j->err)) {
            if (j->kind == NV_JOB_PROXIES) rc = -1;
            else { /* the detail without its proxies */
                nv_vec_free(&sc.cells), nv_vec_free(&sc.roots), nv_vec_free(&sc.kids), nv_vec_free(&sc.pv), nv_vec_free(&sc.pi), nv_vec_free(&sc.pe);
                j->err[0] = 0;
            }
        }
    }
    buf_free(&bprx);
    if (!rc && job_cancelled(L, j)) rc = -1, j->cancelled = 1;
    if (!rc && nv_finish(&sc)) rc = -1, snprintf(j->err, sizeof j->err, "out of memory");
    j->peak_bytes = scene_bytes(&sc);
    if (!rc && (sc.items.n || sc.cells.n)) j->part = nv_part_upload(L->dev, &sc, j->err, sizeof j->err);
    nv_scene_free(&sc);
    j->ms = nv_now_ms() - t0;
}

static void *worker(void *arg) {
    nv_loader *L = arg;
    pthread_mutex_lock(&L->mu);
    for (;;) {
        while (!L->queue && !L->stop) L->idle++, pthread_cond_wait(&L->cv, &L->mu), L->idle--;
        if (L->stop) break;
        nv_job *j = L->queue;
        L->queue = j->next;
        j->next = NULL;
        L->running++;
        pthread_mutex_unlock(&L->mu);
        run(L, j);
        pthread_mutex_lock(&L->mu);
        L->running--;
        nv_job **t = &L->done;
        while (*t) t = &(*t)->next;
        *t = j;
    }
    pthread_mutex_unlock(&L->mu);
    return NULL;
}

nv_loader *nv_loader_new(SDL_GPUDevice *dev, int threads) {
    nv_loader *L = calloc(1, sizeof *L);
    if (!L) return NULL;
    L->dev = dev;
    pthread_rwlockattr_t ra;
    pthread_rwlockattr_init(&ra);
#ifdef __GLIBC__
    pthread_rwlockattr_setkind_np(&ra, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP); /* the UI never starves */
#endif
    pthread_rwlock_init(&L->ses, &ra);
    pthread_rwlockattr_destroy(&ra);
    pthread_mutex_init(&L->mu, NULL);
    pthread_cond_init(&L->cv, NULL);
    if (threads < 1) threads = 1;
    if (threads > 8) threads = 8;
    for (int i = 0; i < threads; i++)
        if (pthread_create(&L->th[L->nthreads], NULL, worker, L) == 0) L->nthreads++;
    return L;
}

void nv_job_free(SDL_GPUDevice *dev, nv_job *j) {
    if (!j) return;
    if (j->part) nv_part_free(dev, j->part);
    if (j->gt) nv_gtile_free(dev, j->gt);
    if (j->ts) nv_tileset_unref(dev, j->ts);
    free(j->section);
    free(j);
}

void nv_loader_free(nv_loader *L) {
    if (!L) return;
    pthread_mutex_lock(&L->mu);
    L->stop = 1;
    L->ncancel = 1, L->cancel_uid[0] = 0;
    pthread_cond_broadcast(&L->cv);
    pthread_mutex_unlock(&L->mu);
    for (int i = 0; i < L->nthreads; i++) pthread_join(L->th[i], NULL);
    for (nv_job *j = L->queue, *n; j; j = n) n = j->next, nv_job_free(L->dev, j);
    for (nv_job *j = L->done, *n; j; j = n) n = j->next, nv_job_free(L->dev, j);
    pthread_rwlock_destroy(&L->ses);
    pthread_mutex_destroy(&L->mu);
    pthread_cond_destroy(&L->cv);
    free(L);
}

void nv_loader_submit(nv_loader *L, nv_job *j) {
    pthread_mutex_lock(&L->mu);
    for (int i = 0; i < L->ncancel; i++) /* a new job of a cancelled link: it runs again */
        if (L->cancel_uid[i] == j->uid) L->cancel_uid[i] = L->cancel_uid[--L->ncancel], i--;
    j->next = NULL;
    nv_job **t = &L->queue;
    while (*t) t = &(*t)->next;
    *t = j;
    pthread_cond_signal(&L->cv);
    pthread_mutex_unlock(&L->mu);
}

int nv_loader_idle(nv_loader *L) {
    pthread_mutex_lock(&L->mu);
    int n = L->queue ? 0 : L->idle;
    pthread_mutex_unlock(&L->mu);
    return n;
}

int nv_loader_busy(nv_loader *L) {
    pthread_mutex_lock(&L->mu);
    int b = L->queue != NULL || L->running > 0;
    pthread_mutex_unlock(&L->mu);
    return b;
}

nv_job *nv_loader_take(nv_loader *L) {
    pthread_mutex_lock(&L->mu);
    nv_job *j = L->done;
    if (j) L->done = j->next, j->next = NULL;
    if (!L->queue && !L->running) L->ncancel = 0; /* nothing left to cancel */
    pthread_mutex_unlock(&L->mu);
    return j;
}

void nv_loader_cancel(nv_loader *L, uint32_t uid) {
    pthread_mutex_lock(&L->mu);
    nv_job **t = &L->queue;
    while (*t) { /* queued: dropped into done, cancelled */
        if (!uid || (*t)->uid == uid) {
            nv_job *j = *t;
            *t = j->next;
            j->cancelled = 1, j->next = NULL;
            nv_job **d = &L->done;
            while (*d) d = &(*d)->next;
            *d = j;
        } else t = &(*t)->next;
    }
    if (L->ncancel < 64) L->cancel_uid[L->ncancel++] = uid;
    pthread_mutex_unlock(&L->mu);
}
