/* LINKF: a linked IFC read once, its light form and report beside it (link_cache.h, ADR 0079). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guid.h"
#include "hlod.h"
#include "ifc_psets.h"
#include "inflate.h"
#include "links.h"
#include "json_r.h"
#include "jobs.h"
#include "json_w.h"
#include "link_cache.h"
#include "ifc_psets.h"
#include "session.h"

#define ENDS ((size_t)1 << 20) /* the first and last MiB hashed */


static int ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    if (n < k) return 0;
    for (size_t i = 0; i < k; i++) {
        char c = s[n - k + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suffix[i]) return 0;
    }
    return 1;
}

static void hex(const unsigned char *p, size_t n, char *out) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) out[2 * i] = H[p[i] >> 4], out[2 * i + 1] = H[p[i] & 15];
    out[2 * n] = 0;
}

/* BLAKE3(first MiB || last MiB || size as 8 bytes LE), 64 hex digits. -1 out of memory. */
static int ends_hash(const char *head, size_t nh, const char *tail, size_t nt, uint64_t bytes, char out[65]) {
    unsigned char *b = malloc(nh + nt + 8), h[32];
    if (!b) return -1;
    if (nh) memcpy(b, head, nh);
    if (nt) memcpy(b + nh, tail, nt);
    for (int i = 0; i < 8; i++) b[nh + nt + i] = (unsigned char)(bytes >> (8 * i));
    cc_blake3(b, nh + nt + 8, h);
    free(b);
    hex(h, 32, out);
    return 0;
}

int cc_link_cache_source(uint64_t bytes, int64_t mtime_s, const char *head, size_t nh, const char *tail, size_t nt,
                         const double *base_mm, char *out, size_t n) {
    char e[65];
    if (ends_hash(head, nh, tail, nt, bytes, e)) return -1;
    Buf b;
    buf_init(&b);
    JsonW w; /* compact, keys sorted */
    jw_init(&w, &b, 0);
    jw_obj_begin(&w);
    if (base_mm) { /* LINKF: the IFC's base point (its building's origin), for the link's positioning */
        jw_key(&w, "base_point_mm"), jw_arr_begin(&w);
        for (int k = 0; k < 3; k++) jw_f64(&w, base_mm[k] + 0.0);
        jw_arr_end(&w);
    }
    jw_key(&w, "bytes"), jw_u64(&w, bytes);
    jw_key(&w, "complete"), jw_bool(&w, 0); /* SIZE3 (E): the proxies are baked after (cc_link_cache_mark_complete) */
    jw_key(&w, "ends"), jw_str(&w, e);
    jw_key(&w, "format"), jw_str(&w, CC_LINK_CACHE_FORMAT);
    jw_key(&w, "mtime_s"), jw_i64(&w, mtime_s);
    jw_obj_end(&w);
    int rc = jw_finish(&w) != JW_OK || b.oom || b.len + 1 > n ? -1 : 0;
    if (!rc) memcpy(out, b.p, b.len), out[b.len] = 0;
    buf_free(&b);
    return rc;
}

/* LINKF: the base point a cache's SRCE names (0 ok; -1 none). */
int cc_link_cache_base_point(const char *src, size_t n, double out[3]) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = jr_parse(&a, src, n, &je), *b = v && v->type == JR_OBJ ? jr_get(v, "base_point_mm") : NULL;
    int rc = -1;
    if (b && b->type == JR_ARR && b->n == 3 && b->items[0].type == JR_NUM && b->items[1].type == JR_NUM && b->items[2].type == JR_NUM) {
        rc = 0;
        for (int k = 0; k < 3; k++) {
            out[k] = b->items[k].num + 0.0;
            if (!(out[k] > -1e12 && out[k] < 1e12)) rc = -1;
        }
    }
    arena_free(&a);
    return rc;
}

int cc_link_cache_matches(const char *src, size_t n, uint64_t bytes, int64_t mtime_s, const char *ends_hex, int ends_known) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = jr_parse(&a, src, n, &je);
    int ok = 0;
    if (v && v->type == JR_OBJ) {
        const JrVal *b = jr_get(v, "bytes"), *m = jr_get(v, "mtime_s"), *f = jr_get(v, "format"), *e = jr_get(v, "ends");
        ok = b && b->type == JR_NUM && b->num == (double)bytes && m && m->type == JR_NUM && m->num == (double)mtime_s &&
             f && f->type == JR_STR && !strcmp(f->s, CC_LINK_CACHE_FORMAT) && e && e->type == JR_STR && e->n == 64;
        if (ok && ends_known) ok = !memcmp(e->s, ends_hex, 64);
    }
    arena_free(&a);
    return ok;
}

int cc_link_cache_complete(const char *src, size_t n) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = jr_parse(&a, src, n, &je), *c = v && v->type == JR_OBJ ? jr_get(v, "complete") : NULL;
    int ok = c && c->type == JR_BOOL && c->b;
    arena_free(&a);
    return ok;
}

