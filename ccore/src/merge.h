/* Three-way merge of models (bimble_core::repo::merge_models, repo.rs:620-861; ticket H2b).
 * Object by object, matched by GlobalId: a side that left an object as it was in `base` takes the
 * other side's version (changed, added or removed). When both changed one object, its fields are
 * merged the same way, key by key over the serde form (objects of the same `kind`/`type` tag
 * recurse, anything else is taken whole): only a field both changed differently is a conflict.
 * A field merge that does not decode as an object again counts as one conflict on the whole
 * object, which then comes from the preferred side. The project name and each project
 * information key are picked alike; schedules by name, whole; the georeference from whichever
 * side changed it (ours when both did).
 *
 * The C model's own sections (relations, grids: DEVIATIONS 27) merge by the same rules per object
 * id, after Rust's sections (DEVIATIONS 28).
 *
 * L4, pure: no file, no clock. The merged model is not validated (Rust validates it in
 * Repo::merge, not here): an element on a storey the other side removed comes out as it is. */
#ifndef CCORE_MERGE_H
#define CCORE_MERGE_H
#include <stddef.h>
#include "arena.h"
#include "buf.h"
#include "model.h"

/* repo.rs Prefer: which side wins when both changed the same thing differently. NEITHER reports
 * the conflicts (and merges ours, as Rust's pick does); Repo::merge then merges nothing. */
typedef enum { CC_PREFER_NEITHER, CC_PREFER_OURS, CC_PREFER_THEIRS } cc_prefer;

/* repo.rs Conflict: kind ("element", "IfcBuildingStorey", "IfcMaterial", "view", "sheet",
 * "IfcProject", "schedule"; C: "relation", "IfcGrid"), the object's id (none for the project and
 * schedules), and a name ("IfcColumn C1: generator.width"). Strings in the list's arena. */
typedef struct {
    const char *kind;
    int has_id;
    char id[23]; /* NUL-terminated when has_id */
    const char *name;
} cc_conflict;

typedef struct {
    Arena arena;
    cc_conflict *v;
    size_t n, cap;
} cc_conflicts;

void cc_conflicts_init(cc_conflicts *c);
void cc_conflicts_free(cc_conflicts *c);

/* impl Display for Conflict: "{kind} {name} [{id}]", or "{kind} {name}" without an id. */
void cc_conflict_display(const cc_conflict *c, Buf *b);

/* merge_models: the merged model into `out` (always initialized: cc_model_free it either way;
 * canonical, not validated) and the conflicts appended to `conflicts`, in Rust's order (project
 * name, project information, schedules, storeys, materials, elements, views, sheets; then the C
 * sections relations, grids). 0 ok; -1 with `err` (out of memory, an object that does not
 * serialize). */
int cc_merge_models(const cc_model *base, const cc_model *ours, const cc_model *theirs, cc_prefer prefer,
                    cc_model *out, cc_conflicts *conflicts, Buf *err);
#endif
