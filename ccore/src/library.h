/* The material library (crates/bimble-core/src/library.rs, ADR 0031): the standard grades with
 * density, modulus and strength, transcribed in library() order. Only what the verbs need. */
#ifndef CCORE_LIBRARY_H
#define CCORE_LIBRARY_H
#include <stddef.h>
#include "model.h"

typedef struct {
    const char *name;
    cc_category category;
    double density_kg_m3, e_mpa, strength_mpa;
    const char *strength_symbol; /* "fck", "fy", "fyk", "fm,k", "fm,g,k" */
    const char *source;
} cc_library_material;

/* The whole library, in Rust's order. */
const cc_library_material *cc_library(size_t *n);

/* library::lookup: the grade a material name refers to ("C30/37", "S235(R)", "Steel S355 J2",
 * "gl24h 24x46"); the longest grade name found wins (the last one on a tie, as max_by_key).
 * NULL when none. */
const cc_library_material *cc_library_lookup(const char *name, size_t n);

/* library::to_material: a model material from an entry, with a new GlobalId (cc_guid_new).
 * Strings point to static storage. */
cc_material cc_library_to_material(const cc_library_material *lm);
#endif
