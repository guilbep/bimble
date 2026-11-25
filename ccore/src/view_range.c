/* view.rs resolve: see view_range.h. */
#include "view_range.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int64_t tkey(double x) { /* f64::total_cmp */
    int64_t k;
    memcpy(&k, &x, 8);
    return k ^ (int64_t)((uint64_t)(k >> 63) >> 1);
}

static int fail(Buf *err, const char *s) {
    if (err) buf_puts(err, s);
    return 1;
}

int cc_view_resolve(const cc_model *m, const cc_view *v, double out[4], Buf *err) {
    Arena a;
    arena_init(&a, 0);
    cc_level *lv;
    size_t n;
    int rc = cc_levels_of_model(m, &a, &lv, &n) ? -1 : cc_view_resolve_lv(lv, n, v, out, err);
    arena_free(&a);
    return rc;
}

int cc_view_resolve_lv(const cc_level *lv, size_t n, const cc_view *v, double out[4], Buf *err) {
    const double inf = 1.0 / 0.0;
    size_t *o = malloc((n ? n : 1) * sizeof *o);
    if (!o) return -1;
    /* The levels by elevation (a stable sort, total_cmp). */
    for (size_t i = 0; i < n; i++) {
        size_t j = i;
        int64_t k = tkey(lv[i].elevation);
        while (j > 0 && tkey(lv[o[j - 1]].elevation) > k) o[j] = o[j - 1], j--;
        o[j] = i;
    }
    size_t i = n;
    if (v->kind == CC_VIEW_PLAN)
        for (size_t k = 0; k < n && i == n; k++)
            if (lv[o[k]].key.n == v->storey.n && (!v->storey.n || !memcmp(lv[o[k]].key.p, v->storey.p, v->storey.n))) i = k;
    if (i == n) {
        free(o);
        if (err) {
            buf_puts(err, "unknown storey ");
            if (v->kind == CC_VIEW_PLAN) buf_append(err, v->storey.p, v->storey.n);
        }
        return 1;
    }
    double here = lv[o[i]].elevation;
    for (int p = 0; p < 4; p++) {
        const cc_range_plane *r = &v->range[p];
        int unl = 0;
        double base = here;
        switch (r->level) {
        case CC_LEVEL_ASSOCIATED: break;
        case CC_LEVEL_ABOVE: /* the first level more than 1 mm higher; none: unlimited */
            unl = 1;
            for (size_t k = i + 1; k < n; k++)
                if (lv[o[k]].elevation > here + 1.0) {
                    base = lv[o[k]].elevation, unl = 0;
                    break;
                }
            break;
        case CC_LEVEL_BELOW: /* the first level more than 1 mm lower; none: this one */
            for (size_t k = i; k-- > 0;)
                if (lv[o[k]].elevation < here - 1.0) {
                    base = lv[o[k]].elevation;
                    break;
                }
            break;
        default: unl = 1;
        }
        double u = (p == CC_RANGE_BOTTOM || (p == CC_RANGE_DEPTH && v->look == CC_LOOK_DOWN)) ? -inf : inf;
        out[p] = unl ? u : base + r->offset_mm;
    }
    free(o);
    for (int p = 0; p < 4; p++) {
        double d = v->range[p].offset_mm;
        if (!(d == d) || d > 1.0e6 || d < -1.0e6) return fail(err, "offsets must be numbers within \xc2\xb1" "1000 m");
    }
    if (v->range[CC_RANGE_CUT].level == CC_LEVEL_UNLIMITED) return fail(err, "the cut plane needs a level");
    double t = out[CC_RANGE_TOP], c = out[CC_RANGE_CUT], b = out[CC_RANGE_BOTTOM], d = out[CC_RANGE_DEPTH];
    if (v->look == CC_LOOK_DOWN) {
        if (!(d <= b && b <= c && c <= t))
            return fail(err, "looking down, the planes must be in order: view depth \xe2\x89\xa4 bottom \xe2\x89\xa4 cut "
                             "\xe2\x89\xa4 top");
    } else if (!(b <= c && c <= t && t <= d)) {
        return fail(err, "looking up, the planes must be in order: bottom \xe2\x89\xa4 cut \xe2\x89\xa4 top \xe2\x89\xa4 "
                         "view depth");
    }
    return 0;
}