int cc_link_cache_mark_complete(const char *src, size_t n, char *out, size_t cap) {
    static const char F[] = "\"complete\":false";
    const size_t fl = sizeof F - 1;
    size_t at = n;
    for (size_t i = 0; i + fl <= n; i++)
        if (!memcmp(src + i, F, fl)) { at = i; break; }
    if (at == n || n + 1 > cap) return -1;
    memcpy(out, src, at);
    memcpy(out + at, "\"complete\":true", 15);
    memcpy(out + at + 15, src + at + fl, n - at - fl);
    out[n - 1] = 0; /* one byte shorter */
    return cc_link_cache_complete(out, n - 1) ? 0 : -1;
}

cc_link_job *cc_link_job_new(const cc_platform *pf, const char *path) {
    cc_link_job *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->path = malloc(strlen(path) + 1);
    if (!j->path) return free(j), NULL;
    strcpy(j->path, path);
    j->pf = pf;
    j->cache_min = CC_LINK_CACHE_MIN;
    return j;
}

void cc_link_job_free(cc_link_job *j) {
    if (!j) return;
    if (j->vf) cc_viewfile_free(j->vf), free(j->vf);
    if (j->map) j->pf->unmap_file(j->pf->ctx, j->map); /* SIZE3: after the view file over it */
    if (j->ifc) cc_ifc_scene_free(j->ifc), free(j->ifc);
    free(j->psets); /* M1LINKS */
    free(j->path);
    free(j);
}

static void fail(cc_link_job *j, int rc, const char *msg) {
    j->rc = rc;
    if (msg) snprintf(j->err.msg, sizeof j->err.msg, "%s", msg);
}

/* Reads the view file at `path` (untrusted) into a new cc_viewfile. NULL with err. */
static cc_viewfile *read_view(const cc_platform *pf, const char *path, uint64_t size, cc_error *err) {
    if (cc_session_fits(pf, path, size, err)) return NULL;
    char *p = NULL;
    size_t n = 0;
    int st = pf->read_file(pf->ctx, path, cc_session_read_limit(path, NULL), &p, &n);
    if (st != CC_PF_OK) {
        snprintf(err->msg, sizeof err->msg, "%s: %s", path, st == CC_PF_NOT_FOUND ? "cannot open" : st == CC_PF_TOO_LARGE ? "too large" : "read error");
        return NULL;
    }
    cc_viewfile *vf = malloc(sizeof *vf);
    if (!vf) {
        free(p);
        snprintf(err->msg, sizeof err->msg, "out of memory");
        return NULL;
    }
    int rc = cc_viewfile_read((const uint8_t *)p, n, vf, err);
    free(p);
    if (rc) return cc_viewfile_free(vf), free(vf), NULL;
    return vf;
}

/* SIZE3: the cache at `path` mapped and read in place (its meshes' counts and places kept, not their
 * arrays: viewfile.h cc_viewfile_read_mapped); `*map` its mapping. NULL with err (then the caller reads
 * it as before) when the carrier cannot map it or it does not read. */
static cc_viewfile *map_view(const cc_platform *pf, const char *path, void **map, cc_error *err) {
    *map = NULL;
    if (!pf->map_file || !pf->unmap_file) return NULL;
    const char *p = NULL;
    size_t n = 0;
    void *h = NULL;
    if (pf->map_file(pf->ctx, path, cc_session_read_limit(path, NULL), &p, &n, &h) != CC_PF_OK) return NULL;
    cc_viewfile *vf = malloc(sizeof *vf);
    if (!vf || cc_viewfile_read_mapped((const uint8_t *)p, n, vf, err) || !vf->lazy) {
        if (vf) cc_viewfile_free(vf), free(vf);
        pf->unmap_file(pf->ctx, h);
        return NULL;
    }
    *map = h;
    return vf;
}

