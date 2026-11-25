/* See ifc_out_grid.h. The texts follow ifc_out.c's writer: '#<id>=' per entity, REAL as Rust's
 * {:?}, strings as Rust's `string`, relationship GlobalIds derived from the project id. */
#include "ifc_out_grid.h"
#include <stdlib.h>
#include <string.h>
#include "grid.h"
#include "guid.h"
#include "ifc_out.h"

typedef struct {
    Buf *d;
    uint64_t next;
    int err;
} G;

static uint64_t begin(G *g) {
    uint64_t id = g->next++;
    buf_putc(g->d, '#'), buf_u64(g->d, id), buf_putc(g->d, '=');
    return id;
}
static void end(G *g) { buf_puts(g->d, ";\n"); }
static void put(G *g, const char *s) { buf_puts(g->d, s); }
static void ref(G *g, uint64_t id) { buf_putc(g->d, '#'), buf_u64(g->d, id); }
static void real(G *g, double x) {
    if (cc_step_real(g->d, x)) g->err = 1;
}
static void pt2(G *g, const double p[2]) {
    put(g, "IFCCARTESIANPOINT(("), real(g, p[0]), put(g, ","), real(g, p[1]), put(g, "))");
}

/* The storey grids belong to: index in m->storeys, or -1 (the building). */
static long ground(const cc_model *m) {
    long best = -1;
    for (size_t i = 0; i < m->nstoreys; i++) {
        const cc_storey *s = &m->storeys[i];
        if (s->not_storey) continue;
        if (best < 0) { best = (long)i; continue; }
        const cc_storey *b = &m->storeys[best];
        double ds = s->elevation < 0 ? -s->elevation : s->elevation;
        double db = b->elevation < 0 ? -b->elevation : b->elevation;
        if (ds < db || (ds == db && s->elevation < b->elevation)) best = (long)i;
        /* equal: model order is id order, the first stays */
    }
    return best;
}

int cc_ifc_out_grids(const cc_model *m, Buf *d, uint64_t *next, uint64_t ctx, uint64_t world, const uint64_t *st_pl,
                     const uint64_t *st_ent, uint64_t bldg_pl, uint64_t bldg) {
    if (!m->ngrids) return 0;
    G g = {d, *next, 0};
    long gs = ground(m);
    uint64_t rel_pl = gs >= 0 ? st_pl[gs] : bldg_pl, rel_to = gs >= 0 ? st_ent[gs] : bldg;
    uint64_t sub = begin(&g);
    put(&g, "IFCGEOMETRICREPRESENTATIONSUBCONTEXT('FootPrint','Model',*,*,*,*,"), ref(&g, ctx);
    put(&g, ",$,.PLAN_VIEW.,$)"), end(&g);
    uint64_t *grid_ent = malloc(m->ngrids * sizeof *grid_ent);
    uint64_t *ax = NULL, *cv = NULL;
    size_t cap = 0;
    if (!grid_ent) return -1;
    for (size_t i = 0; i < m->ngrids && !g.err; i++) {
        const cc_grid *gr = &m->grids[i];
        if (gr->naxes > cap) {
            free(ax), free(cv);
            cap = gr->naxes;
            ax = malloc(cap * sizeof *ax), cv = malloc(cap * sizeof *cv);
            if (!ax || !cv) { g.err = 1; break; }
        }
        for (size_t k = 0; k < gr->naxes; k++) {
            const cc_grid_axis *a = &gr->axes[k];
            uint64_t p1 = begin(&g);
            pt2(&g, a->start), end(&g);
            uint64_t p2 = begin(&g);
            pt2(&g, a->end), end(&g);
            cv[k] = begin(&g);
            put(&g, "IFCPOLYLINE(("), ref(&g, p1), put(&g, ","), ref(&g, p2), put(&g, "))"), end(&g);
            ax[k] = begin(&g);
            put(&g, "IFCGRIDAXIS("), cc_step_string(d, a->tag.n ? a->tag.p : "", a->tag.n);
            put(&g, ","), ref(&g, cv[k]), put(&g, ",.T.)"), end(&g);
        }
        uint64_t set = begin(&g);
        put(&g, "IFCGEOMETRICCURVESET((");
        for (size_t k = 0; k < gr->naxes; k++) {
            if (k) put(&g, ",");
            ref(&g, cv[k]);
        }
        put(&g, "))"), end(&g);
        uint64_t shape = begin(&g);
        put(&g, "IFCSHAPEREPRESENTATION("), ref(&g, sub), put(&g, ",'FootPrint','GeometricCurveSet',(");
        ref(&g, set), put(&g, "))"), end(&g);
        uint64_t pds = begin(&g);
        put(&g, "IFCPRODUCTDEFINITIONSHAPE($,$,("), ref(&g, shape), put(&g, "))"), end(&g);
        uint64_t pl = begin(&g);
        put(&g, "IFCLOCALPLACEMENT("), ref(&g, rel_pl), put(&g, ","), ref(&g, world), put(&g, ")"), end(&g);
        uint64_t id = begin(&g);
        put(&g, "IFCGRID('"), buf_append(d, gr->id.c, 22), put(&g, "',$,");
        cc_step_string(d, gr->name.n ? gr->name.p : "", gr->name.n);
        put(&g, ",$,$,"), ref(&g, pl), put(&g, ","), ref(&g, pds), put(&g, ",(");
        for (int u = 1; u >= 0; u--) { /* UAxes, then VAxes */
            int first = 1;
            for (size_t k = 0; k < gr->naxes; k++)
                if (cc_grid_axis_is_u(gr, k) == u) {
                    if (!first) put(&g, ",");
                    ref(&g, ax[k]), first = 0;
                }
            put(&g, u ? "),(" : "),$,$)");
        }
        end(&g);
        grid_ent[i] = id;
    }
    if (!g.err) {
        Buf seed;
        buf_init(&seed);
        buf_append(&seed, m->project_id.c, 22);
        buf_puts(&seed, "/contains-grids/");
        if (gs >= 0) buf_append(&seed, m->storeys[gs].id.c, 22);
        else buf_puts(&seed, "building");
        cc_guid rg = cc_guid_derived(seed.p, seed.len);
        if (seed.oom) g.err = 1;
        buf_free(&seed);
        begin(&g);
        put(&g, "IFCRELCONTAINEDINSPATIALSTRUCTURE('"), buf_append(d, rg.c, 22), put(&g, "',$,$,$,(");
        for (size_t i = 0; i < m->ngrids; i++) {
            if (i) put(&g, ",");
            ref(&g, grid_ent[i]);
        }
        put(&g, "),"), ref(&g, rel_to), put(&g, ")"), end(&g);
    }
    free(grid_ent), free(ax), free(cv);
    *next = g.next;
    return g.err || d->oom ? -1 : 0;
}
