/* The model GET /mesh draws: the session's open model and its revision (session.c), so the mesh
 * follows every edit, undo and redo. mesh.c caches the BMSH bytes per revision. */
#ifdef __wasm__
#include "wasm_api.h" /* snprintf, freestanding */
#else
#include <stdio.h>
#endif

#include "mesh.h"
#include "session.h"

const cc_model *cc_mesh_model(uint32_t *rev, cc_error *err) {
    const cc_model *m = cc_host_model();
    *rev = cc_host_rev();
    if (!m) {
        err->offset = CC_NO_OFFSET;
        snprintf(err->msg, sizeof err->msg, "no model open");
        return NULL;
    }
    return m;
}