/* The cache-folder twin of `beside` (`ext` appended): <dir>/links/<16 hex of BLAKE3(path)>-<name><ext>. */
static int alt_path(const cc_platform *pf, const char *path, const char *ext, char *out, size_t n) {
    char dir[4096];
    if (!pf->cache_dir || pf->cache_dir(pf->ctx, dir, sizeof dir)) return -1;
    size_t dl = strlen(dir);
    if (dl + 7 >= sizeof dir) return -1;
    memcpy(dir + dl, "/links", 7);
    if (pf->make_dir && pf->make_dir(pf->ctx, dir)) return -1;
    unsigned char h[32];
    cc_blake3(path, strlen(path), h);
    char hx[65];
    hex(h, 8, hx);
    const char *sl = cc_path_last_sep(path);
    int w = snprintf(out, n, "%s/%s-%s%s", dir, hx, sl ? sl + 1 : path, ext);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

static int str_cmp(const void *x, const void *y) {
    const cc_str *a = x, *b = y;
    size_t n = a->n < b->n ? a->n : b->n;
    int c = n ? memcmp(a->p, b->p, n) : 0;
    return c ? c : (a->n > b->n) - (a->n < b->n);
}

/* The conversion report (link_cache.h): what the IFC held (report), then where the cache went (report_tail). */
static void report(Buf *b, const cc_link_job *j, const cc_ifc_scene *s, double read_ms) {
    const char *sl = cc_path_last_sep(j->path);
    buf_puts(b, "Bimble: link conversion report\n\nsource: "), buf_puts(b, sl ? sl + 1 : j->path);
    buf_puts(b, "\nsource bytes: "), buf_u64(b, j->bytes);
    buf_puts(b, "\nsource modified (s since 1970 UTC): "), buf_i64(b, j->mtime_s);
    buf_puts(b, "\nschema: "), buf_append(b, s->schema.p ? s->schema.p : "", s->schema.n);
    buf_puts(b, "\nSTEP entities parsed: "), buf_u64(b, s->entities);
    buf_puts(b, "\nproducts with a body: "), buf_u64(b, s->products);
    buf_puts(b, "\nelements drawn: "), buf_u64(b, s->scene.nelements);
    buf_puts(b, "\ntriangles: "), buf_u64(b, s->triangles);
    buf_puts(b, "\nshapes after sharing: "), buf_u64(b, s->scene.nshapes);
    buf_puts(b, "\nstoreys: "), buf_u64(b, s->nstoreys);
    buf_puts(b, "\ngrid axes: "), buf_u64(b, s->ngrids);
    buf_puts(b, "\nread time (ms): "), buf_u64(b, (uint64_t)(read_ms + 0.5));
    buf_puts(b, "\n\nelements drawn per IFC class:\n");
    size_t n = s->scene.nelements;
    cc_str *c = malloc((n ? n : 1) * sizeof *c);
    if (c) {
        for (size_t i = 0; i < n; i++) c[i] = s->info[s->scene.elements[i].model_index].class_name;
        qsort(c, n, sizeof *c, str_cmp);
        for (size_t i = 0; i < n;) {
            size_t k = i;
            while (k < n && !str_cmp(&c[k], &c[i])) k++;
            buf_puts(b, "  "), buf_append(b, c[i].p ? c[i].p : "", c[i].n), buf_puts(b, " "), buf_u64(b, k - i), buf_putc(b, '\n');
            i = k;
        }
        free(c);
    }
    buf_puts(b, "\nproducts with a body drawn empty (left out), per entity:\n");
    if (s->empty_classes.n) buf_append(b, s->empty_classes.p, s->empty_classes.n);
    else buf_puts(b, "  none\n");
    buf_puts(b, "\nskipped and why (the reader's warnings):\n");
    for (size_t i = 0; i < s->scene.nwarnings; i++)
        buf_puts(b, "  "), buf_append(b, s->scene.warnings[i].p, s->scene.warnings[i].n), buf_putc(b, '\n');
    if (!s->scene.nwarnings) buf_puts(b, "  none\n");
}

/* SIZE2: the bytes of a view file's sections of one kind (heads included). */
static uint64_t section_bytes(const Buf *vb, const char *kind) {
    uint64_t sum = 0;
    for (size_t at = 12; at + 12 <= vb->len;) {
        uint64_t len = 0;
        for (int i = 0; i < 8; i++) len |= (uint64_t)(unsigned char)vb->p[at + 4 + i] << (8 * i);
        if (len > vb->len - at - 12) break;
        if (!memcmp(vb->p + at, kind, 4)) sum += 12 + len;
        at += 12 + (size_t)len;
    }
    return sum;
}

static void report_tail(Buf *b, const cc_link_job *j, uint64_t cache_bytes, const char *cache, const char *where) {
    /* LNK3D (ADR 0078 amendment 3): the proxies baked into the cache (section HLOD) */
    buf_puts(b, "\nproxies (HLOD, baked for the 3D view): ");
    if (j->hlod_bytes) {
        buf_u64(b, j->hlod_cells), buf_puts(b, " cells, "), buf_u64(b, j->hlod_tris), buf_puts(b, " triangles, ");
        buf_u64(b, j->hlod_bytes), buf_puts(b, " bytes, built in "), buf_u64(b, (uint64_t)(j->hlod_ms + 0.5)), buf_puts(b, " ms");
    } else buf_puts(b, j->hlod_why[0] ? j->hlod_why : "none");
    if (j->hlod_bytes) { /* SIZE2: the budget and what is stored */
        buf_puts(b, "\nproxies' budget: "), buf_u64(b, cc_link_hlod_cap(j->geom_bytes)), buf_puts(b, " bytes (15 % of the geometry), ");
        buf_u64(b, j->hlod_dropped), buf_puts(b, " finer cells left without; stored gzipped: "), buf_u64(b, j->hlod_stored), buf_puts(b, " bytes");
    }
    buf_puts(b, "\ncache sections (bytes):");
    static const char *const KINDS[] = {"HEAD", "META", "GEOR", "ELEM", "SRCE", "HLOD", "PROP", "PRPZ"};
    for (size_t q = 0; q < sizeof KINDS / sizeof *KINDS; q++) {
        uint64_t x = j->sections[q];
        if (x) buf_puts(b, " "), buf_puts(b, KINDS[q]), buf_puts(b, " "), buf_u64(b, x);
    }
    if (j->psets_raw) buf_puts(b, "\nproperty sets: "), buf_u64(b, j->psets_raw), buf_puts(b, " bytes as read, stored once per distinct set and gzipped");
    buf_puts(b, "\ncache: ");
    if (cache[0]) buf_puts(b, cache), buf_puts(b, " ("), buf_u64(b, cache_bytes), buf_puts(b, " bytes)");
    else buf_puts(b, "none written");
    if (where && *where) buf_puts(b, "\n"), buf_puts(b, where);
    buf_puts(b, "\nThe cache is the IFC read for display, read only. Do not edit it; it is written again when the IFC changes.\n");
}

/* SIZE2: the proxies' byte budget of a link: 15 % of its geometry section (ELEM: the mesh codec's bytes),
 * at most CC_LINK_HLOD_MAX, at least CC_LINK_HLOD_MIN (a small link keeps its coarse cells). */
uint64_t cc_link_hlod_cap(uint64_t geom_bytes) {
    uint64_t c = geom_bytes / 100 * 15;
    if (c > CC_LINK_HLOD_MAX) c = CC_LINK_HLOD_MAX;
    if (c < CC_LINK_HLOD_MIN) c = CC_LINK_HLOD_MIN;
    return c;
}

/* SIZE2: the proxies of the finest cells dropped until those kept fit `cap` bytes (cc_hlod_proxy_bytes):
 * the coarsest cells first (depth, then the tree's order), as the page uses a released link's coarsest
 * usable proxies. The cells dropped. */
uint32_t cc_link_hlod_budget(cc_hlod *h, uint64_t cap) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < h->ncells; i++) total += cc_hlod_proxy_bytes(&h->cells[i]);
    uint32_t dropped = 0;
    for (uint32_t d = CC_HLOD_MAX_DEPTH + 1; d-- > 0 && total > cap;)
        for (uint32_t i = h->ncells; i-- > 0 && total > cap;) {
            cc_hlod_cell *c = &h->cells[i];
            if (c->depth != d || !c->nproxy) continue;
            total -= cc_hlod_proxy_bytes(c);
            c->nproxy = 0, dropped++;
        }
    return dropped;
}

