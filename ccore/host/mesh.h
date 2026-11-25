/* GET /mesh: the render scene as BMSH bytes for the lab's WebGL2 viewport (web-ui/src/glviewport.ts,
 * docs/research/c-core-spec.md §2). The layout is documented at the top of mesh.c. */
#ifndef CCORE_HOST_MESH_H
#define CCORE_HOST_MESH_H
#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "model.h"
#include "scene.h"

#define CC_BMSH_VERSION 1u

/* Serializes the scene of `m` (cc_scene_build) as BMSH into `out` (appended). 0 ok, -1 with
 * `err` (unsupported generator, out of memory, a count over u32). */
int cc_mesh_bmsh(const cc_model *m, uint32_t rev, Buf *out, cc_error *err);

/* The model /mesh draws and its revision. NULL with `err` when there is none. Today
 * mesh_model.c (the file named by CCORE_MODEL, loaded once); the model-backed calls replace it. */
const cc_model *cc_mesh_model(uint32_t *rev, cc_error *err);

/* The /mesh answer, built once per (model, rev) and cached. Returns the HTTP status: 200 with
 * the BMSH bytes, 404 (no model) or 500 (the scene cannot be built) with a UTF-8 text. `*p`
 * stays valid until the next call. `*build_ms` is the build time, 0 when served from cache. */
int cc_mesh_get(const char **p, size_t *n, uint32_t *rev, double *build_ms);

/* calls.c: the scene of the model cc_mesh_model() returns, at its revision, built once per
 * revision and shared by query status and GET /mesh; NULL when there is no model or the build
 * failed. */
const cc_scene *cc_host_scene(void);
#endif
