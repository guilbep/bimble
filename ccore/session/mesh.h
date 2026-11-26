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
 * the scene was not built (no proxies: the page draws without), 500 with the error. `*p` stays valid
 * until the next call on `s`; `*ms` is the build time so far. */
int cc_mesh_proxies(cc_session *s, uint64_t budget, const char **p, size_t *n, uint32_t *rev, double *ms);
void cc_mesh_proxies_free(cc_session *s);
#endif
