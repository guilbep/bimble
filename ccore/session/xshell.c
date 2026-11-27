/* The cross-link exterior shell (xshell.h; ticket SHELL1, ADR 0078 amendment 7). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guid.h"
#include "jobs.h"
#include "links.h"
#include "session.h"
#include "shell.h"
#include "tiles.h"
#include "xshell.h"

typedef struct {
    char *cache;     /* the link's cache (read in place by the job) */
    uint32_t uid, nlist;
    double c, s, o[3];
} XIn;

typedef struct {
    const cc_platform *pf;
    char key[65];
    char file[4096]; /* where the result is kept ("" none) */
    uint32_t n;
    XIn *in;
    cc_shell_opts o;
    uint8_t **marks; /* per link: per list entry */
    int cancel, finished, rc;
    double ms;
    uint64_t ext, el, peak;
} XJob;

struct cc_xshell {
    char key[65];
    int state;       /* 0 none, 1 computing, 2 ready, -1 failed */
    XJob *job;
    uint32_t n;
    uint32_t *uid, *nlist;
    uint8_t **marks;
    double ms;
    uint64_t ext, el, peak;
};

static void job_free(XJob *j) {
    if (!j) return;
    for (uint32_t i = 0; i < j->n; i++) {
        free(j->in[i].cache);
        if (j->marks) free(j->marks[i]);
    }
    free(j->in), free(j->marks), free(j);
}

static void marks_free(cc_xshell *x) {
    for (uint32_t i = 0; x->marks && i < x->n; i++) free(x->marks[i]);
    free(x->marks), free(x->uid), free(x->nlist);
    x->marks = NULL, x->uid = x->nlist = NULL, x->n = 0;
}

