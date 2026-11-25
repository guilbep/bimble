/* The model GET /mesh draws until the model-backed calls own one (spec §5): the .bimble file
 * named by the environment variable CCORE_MODEL, loaded once, revision 0 (the stub change feed
 * says rev 0 too). Kept apart from calls_stub.c so that the model-backed calls replace only
 * cc_mesh_model(). The file is untrusted: size capped, cc_model_load validates. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file.h"
#include "mesh.h"

static cc_model model;
static int state; /* 0 not tried, 1 loaded, -1 failed (err kept) */
static cc_error load_err;

static void set_err(cc_error *e, const char *a, const char *b) {
    e->offset = CC_NO_OFFSET;
    snprintf(e->msg, sizeof e->msg, "%s%s", a, b ? b : "");
}

static void load(void) {
    const char *path = getenv("CCORE_MODEL");
    if (!path || !*path) {
        set_err(&load_err, "no model: set CCORE_MODEL to a .bimble file", NULL);
        state = -1;
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(&load_err, "no model: cannot open ", path);
        state = -1;
        return;
    }
    Buf b;
    buf_init(&b);
    char chunk[65536];
    size_t k;
    while ((k = fread(chunk, 1, sizeof chunk, f)) > 0 && b.len <= CC_MAX_FILE_BYTES) buf_append(&b, chunk, k);
    fclose(f);
    if (b.oom || b.len > CC_MAX_FILE_BYTES) {
        set_err(&load_err, "no model: file too large or out of memory: ", path);
        state = -1;
    } else if (cc_model_load(&model, b.p ? b.p : "", b.len, &load_err) != 0) {
        cc_model_free(&model);
        state = -1;
    } else {
        state = 1;
    }
    buf_free(&b);
}

const cc_model *cc_mesh_model(uint32_t *rev, cc_error *err) {
    if (!state) load();
    *rev = 0;
    if (state < 0) {
        *err = load_err;
        return NULL;
    }
    return &model;
}
