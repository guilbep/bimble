/* Suite vis_prev: the Show previous stacks and visibility states of session/visibility.h (ticket
 * ISS72, GitHub issue #72), without a host:
 * - a state captures the temporary set and the view's sets and lists; a hide for now that hides
 *   nothing is no temporary state; equality, restore and the five lists round trip;
 * - one stack per view: push, top, pop; at most CC_VIS_PREV_MAX (32) states a view, the oldest
 *   dropped (the 40 pushed: the last 32 kept, in order); another view's stack untouched;
 * - cc_vis_reset (a model opened, made or closed) clears every stack; cc_vis_free frees them.
 * Fails on the code before ISS72 (none of these functions exist: the suite does not build). */
#include <string.h>
#include "../session/visibility.h"
#include "../src/arena.h"
#include "check.h"

static cc_gid gid(int k) { /* a valid-looking 22-character id, ordered by k */
    cc_gid g;
    memset(g.c, 'A', 22);
    g.c[20] = (char)('A' + k / 26), g.c[21] = (char)('A' + k % 26);
    return g;
}

static void hide_now(cc_vis *v, int k) {
    cc_gid g = gid(k);
    cc_vis_hide_or_isolate(v, CC_VIS_HIDE_NOW, &g, 1);
}

int main(void) {
    cc_vis v;
    cc_vis_init(&v);
    cc_gid a = gid(1), b = gid(2), view1 = gid(100), view2 = gid(101);

    /* ---- states ---- */
    cc_vis_state s0, s1, s2;
    check_true("capture nothing hidden", !cc_vis_capture(&v, NULL, &s0) && !s0.has_temp && !s0.hidden.n && !s0.lists.len);
    hide_now(&v, 1);
    cc_vis_hide_or_isolate(&v, CC_VIS_UNHIDE, &a, 1); /* the temporary hide now hides nothing */
    check_true("a hide for now that hides nothing is no temporary state", !cc_vis_capture(&v, NULL, &s1) && cc_vis_state_eq(&s0, &s1));
    cc_vis_state_free(&s1);
    cc_vis_hide_or_isolate(&v, CC_VIS_ISOLATE_NOW, &b, 1);
    cc_vis_hide_or_isolate(&v, CC_VIS_HIDE_IN_VIEW, &a, 1);
    cc_visibility lists;
    memset(&lists, 0, sizeof lists);
    cc_str cls[2] = {{"IfcSlab", 7}, {"IfcWall", 7}}, mat[1] = {{"", 0}};
    lists.hidden_classes.v = cls, lists.hidden_classes.n = 2;
    lists.hidden_materials.v = mat, lists.hidden_materials.n = 1;
    check_true("capture b isolated for now, a hidden in the view, two classes and one material", !cc_vis_capture(&v, &lists, &s1) &&
               s1.has_temp && s1.temp_isolate && s1.temp.n == 1 && s1.hidden.n == 1 && s1.lists.len > 0);
    check_true("states differ", !cc_vis_state_eq(&s0, &s1));
    check_true("restore the empty state", !cc_vis_restore(&v, &s0) && !v.has_temp && !v.hidden.n && !v.temp.n);
    check_true("restore the other: b isolated for now, a hidden in the view",
               !cc_vis_restore(&v, &s1) && v.has_temp && v.temp_isolate && cc_gidset_has(&v.temp, &b) && cc_gidset_has(&v.hidden, &a));
    check_true("captured again: the same state", !cc_vis_capture(&v, &lists, &s2) && cc_vis_state_eq(&s1, &s2));
    cc_vis_state_free(&s2);
    Arena ar;
    arena_init(&ar, 0);
    cc_visibility back;
    memset(&back, 0, sizeof back);
    check_true("the five lists round trip", !cc_vis_state_lists(&s1, &ar, &back) && back.hidden_classes.n == 2 &&
               !memcmp(back.hidden_classes.v[1].p, "IfcWall", 7) && back.hidden_materials.n == 1 && back.hidden_materials.v[0].n == 0 &&
               !back.hidden_families.n && !back.hidden_storeys.n && !back.hidden_links.n);
    arena_free(&ar);

    /* ---- stacks ---- */
    check_true("empty: no top, count 0", !cc_vis_prev_top(&v, &view1) && !cc_vis_prev_count(&v, &view1));
    check_true("push into view 2", !cc_vis_prev_push(&v, &view2, &s1) && !s1.hidden.v && cc_vis_prev_count(&v, &view2) == 1);
    cc_vis_reset(&v);
    for (int i = 0; i < 40; i++) { /* state i: element i hidden for now */
        hide_now(&v, 10 + i);
        cc_vis_state st;
        if (cc_vis_capture(&v, NULL, &st) || cc_vis_prev_push(&v, &view1, &st)) check_true("push", 0);
        cc_vis_clear_temporary(&v);
    }
    check_true("40 pushed in view 1: 32 kept", cc_vis_prev_count(&v, &view1) == CC_VIS_PREV_MAX && CC_VIS_PREV_MAX == 32);
    cc_gid g39 = gid(10 + 39), g38 = gid(10 + 38), g8 = gid(10 + 8);
    const cc_vis_state *t = cc_vis_prev_top(&v, &view1);
    check_true("top: the last pushed", t && t->temp.n == 1 && !cc_gid_cmp(&t->temp.v[0], &g39));
    cc_vis_prev_pop(&v, &view1);
    t = cc_vis_prev_top(&v, &view1);
    check_true("pop: the one before", t && !cc_gid_cmp(&t->temp.v[0], &g38) && cc_vis_prev_count(&v, &view1) == 31);
    for (int i = 0; i < 30; i++) cc_vis_prev_pop(&v, &view1);
    t = cc_vis_prev_top(&v, &view1);
    check_true("the oldest kept is state 8 (0..7 dropped)", t && !cc_gid_cmp(&t->temp.v[0], &g8) && cc_vis_prev_count(&v, &view1) == 1);
    cc_vis_state st;
    cc_vis_capture(&v, NULL, &st);
    cc_vis_prev_push(&v, &view2, &st);
    check_true("view 2's stack is its own", cc_vis_prev_count(&v, &view2) == 1 && cc_vis_prev_count(&v, &view1) == 1);
    cc_vis_prev_pop(&v, &view2);
    check_true("pop in view 2 leaves view 1", !cc_vis_prev_count(&v, &view2) && cc_vis_prev_count(&v, &view1) == 1);
    cc_vis_reset(&v);
    check_true("reset (another model): every stack cleared", !v.nprev && !cc_vis_prev_top(&v, &view1));
    cc_vis_capture(&v, NULL, &st);
    cc_vis_prev_push(&v, &view1, &st);
    cc_vis_state_free(&s0);
    cc_vis_free(&v); /* frees the stack: ASan's leak check */
    return report("vis_prev");
}
