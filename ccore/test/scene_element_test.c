/* Suite scene_element (ticket FIXUI 1): the properties of an element of a file open read only.
 * Rust's viewer shows them from its scene (viewer/lib.rs:4570 `properties`: the IFC class, Name,
 * GlobalId, Storey, Material, Family, Triangles, Volume); the C core answers them through
 * `query scene_element {"id"}` (links.h cc_scene_element_json), while `query element` keeps
 * Rust's refusal (golden/parity/ifc_readonly/queries.txt).
 *   - golden/ifc/frame.ifc (the IFC export of golden/frame.json, same GlobalIds) open as it is:
 *     every element of its scene answers, and its class, name, storey and material are those
 *     `query element` gives for the same GlobalId in golden/frame.json open for editing (an
 *     oracle the reader does not share); read_only true, triangles and volume above zero.
 *   - golden/view/frame.bimbleview: the same rows.
 *   - scene_tree (the Navigator's Objects: storey, class, count) and scene_elements (a row's
 *     elements): 72 in all, each storey's count is the editable model's `query storeys`
 *     element_count for that name, each class row lists that many elements.
 *   - errors: an unknown id, a missing id, an editable model (use `element`), no model.
 * Fails on the code before FIXUI: scene_element, scene_tree and scene_elements were unknown
 * queries (400): the lab showed only "Read-only file: import it to edit" for a selected IFC
 * element, and Objects 0 in the Navigator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host/host.h" /* cc_platform_native */
#include "../session/calls.h"
#include "../session/session.h"
#include "../src/arena.h"
#include "../src/buf.h"
#include "../src/json_r.h"
#include "../src/scene.h"
#include "check.h"

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_clear(out);
    out->oom = 0;
    return cc_call(s, op, name, body, strlen(body), out);
}

/* The string (or "null") of `key` in the JSON object `j`, into `dst`. */
static void field(const Buf *j, const char *key, char *dst, size_t cap) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = jr_parse(&a, j->p, j->len, &je);
    const JrVal *x = v && v->type == JR_OBJ ? jr_get(v, key) : NULL;
    if (!x) snprintf(dst, cap, "<missing>");
    else if (x->type == JR_STR) snprintf(dst, cap, "%.*s", (int)x->n, x->s);
    else if (x->type == JR_NULL) snprintf(dst, cap, "null");
    else if (x->type == JR_NUM) snprintf(dst, cap, "%.17g", x->num);
    else if (x->type == JR_BOOL) snprintf(dst, cap, "%s", x->b ? "true" : "false");
    else snprintf(dst, cap, "<other>");
    arena_free(&a);
}

static cc_session *open_at(const char *path, int read_only) {
    cc_session *s = cc_session_new(cc_platform_native());
    cc_error e;
    if (!s || (read_only ? cc_session_open_file(s, path, &e) : cc_session_open(s, path, &e))) {
        printf("cannot open %s: %s\n", path, s ? e.msg : "out of memory");
        check_true(path, 0);
        if (s) cc_session_free(s);
        return NULL;
    }
    return s;
}

