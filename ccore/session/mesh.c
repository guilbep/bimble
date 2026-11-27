/* GET /mesh: the render scene (src/scene.h) as BMSH, for the lab's WebGL2 viewport
 * (web-ui/src/glviewport.ts; docs/research/c-core-spec.md §2).
 *
 * BMSH, version 1. Little-endian, no padding between blocks; offsets in bytes.
 *   0   char[4]  "BMSH"
 *   4   u32      version (1)
 *   8   u32      rev: the model revision the mesh was built from (also the X-Rev header)
 *   12  u32      nE  elements
 *   16  u32      nV  vertices
 *   20  u32      nI  indices (3 per triangle)
 *   24  u32      nS  edge segments
 *   28  f64[3]   origin_mm: world mm of the scene origin (scene.rs:211)
 *   52  nE rows of 48 bytes, model order (sorted by GlobalId, empty meshes dropped):
 *         +0  char[22] GlobalId
 *         +22 u8       MaterialCategory index (concrete 0 … other 7, model.rs:49-58)
 *         +23 u8       0 (pad)
 *         +24 u32[6]   first vertex, vertex count, first index, index count,
 *                      first segment, segment count
 *   then f32[3] x nV   positions, mm relative to origin_mm (cc_scene_world_mesh)
 *   then u32 x nI      indices, absolute (first vertex of the element already added)
 *   then f32[6] x nS   edge segments (a xyz, b xyz), scene coordinates
 *   then i8[8] x nS    the two face normals of each segment (snorm8 xyz w, xyz w; 0 = unknown)
 * The positions block starts at 52 + 48 nE, a multiple of 4: a reader may view the positions,
 * indices and edges as Float32Array / Uint32Array over the same buffer.
 * The host is little-endian (x86-64, aarch64); the bytes are written with memcpy.
 * With linked models (links.h), the LNKS trailer follows: one BMSH per link. Without, nothing
 * follows (the bytes above, unchanged). */
#ifdef __clang__
#pragma clang fp contract(off) /* SIZE3: the boxes in the page's f32 arithmetic */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ifc_read.h"
#include "links.h"
#include "link_cache.h"
#include "inflate.h"
#include "mesh.h"
#include "scene.h"
#include "scene_bmsh.h"
#include "meshcodec.h" /* SIZE3 */
#include "tiles.h"     /* TILE1 */
#include "json_w.h"
#include "session.h"

static void put_u32(Buf *b, uint32_t v) { buf_append(b, &v, 4); }

static int fail(cc_error *err, const char *msg) {
    err->offset = CC_NO_OFFSET;
    snprintf(err->msg, sizeof err->msg, "%s", msg);
    return -1;
}

/* BMSH of a built scene. */
static int bmsh_of(const cc_scene *sp, uint32_t rev, Buf *out, cc_error *err) {
    const cc_scene s = *sp;
    int rc = -1;
    Arena tmp;
    arena_init(&tmp, 0);
    size_t ne = s.nelements;
    cc_mesh *w = ne ? arena_alloc(&tmp, ne * sizeof *w, 16) : NULL;
    if (ne && !w) { fail(err, "out of memory"); goto done; }
    uint64_t nv = 0, ni = 0, ns = 0;
    for (size_t k = 0; k < ne; k++) {
        if (cc_scene_world_mesh(&s, k, &tmp, &w[k]) != 0) { fail(err, "out of memory or a mesh not decoded"); goto done; }
        /* nn == ne for meshes built by the C core; a segment without normals gets zeros. */
        nv += w[k].nv, ni += w[k].ni, ns += w[k].ne;
    }
    if (ne > UINT32_MAX || nv > UINT32_MAX || ni > UINT32_MAX || ns > UINT32_MAX) {
        fail(err, "scene too large for BMSH");
        goto done;
    }
    if (buf_reserve(out, 52 + 48 * ne + 12 * nv + 4 * ni + 32 * ns) != 0) { fail(err, "out of memory"); goto done; }
    buf_append(out, "BMSH", 4);
    put_u32(out, CC_BMSH_VERSION);
    put_u32(out, rev);
    put_u32(out, (uint32_t)ne), put_u32(out, (uint32_t)nv), put_u32(out, (uint32_t)ni), put_u32(out, (uint32_t)ns);
    buf_append(out, s.origin, 24);
    uint32_t fv = 0, fi = 0, fs = 0;
    for (size_t k = 0; k < ne; k++) {
        const cc_scene_element *e = &s.elements[k];
        buf_append(out, e->id.c, 22);
        buf_putc(out, (char)(unsigned char)e->category);
        buf_putc(out, 0);
        put_u32(out, fv), put_u32(out, w[k].nv), put_u32(out, fi), put_u32(out, w[k].ni);
        put_u32(out, fs), put_u32(out, w[k].ne);
        fv += w[k].nv, fi += w[k].ni, fs += w[k].ne;
    }
    for (size_t k = 0; k < ne; k++) buf_append(out, w[k].pos, (size_t)w[k].nv * 12);
    fv = 0;
    for (size_t k = 0; k < ne; k++) {
        for (uint32_t i = 0; i < w[k].ni; i++) {
            uint32_t ix = w[k].idx[i];
            if (ix >= w[k].nv) { fail(err, "mesh index out of range"); goto done; }
            put_u32(out, fv + ix);
        }
        fv += w[k].nv;
    }
    for (size_t k = 0; k < ne; k++) buf_append(out, w[k].edges, (size_t)w[k].ne * 24);
    static const int8_t zero[8];
    for (size_t k = 0; k < ne; k++)
        for (uint32_t i = 0; i < w[k].ne; i++) buf_append(out, i < w[k].nn ? w[k].normals[i] : zero, 8);
    if (out->oom) { fail(err, "out of memory"); goto done; }
    rc = 0;
done:
    arena_free(&tmp);
    return rc;
}

int cc_mesh_bmsh(const cc_model *m, uint32_t rev, Buf *out, cc_error *err) {
    cc_scene s;
    int rc = cc_scene_build(&s, m, err) != 0 ? -1 : bmsh_of(&s, rev, out, err);
    cc_scene_free(&s);
    return rc;
}

