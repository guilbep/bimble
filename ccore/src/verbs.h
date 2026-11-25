/* Verbs that change the model (crates/bimble-api/src/verbs.rs and dispatch.rs, ADR 0048): the
 * agent and UI level API. A verb reads the model and returns commands; the caller applies them
 * through the history. Six of Rust's 14 verbs are in the C core: place, move, delete,
 * set_generator, set_material, set_note. */
#ifndef CCORE_VERBS_H
#define CCORE_VERBS_H
#include <stddef.h>
#include "buf.h"
#include "command.h"
#include "model.h"

#define CC_MAX_IDS 10000 /* dispatch::MAX_IDS */
#define CC_MAX_MM 1.0e7  /* dispatch::MAX_MM, mm */

/* dispatch::run: the verb `name` with JSON arguments (args[0..n); "null" or n == 0 is {}).
 * 0 (HTTP 200): the commands are appended to `out` (their strings live in out's arena), maybe
 * none. -1 (HTTP 400): `err` holds Rust's error text ("bad arguments: …", "Beam: b is required",
 * "unknown verb …"); a Rust verb the C core lacks gives "the verb \"copy\" is not in the C core".
 * New GlobalIds come from cc_guid_new, in Rust's call order (seeded runs give Rust's sequence). */
int cc_verb_run(const cc_model *m, const char *name, const char *args, size_t n, cc_cmds *out, Buf *err);

/* f64::hypot. Natively the C core links no libm, so this is the C core's own; see verbs.c. */
double cc_hypot(double x, double y);
double cc_sqrt(double x); /* correctly rounded, as IEEE sqrt */
#endif
