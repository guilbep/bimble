/* Element-level semantic diff between two models (bimble_core::version, version.rs:127-334):
 * `Change` (added, removed, modified with its fields) matched by GlobalId, never by position,
 * and its Display text. The values of a field change are JSON values (the objects' serde form);
 * they print as serde_json::Value prints: compact, object keys sorted.
 * L4 (with the store); pure: no file, no clock. */
#ifndef CCORE_DIFF_H
#define CCORE_DIFF_H
#include <stddef.h>
#include "arena.h"
#include "buf.h"
#include "json_r.h"
#include "json_w.h"
#include "model.h"

enum { CC_CHANGE_ADDED = '+', CC_CHANGE_REMOVED = '-', CC_CHANGE_MODIFIED = '~' };

typedef struct {
    const char *path; /* dotted: "generator.height", "placement.origin.2" */
    const JrVal *before, *after;
} cc_field_change;

typedef struct {
    int change; /* CC_CHANGE_* */
    const char *kind; /* "IfcColumn", "IfcBuildingStorey", "view", "sheet", "schedule"… */
    char id[23];      /* the GlobalId, NUL-terminated */
    const char *name;
    cc_field_change *fields; /* modified only */
    size_t nfields;
} cc_change;

/* Changes and everything they point to (one arena). */
typedef struct {
    Arena arena;
    cc_change *v;
    size_t n, cap;
} cc_changes;

void cc_changes_init(cc_changes *c);
void cc_changes_free(cc_changes *c);

/* version::diff(a, b), appended to `out`. 0 ok; -1 out of memory (or a model that does not
 * serialize), with `err`. */
int cc_diff(const cc_model *a, const cc_model *b, cc_changes *out, Buf *err);

/* `impl Display for Change`: "+ kind name [id]", "~ kind name [id]" and a line
 * "\n    path: before → after" per field. No final newline. */
void cc_change_display(const cc_change *c, Buf *b);

/* serde's form of a Change ({"change":"added"|"removed"|"modified","kind","id","name"[,"fields"]}). */
void cc_version_change_json(const cc_change *c, JsonW *w);

/* A JSON value as serde_json::Value's Display writes it: compact, object keys sorted by bytes. */
void cc_json_value_write(const JrVal *v, Buf *b);
#endif
