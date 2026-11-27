/* The vertex clusters of hlod.c (Rossignac & Borrel on one power-of-two lattice), shared with tiles.c
 * (ticket TILE1): internal to ccore/src, not an API. Keys are lattice cells; a grid at e is cells of
 * g0 x 2^e; cc_hg_up moves a key to a grid s steps coarser. */
#ifndef CCORE_HLOD_GRID_H
#define CCORE_HLOD_GRID_H
#include <stddef.h>
#include <stdint.h>
#include "model.h"

typedef struct {
    int64_t k[3];
} Key;
typedef struct {
    Key k;
    double s[3]; /* sum of the original vertices in the grid cell */
    uint64_t n;  /* their count */
    float lo[3], hi[3]; /* TILE1: their box (then hi follows lo: a float[6]) */
    double q[10];       /* TILE1: the sum of their faces' plane quadrics (a2 ab ac ad b2 bc bd c2 cd d2), area weighted */
} Vert;
typedef struct {
    Key t[3];
} Tri;
typedef struct {
    Vert *v;
    size_t nv, cv;
    Tri *t;
    size_t nt, ct;
} Fam;
typedef struct {
    uint32_t e; /* the grid's cell size is g0 x 2^e */
    Fam f[CC_CAT_COUNT];
    uint64_t tmark, vmark; /* triangles and vertices after the last merge */
} Grid;
typedef struct { /* a finished family: verts sorted by key, triangles as indices into them */
    Vert *v;
    size_t nv;
    uint32_t (*t)[3];
    size_t nt;
} Part;
typedef struct {
    uint32_t e;
    int built;
    Part p[CC_CAT_COUNT];
} Clusters;

Key cc_hg_up(Key k, unsigned s);
int cc_hg_fam_vert(Fam *f, Key k, const double s[3], uint64_t n, const float *box /* lo xyz, hi xyz; NULL: the point */,
                   const double *q /* 10, or NULL: none */);
int cc_hg_fam_tri(Fam *f, Key a, Key b, Key c);
void cc_hg_grid_free(Grid *g);
uint64_t cc_hg_grid_triangles(const Grid *g);
int cc_hg_grid_fit(Grid *g, uint64_t max, int force);
int cc_hg_grid_add_clusters(Grid *g, const Clusters *cl);
void cc_hg_clusters_free(Clusters *c);
int cc_hg_grid_finish(Grid *g, Clusters *out);
uint64_t cc_hg_clusters_triangles(const Clusters *c);
#endif