/* LNK3D (ADR 0078 amendment 3): the cache's proxies, baked once here (on the conversion's thread): the
 * view file just written read back as the session will read it (its scene's element order is the
 * link's BMSH rows), its HLOD built in steps (3D2's builder and options: bounded; cancellable between
 * steps), appended as a section HLOD. A failure leaves the cache without proxies (the page draws the
 * link in detail), said in the report. */
static void bake_hlod(cc_link_job *j, Buf *vb) {
    const cc_platform *pf = j->pf;
    double t0 = pf->now_ms(pf->ctx);
    cc_viewfile vf;
    cc_scene sc;
    cc_error e;
    if (cc_viewfile_read((const uint8_t *)vb->p, vb->len, &vf, &e)) {
        snprintf(j->hlod_why, sizeof j->hlod_why, "none (%s)", e.msg);
        return;
    }
    if (cc_view_scene(&sc, &vf, &e)) {
        cc_viewfile_free(&vf);
        snprintf(j->hlod_why, sizeof j->hlod_why, "none (%s)", e.msg);
        return;
    }
    cc_hlod h;
    cc_hlod_opts o;
    cc_hlod_default(&o);
    o.max_proxy_triangles = CC_LINK_HLOD_PROXY_TRIS; /* SIZE2: a coarser stand-in per cell (the grid coarsens to fit) */
    Buf b;
    buf_init(&b);
    int rc = cc_hlod_start(&h, &sc, &o, &e);
    while (rc == 0 && !cc_atomic_load(&j->pg.cancel)) {
        int st = cc_hlod_step(&h, 1000000, &e);
        if (st < 0) rc = -1;
        if (st != 0) break;
    }
    if (rc == 0 && h.work) snprintf(j->hlod_why, sizeof j->hlod_why, "none (cancelled)"), rc = 1;
    if (rc == 0) j->hlod_dropped = cc_link_hlod_budget(&h, cc_link_hlod_cap(j->geom_bytes)); /* SIZE2: coarser cells first */
    if (rc == 0 && cc_hlod_bprx(&h, 0, (uint32_t)sc.nelements, &b, &e)) rc = -1;
    if (rc == 0 && sc.nelements && h.ncells) {
        j->hlod_cells = h.ncells;
        for (uint32_t i = 0; i < h.ncells; i++) j->hlod_tris += cc_hlod_proxy_triangles(&h.cells[i]);
        Buf z; /* SIZE2: one gzip member (read once per link, sequentially) */
        buf_init(&z);
        int bad = cc_gzip((const unsigned char *)b.p, b.len, &z) || z.oom || cc_viewfile_append_hlod(vb, z.p, z.len);
        if (bad) j->hlod_cells = j->hlod_tris = 0;
        else j->hlod_bytes = b.len, j->hlod_stored = z.len;
        buf_free(&z);
    } else if (rc < 0) snprintf(j->hlod_why, sizeof j->hlod_why, "none (%s)", e.msg);
    else if (!j->hlod_why[0]) snprintf(j->hlod_why, sizeof j->hlod_why, "none (nothing drawn)");
    buf_free(&b);
    cc_hlod_free(&h);
    cc_scene_free(&sc);
    cc_viewfile_free(&vf);
    j->hlod_ms = pf->now_ms(pf->ctx) - t0;
}

