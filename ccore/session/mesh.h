/* GET /mesh: the render scene as BMSH bytes for the lab's WebGL2 viewport (web-ui/src/glviewport.ts,
 * docs/research/c-core-spec.md §2). The layout is documented at the top of mesh.c. */
#ifndef CCORE_SESSION_MESH_H
#define CCORE_SESSION_MESH_H
#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "model.h"
#include "scene.h"
#include "session.h"

#define CC_BMSH_VERSION 1u

/* Serializes the scene of `m` (cc_scene_build) as BMSH into `out` (appended). 0 ok, -1 with
 * `err` (unsupported generator, out of memory, a count over u32). */
int cc_mesh_bmsh(const cc_model *m, uint32_t rev, Buf *out, cc_error *err);

/* The model /mesh draws and its revision (mesh_model.c: the session's open model). NULL with
 * `err` when there is none. */
const cc_model *cc_mesh_model(const cc_session *s, uint32_t *rev, cc_error *err);

/* The /mesh answer, built once per (model, rev) and cached in the session: the model's scene,
 * or the read-only scene (an IFC's through cc_ifc_scene_bmsh, a view file's through the same
 * writer as a model's). Returns the HTTP status: 200 with
 * the BMSH bytes, 404 (nothing open) or 500 (the scene cannot be built) with a UTF-8 text. `*p`
 * stays valid until the next call on `s`. `*build_ms` is the build time, 0 when served from cache. */
int cc_mesh_get(cc_session *s, const char **p, size_t *n, uint32_t *rev, double *build_ms);
/* 3D1 (ADR 0078): the same in BMSH version `form`: 1 (above), or 3 (src/scene_bmsh.h: each shared
 * shape once and a placement per element; the LNKS trailer's blocks in version 3 too). One form is
 * cached at a time. */
int cc_mesh_get_form(cc_session *s, int form, const char **p, size_t *n, uint32_t *rev, double *build_ms);
/* 3D2 (ADR 0078): the HLOD proxies (src/hlod.h) of the scene the version 3 mesh holds (the open
 * model's, or the read-only IFC's; not the links'), built in steps so that no call takes long (ADR
 * 0074; the wasm core runs on the page's thread): each call reads about `budget` triangles more and
 * returns 202 with "read total" (the triangles read so far and in all) until it is done, then 200
 * with the BPRX bytes (hlod.h), the same on every later call of that rev; 404 when nothing is open or
 * the scene was not built (no proxies: the page draws without), 500 with the error. A budget of 0
 * reads nothing: 200 with the tree alone while building (hlod.h cc_hlod_bprx_tree: the cells, no
 * proxy), the whole BPRX once built. `*p` stays valid
 * until the next call on `s`; `*ms` is the build time so far. */
int cc_mesh_proxies(cc_session *s, uint64_t budget, const char **p, size_t *n, uint32_t *rev, double *ms);
void cc_mesh_proxies_free(cc_session *s);
/* LNK3D (ADR 0078 amendment 3): link k's baked proxies (its cache's section HLOD, BPRX with rev 0 and
 * nE its elements: the rows of its BMSH in the /mesh trailer), read from the cache file and checked
 * (hlod.h cc_hlod_bprx_check) on each call: 200 with the bytes in `*p` (valid until the next call),
 * 204 none, no body (not ready, not from a cache, no proxies, no file access: not an error), 500 a
 * damaged section (the error text). The native host serves it as GET /mesh?link_proxies=k. */
int cc_link_proxies(cc_session *s, uint64_t k, const char **p, size_t *n);
/* SIZE2: cc_mesh_get_form's form 6: version 3 of the open model, then the LNKS trailer version 3 (mesh.c
 * links_lazy: each link's head, no mesh): the page asks each link's mesh on its own. */
#define CC_MESH_FORM_LAZY 6
/* SIZE3: 204 (no body) for a link not there or not loaded (the page's request raced a change). SIZE2: link k's read scene as BMSH version 4 (src/scene_bmsh.h: shapes once, a placement per element,
 * the blocks in the mesh codec), its rev the links' mesh key (links_mesh_gen). 200 with the bytes in `*p`
 * (valid until the next call; the bytes of the call before are freed), 404 no such link (or missing), 500
 * with the error. The native host serves it as GET /mesh?link=k. */
int cc_link_mesh(cc_session *s, uint64_t k, const char *name, size_t nname, int has_key, uint64_t key, const char **p, size_t *n);
/* SIZE2FIX: with `name` (the link's name, NULL: any) and the mesh key (`has_key`: the LNKS heads' key the
 * page read), a link that is no longer k under that name, a key that is no longer the links' mesh
 * generation, or a link with no geometry now (unloaded, missing, queued, converting) answers 204 (superseded:
 * no body; the page drops that fetch and asks the heads again): never 404 (SIZE2FIX2). */
