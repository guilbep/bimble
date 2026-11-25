/* Suite boolean: src/boolean.h (boolean.rs in C, ticket C6).
 * - with the kernel (manifold, CC_HAVE_MANIFOLD): box minus box volumes (boolean.rs tests: wall
 *   minus door and window at site coordinates, corners 0.05 mm apart sewn, a real gap refused),
 *   is_closed on a tetrahedron and an open one, degenerate inputs (an open box, empty, one
 *   triangle, NaN, a flat box) give an error or an empty result, never a crash.
 * - always: nothing to cut gives the input (intersection: nothing); snap; without a kernel
 *   (cc_boolean_set(NULL), or a build without manifold) apply says CC_BOOL_NO_KERNEL.
 * Run it under ASan/UBSan too (CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh boolean). */
#include <stdint.h>
#include <string.h>
#include "../src/boolean.h"
#include "check.h"

typedef double T9[9];

/* boolean.rs cuboid: closed box, outward, two triangles per face. Returns 12. */
static size_t cuboid(const double lo[3], const double hi[3], T9 *o) {
    double p[8][3];
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 3; k++) p[i][k] = (i >> k & 1) ? hi[k] : lo[k];
#define P(x, y, z) ((x) | (y) << 1 | (z) << 2)
    static const int q[6][4] = {
        {P(0, 0, 0), P(0, 1, 0), P(1, 1, 0), P(1, 0, 0)}, {P(0, 0, 1), P(1, 0, 1), P(1, 1, 1), P(0, 1, 1)},
        {P(0, 0, 0), P(1, 0, 0), P(1, 0, 1), P(0, 0, 1)}, {P(0, 1, 0), P(0, 1, 1), P(1, 1, 1), P(1, 1, 0)},
        {P(0, 0, 0), P(0, 0, 1), P(0, 1, 1), P(0, 1, 0)}, {P(1, 0, 0), P(1, 1, 0), P(1, 1, 1), P(1, 0, 1)}};
#undef P
    size_t n = 0;
    for (int f = 0; f < 6; f++) {
        const int t[2][3] = {{q[f][0], q[f][1], q[f][2]}, {q[f][0], q[f][2], q[f][3]}};
        for (int h = 0; h < 2; h++, n++)
            for (int c = 0; c < 3; c++) memcpy(&o[n][3 * c], p[t[h][c]], 24);
    }
    return n;
}

static double volume(const T9 *t, size_t n) {
    if (!n) return 0;
    const double *o = t[0];
    double v = 0;
    for (size_t i = 0; i < n; i++) {
        double a[3], b[3], c[3];
        for (int k = 0; k < 3; k++) a[k] = t[i][k] - o[k], b[k] = t[i][3 + k] - o[k], c[k] = t[i][6 + k] - o[k];
        v += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
    }
    return v / 6.0;
}
static double fabs_(double x) { return x < 0 ? -x : x; }
static cc_tri_set set(const T9 *t, size_t n) {
    cc_tri_set s = {(const double(*)[9])t, n};
    return s;
}

/* A welded mesh of a soup (exact bits), for is_closed. */
static void weld(const T9 *t, size_t n, double *pos, uint64_t *idx, cc_bmesh *m) {
    size_t nv = 0;
    for (size_t i = 0; i < 3 * n; i++) {
        const double *p = &t[i / 3][3 * (i % 3)];
        size_t j;
        for (j = 0; j < nv && memcmp(&pos[3 * j], p, 24); j++) {}
        if (j == nv) memcpy(&pos[3 * nv++], p, 24);
        idx[i] = j;
    }
    m->pos = pos, m->nv = nv, m->idx = idx, m->nt = n;
}

