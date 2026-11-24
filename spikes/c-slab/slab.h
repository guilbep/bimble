/* C port of the mesh of one IfcSlab (spike c-slab). Byte-for-byte with bimble-geom:
 * `local` = element_mesh, `scene` = Scene::from_model's local_mesh for a one-slab model. */
#ifndef SLAB_H
#define SLAB_H
#include <stdint.h>

#define SLAB_MAX_PTS 64
#define SLAB_MAX_TRIS (2 * (SLAB_MAX_PTS - 2) + 2 * SLAB_MAX_PTS)
#define SLAB_MAX_VERTS (2 * SLAB_MAX_PTS)
#define SLAB_MAX_EDGES (3 * SLAB_MAX_TRIS)

typedef struct {
    double origin[3];                      /* scene origin (mm, f64); zero in local mode */
    int nv, nt, ne;                        /* vertices, triangles, edges */
    float pos[SLAB_MAX_VERTS][3];
    uint32_t idx[SLAB_MAX_TRIS * 3];
    float edges[SLAB_MAX_EDGES][6];
    int8_t normals[SLAB_MAX_EDGES][8];     /* filled in scene mode only */
} SlabMesh;

/* Mesh an IfcSlab: CCW outline (mm), thickness along +Z, placement origin with default axes,
 * storey elevation 0. scene = 0: element coordinates; scene = 1: relative to the scene origin.
 * Returns 0 on success, -1 on bad input. Uses static storage only (not reentrant). */
int slab_mesh(const double outline[][2], int n, double thickness,
              const double origin_placement[3], SlabMesh *out, int scene);

#endif