/* SIZE3 (B): link k's rows without its meshes: BMSH version 4 with no block (nB = 0), then "BOXS" and per element its
 * box (f32 min xyz, max xyz; the page's arithmetic: a placed shape's vertices placed one by one; +inf/-inf when
 * empty), what the page needs to merge a link, cut its batches and draw its proxies before any of its detail. A link
 * read in place is decoded one mesh at a time into a scratch, nothing kept. The same rule as cc_link_mesh for `name`
 * and `key` (204 superseded, never 404 with either). The native host serves it as GET /mesh?link=k&rows=1&name=&key=. */
int cc_link_rows_answer(cc_session *s, uint64_t k, const char *name, size_t nname, int has_key, uint64_t key, const char **p, size_t *n);
/* TILE1 (ADR 0078 amendment 6, src/tiles.h): link k's tile pyramid. `req` "toc": its table of contents (the section's
 * bytes [0, toc)); "a,b,c" (at most 4,096 ids): "TILS" u32 count, then per tile u32 id, u32 kind (1 leaf: TLF1 gathered
 * from the cache's ELEM blocks; 2 proxy: its TPX1 blob), u32 bytes, the bytes padded to 4. From the mapped cache (its
 * section TILE checked on each call), else a small link's pyramid built at once (kept for one link). The rule of
 * cc_link_mesh for `name` and `key` (204 superseded); 202 "indexing" while its indexing job runs; 204 no tiles (a large
 * link whose cache has none: indexed again at its next open); 400 bad ids; 500 a damaged section. The native host
 * serves it as GET /mesh?link=k&name=&key=&tiles=toc (or &tile=a,b,c). */
int cc_link_tiles(cc_session *s, uint64_t k, const char *name, size_t nname, int has_key, uint64_t key, const char *req, size_t nreq,
                  const char **p, size_t *n);
/* TILE1: the open model's pyramid (the session's scene: a model, an IFC, a view file), built in steps after the open as
 * 3D2's proxies were: each call clusters about `budget` triangles more and answers 202 "done total" until built, then
 * `req` as cc_link_tiles; a view file opened with a section TILE is served at once. 404 nothing open. */
int cc_mesh_tiles(cc_session *s, uint64_t budget, const char *req, size_t nreq, const char **p, size_t *n, uint32_t *rev);
void cc_mesh_tiles_free(cc_session *s);
/* TILE1: what the reference walker (tiles.h cc_tiles_select) draws for one camera in the host's world mm, per link (its
 * eye and frustum moved into the link's frame by its placement): tiles, triangles, proxy bytes and (leaf_bytes) the
 * leaves' answers' bytes, the tables' bytes; or (bounds_only) the world box of every link's root. JSON into `out`.
 * The native host serves it as GET /mesh?tiles_frame=1&eye=x,y,z&at=x,y,z&fov=deg&w=&h=&err=px&budget=&leaf_bytes=1&occ=1&lod=0
 * (every input a renderer's setting: err 1, budget none, lod 1, occ 0 by default; tiles_frame=bounds: the box; occ=1: tiles hidden by the drawn tiles' occluders left out, counted apart).
 * SHELL1: &shell=off|outside|auto (default off here; the renderers' shell_mode defaults to auto), &margin=mm, &truth=1.
 * Tools and tests (ccore/tools/tiles.mjs). The answers of cc_link_tiles: req "shell" the shell table (204 none),
 * "shell:a,b,c" the outside walk's content (GET ...&tile=a,b,c&shell=1). */
typedef struct {
    double eye[3], at[3], tan_half, w, h, error_px;
    uint64_t budget;
    int leaf_bytes, bounds_only;
    int occlusion; /* the drawn tiles' occluders in one depth buffer (tiles.h cc_tiles_occ): tiles hidden left out */
    int lod;       /* levels of detail: 1 on (the default), 0 off: every leaf in the frustum */
    int shell;     /* SHELL1: 0 off, 1 outside (the outside walk wherever baked), 2 auto (where the eye is outside the link's
                    * box grown by shell_margin mm) */
    double shell_margin;
    int truth;     /* SHELL1: a full-detail software render of every link at w x h (glazing not occluding): per link the
                    * elements seen and those the selection does not depict (missing), with the largest missing listed */
} cc_tiles_frame_q;
int cc_tiles_frame(cc_session *s, const cc_tiles_frame_q *q, Buf *out);
#endif
