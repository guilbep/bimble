/* The model GET /mesh draws: the session's open model and its revision (session.c), so the mesh
 * follows every edit, undo and redo. mesh.c caches the BMSH bytes per revision. */
#include <stdio.h>

#include "mesh.h"
#include "session.h"

const cc_model *cc_mesh_model(const cc_session *s, uint32_t *rev, cc_error *err) {
    const cc_model *m = cc_session_model(s);
    *rev = cc_session_rev(s);
    if (!m) {
        err->offset = CC_NO_OFFSET;
        snprintf(err->msg, sizeof err->msg, "no model open");
        return NULL;
    }
    return m;
}
