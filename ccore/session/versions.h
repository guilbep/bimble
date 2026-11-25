/* The model's history in the session (ticket H1, read only; L5): the queries the History page
 * fills its rows from, over the history folder next to the open model (`<path>-history`, repo.h,
 * ADR 0017). Nothing here commits, switches or merges (ticket H2).
 *
 *   query versions        {rev?: "HEAD", limit?: 100}: Repo::log from `rev`, newest first:
 *                         {"history": true, "head": HEAD, "versions": [{"hash", "parents",
 *                         "author", "time", "message"}]}. HEAD is {"branch": name|null,
 *                         "commit": hash|null}. Before the first commit: "versions": [].
 *   query branches        {"history": true, "head": HEAD, "branches": [{"name", "commit"}]}
 *                         (sorted by name).
 *   query version_changes {a, b?: "HEAD"}: version::diff(model_at(a), model_at(b)):
 *                         {"history": true, "a": hash, "b": hash, "changes": [Change]}; a Change
 *                         is serde's form ({"change": "added"|"removed"|"modified", "kind", "id",
 *                         "name", "fields"?: [{"path", "before", "after"}]}).
 *   query pending         Repo::status of the open model against HEAD: {"history": true,
 *                         "base": hash|null, "changes": [Change]}.
 * Without a history (no model, a read-only scene, a new model without a file, no folder next to
 * the file, the browser): {"history": false, "reason": "<text>"}, 200; the wasm module's reason
 * is "no history folder in the browser". A history that is there but damaged, an unknown
 * revision or a bad body: 400 with the text (Rust's RepoError). */
#ifndef CCORE_SESSION_VERSIONS_H
#define CCORE_SESSION_VERSIONS_H
#include <stddef.h>
#include "calls.h"
#include "session.h"

/* One of the queries above: CC_OK or CC_ERR with the answer in `out`. -1: not one of them. */
int cc_versions_query(cc_session *s, const char *name, const char *body, size_t n, cc_buf *out);
#endif
