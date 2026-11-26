/* The history store, write side (L4; bimble_core::repo, ADR 0017, ticket H2a): init, write_tree,
 * commit, set_head, set_branch, create_branch, switch. The folder layout, the reading and the
 * error texts are repo.h's.
 *
 * Interoperability is the oracle: for the same model, author, time and message, a tree and a
 * commit written here have the object names Rust gives them (the hash is BLAKE3 of the plain
 * JSON, byte for byte serde_json::to_vec; the gzip bytes may differ, any reader inflates them),
 * and Rust reads a folder written here.
 *
 * Crash safety: every file goes through the platform's write_file (a temporary file, then a
 * rename). Objects are written first, refs last: a write that stops half way leaves the old HEAD
 * and branch valid (objects nobody refers to are harmless; an object is never rewritten). The
 * native write_file does not fsync: a process crash is covered, a power loss is not.
 *
 * Folders are made with the platform's make_dir (FS1); a platform without it (the wasm module)
 * cannot write a history: "io: no history folder here". The C model holds two sections Rust's has
 * not (relations, grids): a tree of a model with them carries them too (left out when empty, so
 * every other tree is Rust's); Rust refuses such a tree (DEVIATIONS 27). */
#ifndef CCORE_REPO_WRITE_H
#define CCORE_REPO_WRITE_H
#include "repo.h"

/* cc_repo_commit's answer when the tree is HEAD's: Rust's RepoError::NothingToCommit. */
#define CC_REPO_NOTHING (-2)

/* Repo::init_dir: creates the history in `dir` (and the folders above it), HEAD on `main`, no
 * commit, and opens it into `r` (cc_repo_close it either way). "<dir> already has a history". */
int cc_repo_init_dir(cc_repo *r, const cc_platform *pf, const char *dir, Buf *err);
/* Repo::init: the history next to a model file (`<model>-history`). */
int cc_repo_init(cc_repo *r, const cc_platform *pf, const char *model_path, Buf *err);

/* put_bytes: stores the bytes under their hash (gzipped; an object already there is not
 * rewritten). */
int cc_repo_put_bytes(const cc_repo *r, const char *p, size_t n, cc_hash *out, Buf *err);

/* write_tree: the model's objects, its mesh arrays and its tree; `out` is the tree's hash. */
int cc_repo_write_tree(const cc_repo *r, const cc_model *m, cc_hash *out, Buf *err);

/* Repo::commit: `m` as a new commit on HEAD (the branch moves, or a detached HEAD). The message
 * is trimmed (Unicode white space, as str::trim) and must not be empty ("a commit needs a
 * message"); the author is kept as given. CC_REPO_NOTHING with "nothing to commit: the model is
 * the same as <12 hex>" when the tree is HEAD's. */
int cc_repo_commit(const cc_repo *r, const cc_model *m, const char *message, const char *author, int64_t time,
                   cc_hash *out, Buf *err);

int cc_repo_set_head(const cc_repo *r, const cc_repo_head_t *h, Buf *err);
int cc_repo_set_branch(const cc_repo *r, const char *name, const char *hash, Buf *err);

/* Repo::create_branch: `name` at revision `at` (NULL: HEAD); "branch <name> already exists". */
int cc_repo_create_branch(const cc_repo *r, const char *name, const char *at, cc_hash *out, Buf *err);

/* Repo::switch: HEAD on branch `rev` (or, any other revision, detached at that commit); `m` is
 * the model there, for the working file (always initialized: cc_model_free it either way). The
 * model is read before HEAD moves: a damaged target leaves HEAD as it was. */
int cc_repo_switch(const cc_repo *r, const char *rev, cc_model *m, Buf *err);
/* ---- H2b ----
 * The commit object (repo.rs Commit) with `nparents` parents, the message as given (Repo::merge
 * does not trim it); `out` its hash. No ref moves. */
int cc_repo_put_commit(const cc_repo *r, const char *tree, const cc_hash *parents, size_t nparents,
                       const char *author, int64_t time, const char *message, cc_hash *out, Buf *err);
/* HEAD's branch to `hash`, or a detached HEAD to it (repo.rs: the end of commit and merge). */
int cc_repo_move_head(const cc_repo *r, const char *hash, Buf *err);
/* ---- end H2b ---- */
#endif
