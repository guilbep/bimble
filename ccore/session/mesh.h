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
#endif