/* 3D1: a link's ids in BMSH version 3 (links.c cc_link_id). */
static void link_bmsh_id(const void *ctx, const cc_scene *sc, size_t k, char out[22]) {
    (void)sc;
    cc_link_id(ctx, k, out);
}

/* LNK3D (ADR 0078 amendment 3, DEVIATIONS 39): a link's hidden elements stay in its BMSH (M1LINKS
 * left them out, and every change of what is hidden made the page fetch the whole mesh again); the
 * page hides them by their ids (query visibility_sets) in its per-element draw state. */

/* The trailer's light fields (visible, placement, colour), hashed (FNV-1a): a change of them alone
 * writes the trailer again for a reader that asks, without bumping links_mesh_gen. */
static uint64_t links_head(cc_session *s) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < s->nlinks; i++) {
        const cc_link *k = &s->links[i];
        double c, sn, t[3];
        cc_link_placement(s, k, &c, &sn, t);
        double v[6] = {c, sn, t[0], t[1], t[2], (double)k->colour + 2.0 * (k->visible && k->kind)};
        const unsigned char *p = (const unsigned char *)v;
        for (size_t j = 0; j < sizeof v; j++) h = (h ^ p[j]) * 1099511628211ull;
    }
    return h;
}

/* SIZE2: the LNKS trailer, version 3 (`form` CC_MESH_FORM_LAZY): each link's head without its mesh, which
 * the page asks on its own (GET /mesh?link=k, cc_link_mesh: BMSH version 4) when it needs it:
 *   "LNKS", u32 3, u32 n; per link: u32 L, the name (L bytes, padded to 4), u32 flags (1: drawn),
 *   f64 cos, f64 sin, f64[3] the placed origin, i32 colour (as version 2, always), u32 nE its elements
 *   (0: nothing to ask), u32 its mesh key (links_mesh_gen: an answer of another key is stale). */
static int links_lazy(cc_session *s, Buf *out) {
    buf_append(out, "LNKS", 4);
    put_u32(out, 3);
    put_u32(out, (uint32_t)s->nlinks);
    for (size_t i = 0; i < s->nlinks; i++) {
        const cc_link *k = &s->links[i];
        size_t L = strlen(k->name);
        put_u32(out, (uint32_t)L);
        buf_append(out, k->name, L);
        static const char zero[4];
        buf_append(out, zero, (4 - L % 4) % 4);
        put_u32(out, k->visible && k->kind ? 1u : 0u);
        double c = 1, sn = 0, t[3] = {0, 0, 0}, o[3] = {0, 0, 0};
        if (k->kind) {
            cc_link_placement(s, k, &c, &sn, t);
            const double *lo = cc_link_rows(k)->origin; /* SIZE3: no mesh decoded for the trailer */
            o[0] = c * lo[0] - sn * lo[1] + t[0], o[1] = sn * lo[0] + c * lo[1] + t[1], o[2] = lo[2] + t[2];
        }
        buf_append(out, &c, 8), buf_append(out, &sn, 8);
        for (int q = 0; q < 3; q++) buf_append(out, &o[q], 8);
        int32_t col = k->colour;
        buf_append(out, &col, 4);
        put_u32(out, k->kind ? (uint32_t)cc_link_rows(k)->nelements : 0u);
        put_u32(out, (uint32_t)s->links_mesh_gen);
    }
    return out->oom ? -1 : 0;
}

/* links.h: the LNKS trailer (`form` 3: each link's BMSH in version 3). */
static int links_bmsh(cc_session *s, uint32_t rev, int form, Buf *out, cc_error *err) {
    if (!s->nlinks) return 0;
    if (form == CC_MESH_FORM_LAZY) return links_lazy(s, out) ? fail(err, "out of memory") : 0;
    /* LINKF: version 2 when a link is placed or coloured (links.h); else version 1, as before */
    int v2 = 0;
    for (size_t i = 0; i < s->nlinks; i++) {
        const cc_link *k = &s->links[i];
        double c, sn, t[3];
        cc_link_placement(s, k, &c, &sn, t);
        if (k->colour >= 0 || (k->kind && !(c == 1.0 && sn == 0.0 && t[0] == 0.0 && t[1] == 0.0 && t[2] == 0.0))) v2 = 1;
    }
    buf_append(out, "LNKS", 4);
    put_u32(out, v2 ? 2 : 1);
    put_u32(out, (uint32_t)s->nlinks);
    Buf one;
    buf_init(&one);
    int rc = 0;
    for (size_t i = 0; i < s->nlinks && !rc; i++) {
        const cc_link *k = &s->links[i];
        size_t L = strlen(k->name);
        put_u32(out, (uint32_t)L);
        buf_append(out, k->name, L);
        static const char zero[4];
        buf_append(out, zero, (4 - L % 4) % 4);
        put_u32(out, k->visible && k->kind ? 1u : 0u); /* LNK1: a missing link (kind 0) draws nothing */
        if (v2) { /* LINKF: the turn about Z (cos, sin), the placed origin (host world mm), the colour */
            double c, sn, t[3], o[3];
            cc_link_placement(s, k, &c, &sn, t);
            const double *lo = cc_link_rows(k)->origin;
            o[0] = c * lo[0] - sn * lo[1] + t[0], o[1] = sn * lo[0] + c * lo[1] + t[1], o[2] = lo[2] + t[2];
            buf_append(out, &c, 8), buf_append(out, &sn, 8);
            for (int q = 0; q < 3; q++) buf_append(out, &o[q], 8);
            int32_t col = k->colour;
            buf_append(out, &col, 4);
        }
        buf_clear(&one);
        one.oom = 0;
        if (form == 3) rc = cc_scene_bmsh3(cc_link_raw_scene(k), link_bmsh_id, k, rev, &one, err);
        else rc = k->kind == CC_LINK_IFC ? cc_ifc_scene_bmsh(k->ifc, rev, &one, err) : bmsh_of(cc_link_raw_scene(k), rev, &one, err); /* SIZE3: decoded first */
        if (!rc && (one.oom || one.len > UINT32_MAX)) rc = fail(err, "out of memory");
        if (!rc) put_u32(out, (uint32_t)one.len), buf_append(out, one.p, one.len);
    }
    buf_free(&one);
    if (!rc && out->oom) rc = fail(err, "out of memory");
    return rc;
}