/* ---- SIZE3 (E): the proxies baked after the link is ready ----
 * The cache at j->cache (written by the conversion with SRCE "complete": false, read untrusted again here) rebuilt as
 * its sections but the lazy ones, SRCE marked complete, its proxies baked (HLOD), then its property sets (PRPZ, PROP:
 * last, read lazily); written in place (tmp + rename: a crash leaves the incomplete cache, completed at the next open);
 * its report appended; the completed cache read in place for the session. Cancellable between proxy steps. */
static void sleep_cancellable(cc_link_job *j, double ms) {
    const cc_platform *pf = j->pf;
    double t0 = pf->now_ms(pf->ctx);
    while (pf->now_ms(pf->ctx) - t0 < ms && !cc_atomic_load(&j->pg.cancel))
        for (volatile unsigned long spin = 0; spin < 200000ul; spin++) {} /* (no sleep in the platform: tests only) */
}

static void index_run(cc_link_job *j) {
    const cc_platform *pf = j->pf;
    if (j->index_delay_ms > 0) sleep_cancellable(j, j->index_delay_ms);
    if (cc_atomic_load(&j->pg.cancel)) return fail(j, CC_JOB_CANCELLED, NULL);
    char *p = NULL;
    size_t n = 0;
    if (pf->read_file(pf->ctx, j->cache, cc_session_read_limit(j->cache, NULL), &p, &n) != CC_PF_OK)
        return fail(j, CC_JOB_FAILED, "the cache cannot be read again");
    Buf vb, tail, lg;
    buf_init(&vb), buf_init(&tail), buf_init(&lg);
    int bad = n < 12 || memcmp(p, "BIMBLEVW", 8) != 0, have_src = 0;
    if (!bad) buf_append(&vb, p, 12);
    for (size_t at = 12; !bad && at < n;) { /* every section checked against what is there */
        if (n - at < 12) { bad = 1; break; }
        uint64_t len = 0;
        for (int i = 0; i < 8; i++) len |= (uint64_t)(unsigned char)p[at + 4 + i] << (8 * i);
        if (len > n - at - 12) { bad = 1; break; }
        const char *kind = p + at;
        if (!memcmp(kind, "SRCE", 4)) { /* its JSON text (a string: varint length, bytes) marked complete */
            char *src = NULL;
            size_t sn = 0;
            {
                size_t q = 0, shift = 0;
                uint64_t ln = 0;
                const unsigned char *s = (const unsigned char *)p + at + 12;
                while (q < len && q < 10) { ln |= (uint64_t)(s[q] & 0x7f) << shift; shift += 7; if (!(s[q++] & 0x80)) break; }
                if (ln == len - q) src = (char *)s + q, sn = (size_t)ln;
            }
            char out[1024];
            if (!src || sn + 1 > sizeof out || cc_link_cache_mark_complete(src, sn, out, sizeof out)) bad = 1;
            else if (cc_viewfile_append_source(&vb, out, strlen(out))) bad = 1;
            have_src = 1;
        } else if (!memcmp(kind, "HLOD", 4)) { /* (none expected: dropped, baked again) */
        } else if (!memcmp(kind, "PROP", 4) || !memcmp(kind, "PRPZ", 4)) buf_append(&tail, p + at, 12 + (size_t)len);
        else buf_append(&vb, p + at, 12 + (size_t)len);
        at += 12 + (size_t)len;
    }
    free(p);
    if (bad || !have_src || vb.oom || tail.oom) {
        buf_free(&vb), buf_free(&tail);
        return fail(j, CC_JOB_FAILED, "the cache is damaged: converted again at the next open");
    }
    j->geom_bytes = section_bytes(&vb, "ELEM");
    bake_hlod(j, &vb);
    if (cc_atomic_load(&j->pg.cancel)) {
        buf_free(&vb), buf_free(&tail);
        return fail(j, CC_JOB_CANCELLED, NULL);
    }
    buf_append(&vb, tail.p, tail.len);
    buf_free(&tail);
    if (vb.oom || pf->write_file(pf->ctx, j->cache, vb.p, vb.len)) {
        buf_free(&vb);
        return fail(j, CC_JOB_FAILED, "the completed cache cannot be written");
    }
    { /* its report: the proxies' lines appended */
        static const char *const KINDS[] = {"HEAD", "META", "GEOR", "ELEM", "SRCE", "HLOD", "PROP", "PRPZ"};
        for (size_t q = 0; q < sizeof KINDS / sizeof *KINDS; q++) j->sections[q] = section_bytes(&vb, KINDS[q]);
        j->cache_bytes = vb.len;
        char *old = NULL;
        size_t on = 0;
        if (j->log[0] && pf->read_file(pf->ctx, j->log, (size_t)4 << 20, &old, &on) == CC_PF_OK) {
            buf_append(&lg, old, on);
            buf_puts(&lg, "\n\nindexed after the link was ready (SIZE3):");
            report_tail(&lg, j, vb.len, j->cache, "");
            if (!lg.oom) pf->write_file(pf->ctx, j->log, lg.p, lg.len);
        }
        free(old);
        buf_free(&lg);
    }
    cc_pf_stat cs;
    if (pf->stat_path(pf->ctx, j->cache, 1, &cs) == CC_PF_OK) j->cache_mtime_s = cs.mtime_s;
    cc_error me;
    cc_viewfile *mv = map_view(pf, j->cache, &j->map, &me);
    if (!mv) {
        mv = malloc(sizeof *mv);
        if (!mv || cc_viewfile_read((const uint8_t *)vb.p, vb.len, mv, &j->err)) {
            if (mv) cc_viewfile_free(mv), free(mv);
            buf_free(&vb);
            return fail(j, CC_JOB_FAILED, mv ? NULL : "out of memory");
        }
    }
    buf_free(&vb);
    j->vf = mv;
}

