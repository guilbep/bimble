/* Suite two_sessions (ticket A1): two sessions in one process (Mission 1: superposed files).
 * Session A opens golden/frame.json, session B golden/frame2.json, both on the native platform.
 * A verb in A changes A's model, rev, history, scene and /mesh; B's stay byte for byte. Each
 * query answers from its own session, and each session keeps its own scene and /mesh cache. */
#include <string.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/mesh.h"
#include "../session/session.h"
#include "check.h"

/* cc_call into a fresh buffer; returns the status, the answer in *out (caller frees). */
static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_init(out);
    return cc_call(s, op, name, body, strlen(body), out);
}

/* The answer to a query, as a NUL-terminated heap string. */
static char *ask(cc_session *s, const char *name, const char *body) {
    Buf b;
    int st = call(s, "query", name, body, &b);
    if (st != CC_OK) printf("query %s: %d %.*s\n", name, st, (int)b.len, b.p ? b.p : "");
    const char *t = buf_cstr(&b);
    char *z = malloc(strlen(t) + 1);
    strcpy(z, t);
    buf_free(&b);
    return z;
}

static int contains(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

/* The /mesh bytes of `s` (copied), its rev and whether it was built now. */
static char *mesh(cc_session *s, size_t *n, uint32_t *rev, int *built) {
    const char *p;
    double ms;
    int st = cc_mesh_get(s, &p, n, rev, &ms);
    check_true("mesh 200", st == 200);
    *built = ms > 0;
    char *c = malloc(*n ? *n : 1);
    memcpy(c, p, *n);
    return c;
}

int main(int argc, char **argv) {
    char a_path[4096], b_path[4096];
    const char *floats = argc > 1 ? argv[1] : "ccore/golden/floats";
    snprintf(a_path, sizeof a_path, "%s/../frame.json", floats);
    snprintf(b_path, sizeof b_path, "%s/../frame2.json", floats);

    const cc_platform *pf = cc_platform_native();
    cc_session *A = cc_session_new(pf), *B = cc_session_new(pf);
    check_true("two sessions", A && B && A != B);
    cc_error err;
    if (cc_session_open(A, a_path, &err) || cc_session_open(B, b_path, &err)) {
        printf("open: %s\n", err.msg);
        return 1;
    }
    size_t na = cc_session_model(A)->nelements, nb = cc_session_model(B)->nelements;
    check_true("the two files differ", na != nb);
    check_true("both at rev 1", cc_session_rev(A) == 1 && cc_session_rev(B) == 1);

    /* Queries answer per session. */
    char *sa = ask(A, "state", ""), *sb = ask(B, "state", "");
    check_true("A's state names frame.json", contains(sa, "\"frame.json\"") && !contains(sa, "frame2"));
    check_true("B's state names frame2.json", contains(sb, "\"frame2.json\""));
    char *pa = ask(A, "project", ""), *pb = ask(B, "project", "");
    check_true("B's project is frame 2", contains(pb, "frame 2") && !contains(pa, "frame 2"));
    char *eb0 = ask(B, "elements", "");
    char *hb0 = ask(B, "history", "");

    /* Each session builds and caches its own scene and /mesh. */
    size_t ma_n, mb_n;
    uint32_t ra, rb;
    int built;
    char *ma = mesh(A, &ma_n, &ra, &built);
    check_true("A's mesh built", built && ra == 1);
    char *mb = mesh(B, &mb_n, &rb, &built);
    check_true("B's mesh built", built && rb == 1);
    check_true("the meshes differ", ma_n != mb_n || memcmp(ma, mb, ma_n));
    free(mesh(A, &ma_n, &ra, &built));
    check_true("A's mesh still cached after B's", !built);
    const cc_scene *scb = cc_session_scene(B);
    check_true("two scenes", cc_session_scene(A) && scb && cc_session_scene(A) != scb);

    /* A verb in A: A changes, B does not. */
    Buf out;
    int st = call(A, "verb", "place", "{\"a\":[1500.0,1500.0],\"kind\":\"column\",\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\"}", &out);
    if (st != CC_OK) printf("verb: %.*s\n", (int)out.len, out.p ? out.p : "");
    buf_free(&out);
    check_true("verb in A", st == CC_OK);
    check_true("A has one more element", cc_session_model(A)->nelements == na + 1);
    check_true("A at rev 2", cc_session_rev(A) == 2);
    check_true("A is unsaved", cc_session_unsaved(A));
    check_true("B untouched: elements", cc_session_model(B)->nelements == nb);
    check_true("B untouched: rev", cc_session_rev(B) == 1);
    check_true("B untouched: saved", !cc_session_unsaved(B) && cc_session_step(B) == 0);
    char *eb1 = ask(B, "elements", ""), *hb1 = ask(B, "history", ""), *ha1 = ask(A, "history", "");
    check_true("B's elements answer unchanged", !strcmp(eb0, eb1));
    check_true("B's history unchanged", !strcmp(hb0, hb1));
    check_true("A's history has the step", strcmp(ha1, hb1) != 0);
    char *mb2 = mesh(B, &ma_n, &rb, &built);
    check_true("B's mesh served from its cache, same bytes", !built && rb == 1 && ma_n == mb_n && !memcmp(mb, mb2, mb_n));
    free(mesh(A, &ma_n, &ra, &built));
    check_true("A's mesh rebuilt at rev 2", built && ra == 2);

    /* Undo in B does nothing to A; undo in A takes the column back. */
    st = call(B, "undo", "-", "", &out);
    buf_free(&out);
    check_true("undo in B: nothing", st == CC_OK && cc_session_rev(B) == 1 && cc_session_model(A)->nelements == na + 1);
    st = call(A, "undo", "-", "", &out);
    buf_free(&out);
    check_true("undo in A", st == CC_OK && cc_session_model(A)->nelements == na && cc_session_rev(A) == 3);
    check_true("B still untouched", cc_session_model(B)->nelements == nb && cc_session_rev(B) == 1);

    /* Selection is per session. */
    const cc_gid *g = &cc_session_model(A)->elements[0].id;
    char body[64];
    snprintf(body, sizeof body, "[\"%.22s\"]", g->c);
    st = call(A, "select", "-", body, &out);
    if (st != CC_OK) printf("select: %.*s\n", (int)out.len, out.p ? out.p : "");
    buf_free(&out);
    check_true("select in A", st == CC_OK && A->has_primary);
    check_true("B has no selection", !B->has_primary && !B->nalso);

    free(sa), free(sb), free(pa), free(pb), free(eb0), free(hb0), free(eb1), free(hb1), free(ha1);
    free(ma), free(mb), free(mb2);
    cc_session_free(A);
    cc_session_free(B);
    cc_session_free(NULL);
    return report("two_sessions");
}
