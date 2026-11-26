/* The command palette's filter in the session (ticket Q2): the viewer's call `palette_match`
 * {q, items} (crates/bimble-viewer/src/calls.rs:313-321) answers the indices of `items` that
 * match `q`, in their order, as ribbon.rs:469-519 `palette_match` and `matches` decide:
 *   fold(s) = s.to_lowercase() with à…å, ç, è…ë, ì…ï, ñ, ò…ö, ù…ü, ý, ÿ to their base letter;
 *   a label matches when the folded, trimmed query is empty or part of the folded label, or when
 *   every whitespace-separated word of the query starts a word of the label (words: runs of
 *   alphanumeric characters).
 * Deviation (golden/DEVIATIONS.md 17): to_lowercase and is_alphanumeric are Unicode-wide in Rust;
 * here lower case covers the Latin, Greek and Cyrillic blocks (as src/quantity.c's natural sort)
 * and the separators are ASCII non-alphanumerics, Latin-1 punctuation and symbols, and the
 * General Punctuation block; final sigma lowers to σ, not ς. */
#ifndef CCORE_SESSION_PALETTE_H
#define CCORE_SESSION_PALETTE_H
#include <stddef.h>
#include "buf.h"

/* 1 when `label` matches `query` (ribbon.rs:507 `matches`); -1 out of memory. */
int cc_palette_matches(const char *label, size_t ln, const char *query, size_t qn);
/* The call: `body` is {"q": string, "items": [string]}. CC_OK with the JSON array of indices, or
 * CC_ERR with the reason in `out`. */
int cc_palette_match(const char *body, size_t n, Buf *out);
#endif
