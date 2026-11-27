/* The render scene of a model: bimble_geom::Scene::from_model, byte for byte (ticket 5).
 *
 * Per element: a local triangle mesh (f32 positions in mm, u32 indices, crease edges with the
 * packed normals of their two faces). Rust shares identical meshes (dedup::share_copies): such an
 * element points at a shared shape and carries a placement (`cc_placement32`) from the shape's
 * own coordinates to the scene; the other elements keep their own mesh, already in scene
 * coordinates. Scene coordinates are mm relative to `origin` (f64, world mm).
 *
 * Walking a scene (e.g. to serialize it):
 *   for k in 0..s.nelements:
 *     e = &s.elements[k]                   id, class, category, model index
 *     m = cc_scene_local_mesh(&s, k)       pos[nv][3], idx[ni], edges[ne][6], normals[nn][8]
 *     if e->shape >= 0: e->to_scene places m in the scene (columns x, y, z, translation t);
 *                       cc_scene_world_mesh() gives the placed copy.
 *
 * Generators: all of them (rect_column, pad_footing, rect_beam, wall, slab, extruded_polygon with
 * voids, mesh). Profiles are triangulated by earcut.c, a faithful port of the earcutr crate. An
 * element Rust cannot mesh is left out with Rust's warning text in `warnings`. Mesh generators
 * holding the same positions array (several uncut elements: the binary .bimble's table, ADR 0042;
 * the JSON text form never gives it) are one shape meshed once and placed, as Rust shares one Arc
 * (ticket SIZE3); share_copies then applies to the own meshes.
 * Openings are not in the C model. All memory lives in the scene's arena. No libm: sqrt is exact
 * in software, and the 20 degree crease cosine is glibc's cosf value (ticket 13 must recheck it
 * on wasm). */
#ifndef CCORE_SCENE_H
#define CCORE_SCENE_H
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "model.h"
#include "math.h" /* cc_sqrt, cc_sqrtf: exact helpers for callers that must match Rust's bits */

/* bimble_geom::Mesh. Triangles are idx[3t..3t+3]; normals[k] belongs to edges[k] (snorm8:
 * face 1 xyz, w, face 2 xyz, w; all zero = unknown). nn == ne for meshes built here. */
typedef struct {
    float (*pos)[3];
    uint32_t nv;
    uint32_t *idx;
    uint32_t ni; /* index count: 3 per triangle */
    float (*edges)[6];
    uint32_t ne;
    int8_t (*normals)[8];
    uint32_t nn;
} cc_mesh;

/* scene.rs Placement32: p_scene = x * p.x + y * p.y + z * p.z + t (f32, mm). */
typedef struct {
    float x[3], y[3], z[3], t[3];
} cc_placement32;

typedef struct {
    cc_gid id;
    cc_ifc_class cls;
    cc_str class_name;  /* Element::class_name: the IFC entity, or the class name */
    cc_category category;
    size_t model_index; /* index in cc_model.elements */
    int32_t shape;      /* index in cc_scene.shapes, or -1: own mesh in `mesh` */
    cc_mesh mesh;       /* own mesh, scene coordinates (valid when shape < 0) */
    cc_placement32 to_scene; /* valid when shape >= 0 */
} cc_scene_element;

typedef struct {
    Arena arena;
    double origin[3]; /* world mm of the scene coordinate origin */
    cc_scene_element *elements; /* model order (sorted by GlobalId); empty meshes dropped */
    size_t nelements;
    cc_mesh *shapes; /* shared shapes, in their own coordinates */
    size_t nshapes;
    cc_str *warnings; /* elements Rust would skip with a warning (none for a valid model) */
    size_t nwarnings;
} cc_scene;

/* Builds the scene. Always initializes `s` (free it with cc_scene_free either way).
 * 0 ok, -1 with `err` (out of memory, or an outline past the triangulation limits, see
 * earcut.h; Rust has no such limit). */
int cc_scene_build(cc_scene *s, const cc_model *m, cc_error *err);
void cc_scene_free(cc_scene *s);

/* SceneElement::local_mesh: the own mesh, or the shared shape in its own coordinates. */
const cc_mesh *cc_scene_local_mesh(const cc_scene *s, size_t k);

/* SceneElement::world_mesh: the element's mesh in scene coordinates (a placed shape is
 * computed into `a`; an own mesh is copied by pointer). 0 ok, -1 out of memory. */
int cc_scene_world_mesh(const cc_scene *s, size_t k, Arena *a, cc_mesh *out);
#endif
