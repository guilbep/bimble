/* Verbs that change the model (crates/bimble-api/src/verbs.rs and dispatch.rs, ADR 0048): the
 * agent and UI level API. A verb reads the model and returns commands; the caller applies them
 * through the history. All 14 verbs of Rust's dispatch::VERBS, through one table (verb_table.h). */
#ifndef CCORE_VERBS_H
#define CCORE_VERBS_H
#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "command.h"
#include "math.h" /* cc_sqrt, cc_hypot (verbs_test, callers that must match Rust's bits) */
#include "model.h"

#define CC_MAX_IDS 10000 /* dispatch::MAX_IDS */
#define CC_MAX_MM 1.0e7  /* dispatch::MAX_MM, mm */

#define CC_VERB_CONFLICT (-2) /* cc_verb_run_ex: if_rev is not the model's revision (HTTP 409) */

/* dispatch::run: the verb `name` with JSON arguments (args[0..n); "null" or n == 0 is {}).
 * 0 (HTTP 200): the commands are appended to `out` (their strings live in out's arena), maybe
 * none. -1 (HTTP 400): `err` holds Rust's error text ("bad arguments: …", "Beam: b is required",
 * "unknown verb …"). New GlobalIds come from cc_guid_new, in Rust's call order (seeded runs give
 * Rust's sequence; a refused add_sheet uses one, as Sheet::new does before the checks). */
int cc_verb_run(const cc_model *m, const char *name, const char *args, size_t n, cc_cmds *out, Buf *err);

/* cc_verb_run with the two arguments every verb takes (ADR 0068 §3), a C addition (Rust's
 * dispatch::run has neither):
 *   if_rev: < 0 not given; else the revision the caller read. When it is not `current_rev` (the
 *     session's revision of `m`), nothing runs: CC_VERB_CONFLICT with the text
 *     "conflict: the model changed (rev <current_rev>, expected <if_rev>)".
 *   dry_run: the commands are answered and must not be applied. A verb never changes the model
 *     (`m` is const); the caller (the session) applies the commands through its history unless
 *     dry_run is set, so a dry run answers exactly what the real run would apply.
 * The unknown verb check comes first, then if_rev, then the arguments. */
int cc_verb_run_ex(const cc_model *m, const char *name, const char *args, size_t n, int dry_run, int64_t if_rev,
                   uint64_t current_rev, cc_cmds *out, Buf *err);
#endif