static void run(cc_link_job *j) {
    if (j->index_only) { /* SIZE3 (E) */
        index_run(j);
        return;
    }
    const cc_platform *pf = j->pf;
    cc_pf_stat st;
    int have_stat = pf->stat_path && pf->stat_path(pf->ctx, j->path, 1, &st) == CC_PF_OK && st.kind == CC_PF_FILE;
    if (have_stat) j->bytes = st.size, j->mtime_s = st.mtime_s;
    if (ends_with_ci(j->path, ".bimbleview")) {
        if (!(j->vf = read_view(pf, j->path, j->bytes, &j->err))) fail(j, CC_JOB_FAILED, NULL);
        return;
    }
    int cached = have_stat && j->bytes >= j->cache_min && pf->files;
    char ends[65] = "", beside[4096], alt[4096] = "", alt_log[4096] = "";
    int ends_known = 0;
    if (cached && pf->read_range) { /* the IFC's ends, without reading it */
        char *h = malloc(2 * ENDS);
        size_t nh = 0, nt = 0;
        if (h && pf->read_range(pf->ctx, j->path, 0, ENDS, h, &nh) == CC_PF_OK && nh == (j->bytes < ENDS ? j->bytes : ENDS) &&
            pf->read_range(pf->ctx, j->path, j->bytes > ENDS ? j->bytes - ENDS : 0, ENDS, h + ENDS, &nt) == CC_PF_OK &&
            nt == nh && !ends_hash(h, nh, h + ENDS, nt, j->bytes, ends))
            ends_known = 1;
        free(h);
    }
    if (cached) {
        if (j->no_beside) beside[0] = 0;
        else if ((size_t)snprintf(beside, sizeof beside, "%s.bimbleview", j->path) >= sizeof beside) cached = 0;
        if (alt_path(pf, j->path, ".bimbleview", alt, sizeof alt) || alt_path(pf, j->path, ".log.txt", alt_log, sizeof alt_log))
            alt[0] = alt_log[0] = 0;
    }
    for (int c = 0; cached && c < 2; c++) { /* a cache that matches: loaded instead of the IFC */
        const char *cp = c ? alt : beside;
        cc_pf_stat cs;
        if (!cp[0] || pf->stat_path(pf->ctx, cp, 1, &cs) != CC_PF_OK || cs.kind != CC_PF_FILE) continue;
        cc_error e;
        void *map = NULL;
        cc_viewfile *vf = cc_session_fits(pf, cp, cs.size, &e) ? NULL : map_view(pf, cp, &map, &e); /* SIZE3: in place */
        if (!vf) vf = read_view(pf, cp, cs.size, &e);
        if (vf && vf->has_source && cc_link_cache_matches(vf->source.p, vf->source.n, j->bytes, j->mtime_s, ends, ends_known)) {
            j->vf = vf, j->from_cache = 1, j->map = map;
            j->incomplete = !cc_link_cache_complete(vf->source.p, vf->source.n); /* SIZE3 (E): its proxies to bake */
            snprintf(j->cache, sizeof j->cache, "%s", cp);
            if (c) snprintf(j->log, sizeof j->log, "%s", alt_log);
            else snprintf(j->log, sizeof j->log, "%s.log.txt", j->path);
            j->cache_bytes = cs.size, j->cache_mtime_s = cs.mtime_s;
            if (!ends_known) snprintf(j->note, sizeof j->note, "the IFC could not be read: its cache serves (same size and time)");
            cc_atomic_store(&j->pg.done, (size_t)1000);
            return;
        }
        snprintf(j->note, sizeof j->note, "%s", vf ? "the cache was of another version of the IFC: read again" : "the cache was damaged: read again");
        if (vf) cc_viewfile_free(vf), free(vf);
        if (map) pf->unmap_file(pf->ctx, map);
    }
    /* read the IFC */
    int fits = have_stat ? cc_session_fits(pf, j->path, j->bytes, &j->err) : 0;
    if (fits) {
        fail(j, fits == -2 ? CC_JOB_REFUSED : CC_JOB_FAILED, NULL);
        return;
    }
    double t0 = pf->now_ms(pf->ctx);
    char *text = NULL;
    size_t n = 0;
    /* SIZE2: mapped when the carrier can (no copy of the text on the heap: the page cache's pages), else read */
    void *mh = NULL;
    const char *mapped = NULL;
    int rs = pf->map_file && pf->unmap_file ? pf->map_file(pf->ctx, j->path, CC_IFC_MAX_FILE_BYTES, &mapped, &n, &mh) : CC_PF_IO;
    if (rs == CC_PF_OK) text = (char *)mapped;
    else if (rs != CC_PF_TOO_LARGE) n = 0, rs = pf->read_file(pf->ctx, j->path, CC_IFC_MAX_FILE_BYTES, &text, &n);
    if (rs != CC_PF_OK) {
        snprintf(j->err.msg, sizeof j->err.msg, "%s: %s", j->path,
                 rs == CC_PF_NOT_FOUND ? "cannot open" : rs == CC_PF_TOO_LARGE ? "too large" : rs == CC_PF_IO ? "read error" : "out of memory");
        fail(j, CC_JOB_FAILED, NULL);
        return;
    }
    if (!have_stat) j->bytes = n;
    char src[512] = "";
    cc_ifc_scene *s = malloc(sizeof *s);
    int rc = s ? cc_ifc_read_progress(text, n, s, &j->err, &j->pg) : -1;
    if (!rc && cached && cc_link_cache_source(j->bytes, j->mtime_s, text, n < ENDS ? n : ENDS, n > ENDS ? text + n - ENDS : text,
                                              n < ENDS ? n : ENDS, s->base_point_mm, src, sizeof src))
        cached = 0;
    Buf psb; /* M1LINKS: the property sets, read beside the display (ifc_psets.h); none when they cannot be */
    buf_init(&psb);
    if (!rc) {
        cc_error pe;
        if (cc_ifc_psets(text, n, s, &psb, &pe) || psb.oom) buf_free(&psb), buf_init(&psb);
    }
    if (mh) pf->unmap_file(pf->ctx, mh); /* SIZE2 */
    else free(text); /* the scene owns its strings (ifc_read.h) */
    if (!s) snprintf(j->err.msg, sizeof j->err.msg, "out of memory");
    if (rc) {
        if (s) cc_ifc_scene_free(s), free(s);
        buf_free(&psb);
        fail(j, cc_atomic_load(&j->pg.cancel) ? CC_JOB_CANCELLED : CC_JOB_FAILED, NULL);
        return;
    }
    double read_ms = pf->now_ms(pf->ctx) - t0;
    if (!cached) { /* small: kept as read */
        j->ifc = s;
        j->psets = psb.p, j->npsets = psb.len; /* M1LINKS: in memory */
        return;
    }
    cc_viewfile_meta mt;
    Buf vb, lg;
    buf_init(&vb), buf_init(&lg);
    int wr = cc_ifc_scene_meta(s, &mt) || cc_viewfile_write_meta(&mt, &s->scene, 0, &vb);
    if (!wr) j->geom_bytes = section_bytes(&vb, "ELEM"); /* SIZE2: the proxies' budget follows it */
    wr = wr || cc_viewfile_append_source(&vb, src, strlen(src));
    cc_viewfile_meta_free(&mt);
    report(&lg, j, s, read_ms);
    cc_ifc_scene_free(s), free(s); /* LNK3D: the IFC's scene goes before the proxies are built (its peak) */
    /* SIZE3 (E): the proxies are baked after the link is ready (index_run), not here: the link draws sooner */
    snprintf(j->hlod_why, sizeof j->hlod_why, "none yet (baked after the link is ready: see below)");
    if (!wr && psb.len) { /* M1LINKS: last, read lazily; SIZE2: packed (each set once, in gzip blocks) */
        Buf pk;
        buf_init(&pk);
        if (cc_psets_pack((const uint8_t *)psb.p, psb.len, &pk) == 0 && !pk.oom) wr = cc_viewfile_append_psets_packed(&vb, pk.p, pk.len);
        else wr = cc_viewfile_append_psets(&vb, psb.p, psb.len); /* (a table the packer refuses: as it is) */
        j->psets_raw = psb.len;
        buf_free(&pk);
    }
    buf_free(&psb);
    if (wr || vb.oom) {
        buf_free(&vb), buf_free(&lg);
        fail(j, CC_JOB_FAILED, "out of memory");
        return;
    }
    const char *where = "";
    char logp[4096];
    if (beside[0] && pf->write_file(pf->ctx, beside, vb.p, vb.len) == 0) {
        snprintf(j->cache, sizeof j->cache, "%s", beside);
        snprintf(logp, sizeof logp, "%s.log.txt", j->path);
    } else if (alt[0] && pf->write_file(pf->ctx, alt, vb.p, vb.len) == 0) {
        snprintf(j->cache, sizeof j->cache, "%s", alt);
        snprintf(logp, sizeof logp, "%s", alt_log);
        where = beside[0] ? "The IFC's folder could not be written: the cache is in the user's cache folder."
                          : "The cache is in the cache folder (asked: nothing written beside the IFC).";
        snprintf(j->note, sizeof j->note, "%s", beside[0] ? "cache in the user's cache folder (the IFC's folder is read only)" : "cache in the cache folder");
    } else {
        logp[0] = 0;
        snprintf(j->note, sizeof j->note, "no cache written (neither the IFC's folder nor the cache folder could be written)");
    }
    {
        static const char *const KINDS[] = {"HEAD", "META", "GEOR", "ELEM", "SRCE", "HLOD", "PROP", "PRPZ"};
        for (size_t q = 0; q < sizeof KINDS / sizeof *KINDS; q++) j->sections[q] = section_bytes(&vb, KINDS[q]);
    }
    if (j->cache[0]) {
        cc_pf_stat cs;
        j->cache_bytes = vb.len;
        if (pf->stat_path(pf->ctx, j->cache, 1, &cs) == CC_PF_OK) j->cache_mtime_s = cs.mtime_s;
        report_tail(&lg, j, vb.len, j->cache, where);
        if (!lg.oom && pf->write_file(pf->ctx, logp, lg.p, lg.len) == 0) snprintf(j->log, sizeof j->log, "%s", logp);
    }
    buf_free(&lg); /* the session keeps the light scene only (the IFC's went above) */
    j->incomplete = j->cache[0] != 0; /* SIZE3 (E): written without proxies: the session queues its indexing */
    if (j->cache[0]) { /* SIZE3: the cache just written, read in place (its meshes stay in the file) */
        cc_error me;
        cc_viewfile *mv = map_view(pf, j->cache, &j->map, &me);
        if (mv) {
            buf_free(&vb);
            j->vf = mv;
            return;
        }
    }
    cc_viewfile *vf = malloc(sizeof *vf);
    if (!vf || cc_viewfile_read((const uint8_t *)vb.p, vb.len, vf, &j->err)) {
        if (vf) cc_viewfile_free(vf), free(vf);
        buf_free(&vb);
        fail(j, CC_JOB_FAILED, vf ? NULL : "out of memory");
        return;
    }
    buf_free(&vb);
    j->vf = vf;
}

void cc_link_job_run(void *p) {
    cc_link_job *j = p;
    double t0 = j->pf->now_ms(j->pf->ctx);
    j->rc = CC_JOB_OK;
    run(j);
    if (j->rc == CC_JOB_OK) cc_atomic_store(&j->pg.done, (size_t)1000);
    j->ms = j->pf->now_ms(j->pf->ctx) - t0;
#if !defined(__wasm__)
    int exp = 0;
    if (!__atomic_compare_exchange_n(&j->finished, &exp, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        cc_link_job_free(j); /* orphaned (2): the session went, nobody takes the results */
#else
    j->finished = 1;
#endif
}

int cc_link_job_done(cc_link_job *j) {
#if !defined(__wasm__)
    return __atomic_load_n(&j->finished, __ATOMIC_ACQUIRE) == 1;
#else
    return j->finished == 1;
#endif
}

void cc_link_job_orphan(cc_link_job *j) {
    cc_atomic_store(&j->pg.cancel, 1);
#if !defined(__wasm__)
    int exp = 0;
    if (__atomic_compare_exchange_n(&j->finished, &exp, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return; /* it frees itself */
#endif
    cc_link_job_free(j);
}