int cc_links_bmsh(cc_session *s, uint32_t rev, Buf *out, cc_error *err) { return links_bmsh(s, rev, 1, out, err); }

/* ---- the cached answer (in the session) ---- */

int cc_mesh_get(cc_session *s, const char **p, size_t *n, uint32_t *rev, double *build_ms) {
    return cc_mesh_get_form(s, 1, p, n, rev, build_ms);
}

int cc_mesh_get_form(cc_session *s, int form, const char **p, size_t *n, uint32_t *rev, double *build_ms) {
    cc_error err;
    if (form != 3 && form != CC_MESH_FORM_LAZY) form = 1;
    *build_ms = 0;
    Buf *cache = &s->mesh;
    const cc_model *m = cc_mesh_model(s, rev, &err);
    if (!m && !s->view) {
        buf_clear(cache);
        s->mesh_status = 0; /* not cached: a later model is picked up */
        buf_puts(cache, err.msg);
        *p = buf_cstr(cache), *n = cache->len;
        return 404;
    }
    if (!s->mesh_status || s->mesh_model != m || s->mesh_rev != *rev || s->mesh_links_mesh_gen != s->links_mesh_gen ||
        s->mesh_form != form || (s->nlinks && s->mesh_links_head != links_head(s))) {
        double t0 = s->pf->now_ms(s->pf->ctx);
        buf_clear(cache);
        cache->oom = 0;
        /* The session's scene of this rev (calls.c builds it once per rev for query status),
         * not a second build; when it has none (or it failed), a build here gives the error. */
        const cc_scene *sc = cc_session_scene(s);
        int rc;
        if (form == 3 || form == CC_MESH_FORM_LAZY) { /* 3D1: shapes once */
            cc_scene own;
            int built = 0;
            if (s->view == CC_VIEW_IFC) rc = cc_ifc_scene_bmsh3(s->ifc, *rev, cache, &err);
            else if (sc) rc = cc_scene_bmsh3(sc, NULL, NULL, *rev, cache, &err);
            else {
                built = 1;
                rc = cc_scene_build(&own, m, &err) != 0 ? -1 : cc_scene_bmsh3(&own, NULL, NULL, *rev, cache, &err);
            }
            if (built) cc_scene_free(&own);
        } else if (s->view == CC_VIEW_IFC) rc = cc_ifc_scene_bmsh(s->ifc, *rev, cache, &err); /* the file's id text */
        else rc = sc ? bmsh_of(sc, *rev, cache, &err) : cc_mesh_bmsh(m, *rev, cache, &err);
        if (rc == 0) rc = links_bmsh(s, *rev, form, cache, &err); /* nothing without links */
        if (rc != 0) {
            buf_clear(cache);
            cache->oom = 0;
            buf_puts(cache, err.msg);
            s->mesh_status = 500;
        } else {
            s->mesh_status = 200;
        }
        s->mesh_model = m, s->mesh_rev = *rev, s->mesh_links_mesh_gen = s->links_mesh_gen, s->mesh_form = form;
        s->mesh_links_head = s->nlinks ? links_head(s) : 0; /* LNK3D */
        *build_ms = s->pf->now_ms(s->pf->ctx) - t0;
    }
    *p = cache->p ? cache->p : "", *n = cache->len;
    return s->mesh_status;
}

/* ---- 3D2 (ADR 0078): the HLOD proxies, in steps (mesh.h) ---- */

void cc_mesh_proxies_free(cc_session *s) {
    cc_mesh_tiles_free(s); /* TILE1: built on the same scene */
    if (s->hlod) cc_hlod_free(s->hlod);
    free(s->hlod);
    buf_free(&s->hlod_out);
    s->hlod = NULL, s->hlod_scene = NULL, s->hlod_status = 0, s->hlod_rev = 0, s->hlod_ms = 0;
}

int cc_mesh_proxies(cc_session *s, uint64_t budget, const char **p, size_t *n, uint32_t *rev, double *ms) {
    cc_error err;
    const cc_model *m = cc_mesh_model(s, rev, &err);
    const cc_scene *sc = m || s->view ? cc_session_scene(s) : NULL; /* the open model's, an IFC's, a view file's */
    if (!sc) {
        cc_mesh_proxies_free(s);
        buf_puts(&s->hlod_out, "no scene");
        *p = buf_cstr(&s->hlod_out), *n = s->hlod_out.len, *ms = 0;
        return 404;
    }
    if (s->hlod_scene != sc || s->hlod_rev != *rev || !s->hlod_status) {
        cc_mesh_proxies_free(s);
        s->hlod_scene = sc, s->hlod_rev = *rev;
        s->hlod = calloc(1, sizeof *s->hlod);
        cc_hlod_opts o;
        cc_hlod_default(&o);
        double t0 = s->pf->now_ms(s->pf->ctx);
        if (!s->hlod) s->hlod_status = 500, buf_puts(&s->hlod_out, "out of memory");
        else if (cc_hlod_start(s->hlod, sc, &o, &err)) s->hlod_status = 500, buf_puts(&s->hlod_out, err.msg);
        else s->hlod_status = 202;
        s->hlod_ms += s->pf->now_ms(s->pf->ctx) - t0;
    }
    if (s->hlod_status == 202 && !budget) { /* the tree alone (hlod.h cc_hlod_bprx_tree), no step */
        buf_clear(&s->hlod_out);
        s->hlod_out.oom = 0;
        int rc = cc_hlod_bprx_tree(s->hlod, *rev, (uint32_t)sc->nelements, &s->hlod_out, &err);
        *ms = s->hlod_ms;
        if (rc) buf_clear(&s->hlod_out), buf_puts(&s->hlod_out, err.msg);
        *p = s->hlod_out.p ? s->hlod_out.p : "", *n = s->hlod_out.len;
        return rc ? 500 : 200;
    }
    if (s->hlod_status == 202) {
        double t0 = s->pf->now_ms(s->pf->ctx);
        int r = cc_hlod_step(s->hlod, budget, &err);
        buf_clear(&s->hlod_out);
        s->hlod_out.oom = 0;
        if (r < 0) s->hlod_status = 500, buf_puts(&s->hlod_out, err.msg);
        else if (r == 0) buf_u64(&s->hlod_out, s->hlod->read), buf_putc(&s->hlod_out, ' '), buf_u64(&s->hlod_out, s->hlod->total);
        else if (cc_hlod_bprx(s->hlod, *rev, (uint32_t)sc->nelements, &s->hlod_out, &err)) {
            buf_clear(&s->hlod_out);
            s->hlod_out.oom = 0;
            s->hlod_status = 500, buf_puts(&s->hlod_out, err.msg);
        } else s->hlod_status = 200;
        s->hlod_ms += s->pf->now_ms(s->pf->ctx) - t0;
    }
    *ms = s->hlod_ms;
    *p = s->hlod_out.p ? s->hlod_out.p : "", *n = s->hlod_out.len;
    return s->hlod_status;
}