static void with_kernel(void) {
    const double x = 2500000000.0, y = 1100000000.0;
    T9 wall[12], door[12], window[12];
    cuboid((double[]){x, y, 0}, (double[]){x + 6000, y + 200, 2800}, wall);
    cuboid((double[]){x + 1000, y - 10, 0}, (double[]){x + 2200, y + 210, 2100}, door);
    cuboid((double[]){x + 3000, y - 10, 900}, (double[]){x + 4200, y + 210, 2100}, window);
    double (*r)[9];
    size_t nr;
    char err[CC_BOOL_ERR];
    /* boolean.rs wall_minus_openings_is_exact_at_site_coordinates */
    cc_tri_set b[2] = {set(door, 12), set(window, 12)};
    int rc = cc_boolean_apply(CC_BOOL_DIFFERENCE, set(wall, 12), b, 2, &r, &nr, err);
    double want = 6000.0 * 200 * 2800 - 1200.0 * 200 * 2100 - 1200.0 * 200 * 1200;
    double v = rc ? 0 : volume((const T9 *)r, nr);
    printf("wall - door - window: rc %d, %zu triangles, volume %.6f (want %.0f)\n", rc, nr, v, want);
    check_true("box minus boxes: exact volume at site coordinates", rc == 0 && fabs_(v - want) / want < 1e-9);
    free(r);
    /* Intersection and union of two unit boxes overlapping by half. */
    T9 a[12], c[12];
    cuboid((double[]){0, 0, 0}, (double[]){1, 1, 1}, a);
    cuboid((double[]){0.5, 0, 0}, (double[]){1.5, 1, 1}, c);
    cc_tri_set cs = set(c, 12);
    rc = cc_boolean_apply(CC_BOOL_INTERSECTION, set(a, 12), &cs, 1, &r, &nr, err);
    check_true("intersection volume 0.5", rc == 0 && fabs_(volume((const T9 *)r, nr) - 0.5) < 1e-12);
    free(r);
    rc = cc_boolean_apply(CC_BOOL_UNION, set(a, 12), &cs, 1, &r, &nr, err);
    check_true("union volume 1.5", rc == 0 && fabs_(volume((const T9 *)r, nr) - 1.5) < 1e-12);
    free(r);
    /* boolean.rs corners_a_hair_apart_are_sewn */
    double top[3] = {x + 6000, y + 200, 2800};
    size_t k = 0;
    for (; k < 12; k++)
        if (!memcmp(&wall[k][0], top, 24) || !memcmp(&wall[k][3], top, 24) || !memcmp(&wall[k][6], top, 24)) break;
    for (int c3 = 0; c3 < 3; c3++)
        if (!memcmp(&wall[k][3 * c3], top, 24)) wall[k][3 * c3] += 0.05;
    cc_tri_set d1 = set(door, 12);
    rc = cc_boolean_apply(CC_BOOL_DIFFERENCE, set(wall, 12), &d1, 1, &r, &nr, err);
    want = 6000.0 * 200 * 2800 - 1200.0 * 200 * 2100;
    check_true("corners 0.05 mm apart: sewn and cut", rc == 0 && fabs_(volume((const T9 *)r, nr) - want) / want < 1e-5);
    free(r);
    for (int c3 = 0; c3 < 3; c3++)
        if (wall[k][3 * c3] == top[0] + 0.05) wall[k][3 * c3] += 5.0;
    rc = cc_boolean_apply(CC_BOOL_DIFFERENCE, set(wall, 12), &d1, 1, &r, &nr, err);
    printf("a 5 mm gap: rc %d (%s)\n", rc, err);
    check_true("a real gap: not closed", rc == CC_BOOL_NOT_CLOSED && !strcmp(err, "not a closed solid: manifold3d status: NotManifold"));
    free(r);
    /* is_closed: a tetrahedron, then one face dropped. */
    const cc_boolean *kk = cc_boolean_kernel();
    T9 tet[4] = {{0, 0, 0, 0, 1, 0, 1, 0, 0}, {0, 0, 0, 1, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 1, 0, 1, 0},
                 {1, 0, 0, 0, 1, 0, 0, 0, 1}};
    double pos[12 * 3];
    uint64_t idx[12];
    cc_bmesh m;
    weld((const T9 *)tet, 4, pos, idx, &m);
    check_true("is_closed: tetrahedron", m.nv == 4 && kk->is_closed(&m));
    weld((const T9 *)tet, 3, pos, idx, &m);
    check_true("is_closed: open tetrahedron is not", !kk->is_closed(&m));
    /* Degenerate inputs: an error or an empty result, no crash. */
    T9 open[12];
    cuboid((double[]){0, 0, 0}, (double[]){1, 1, 1}, open);
    rc = cc_boolean_apply(CC_BOOL_DIFFERENCE, set(open, 10), &cs, 1, &r, &nr, err);
    check_true("open box: not closed", rc == CC_BOOL_NOT_CLOSED);
    free(r);
    T9 one = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    rc = cc_boolean_apply(CC_BOOL_DIFFERENCE, set(&one, 1), &cs, 1, &r, &nr, err);
    check_true("one triangle: not closed", rc == CC_BOOL_NOT_CLOSED);
    free(r);
    T9 nan[12];
    cuboid((double[]){0, 0, 0}, (double[]){1, 1, 1}, nan);
    nan[3][4] = 0.0 / 0.0;
    rc = cc_boolean_apply(CC_BOOL_DIFFERENCE, set(nan, 12), &cs, 1, &r, &nr, err);
    printf("NaN corner: rc %d (%s)\n", rc, err);
    check_true("NaN corner: an error", rc == CC_BOOL_NOT_CLOSED);
    free(r);
    T9 flat[12];
    cuboid((double[]){0, 0, 0}, (double[]){1, 1, 0}, flat);
    rc = cc_boolean_apply(CC_BOOL_DIFFERENCE, set(flat, 12), &cs, 1, &r, &nr, err);
    printf("flat box: rc %d, %zu triangles\n", rc, nr);
    check_true("flat box: an error or nothing", rc > 0 || (rc == 0 && fabs_(volume((const T9 *)r, nr)) < 1e-12));
    free(r);
    rc = cc_boolean_apply(CC_BOOL_DIFFERENCE, set(a, 0), &cs, 1, &r, &nr, err);
    check_true("empty minus a box: nothing", rc == 0 && nr == 0);
    free(r);
    rc = cc_boolean_apply(CC_BOOL_DIFFERENCE, set(c, 12), &cs, 1, &r, &nr, err);
    check_true("a box minus itself: nothing", rc == 0 && nr == 0);
    free(r);
}

