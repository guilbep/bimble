/* view.rs default_views and effective: see view_effective.h. */
#include "view_effective.h"
#include <stdint.h>
#include <string.h>
#include "guid.h"
#include "view_io.h"

cc_gid cc_view_derived_id(const char *pre, const cc_gid *storey) {
    char seed[64];
    size_t n = strlen(pre);
    memcpy(seed, pre, n);
    if (storey) memcpy(seed + n, storey->c, 22), n += 22;
    cc_guid g = cc_guid_derived(seed, n);
    cc_gid out;
    memcpy(out.c, g.c, 22);
    return out;
}

static int64_t tkey(double x) { /* f64::total_cmp */
    int64_t k;
    memcpy(&k, &x, 8);
    return k ^ (int64_t)((uint64_t)(k >> 63) >> 1);
}

static double round_(double x) { /* f64::round: half away from zero */
    double a = x < 0 ? -x : x;
    if (!(a < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x;
    double d = x - t;
    if (d >= 0.5) t += 1.0;
    else if (d <= -0.5) t -= 1.0;
    return t == 0.0 ? x * 0.0 : t;
}

/* The storeys by elevation (a stable sort of the id-ordered storeys, total_cmp): indices. */
static size_t *sorted_storeys(const cc_model *m, Arena *a) {
    size_t n = m->nstoreys;
    size_t *o = arena_alloc(a, (n ? n : 1) * sizeof *o, sizeof(size_t));
    if (!o) return NULL;
    for (size_t i = 0; i < n; i++) {
        size_t j = i;
        int64_t k = tkey(m->storeys[i].elevation);
        while (j > 0 && tkey(m->storeys[o[j - 1]].elevation) > k) o[j] = o[j - 1], j--;
        o[j] = i;
    }
    return o;
}

static cc_str cat2(Arena *a, const char *pre, cc_str s) {
    size_t n = strlen(pre);
    char *p = arena_alloc(a, n + s.n + 1, 1);
    cc_str out = {NULL, 0};
    if (!p) return out;
    memcpy(p, pre, n);
    if (s.n) memcpy(p + n, s.p, s.n);
    p[n + s.n] = 0;
    out.p = p, out.n = n + s.n;
    return out;
}

int cc_views_default(const cc_model *m, Arena *a, cc_view *out) {
    size_t n = m ? m->nstoreys : 0, k = 0;
    memset(&out[k], 0, sizeof out[k]);
    out[k].id = cc_view_derived_id("bimble view 3d", NULL);
    out[k].name.p = "3D", out[k].name.n = 2;
    out[k].kind = CC_VIEW_THREE_D;
    out[k].scale = 100;
    k++;
    if (!n) return 0;
    size_t *o = sorted_storeys(m, a);
    if (!o) return -1;
    for (int look = 0; look < 2; look++)
        for (size_t i = 0; i < n; i++) {
            const cc_storey *s = &m->storeys[o[i]];
            int any = 0;
            double h = 0.0;
            for (size_t j = i + 1; j < n; j++) { /* the first storey above more than 1 mm higher */
                double d = m->storeys[o[j]].elevation - s->elevation;
                if (d > 1.0) {
                    any = 1, h = d;
                    break;
                }
            }
            cc_view *v = &out[k++];
            memset(v, 0, sizeof *v);
            v->id = cc_view_derived_id(look ? "bimble view plan up " : "bimble view plan down ", &s->id);
            v->name = cat2(a, look ? "Ceiling " : "Plan ", s->name);
            char *sid = arena_dup(a, s->id.c, 22);
            if (!v->name.p || !sid) return -1;
            v->kind = CC_VIEW_PLAN;
            v->storey.p = sid, v->storey.n = 22;
            v->look = look ? CC_LOOK_UP : CC_LOOK_DOWN;
            cc_range_plane top = {CC_LEVEL_ABOVE, 0.0}, cut = {CC_LEVEL_ASSOCIATED, look ? 2300.0 : 1200.0},
                           bottom = {CC_LEVEL_ASSOCIATED, 0.0}, depth = {look ? CC_LEVEL_ABOVE : CC_LEVEL_ASSOCIATED, 0.0};
            if (any && cut.offset_mm >= h) cut.offset_mm = round_(h / 2.0);
            v->range[CC_RANGE_TOP] = top, v->range[CC_RANGE_CUT] = cut;
            v->range[CC_RANGE_BOTTOM] = bottom, v->range[CC_RANGE_DEPTH] = depth;
            v->scale = 100;
        }
    return 0;
}

int cc_views_effective(const cc_model *m, Arena *a, cc_view **out, size_t *n) {
    *out = NULL, *n = 0;
    size_t nd = 1 + (m ? 2 * m->nstoreys : 0), ns = m ? m->nviews : 0;
    cc_view *v = arena_alloc(a, (nd + ns) * sizeof *v, sizeof(double));
    if (!v || cc_views_default(m, a, v)) return -1;
    size_t k = nd;
    for (size_t i = 0; i < nd; i++) { /* stored over the defaults */
        const cc_view *st = m ? cc_model_view(m, &v[i].id) : NULL;
        if (st) v[i] = *st;
    }
    for (size_t i = 0; i < ns; i++) { /* the other stored views, id order */
        const cc_view *st = &m->views[i];
        int dflt = 0;
        for (size_t j = 0; j < nd && !dflt; j++) dflt = !cc_gid_cmp(&v[j].id, &st->id);
        if (!dflt) v[k++] = *st;
    }
    *out = v, *n = k;
    return 0;
}
