/* The file dialog's queries (ticket FS1): the page draws its own Open / Import / Link / Save as
 * dialog (web-ui/src/dialogs/FileBrowser.vue) and asks the native host what a folder holds. Only
 * the native host answers them (server.c, before the session's calls): the wasm module has no
 * folders, and the session layer never lists one for the page.
 *   query fs_roots          [{"kind": "home"|"root", "path"}]: the folders the dialog may list
 *   query fs_home {purpose?}  {"last", "path"}: the folder `purpose` listed last (null: none; the
 *                           host's memory, for its lifetime) and the user's home folder (null: none)
 *   query fs_list {dir, ext?: [".bimble", ...], purpose?}
 *       {"dir", "parent", "entries": [{"kind", "mtime", "name", "size", "target"?, "outside"?}],
 *        "hidden", "skipped", "truncated"}
 * THE PATH RULE (tested by test/fs_test.mjs):
 *   - `dir` is an absolute path (POSIX "/…", Windows "X:/…", "X:\…", "\\…") without NUL, at most
 *     4096 bytes; anything else is refused.
 *   - It is resolved (realpath: every "..", "." and symbolic link) before the check, then it must
 *     be one of the roots or under one: the user's home folder (HOME, Windows USERPROFILE) and
 *     the folders given at start (`--root <dir>`, repeatable; the folder of the model on the
 *     command line is one). Anything else is refused with one text: OUTSIDE below.
 *   - The answer holds names and metadata only, never a file's bytes. An entry that is a link is
 *     listed as a link ("kind": "link"), never followed silently: "target" says what it points
 *     to when that is inside the roots, else "outside": true (listing it is refused, as above).
 *   - Hidden entries (a name that starts with ".") are left out ("hidden": their count). A name
 *     that is not UTF-8 is left out ("skipped": their count): no JSON string can name it back.
 *   - At most FS_MAX_ENTRIES entries per answer, sorted by name ("truncated": true beyond).
 *   - `ext` (optional, at most 16, case-insensitive): files with another extension are left out;
 *     folders always listed.
 * The calls need the session token like every call (server.c), /stage too. The actions that open or write a
 * file (open, import_ifc, link_*, save_as, export_*) take the picked path as before: the token
 * is their gate, not this rule (the tests, the MCP server and scripts give them any path). */
#ifndef CCORE_HOST_FS_H
#define CCORE_HOST_FS_H
#include <stddef.h>
#include "buf.h"

#define FS_MAX_ENTRIES 5000
#define FS_STAGE_MAX ((size_t)256 << 20) /* a dropped file's bytes (POST /stage) */
#define FS_MAX_ROOTS 16
#define FS_OUTSIDE "outside the folders Bimble lists (query fs_roots)"

/* Adds a root folder (resolved). 0, -1 when it is not a folder or there are too many. The home
 * folder is added by cc_host_fs_init. */
int cc_host_fs_add_root(const char *dir);
/* The home folder as the first root (HOME, USERPROFILE on Windows); once, at start. */
void cc_host_fs_init(void);
/* query `name` with `body`: CC_OK or CC_ERR with the answer or the error text in `out`; -1 when
 * `name` is not one of these queries (the session answers it). */
int cc_host_fs_query(const char *name, const char *body, size_t n, Buf *out);

/* POST /stage?name=<file name>&t=<token> (server.c), the body the file's bytes (at most
 * FS_STAGE_MAX): a file dropped on the page, whose path the browser does not give. Written to a
 * folder of the host's own (made once, 0700, under TMPDIR or /tmp; Windows TEMP), named `name`
 * (a plain file name: no separator, no NUL, no control character, not hidden, at most 255 bytes;
 * the same name again replaces it). Answers {"path"} with CC_OK, or the error text with CC_ERR.
 * The page then opens that path as a dropped document (`action open {path, dropped: true}`:
 * Save asks where, it is no recent file). */
int cc_host_stage(const char *name, const char *bytes, size_t n, Buf *out);
/* Removes the staged files and their folder (main.c, when the host stops). */
void cc_host_stage_cleanup(void);
#endif
