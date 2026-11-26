/* Merging history branches (L4; bimble_core::repo, repo.rs:864-959; ticket H2b): the ancestors
 * walk, merge_base, and Repo::merge (up to date, fast-forward, or a merge commit with two
 * parents). The model merge itself is merge.h's.
 *
 * Untrusted input: the walk is breadth first over every parent, each commit once, and stops after
 * CC_REPO_MAX_WALK commits as Rust's does. A hash is queued once (Rust queues it again and skips
 * it when seen: the same order), so a commit listing one parent a million times costs one entry.
 * A parent that is not a hash fails when it is queued (Rust fails when it is reached).
 *
 * Crash safety: objects first (the merged objects, the tree, the commit), the ref last; a merge
 * with conflicts and Prefer neither writes nothing; a fast-forward writes only the ref. */
#ifndef CCORE_REPO_MERGE_H
#define CCORE_REPO_MERGE_H
#include "merge.h"
#include "repo.h"

/* All ancestors of commit `h` (itself included), breadth first, at most `limit` (Rust:
 * CC_REPO_MAX_WALK). `*out` malloc'd (free it), `*n` entries. 0 ok, -1 with err. */
int cc_repo_ancestors(const cc_repo *r, const char *h, size_t limit, cc_hash **out, size_t *n, Buf *err);

/* Nearest common ancestor of two commits; *found 0 when none. */
int cc_repo_merge_base(const cc_repo *r, const char *a, const char *b, cc_hash *out, int *found, Buf *err);

enum { CC_MERGED_UP_TO_DATE = 1, CC_MERGED_FAST_FORWARD, CC_MERGED_COMMIT, CC_MERGED_CONFLICTS };

/* Repo::merge_with_message: merges revision `rev` into HEAD. `message` NULL: "Merge <rev>".
 * *how: UP_TO_DATE (HEAD already holds it; `m` HEAD's model), FAST_FORWARD (HEAD's ref now at
 * `*commit`, `m` its model), COMMIT (`*commit` the merge commit, parents HEAD's and theirs; `m` the
 * merged model), CONFLICTS (Prefer neither and conflicts: nothing written, `conflicts` lists them;
 * with Prefer ours/theirs the conflicts are listed too and the merge goes on). `m` always
 * initialized (cc_model_free it either way). 0 ok, -1 with err: "nothing committed yet",
 * "unknown revision …", "the merged model is invalid: …", io and damaged-history texts. */
int cc_repo_merge(const cc_repo *r, const char *rev, cc_prefer prefer, const char *author, int64_t time,
                  const char *message, int *how, cc_hash *commit, cc_model *m, cc_conflicts *conflicts, Buf *err);
#endif
