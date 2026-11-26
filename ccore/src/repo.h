/* The history store, read side (L4; bimble_core::repo, ADR 0017): `house.bimble` keeps its
 * history in `house.bimble-history/`:
 *   format                 "bimble-history 1"
 *   HEAD                   "ref: main" or a commit hash (detached)
 *   refs/heads/<branch>    a commit hash
 *   objects/ab/cdef…       objects named by the BLAKE3 hash of their bytes (gzipped, or plain
 *                          JSON in older histories)
 * Objects are canonical JSON: one per storey, material, element, view and sheet, one tree per
 * model state, one per commit, and the mesh arrays elements refer to by hash (ADR 0042). Every
 * object read is checked against its name: a damaged history is reported, never used.
 *
 * Every file goes through the carrier's cc_platform (read_file, exists, list_dir): no file call
 * here. A history folder is untrusted input (from disk or a peer): sizes are capped (an object
 * 256 MiB, a small file 4096 bytes), walks are bounded (1,000,000 commits, refs 16 folders deep),
 * nothing aborts. Errors are Rust's RepoError texts (`Display`), in `err`.
 * Read only: writing (commit, branch, switch, merge) is ticket H2. */
#ifndef CCORE_REPO_H
#define CCORE_REPO_H
#include <stddef.h>
#include <stdint.h>
#include "../session/platform.h"
#include "arena.h"
#include "buf.h"
#include "diff.h"
#include "model.h"

#define CC_REPO_FORMAT "bimble-history 1"
#define CC_REPO_MAX_OBJECT ((size_t)256 << 20) /* repo.rs:28 MAX_OBJECT_BYTES */
#define CC_REPO_MAX_WALK 1000000               /* repo.rs:30 MAX_WALK */
#define CC_REPO_MAX_SMALL 4096                 /* repo.rs:141 read_small */

/* A hash: 64 lowercase hex digits and a NUL. */
typedef struct {
    char h[65];
} cc_hash;

typedef struct {
    const cc_platform *pf;
    char *dir; /* malloc'd, as given (no final '/') */
} cc_repo;

/* A commit object (repo.rs Commit). Strings in the caller's arena, NUL-terminated; `tree` and
 * the parents are any text the object held (a hash is checked when it is read). */
typedef struct {
    const char *tree;
    const char **parents;
    size_t nparents;
    const char *author;
    int64_t time; /* seconds since 1970-01-01 UTC */
    const char *message;
} cc_commit;

typedef struct {
    int detached;      /* 0: on a branch */
    char branch[101];  /* the branch name (at most 100 bytes) */
    cc_hash commit;    /* detached: the commit */
} cc_repo_head_t;

typedef struct {
    const char *name; /* arena */
    cc_hash commit;
} cc_branch;

typedef struct {
    cc_hash hash;
    cc_commit c;
} cc_log_entry;

/* history_dir: `<model>-history` appended to `out`. */
void cc_repo_history_dir(const char *model_path, Buf *out);

/* Repo::open (the history next to a model file) and Repo::open_dir. 0 ok, -1 with `err`.
 * The repo keeps `pf` by pointer. Free with cc_repo_close (also after a failed open). */
int cc_repo_open(cc_repo *r, const cc_platform *pf, const char *model_path, Buf *err);
int cc_repo_open_dir(cc_repo *r, const cc_platform *pf, const char *dir, Buf *err);
void cc_repo_close(cc_repo *r);

/* The object's bytes (plain JSON), inflated when gzipped and checked against its hash. */
int cc_repo_get_bytes(const cc_repo *r, const char *hash, Buf *out, Buf *err);

int cc_repo_commit_info(const cc_repo *r, const char *hash, Arena *a, cc_commit *c, Buf *err);

/* read_tree: the model a tree describes, mesh references resolved, validated like a model file
 * and canonical. `m` is always initialized (cc_model_free it either way). */
int cc_repo_read_tree(const cc_repo *r, const char *tree, cc_model *m, Buf *err);

int cc_repo_head(const cc_repo *r, cc_repo_head_t *h, Buf *err);
/* A branch's commit; *found 0 for a branch without commits. */
int cc_repo_branch_commit(const cc_repo *r, const char *name, cc_hash *out, int *found, Buf *err);
/* HEAD's commit; *found 0 before the first commit. */
int cc_repo_head_commit(const cc_repo *r, cc_hash *out, int *found, Buf *err);
/* Branches and their commits, sorted by name (byte order). */
int cc_repo_branches(const cc_repo *r, Arena *a, cc_branch **out, size_t *n, Buf *err);

/* A commit from a revision: HEAD (or ""), a branch, a full hash or a unique prefix of at least 4
 * hex digits, each optionally followed by `~n`. */
int cc_repo_resolve(const cc_repo *r, const char *rev, cc_hash *out, Buf *err);
int cc_repo_model_at(const cc_repo *r, const char *rev, cc_model *m, Buf *err);
/* Commits from `rev` back along first parents, newest first, at most `limit`. */
int cc_repo_log(const cc_repo *r, const char *rev, size_t limit, Arena *a, cc_log_entry **out, size_t *n,
                Buf *err);
/* What changed in `working` since HEAD's commit (everything, before the first commit). */
int cc_repo_status(const cc_repo *r, const cc_model *working, cc_changes *out, Buf *err);

/* Repo::check_branch_name: 0 ok, -1 with Rust's text in err. */
int cc_repo_check_branch_name(const char *name, size_t n, Buf *err);
#endif