/* Every element of `ro`'s scene against `ed`'s `query element`. */
static void same_as_model(const char *what, cc_session *ro, cc_session *ed, size_t want_n) {
    const cc_scene *sc = cc_session_scene(ro);
    check_true("the read-only scene is there", sc != NULL);
    if (!sc) return;
    Buf a, b;
    buf_init(&a), buf_init(&b);
    size_t n = 0, answered = 0, same = 0, rows = 0;
    for (size_t i = 0; i < sc->nelements; i++) {
        if (!sc->elements[i].id.c[0]) continue;
        n++;
        char body[64], fa[256], fb[256];
        snprintf(body, sizeof body, "{\"id\":\"%.22s\"}", sc->elements[i].id.c);
        int ra = call(ro, "query", "scene_element", body, &a);
        int rb = call(ed, "query", "element", body, &b);
        answered += ra == CC_OK;
        if (ra != CC_OK || rb != CC_OK) {
            if (n - answered < 3) printf("  %s %s: %.*s | %.*s\n", what, body, (int)a.len, a.p, (int)b.len, b.p);
            continue;
        }
        int ok = 1;
        static const char *const KEYS[] = {"id", "class", "name", "storey_name", "material_name"};
        for (size_t k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++) {
            field(&a, KEYS[k], fa, sizeof fa), field(&b, KEYS[k], fb, sizeof fb);
            if (strcmp(fa, fb)) {
                ok = 0;
                if (same + 3 > n) printf("  %s %s %s: \"%s\" vs the model's \"%s\"\n", what, body, KEYS[k], fa, fb);
            }
        }
        same += ok;
        field(&a, "read_only", fa, sizeof fa);
        int ro_ok = !strcmp(fa, "true");
        field(&a, "triangles", fa, sizeof fa);
        ro_ok &= atof(fa) > 0;
        field(&a, "volume_m3", fa, sizeof fa);
        ro_ok &= atof(fa) > 0;
        field(&a, "family", fa, sizeof fa);
        ro_ok &= strcmp(fa, "<missing>") != 0;
        rows += ro_ok;
    }
    char m[160];
    snprintf(m, sizeof m, "%s: %zu elements with a GlobalId (want %zu)", what, n, want_n);
    check_true(m, n == want_n);
    snprintf(m, sizeof m, "%s: scene_element answers each (%zu of %zu)", what, answered, n);
    check_true(m, n && answered == n);
    snprintf(m, sizeof m, "%s: id, class, name, storey, material as the model's (%zu of %zu)", what, same, n);
    check_true(m, n && same == n);
    snprintf(m, sizeof m, "%s: read_only, family, triangles > 0, volume > 0 (%zu of %zu)", what, rows, n);
    check_true(m, n && rows == n);
    buf_free(&a), buf_free(&b);
}

/* scene_tree: the total, and per storey the count the editable model's `query storeys` gives
 * (element_count by name); scene_elements of each class row gives that many ids. */
static void tree(const char *what, cc_session *ro, cc_session *ed) {
    Buf t, st, el;
    buf_init(&t), buf_init(&st), buf_init(&el);
    int rc = call(ro, "query", "scene_tree", "", &t);
    check_true("scene_tree answers", rc == CC_OK);
    call(ed, "query", "storeys", "", &st);
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = jr_parse(&a, t.p, t.len, &je), *m = jr_parse(&a, st.p, st.len, &je);
    const JrVal *ss = v && v->type == JR_OBJ ? jr_get(v, "storeys") : NULL, *tot = v && v->type == JR_OBJ ? jr_get(v, "total") : NULL;
    size_t sum = 0, matched = 0, rows = 0, rows_ok = 0, nst = ss && ss->type == JR_ARR ? ss->n : 0;
    for (size_t i = 0; i < nst; i++) {
        const JrVal *g = &ss->items[i], *nm = jr_get(g, "name"), *cnt = jr_get(g, "count"), *cls = jr_get(g, "classes");
        if (!nm || !cnt || !cls) continue;
        sum += (size_t)cnt->num;
        for (size_t k = 0; m && m->type == JR_ARR && k < m->n; k++) {
            const JrVal *mn = jr_get(&m->items[k], "name"), *mc = jr_get(&m->items[k], "element_count");
            if (mn && mc && mn->n == nm->n && !memcmp(mn->s, nm->s, nm->n) && mc->num == cnt->num) matched++;
        }
        for (size_t c = 0; cls->type == JR_ARR && c < cls->n; c++) {
            const JrVal *cn = jr_get(&cls->items[c], "class"), *cc = jr_get(&cls->items[c], "count");
            char body[512];
            snprintf(body, sizeof body, "{\"storey\":\"%.*s\",\"class\":\"%.*s\"}", (int)nm->n, nm->s, (int)cn->n, cn->s);
            rows++;
            if (call(ro, "query", "scene_elements", body, &el) != CC_OK) continue;
            Arena b;
            arena_init(&b, 0);
            const JrVal *x = jr_parse(&b, el.p, el.len, &je);
            rows_ok += x && x->type == JR_ARR && x->n == (size_t)cc->num;
            arena_free(&b);
        }
    }
    char msg[160];
    snprintf(msg, sizeof msg, "%s: scene_tree total 72 and the storeys sum to it (%zu)", what, sum);
    check_true(msg, tot && tot->num == 72 && sum == 72);
    snprintf(msg, sizeof msg, "%s: each storey's count is the model's element_count (%zu of %zu)", what, matched, nst);
    check_true(msg, nst >= 2 && matched == nst);
    snprintf(msg, sizeof msg, "%s: scene_elements lists each class row's count (%zu of %zu)", what, rows_ok, rows);
    check_true(msg, rows && rows_ok == rows);
    arena_free(&a);
    rc = call(ed, "query", "scene_tree", "", &t);
    check_true("scene_tree on an editable model: refused", rc == CC_ERR);
    rc = call(ro, "query", "scene_elements", "{}", &t);
    check_eq("scene_elements without a storey", t.p, rc == CC_ERR ? t.len : 0, "storey: a string", 16);
    buf_free(&t), buf_free(&st), buf_free(&el);
}