/* ---- SIZE2: a link's mesh on its own, BMSH version 4 (mesh.h) ---- */
/* SIZE3 (B): the page's element box (glviewport.ts elementBox: f32, each sum rounded in this order) of positions
 * `pos` placed by `pl` (NULL: as they are), min and max grown into b[6]. */
static void box_grow(const float (*pos)[3], uint32_t nv, const cc_placement32 *pl, float b[6]) {
    for (uint32_t v = 0; v < nv; v++) {
        float x = pos[v][0], y = pos[v][1], z = pos[v][2];
        if (pl) {
            float t0 = pl->x[0] * x, t1 = pl->y[0] * y, t2 = pl->z[0] * z, qx = t0 + t1;
            qx = qx + t2, qx = qx + pl->t[0];
            float u0 = pl->x[1] * x, u1 = pl->y[1] * y, u2 = pl->z[1] * z, qy = u0 + u1;
            qy = qy + u2, qy = qy + pl->t[1];
            float w0 = pl->x[2] * x, w1 = pl->y[2] * y, w2 = pl->z[2] * z, qz = w0 + w1;
            qz = qz + w2, qz = qz + pl->t[2];
            x = qx, y = qy, z = qz;
        }
        if (x < b[0]) b[0] = x;
        if (x > b[3]) b[3] = x;
        if (y < b[1]) b[1] = y;
        if (y > b[4]) b[4] = y;
        if (z < b[2]) b[2] = z;
        if (z > b[5]) b[5] = z;
    }
}

/* SIZE3: block i's BMG1 bytes from a lazy view file (viewfile.h cc_viewfile_block). */
static int link_block(const void *ctx, int shape, size_t i, Buf *out) {
    const cc_viewfile *v = ctx;
    const uint8_t *p;
    size_t n;
    if (cc_viewfile_block(v, shape ? i : v->nshapes + i, &p, &n)) return -1;
    buf_append(out, p, n);
    return out->oom ? -1 : 0;
}

/* SIZE2FIX, SIZE2FIX2, SIZE3: the link a page's request for link k means, or NULL with *st set: 204 (superseded: a
 * request that names the link or carries the heads' key and finds it gone, renamed, of another key or with no
 * geometry now) or 404 (a bare index of no such link, or not loaded). One rule for its mesh and its rows answer. */
static cc_link *link_asked(cc_session *s, uint64_t k, const char *name, size_t nname, int has_key, uint64_t key, int *st, Buf *out) {
    cc_link *l = k < s->nlinks ? &s->links[k] : NULL;
    if (l && name && (strlen(l->name) != nname || memcmp(l->name, name, nname))) l = NULL;
    if (l && has_key && key != (uint32_t)s->links_mesh_gen) l = NULL;
    if ((!l || !l->kind) && (name || has_key)) return *st = 204, NULL;
    if (!l || !l->kind) return *st = 404, buf_puts(out, "no such link"), NULL;
    *st = 200;
    return l;
}

int cc_link_mesh(cc_session *s, uint64_t k, const char *name, size_t nname, int has_key, uint64_t key, const char **p, size_t *n) {
    Buf *out = &s->link_mesh_out;
    buf_free(out); /* the last answer's bytes go (a link's mesh is large: none is kept between calls) */
    buf_init(out);
    int st;
    cc_error err;
    const cc_link *l = link_asked(s, k, name, nname, has_key, key, &st, out); /* SIZE2FIX, SIZE2FIX2 */
    if (!l) {
        /* 204 superseded or 404: st set */
    } else if (l->vf && l->vf->lazy && !l->resident) { /* SIZE3: its cache's own codec bytes, no mesh decoded */
        /* the rows with the view file's counts (the link's scene shows empty meshes until decoded); the blocks
         * appended from the file, never read from these arrays (NULL) */
        const cc_scene *rs = cc_link_rows(l);
        cc_scene cs = *rs;
        cs.elements = malloc((rs->nelements + 1) * sizeof *cs.elements);
        if (!cs.elements) buf_puts(out, "out of memory"), st = 500;
        else {
            for (size_t i = 0; i < rs->nelements; i++) {
                cs.elements[i] = rs->elements[i];
                cs.elements[i].mesh = i < l->vf->nelements ? l->vf->elements[i].mesh : (cc_mesh){0};
            }
            cs.shapes = l->vf->shapes, cs.nshapes = l->vf->nshapes;
            if (cc_scene_bmsh4_blocks(&cs, link_bmsh_id, l, (uint32_t)s->links_mesh_gen, link_block, l->vf, out, &err))
                buf_clear(out), out->oom = 0, buf_puts(out, err.msg), st = 500;
            free(cs.elements);
        }
    } else if (cc_scene_bmsh4(cc_link_raw_scene(l), link_bmsh_id, l, (uint32_t)s->links_mesh_gen, out, &err))
        buf_clear(out), out->oom = 0, buf_puts(out, err.msg), st = 500;
    *p = out->p ? out->p : "", *n = out->len;
    return st;
}

