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
#include <string.h>

#include "ifc_read.h"
#include "links.h"
#include "mesh.h"
#include "scene.h"
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

/* links.h: the LNKS trailer. */
int cc_links_bmsh(cc_session *s, uint32_t rev, Buf *out, cc_error *err) {
    if (!s->nlinks) return 0;
    buf_append(out, "LNKS", 4);
    put_u32(out, 1);
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
        put_u32(out, k->visible ? 1u : 0u);
        buf_clear(&one);
        one.oom = 0;
        rc = k->kind == CC_LINK_IFC ? cc_ifc_scene_bmsh(k->ifc, rev, &one, err) : bmsh_of(&k->scene, rev, &one, err);
        if (!rc && (one.oom || one.len > UINT32_MAX)) rc = fail(err, "out of memory");
        if (!rc) put_u32(out, (uint32_t)one.len), buf_append(out, one.p, one.len);
    }
    buf_free(&one);
    if (!rc && out->oom) rc = fail(err, "out of memory");
    return rc;
}

/* ---- the cached answer (in the session) ---- */

int cc_mesh_get(cc_session *s, const char **p, size_t *n, uint32_t *rev, double *build_ms) {
    cc_error err;
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
    if (!s->mesh_status || s->mesh_model != m || s->mesh_rev != *rev || s->mesh_links_gen != s->links_gen) {
        double t0 = s->pf->now_ms(s->pf->ctx);
        buf_clear(cache);
        cache->oom = 0;
        /* The session's scene of this rev (calls.c builds it once per rev for query status),
         * not a second build; when it has none (or it failed), a build here gives the error. */
        const cc_scene *sc = cc_session_scene(s);
        int rc;
        if (s->view == CC_VIEW_IFC) rc = cc_ifc_scene_bmsh(s->ifc, *rev, cache, &err); /* the file's id text */
        else rc = sc ? bmsh_of(sc, *rev, cache, &err) : cc_mesh_bmsh(m, *rev, cache, &err);
        if (rc == 0) rc = cc_links_bmsh(s, *rev, cache, &err); /* nothing without links */
        if (rc != 0) {
            buf_clear(cache);
            cache->oom = 0;
            buf_puts(cache, err.msg);
            s->mesh_status = 500;
        } else {
            s->mesh_status = 200;
        }
        s->mesh_model = m, s->mesh_rev = *rev, s->mesh_links_gen = s->links_gen;
        *build_ms = s->pf->now_ms(s->pf->ctx) - t0;
    }
    *p = cache->p ? cache->p : "", *n = cache->len;
    return s->mesh_status;
}