int main(void) {
    cc_session *ed = open_at("golden/frame.json", 0);
    cc_session *ifc = open_at("golden/ifc/frame.ifc", 1);
    cc_session *vf = open_at("golden/view/frame.bimbleview", 1);
    if (!ed || !ifc || !vf) return report("scene_element");
    same_as_model("frame.ifc", ifc, ed, 72);
    same_as_model("frame.bimbleview", vf, ed, 72);
    tree("frame.ifc", ifc, ed);
    tree("frame.bimbleview", vf, ed);

    Buf b;
    buf_init(&b);
    const char *first = NULL;
    const cc_scene *sc = cc_session_scene(ifc);
    for (size_t i = 0; sc && i < sc->nelements && !first; i++)
        if (sc->elements[i].id.c[0]) first = sc->elements[i].id.c;
    char body[64];
    snprintf(body, sizeof body, "{\"id\":\"%.22s\"}", first ? first : "");
    int rc = call(ifc, "query", "element", body, &b);
    check_eq("element on a read-only file keeps Rust's refusal", b.p, rc == CC_ERR ? b.len : 0,
             "this file is read only: import it to edit", 41);
    rc = call(ifc, "query", "scene_element", "{\"id\":\"0000000000000000000000\"}", &b);
    check_eq("unknown id", b.p, rc == CC_ERR ? b.len : 0, "no element with this id", 23);
    rc = call(ifc, "query", "scene_element", "{}", &b);
    check_eq("missing id", b.p, rc == CC_ERR ? b.len : 0, "id: a string", 12);
    rc = call(ed, "query", "scene_element", body, &b);
    check_true("an editable model: refused (query element)", rc == CC_ERR && strstr(b.p ? b.p : "", "editable"));
    cc_session *none = cc_session_new(cc_platform_native());
    rc = none ? call(none, "query", "scene_element", body, &b) : CC_OK;
    check_eq("no model", b.p, rc == CC_ERR ? b.len : 0, "no model open", 13);
    rc = none ? call(none, "query", "scene_tree", "", &b) : CC_ERR;
    check_eq("scene_tree without a model: the empty tree", b.p, rc == CC_OK ? b.len : 0, "{\"storeys\":[],\"total\":0}", 24);
    if (none) cc_session_free(none);
    buf_free(&b);
    cc_session_free(ed), cc_session_free(ifc), cc_session_free(vf);
    return report("scene_element");
}