/* ---- SIZE3 (B): a link's rows and its elements' boxes (mesh.h cc_link_rows_answer) ---- */
int cc_link_rows_answer(cc_session *s, uint64_t k, const char *name, size_t nname, int has_key, uint64_t key, const char **p, size_t *n) {
    Buf *out = &s->link_mesh_out;
    buf_free(out);
    buf_init(out);
    int st;
    cc_error err;
    cc_link *l = link_asked(s, k, name, nname, has_key, key, &st, out); /* (as cc_link_mesh) */
    if (l) {
        int lazy = l->vf && l->vf->lazy && !l->resident;
        const cc_scene *rs = lazy ? cc_link_rows(l) : cc_link_raw_scene(l);
        cc_scene cs = *rs;
        size_t ne = rs->nelements;
        float (*bx)[6] = malloc((ne + 1) * sizeof *bx);
        cs.elements = lazy ? malloc((ne + 1) * sizeof *cs.elements) : NULL;
        if (!bx || (lazy && !cs.elements)) st = 500, buf_puts(out, "out of memory");
        else {
            for (size_t i = 0; i < ne; i++) {
                for (int q = 0; q < 3; q++) bx[i][q] = 1.0f / 0.0f, bx[i][3 + q] = -1.0f / 0.0f;
                if (lazy) cs.elements[i] = rs->elements[i], cs.elements[i].mesh = i < l->vf->nelements ? l->vf->elements[i].mesh : (cc_mesh){0};
            }
            if (lazy) cs.shapes = l->vf->shapes, cs.nshapes = l->vf->nshapes;
            const cc_scene *sc = lazy ? &cs : rs;
            if (lazy) { /* each mesh decoded once into a scratch, never kept */
                Arena a;
                arena_init(&a, 0);
                size_t ns = l->vf->nshapes;
                size_t *first = calloc(ns + 1, sizeof *first), *next = malloc((ne + 1) * sizeof *next);
                if (!first || !next) st = 500, buf_puts(out, "out of memory");
                for (size_t i = ne; st == 200 && i-- > 0;) /* each shape's elements, in order */
                    if (sc->elements[i].shape >= 0 && (size_t)sc->elements[i].shape < ns)
                        next[i] = first[sc->elements[i].shape], first[sc->elements[i].shape] = i + 1;
                for (size_t b = 0; st == 200 && b < ns + ne; b++) {
                    if (b >= ns && sc->elements[b - ns].shape >= 0) continue;
                    const uint8_t *q;
                    size_t qn;
                    cc_mesh m;
                    const char *why;
                    arena_trim(&a, (size_t)1 << 22);
                    if (cc_viewfile_block(l->vf, b, &q, &qn) || cc_meshcodec_decode(q, qn, &a, &m, &why) != CC_MC_OK) {
                        st = 500, buf_puts(out, "a mesh of the cache does not decode");
                        break;
                    }
                    if (b >= ns) box_grow((const float(*)[3])m.pos, m.nv, NULL, bx[b - ns]);
                    else
                        for (size_t e = first[b]; e; e = next[e - 1]) box_grow((const float(*)[3])m.pos, m.nv, &sc->elements[e - 1].to_scene, bx[e - 1]);
                }
                free(first), free(next);
                arena_free(&a);
            } else
                for (size_t i = 0; i < ne; i++) {
                    const cc_scene_element *e = &sc->elements[i];
                    const cc_mesh *m = e->shape >= 0 ? ((size_t)e->shape < sc->nshapes ? &sc->shapes[e->shape] : NULL) : &e->mesh;
                    if (m && m->pos) box_grow((const float(*)[3])m->pos, m->nv, e->shape >= 0 ? &e->to_scene : NULL, bx[i]);
                }
            if (st == 200 && cc_scene_bmsh4_blocks(sc, link_bmsh_id, l, (uint32_t)s->links_mesh_gen, CC_BMSH_ROWS_ONLY, NULL, out, &err))
                buf_clear(out), out->oom = 0, buf_puts(out, err.msg), st = 500;
            if (st == 200) {
                buf_append(out, "BOXS", 4), buf_append(out, bx, ne * sizeof *bx);
                if (out->oom) buf_clear(out), out->oom = 0, buf_puts(out, "out of memory"), st = 500;
            }
        }
        free(bx);
        if (lazy) free(cs.elements);
    }
    *p = out->p ? out->p : "", *n = out->len;
    return st;
}

/* ---- LNK3D (ADR 0078 amendment 3): a link's baked proxies (mesh.h) ---- */
int cc_link_proxies(cc_session *s, uint64_t k, const char **p, size_t *n) {
    Buf *out = &s->link_prx_out;
    buf_clear(out);
    out->oom = 0;
    const cc_link *l = k < s->nlinks ? &s->links[k] : NULL;
    const cc_platform *pf = s->pf;
    if (!l || l->kind != CC_LINK_VIEW || !l->vf || !l->vf->has_hlod || !l->cache || !pf->read_range) {
        *p = "", *n = 0;
        return 204; /* none: not an error (a small link, a .bimbleview, no file access) */
    }
    uint64_t len = l->vf->hlod_len;
    int st = 200;
    cc_error err;
    size_t got = 0;
    if (len > ((uint64_t)1 << 31) || buf_reserve(out, (size_t)len + 1)) st = 500, buf_puts(out, "proxies: too large");
    else if (pf->read_range(pf->ctx, l->cache, l->vf->hlod_off, (size_t)len, out->p, &got) != CC_PF_OK || got != len)
        st = 204, buf_clear(out); /* the cache gone since: none */
    else {
        out->len = (size_t)len;
        if (len >= 2 && (unsigned char)out->p[0] == 0x1f && (unsigned char)out->p[1] == 0x8b) { /* SIZE2: gzipped (cache format 5) */
            Buf raw;
            buf_init(&raw);
            const char *why = NULL;
            /* bounded: at most 64 times its stored size and CC_LINK_HLOD_MAX x 2 (the budget's cap with room) */
            size_t cap = (size_t)(len > (CC_LINK_HLOD_MAX * 2) / 64 ? CC_LINK_HLOD_MAX * 2 : len * 64);
            if (cc_gunzip((const unsigned char *)out->p, out->len, cap, &raw, &why)) buf_free(&raw), buf_clear(out), buf_puts(out, "proxies: damaged"), st = 500;
            else buf_free(out), *out = raw;
        }
        if (st == 200 && cc_hlod_bprx_check((const uint8_t *)out->p, out->len, (uint32_t)cc_link_rows(l)->nelements, &err))
            buf_clear(out), buf_puts(out, err.msg), st = 500;
    }
    *p = out->p ? out->p : "", *n = out->len;
    return st;
}


/* ---- TILE1 (ADR 0078 amendment 6): the tile pyramid served (mesh.h cc_link_tiles, cc_mesh_tiles) ---- */