/* the job (another thread): every link's cache mapped and its TILE checked, classified together, the result written */
static void job_run(void *arg) {
    XJob *j = arg;
    const cc_platform *pf = j->pf;
    double t0 = pf->now_ms(pf->ctx);
    cc_viewfile *vf = calloc(j->n + 1, sizeof *vf);
    void **map = calloc(j->n + 1, sizeof *map);
    cc_shell_part *parts = calloc(j->n + 1, sizeof *parts);
    j->marks = calloc(j->n + 1, sizeof *j->marks);
    cc_shell *sh = NULL;
    cc_error e;
    uint32_t mapped = 0;
    int rc = !vf || !map || !parts || !j->marks ? -1 : 0;
    for (uint32_t i = 0; !rc && i < j->n; i++) {
        const char *p = NULL;
        size_t n = 0;
        if (!pf->map_file || pf->map_file(pf->ctx, j->in[i].cache, cc_session_read_limit(j->in[i].cache, NULL), &p, &n, &map[i]) != CC_PF_OK) { rc = -1; break; }
        mapped = i + 1;
        if (cc_viewfile_read_mapped((const uint8_t *)p, n, &vf[i], &e) || !vf[i].lazy || !vf[i].has_tiles || vf[i].tiles_off > vf[i].base_len ||
            vf[i].tiles_len > vf[i].base_len - vf[i].tiles_off) { rc = -1; break; }
        cc_shell_part *q = &parts[i];
        q->src.vf = &vf[i];
        if (cc_tiles_check(vf[i].base + vf[i].tiles_off, (size_t)vf[i].tiles_len, 1, (uint32_t)vf[i].nelements, &q->toc, &e) ||
            q->toc.nlist != j->in[i].nlist) { rc = -1; break; }
        q->c = j->in[i].c, q->s = j->in[i].s;
        memcpy(q->o, j->in[i].o, sizeof q->o);
    }
    if (!rc) rc = cc_shell_start_parts(&sh, parts, j->n, &j->o, &e);
    while (!rc && !cc_atomic_load(&j->cancel)) {
        int st = cc_shell_step(sh, 8000000, &e);
        if (st < 0) rc = -1;
        if (st != 0) break;
    }
    if (!rc && cc_atomic_load(&j->cancel)) rc = 1;
    if (!rc) {
        cc_shell_stats st;
        cc_shell_get_stats(sh, &st);
        j->peak = st.peak_bytes;
        for (uint32_t i = 0; !rc && i < j->n; i++) {
            const uint8_t *ext = cc_shell_exterior_of(sh, i);
            uint32_t nl = parts[i].toc.nlist;
            j->marks[i] = malloc((size_t)nl + 1);
            if (!j->marks[i]) { rc = -1; break; }
            for (uint32_t k = 0; k < nl; k++) {
                uint32_t r = cc_tiles_list(&parts[i].toc, k);
                j->marks[i][k] = r < vf[i].nelements ? (uint8_t)(ext[r] != 0) : 0;
                j->ext += j->marks[i][k], j->el++;
            }
        }
    }
    if (!rc && j->file[0]) { /* kept: "XSH1", u32 1, the key (64), u32 n, per link u32 nlist and its bytes (padded to 4) */
        Buf b;
        buf_init(&b);
        buf_append(&b, "XSH1", 4);
        unsigned char c4[4];
#define PUT32(v) (c4[0] = (unsigned char)(v), c4[1] = (unsigned char)((v) >> 8), c4[2] = (unsigned char)((v) >> 16), c4[3] = (unsigned char)((v) >> 24), buf_append(&b, c4, 4))
        PUT32(1u);
        buf_append(&b, j->key, 64);
        PUT32(j->n);
        for (uint32_t i = 0; i < j->n; i++) {
            PUT32(j->in[i].nlist);
            buf_append(&b, j->marks[i], j->in[i].nlist);
            while (b.len % 4) buf_putc(&b, 0);
        }
#undef PUT32
        if (!b.oom) pf->write_file(pf->ctx, j->file, b.p, b.len); /* a cache: a failure only costs the pass again */
        buf_free(&b);
    }
    cc_shell_free(sh);
    for (uint32_t i = 0; i < mapped; i++) {
        cc_viewfile_free(&vf[i]);
        if (map[i]) pf->unmap_file(pf->ctx, map[i]);
    }
    free(vf), free(map), free(parts);
    j->rc = rc;
    j->ms = pf->now_ms(pf->ctx) - t0;
#if !defined(__wasm__)
    int exp = 0;
    if (!__atomic_compare_exchange_n(&j->finished, &exp, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) job_free(j); /* orphaned */
#else
    j->finished = 1;
#endif
}

static void job_orphan(XJob *j) {
    if (!j) return;
    cc_atomic_store(&j->cancel, 1);
#if !defined(__wasm__)
    int exp = 0;
    if (__atomic_compare_exchange_n(&j->finished, &exp, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return; /* it frees itself */
#endif
    job_free(j);
}

static int job_done(XJob *j) {
#if !defined(__wasm__)
    return __atomic_load_n(&j->finished, __ATOMIC_ACQUIRE) == 1;
#else
    return j->finished == 1;
#endif
}

static void hex(const unsigned char *p, size_t n, char *out) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) out[2 * i] = H[p[i] >> 4], out[2 * i + 1] = H[p[i] & 15];
    out[2 * n] = 0;
}

/* the visible links with a shell baked, and the key of them now */
static uint32_t inputs(cc_session *s, const cc_shell_opts *o, XIn **out, char key[65]) {
    *out = NULL;
    XIn *in = calloc(s->nlinks + 1, sizeof *in);
    if (!in) return 0;
    uint32_t n = 0;
    Buf kb;
    buf_init(&kb);
    char t[256];
    snprintf(t, sizeof t, "shell %u %u %u;", o->views, o->res, o->tol_mm);
    buf_puts(&kb, t);
    for (size_t i = 0; i < s->nlinks; i++) {
        cc_link *l = &s->links[i];
        if (!l->kind || !l->visible || !l->vf || !l->vf->lazy || !l->vf->has_tiles || !l->cache || !l->vf->base) continue;
        const cc_viewfile *v = l->vf;
        if (v->tiles_off > v->base_len || v->tiles_len > v->base_len - v->tiles_off) continue;
        cc_tiles_toc toc;
        if (cc_tiles_check(v->base + v->tiles_off, (size_t)v->tiles_len, 0, CC_TILES_NONE, &toc, NULL) || !(toc.bake & CC_TILES_BAKE_SHELL) || !toc.ntiles) continue;
        double c, sn, tr[3];
        cc_link_placement(s, l, &c, &sn, tr);
        const double *lo = cc_link_rows(l)->origin;
        XIn *x = &in[n];
        x->c = c, x->s = sn, x->uid = l->uid, x->nlist = toc.nlist;
        x->o[0] = c * lo[0] - sn * lo[1] + tr[0], x->o[1] = sn * lo[0] + c * lo[1] + tr[1], x->o[2] = lo[2] + tr[2];
        x->cache = malloc(strlen(l->cache) + 1);
        if (!x->cache) continue;
        strcpy(x->cache, l->cache);
        snprintf(t, sizeof t, "|%lld %llu %u %.17g %.17g %.17g %.17g %.17g;", (long long)l->cache_mtime_s, (unsigned long long)v->tiles_len, toc.nlist, x->c, x->s, x->o[0], x->o[1], x->o[2]);
        buf_puts(&kb, x->cache), buf_puts(&kb, t);
        n++;
    }
    unsigned char h[32];
    cc_blake3(kb.p ? kb.p : "", kb.len, h);
    hex(h, 32, key);
    buf_free(&kb);
    *out = in;
    return n;
}

/* the kept result for `key` read (untrusted): 0 ok */
static int load(cc_session *s, cc_xshell *x, const char *file, const char *key, const XIn *in, uint32_t n) {
    char *p = NULL;
    size_t len = 0;
    if (s->pf->read_file(s->pf->ctx, file, (size_t)1 << 30, &p, &len) != CC_PF_OK) return -1;
    const unsigned char *u = (const unsigned char *)p;
#define RD32(q) ((uint32_t)(q)[0] | (uint32_t)(q)[1] << 8 | (uint32_t)(q)[2] << 16 | (uint32_t)(q)[3] << 24)
    int bad = len < 76 || memcmp(p, "XSH1", 4) || RD32(u + 4) != 1 || memcmp(p + 8, key, 64) || RD32(u + 72) != n;
    size_t at = 76;
    x->marks = calloc(n + 1, sizeof *x->marks), x->uid = calloc(n + 1, sizeof *x->uid), x->nlist = calloc(n + 1, sizeof *x->nlist);
    if (!x->marks || !x->uid || !x->nlist) bad = 1;
    for (uint32_t i = 0; !bad && i < n; i++) {
        uint32_t nl = len - at >= 4 ? RD32(u + at) : 0;
        if (len - at < 4 || nl != in[i].nlist || nl > len - at - 4) { bad = 1; break; }
        x->marks[i] = malloc((size_t)nl + 1);
        if (!x->marks[i]) { bad = 1; break; }
        memcpy(x->marks[i], u + at + 4, nl);
        for (uint32_t k = 0; k < nl; k++) bad |= x->marks[i][k] > 1;
        x->uid[i] = in[i].uid, x->nlist[i] = nl;
        at += 4 + (((size_t)nl + 3) & ~(size_t)3);
        x->n = i + 1;
    }
#undef RD32
    free(p);
    if (bad || at != len) return marks_free(x), -1;
    x->ext = x->el = 0;
    for (uint32_t i = 0; i < x->n; i++)
        for (uint32_t k = 0; k < x->nlist[i]; k++) x->ext += x->marks[i][k], x->el++;
    return 0;
}

static void take(cc_xshell *x, XJob *j) { /* a finished job's result moves to the state */
    marks_free(x);
    if (j->rc) {
        x->state = -1;
        return;
    }
    x->marks = j->marks, j->marks = NULL;
    x->uid = calloc(j->n + 1, sizeof *x->uid), x->nlist = calloc(j->n + 1, sizeof *x->nlist);
    if (!x->uid || !x->nlist) {
        x->state = -1;
        for (uint32_t i = 0; x->marks && i < j->n; i++) free(x->marks[i]);
        free(x->marks), x->marks = NULL;
        return;
    }
    x->n = j->n;
    for (uint32_t i = 0; i < j->n; i++) x->uid[i] = j->in[i].uid, x->nlist[i] = j->in[i].nlist;
    x->ms = j->ms, x->ext = j->ext, x->el = j->el, x->peak = j->peak;
    x->state = 2;
}

int cc_xshell_poll(cc_session *s) {
    cc_tiles_opts to = s->tiles_opts;
    if (!to.leaf_tris) cc_tiles_default(&to);
    if (!s->xshell) s->xshell = calloc(1, sizeof *s->xshell);
    cc_xshell *x = s->xshell;
    if (!x) return 0;
    if (!to.shell_views || !to.shell_cross || !s->pf->spawn || !s->pf->files) {
        job_orphan(x->job), x->job = NULL, marks_free(x), x->state = 0, x->key[0] = 0;
        return 0;
    }
    cc_shell_opts so = {to.cross_views ? to.cross_views : to.shell_views, to.cross_res ? to.cross_res : to.shell_res, to.shell_tol, 0};
    XIn *in;
    char key[65];
    uint32_t n = inputs(s, &so, &in, key);
    if (n < 2) { /* one link: its own shell is the whole */
        for (uint32_t i = 0; in && i < n; i++) free(in[i].cache);
        free(in);
        job_orphan(x->job), x->job = NULL, marks_free(x), x->state = 0, x->key[0] = 0;
        return 0;
    }
    if (!strcmp(key, x->key)) {
        for (uint32_t i = 0; i < n; i++) free(in[i].cache);
        free(in);
        if (x->job && job_done(x->job)) take(x, x->job), job_free(x->job), x->job = NULL;
        return x->state;
    }
    job_orphan(x->job), x->job = NULL, marks_free(x);
    memcpy(x->key, key, sizeof x->key);
    char dir[4096], file[4096] = "";
    if (s->pf->cache_dir && !s->pf->cache_dir(s->pf->ctx, dir, sizeof dir)) {
        size_t dl = strlen(dir);
        if (dl + 7 < sizeof dir) {
            memcpy(dir + dl, "/links", 7);
            if (!s->pf->make_dir || !s->pf->make_dir(s->pf->ctx, dir)) snprintf(file, sizeof file, "%s/xshell-%.16s.bin", dir, key);
        }
    }
    if (file[0] && !load(s, x, file, key, in, n)) {
        for (uint32_t i = 0; i < n; i++) free(in[i].cache);
        free(in);
        x->state = 2, x->ms = 0;
        return 2;
    }
    XJob *j = calloc(1, sizeof *j);
    if (!j) {
        for (uint32_t i = 0; i < n; i++) free(in[i].cache);
        free(in);
        return x->state = -1;
    }
    j->pf = s->pf, j->n = n, j->in = in, j->o = so;
    memcpy(j->key, key, sizeof j->key);
    snprintf(j->file, sizeof j->file, "%s", file);
    x->job = j, x->state = 1;
    if (s->pf->spawn(s->pf->ctx, job_run, j)) job_run(j); /* no thread: here */
    if (x->job && job_done(x->job)) take(x, x->job), job_free(x->job), x->job = NULL;
    return x->state;
}

const uint8_t *cc_xshell_marks(cc_session *s, const struct cc_link *l, uint32_t *n) {
    cc_xshell *x = s->xshell;
    if (!x || x->state != 2) return NULL;
    for (uint32_t i = 0; i < x->n; i++)
        if (x->uid[i] == l->uid) return *n = x->nlist[i], x->marks[i];
    return NULL;
}

void cc_xshell_stats(cc_session *s, uint64_t *exterior, uint64_t *elements, double *ms, uint64_t *peak) {
    cc_xshell *x = s->xshell;
    *exterior = x ? x->ext : 0, *elements = x ? x->el : 0, *ms = x ? x->ms : 0, *peak = x ? x->peak : 0;
}

void cc_xshell_free(cc_session *s) {
    cc_xshell *x = s->xshell;
    if (!x) return;
    job_orphan(x->job);
    marks_free(x);
    free(x);
    s->xshell = NULL;
}
