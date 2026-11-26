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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ifc_read.h"
#include "links.h"
#include "mesh.h"
#include "scene.h"
#include "scene_bmsh.h"
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
        if (cc_scene_world_mesh(&s, k, &tmp, &w[k]) != 0) { fail(err, "out of memory"); goto done; }
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

/* links.h: the LNKS trailer (`form` 3: each link's BMSH in version 3). */
static int links_bmsh(cc_session *s, uint32_t rev, int form, Buf *out, cc_error *err) {
    if (!s->nlinks) return 0;
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
            const double *lo = cc_link_raw_scene(k)->origin;
            o[0] = c * lo[0] - sn * lo[1] + t[0], o[1] = sn * lo[0] + c * lo[1] + t[1], o[2] = lo[2] + t[2];
            buf_append(out, &c, 8), buf_append(out, &sn, 8);
            for (int q = 0; q < 3; q++) buf_append(out, &o[q], 8);
            int32_t col = k->colour;
            buf_append(out, &col, 4);
        }
        buf_clear(&one);
        one.oom = 0;
        if (form == 3) rc = cc_scene_bmsh3(cc_link_raw_scene(k), link_bmsh_id, k, rev, &one, err);
        else rc = k->kind == CC_LINK_IFC ? cc_ifc_scene_bmsh(k->ifc, rev, &one, err) : bmsh_of(&k->scene, rev, &one, err);
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
    if (form != 3) form = 1;
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
    if (!s->mesh_status || s->mesh_model != m || s->mesh_rev != *rev || s->mesh_links_gen != s->links_gen || s->mesh_form != form) {
        double t0 = s->pf->now_ms(s->pf->ctx);
        buf_clear(cache);
        cache->oom = 0;
        /* The session's scene of this rev (calls.c builds it once per rev for query status),
         * not a second build; when it has none (or it failed), a build here gives the error. */
        const cc_scene *sc = cc_session_scene(s);
        int rc;
        if (form == 3) { /* 3D1: shapes once */
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
        s->mesh_model = m, s->mesh_rev = *rev, s->mesh_links_gen = s->links_gen, s->mesh_form = form;
        *build_ms = s->pf->now_ms(s->pf->ctx) - t0;
    }
    *p = cache->p ? cache->p : "", *n = cache->len;
    return s->mesh_status;
}

/* ---- 3D2 (ADR 0078): the HLOD proxies, in steps (mesh.h) ---- */

void cc_mesh_proxies_free(cc_session *s) {
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