#define TILES_MAX_ASKED 4096u

/* The answer to `req` ("toc", or tile ids "a,b,c") from the section sec[0..n) (checked here: untrusted) of a scene
 * whose leaves come from `src`. The HTTP status; `out` the bytes or the error text. */
static int tiles_answer(const cc_tiles_src *src, const uint8_t *sec, size_t n, uint32_t ne, const char *req, size_t nreq, Buf *out) {
    cc_tiles_toc toc;
    cc_error err;
    if (cc_tiles_check(sec, n, 1, ne, &toc, &err)) return buf_puts(out, err.msg), 500;
    if (nreq == 3 && !memcmp(req, "toc", 3)) {
        buf_append(out, sec, toc.toc);
        return out->oom ? (buf_clear(out), out->oom = 0, buf_puts(out, "out of memory"), 500) : 200;
    }
    uint32_t ids[TILES_MAX_ASKED], nid = 0;
    for (size_t i = 0; i < nreq;) {
        uint64_t v = 0;
        size_t d = 0;
        while (i < nreq && req[i] >= '0' && req[i] <= '9' && d < 10) v = v * 10 + (uint64_t)(req[i++] - '0'), d++;
        if (!d || v >= toc.ntiles || nid >= TILES_MAX_ASKED || (i < nreq && req[i] != ',')) return buf_puts(out, "bad tile ids"), 400;
        ids[nid++] = (uint32_t)v;
        if (i < nreq) i++;
    }
    if (!nid) return buf_puts(out, "no tile asked"), 400;
    const uint8_t *blobs = sec + ((toc.toc + 7u) & ~7u);
    buf_append(out, "TILS", 4);
    unsigned char c4[4];
#define PUT32(v) (c4[0] = (unsigned char)(v), c4[1] = (unsigned char)((v) >> 8), c4[2] = (unsigned char)((v) >> 16), c4[3] = (unsigned char)((v) >> 24), buf_append(out, c4, 4))
    PUT32(nid);
    for (uint32_t i = 0; i < nid; i++) {
        cc_tile t;
        cc_tiles_get(&toc, ids[i], &t);
        uint32_t kind = (t.flags & CC_TILE_LEAF) ? 1 : (t.flags & CC_TILE_PROXY) ? 2 : 0;
        PUT32(ids[i]);
        PUT32(kind);
        size_t at = out->len;
        PUT32(0u);
        if (kind == 2) buf_append(out, blobs + t.off, t.len);
        else if (kind == 1 && cc_tiles_leaf_answer(src, &toc, ids[i], out, &err)) {
            buf_clear(out), out->oom = 0, buf_puts(out, err.msg);
            return 500;
        }
        uint32_t len = (uint32_t)(out->len - at - 4);
        while (out->len % 4) buf_putc(out, 0);
        if (!out->oom) memcpy(out->p + at, (unsigned char[4]){(unsigned char)len, (unsigned char)(len >> 8), (unsigned char)(len >> 16), (unsigned char)(len >> 24)}, 4);
    }
#undef PUT32
    if (out->oom) return buf_clear(out), out->oom = 0, buf_puts(out, "out of memory"), 500;
    return 200;
}

/* a small link (or a view file linked as it is) without a section TILE: its pyramid built at once, kept one at a time */
#define TILES_ONCE_MAX_TRIS 2000000ull

int cc_link_tiles(cc_session *s, uint64_t k, const char *name, size_t nname, int has_key, uint64_t key, const char *req, size_t nreq,
                  const char **p, size_t *n) {
    Buf *out = &s->link_mesh_out;
    buf_free(out);
    buf_init(out);
    int st;
    s->link_tiles_none = 0;
    cc_link *l = link_asked(s, k, name, nname, has_key, key, &st, out);
    if (l) {
        cc_tiles_src src = {NULL, NULL};
        if (l->vf) src.vf = l->vf;
        else if (l->ifc) src.scene = &l->ifc->scene;
        else src.scene = cc_link_raw_scene(l);
        uint32_t ne = (uint32_t)cc_link_rows(l)->nelements;
        const uint8_t *sec = NULL;
        size_t sn = 0;
        Buf rd;
        buf_init(&rd);
        if (l->vf && l->vf->has_tiles) {
            const cc_viewfile *v = l->vf;
            if (v->lazy && v->base && v->tiles_off <= v->base_len && v->tiles_len <= v->base_len - v->tiles_off)
                sec = v->base + v->tiles_off, sn = (size_t)v->tiles_len;
            else if (l->cache && s->pf->read_range && v->tiles_len < ((uint64_t)1 << 31) && !buf_reserve(&rd, (size_t)v->tiles_len + 1)) {
                size_t got = 0;
                if (s->pf->read_range(s->pf->ctx, l->cache, v->tiles_off, (size_t)v->tiles_len, rd.p, &got) == CC_PF_OK && got == v->tiles_len)
                    rd.len = got, sec = (const uint8_t *)rd.p, sn = got;
            }
        }
        if (!sec && s->link_tiles_uid == l->uid + 1 && s->link_tiles_gen == s->links_mesh_gen && s->link_tiles.len)
            sec = (const uint8_t *)s->link_tiles.p, sn = s->link_tiles.len;
        if (!sec) {
            uint64_t tris = cc_link_triangles(l);
            if (l->indexing) st = 202, buf_puts(out, "indexing");
            else if (tris > TILES_ONCE_MAX_TRIS) st = 204, s->link_tiles_none = 1; /* none: indexed again at its next open */
            else {
                cc_tiles t;
                cc_tiles_opts o;
                cc_error err;
                cc_tiles_default(&o);
                if (cc_tiles_build(&t, &src, &o, &err)) st = 500, buf_puts(out, err.msg);
                else {
                    buf_free(&s->link_tiles);
                    s->link_tiles = t.out, buf_init(&t.out);
                    s->link_tiles_uid = l->uid + 1, s->link_tiles_gen = s->links_mesh_gen;
                    sec = (const uint8_t *)s->link_tiles.p, sn = s->link_tiles.len;
                }
                cc_tiles_free(&t);
            }
        }
        if (sec) st = tiles_answer(&src, sec, sn, ne, req, nreq, out);
        buf_free(&rd);
    }
    *p = out->p ? out->p : "", *n = out->len;
    return st;
}

