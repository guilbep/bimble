/* Relations as model objects (ADR 0070): one table of IFC relationships, each with its own
 * GlobalId, between elements of the model. `kind` is the IFC class name as text, checked
 * against a table (cc_rel_kinds). Openings stay embedded in their element (IfcRelVoidsElement
 * is not stored here). Layer L1 (model), with the decoder and writer the file and the commands
 * share (as view_io.h does for views). */
#ifndef CCORE_RELATION_H
#define CCORE_RELATION_H
#include <stddef.h>
#include "json_r.h"
#include "json_w.h"
#include "model.h"
#include "view_io.h"

#define CC_REL_MAX_RELATED 10000     /* related[] and realizing[]: at most this many ids each */
#define CC_REL_MAX_PROPERTIES 1000   /* properties of one relation */

/* What a kind allows. Every id names an element of the model. */
typedef struct {
    const char *name;   /* the IFC class: "IfcRelAggregates"… */
    int related_one;    /* related[] holds exactly one id (else 1..CC_REL_MAX_RELATED) */
    int realizing;      /* 0: realizing[] stays empty; 1: it holds 1..CC_REL_MAX_RELATED ids */
    int is_aggregate;   /* one whole per part, no cycle */
    int needs_types;    /* the relating object is a type object: the model holds none yet */
} cc_rel_kind;

extern const cc_rel_kind cc_rel_kinds[];
extern const size_t cc_rel_nkinds;
/* The table row of a kind, NULL when the text is not one. */
const cc_rel_kind *cc_rel_kind_find(cc_str kind);

/* Lookup by id (binary search, relations sorted by id); NULL when absent. */
const cc_relation *cc_model_relation(const cc_model *m, const cc_gid *id);
/* The first relation (in id order) that names `element` as relating, related or realizing;
 * NULL when none. */
const cc_relation *cc_relation_using(const cc_model *m, const cc_gid *element);

/* Validation of a whole relation table against the model's elements (Model::validate and the
 * commands): each relation on its own (kind in the table, ids exist, no relation to itself,
 * related and realizing sorted without duplicates, bounds), its id used by nothing else, then
 * across relations: one whole per part and no cycle in IfcRelAggregates. `keys`: the map keys
 * the file gave (key == id), or NULL. 0 ok, -1 with the text appended to `why`. */
int cc_relations_validate(const cc_model *m, const cc_relation *r, size_t n, const cc_gid *keys, Buf *why);

/* Deep copy into `a` (strings and arrays). 0 ok, -1 out of memory. */
int cc_relation_dup(Arena *a, cc_relation *r);
/* Field equality (for tests and no-op detection). */
int cc_relation_eq(const cc_relation *a, const cc_relation *b);

/* serde-like decoding of {id, kind, relating, related, realizing?, properties?} from the JSON or
 * CBOR DOM. `skip`: one member ignored (a command's "op"), or NULL. Unknown members are ignored,
 * a repeated one is an error. Errors as serde words them (no "parse: " prefix) in d->msg. */
int cc_vd_relation(cc_vd *d, const JrVal *v, const char *skip, cc_relation *out);

/* Writes one relation (JSON when e->j, else CBOR into e->c), empty realizing and properties left
 * out. `op` (JSON only): written first as "op": op, the command's tag. */
void cc_em_relation(cc_em *e, const cc_relation *r, const char *op);
#endif