int main(void) {
    printf("boolean kernel: %s\n", cc_boolean_kernel() ? "manifold" : "none");
    check_true("default kernel is the built-in one", cc_boolean_kernel() == cc_boolean_manifold());
    if (cc_boolean_kernel()) with_kernel();
    /* Nothing to cut: the input; an intersection with nothing: nothing (no kernel needed). */
    T9 a[12];
    cuboid((double[]){0, 0, 0}, (double[]){1, 1, 1}, a);
    double (*r)[9];
    size_t nr;
    char err[CC_BOOL_ERR];
    cc_tri_set none = {NULL, 0};
    int rc = cc_boolean_apply(CC_BOOL_DIFFERENCE, set(a, 12), &none, 1, &r, &nr, err);
    check_true("nothing to cut: the input", rc == 0 && nr == 12 && !memcmp(r, a, sizeof a));
    free(r);
    rc = cc_boolean_apply(CC_BOOL_INTERSECTION, set(a, 12), NULL, 0, &r, &nr, err);
    check_true("intersection with nothing: nothing", rc == 0 && nr == 0 && !r);
    /* snap: a corner 0.05 off joins, a triangle that collapses is dropped. */
    T9 s[2] = {{0, 0, 0, 1, 0, 0, 0, 1, 0}, {0.05, 0, 0, 0, 0.03, 0, 0, 0, 1}};
    rc = cc_boolean_snap(set(s, 2), CC_SEW_MM, &r, &nr);
    check_true("snap: collapsed triangle dropped", rc == 0 && nr == 1 && !memcmp(r[0], s[0], sizeof s[0]));
    free(r);
    T9 s2[1] = {{0.05, -0.05, 0, 1, 0, 0, 0, 1, 0}};
    T9 s3[2];
    memcpy(s3[0], s[0], sizeof s3[0]), memcpy(s3[1], s2[0], sizeof s3[1]);
    s3[1][3] = 1.0, s3[1][4] = 0.0, s3[1][5] = 1.0;
    rc = cc_boolean_snap(set(s3, 2), CC_SEW_MM, &r, &nr);
    check_true("snap: moved onto the earlier point", rc == 0 && nr == 2 && r[1][0] == 0 && r[1][1] == 0);
    free(r);
    /* No kernel: said so. */
    const cc_boolean *k = cc_boolean_kernel();
    cc_boolean_set(NULL);
    T9 c[12];
    cuboid((double[]){0.5, 0, 0}, (double[]){1.5, 1, 1}, c);
    cc_tri_set cs = set(c, 12);
    rc = cc_boolean_apply(CC_BOOL_DIFFERENCE, set(a, 12), &cs, 1, &r, &nr, err);
    check_true("no kernel: CC_BOOL_NO_KERNEL", rc == CC_BOOL_NO_KERNEL && !r && nr == 0);
    cc_boolean_set(k);
    return report("boolean");
}