void cc_mesh_tiles_free(cc_session *s) {
    if (s->tiles) cc_tiles_free(s->tiles), free(s->tiles);
    s->tiles = NULL, s->tiles_scene = NULL, s->tiles_rev = 0, s->tiles_status = 0, s->tiles_ms = 0;
    buf_free(&s->tiles_err);
    buf_free(&s->tiles_out);
    buf_free(&s->link_tiles);
    s->link_tiles_uid = 0;
}

int cc_mesh_tiles(cc_session *s, uint64_t budget, const char *req, size_t nreq, const char **p, size_t *n, uint32_t *rev) {
    cc_error err;
    const cc_model *m = cc_mesh_model(s, rev, &err);
    const cc_scene *sc = m || s->view ? cc_session_scene(s) : NULL;
    Buf *out = &s->tiles_out;
    buf_clear(out);
    out->oom = 0;
    if (!sc) {
        cc_mesh_tiles_free(s);
        buf_puts(out, "no scene");
        *p = buf_cstr(out), *n = out->len;
        return 404;
    }
    cc_tiles_src src = {sc, NULL};
    if (s->view == CC_VIEW_FILE && s->vf && s->view_tiles.len) { /* a view file opened with its pyramid: served as it is */
        int st = tiles_answer(&src, (const uint8_t *)s->view_tiles.p, s->view_tiles.len, (uint32_t)sc->nelements, req, nreq, out);
        if (st != 500) {
            *p = out->p ? out->p : "", *n = out->len;
            return st;
        }
        buf_clear(out), out->oom = 0; /* damaged: built below */
    }
    if (s->tiles_scene != sc || s->tiles_rev != *rev || !s->tiles_status) {
        Buf keep = s->link_tiles;
        uint64_t ku = s->link_tiles_uid, kg = s->link_tiles_gen;
        buf_init(&s->link_tiles);
        cc_mesh_tiles_free(s);
        s->link_tiles = keep, s->link_tiles_uid = ku, s->link_tiles_gen = kg;
        out = &s->tiles_out;
        s->tiles_scene = sc, s->tiles_rev = *rev;
        s->tiles = calloc(1, sizeof *s->tiles);
        cc_tiles_opts o;
        cc_tiles_default(&o);
        double t0 = s->pf->now_ms(s->pf->ctx);
        if (!s->tiles) s->tiles_status = 500, buf_puts(&s->tiles_err, "out of memory");
        else if (cc_tiles_start(s->tiles, &src, &o, &err)) s->tiles_status = 500, buf_puts(&s->tiles_err, err.msg);
        else s->tiles_status = 202;
        s->tiles_ms += s->pf->now_ms(s->pf->ctx) - t0;
    }
    if (s->tiles_status == 202) {
        double t0 = s->pf->now_ms(s->pf->ctx);
        int r = cc_tiles_step(s->tiles, budget ? budget : 1, &err);
        if (r < 0) s->tiles_status = 500, buf_puts(&s->tiles_err, err.msg);
        else if (r == 1) s->tiles_status = 200;
        s->tiles_ms += s->pf->now_ms(s->pf->ctx) - t0;
    }
    int st = s->tiles_status;
    if (st == 202) buf_u64(out, s->tiles->done), buf_putc(out, ' '), buf_u64(out, s->tiles->total);
    else if (st == 500) buf_append(out, s->tiles_err.p, s->tiles_err.len);
    else st = tiles_answer(&src, (const uint8_t *)s->tiles->out.p, s->tiles->out.len, (uint32_t)sc->nelements, req, nreq, out);
    *p = out->p ? out->p : "", *n = out->len;
    return st;
}

/* ---- TILE1: what the reference walker draws for one camera, over the open scene and every link (mesh.h) ---- */

static double fdot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void fcross(const double *a, const double *b, double *o) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
static void fnorm(double *a) {
    double l = cc_sqrt(fdot(a, a));
    if (l > 0) a[0] /= l, a[1] /= l, a[2] /= l;
}

/* the view in a frame where host world w = R(c, sn) p + o (R about Z) */
static void view_in(const cc_tiles_frame_q *q, double c, double sn, const double o[3], cc_tiles_view *v) {
    memset(v, 0, sizeof *v);
    double f[3] = {q->at[0] - q->eye[0], q->at[1] - q->eye[1], q->at[2] - q->eye[2]}, up[3] = {0, 0, 1}, r[3], u[3];
    fnorm(f);
    if (f[2] > 0.999 || f[2] < -0.999) up[0] = 0, up[1] = 1, up[2] = 0;
    fcross(f, up, r), fnorm(r), fcross(r, f, u);
    double ty = q->tan_half, tx = ty * q->w / (q->h > 0 ? q->h : 1);
    v->k = q->h / (2 * ty);
    double n[5][3], d[5];
    for (int k = 0; k < 3; k++) n[0][k] = f[k] * tx + r[k], n[1][k] = f[k] * tx - r[k], n[2][k] = f[k] * ty + u[k], n[3][k] = f[k] * ty - u[k], n[4][k] = f[k];
    for (int p = 0; p < 4; p++) d[p] = -fdot(n[p], q->eye);
    d[4] = -fdot(f, q->eye) - 10;
    /* into the frame: n . (R p + o) + d = (R^T n) . p + n . o + d */
    for (int p = 0; p < 5; p++) {
        double m[3] = {c * n[p][0] + sn * n[p][1], -sn * n[p][0] + c * n[p][1], n[p][2]};
        v->planes[p][0] = m[0], v->planes[p][1] = m[1], v->planes[p][2] = m[2], v->planes[p][3] = fdot(n[p], o) + d[p];
    }
    v->nplanes = 5;
    double e[3] = {q->eye[0] - o[0], q->eye[1] - o[1], q->eye[2] - o[2]};
    v->eye[0] = c * e[0] + sn * e[1], v->eye[1] = -sn * e[0] + c * e[1], v->eye[2] = e[2];
}

