/* Parts of the scene builder (scene.c) for other scene sources: the IFC display reader
 * (ifc_read.c) builds a cc_scene from world triangles and shared shapes, not from a model. */
#ifndef CCORE_SCENE_BUILD_H
#define CCORE_SCENE_BUILD_H
#include <stdint.h>
#include "arena.h"
#include "scene.h"

/* MeshBuilder::build: `ntris` world triangles (f64 mm) relative to `origin` as f32, degenerate
 * ones dropped, welded by f32 bits, crease edges (20 degrees). The mesh goes to `a`, working
 * memory to `scratch` (the caller may reset it after). 0 ok, -1 out of memory. */
int cc_scene_mesh_build(double (*tris)[3][3], uint32_t ntris, const double origin[3], Arena *a, Arena *scratch,
                        cc_mesh *m);

/* dedup::share_copies on s->elements: elements with an own mesh (shape < 0) that are copies of
 * one another become instances of a new shared shape, appended to s->shapes (shapes already
 * there are kept). 0 ok, -1 out of memory. */
int cc_scene_share_copies(cc_scene *s);
#endif