static int frame_one(cc_session *s, const char *name, const cc_tiles_src *src, const uint8_t *sec, size_t sn, uint32_t ne,
                     const cc_tiles_frame_q *q, double c, double si, const double o[3], JsonW *w, double tot[6]) {
    cc_tiles_toc toc;
    cc_error err;
    (void)s;
    if (cc_tiles_check(sec, sn, 1, ne, &toc, &err)) return -1;
    cc_tiles_view v;
    view_in(q, c, si, o, &v);
    uint32_t *ids = malloc(((size_t)toc.ntiles + 1) * sizeof *ids);
    if (!ids) return -1;
    uint64_t tris = 0, pb = 0, lb = 0, nl = 0, np = 0;
    double t0 = s->pf->now_ms(s->pf->ctx);
    uint32_t n = cc_tiles_select(&toc, &v, q->error_px, q->budget, NULL, ids, toc.ntiles, &tris);
    double sel_ms = s->pf->now_ms(s->pf->ctx) - t0;
    cc_tile r;
    if (toc.ntiles) cc_tiles_get(&toc, 0, &r);
    Buf a;
    buf_init(&a);
    for (uint32_t i = 0; i < n; i++) {
        cc_tile t;
        cc_tiles_get(&toc, ids[i], &t);
        if (t.flags & CC_TILE_PROXY) pb += t.len, np++;
        else if (q->leaf_bytes) {
            buf_clear(&a);
            if (!cc_tiles_leaf_answer(src, &toc, ids[i], &a, &err)) lb += a.len;
            nl++;
        } else nl++;
    }
    buf_free(&a);
    jw_obj_begin(w);
    jw_key(w, "detail"), jw_u64(w, toc.ntiles ? r.detail : 0);
    jw_key(w, "leaf_bytes"), jw_u64(w, lb);
    jw_key(w, "leaves"), jw_u64(w, nl);
    jw_key(w, "name"), jw_str(w, name);
    jw_key(w, "proxies"), jw_u64(w, np);
    jw_key(w, "proxy_bytes"), jw_u64(w, pb);
    jw_key(w, "select_ms"), jw_f64(w, sel_ms);
    jw_key(w, "tiles"), jw_u64(w, n);
    jw_key(w, "toc_bytes"), jw_u64(w, toc.toc);
    jw_key(w, "triangles"), jw_u64(w, tris);
    jw_obj_end(w);
    tot[0] += (double)tris, tot[1] += (double)n, tot[2] += (double)pb, tot[3] += (double)lb, tot[4] += (double)toc.toc;
    tot[5] += toc.ntiles ? (double)r.detail : 0;
    free(ids);
    return 0;
}

/* the world box of a pyramid's root, placed (R about Z, then o) */
static void grow_world(const uint8_t *sec, size_t sn, double c, double si, const double o[3], double b[6]) {
    cc_tiles_toc toc;
    if (cc_tiles_check(sec, sn, 0, CC_TILES_NONE, &toc, NULL) || !toc.ntiles) return;
    cc_tile r;
    cc_tiles_get(&toc, 0, &r);
    for (int k = 0; k < 8; k++) {
        double p[3] = {k & 1 ? r.box[3] : r.box[0], k & 2 ? r.box[4] : r.box[1], k & 4 ? r.box[5] : r.box[2]};
        double w3[3] = {c * p[0] - si * p[1] + o[0], si * p[0] + c * p[1] + o[1], p[2] + o[2]};
        for (int j = 0; j < 3; j++) {
            if (w3[j] < b[j]) b[j] = w3[j];
            if (w3[j] > b[3 + j]) b[3 + j] = w3[j];
        }
    }
}

int cc_tiles_frame(cc_session *s, const cc_tiles_frame_q *q, Buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    double tot[6] = {0}, b[6] = {1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
    jw_obj_begin(&w);
    jw_key(&w, "links"), jw_arr_begin(&w);
    for (size_t i = 0; i < s->nlinks; i++) {
        cc_link *l = &s->links[i];
        if (!l->kind || !l->visible) continue;
        const char *p;
        size_t n;
        int st = cc_link_tiles(s, i, NULL, 0, 0, 0, "toc", 3, &p, &n); /* builds a small link's at once */
        (void)p, (void)n;
        if (st != 200) continue;
        cc_tiles_src src = {NULL, NULL};
        if (l->vf) src.vf = l->vf;
        else if (l->ifc) src.scene = &l->ifc->scene;
        else src.scene = cc_link_raw_scene(l);
        const uint8_t *sec = NULL;
        size_t sn = 0;
        if (l->vf && l->vf->has_tiles && l->vf->lazy) sec = l->vf->base + l->vf->tiles_off, sn = (size_t)l->vf->tiles_len;
        else if (s->link_tiles_uid == l->uid + 1) sec = (const uint8_t *)s->link_tiles.p, sn = s->link_tiles.len;
        if (!sec) continue;
        double c, si, t[3], o[3];
        cc_link_placement(s, l, &c, &si, t);
        const double *lo = cc_link_rows(l)->origin;
        o[0] = c * lo[0] - si * lo[1] + t[0], o[1] = si * lo[0] + c * lo[1] + t[1], o[2] = lo[2] + t[2];
        if (q->bounds_only) grow_world(sec, sn, c, si, o, b);
        else frame_one(s, l->name, &src, sec, sn, (uint32_t)cc_link_rows(l)->nelements, q, c, si, o, &w, tot);
    }
    jw_arr_end(&w);
    if (q->bounds_only) {
        jw_key(&w, "bounds"), jw_arr_begin(&w);
        for (int k = 0; k < 6; k++) jw_f64(&w, b[0] <= b[3] ? b[k] : 0.0);
        jw_arr_end(&w);
    } else {
        jw_key(&w, "bytes"), jw_u64(&w, (uint64_t)(tot[2] + tot[3]));
        jw_key(&w, "detail"), jw_u64(&w, (uint64_t)tot[5]);
        jw_key(&w, "leaf_bytes"), jw_u64(&w, (uint64_t)tot[3]);
        jw_key(&w, "proxy_bytes"), jw_u64(&w, (uint64_t)tot[2]);
        jw_key(&w, "tiles"), jw_u64(&w, (uint64_t)tot[1]);
        jw_key(&w, "toc_bytes"), jw_u64(&w, (uint64_t)tot[4]);
        jw_key(&w, "triangles"), jw_u64(&w, (uint64_t)tot[0]);
    }
    jw_obj_end(&w);
    return jw_finish(&w) == JW_OK && !out->oom ? 0 : -1;
}
