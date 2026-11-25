/* The user's settings file (ticket FS1; SET1 extends it): the JSON file the Rust viewer keeps
 * (crates/bimble-viewer/src/settings.rs `path`: ~/.config/bimble/settings.json on Linux,
 * ~/Library/Application Support/Bimble on macOS, %APPDATA%\Bimble on Windows; the carrier picks
 * the folder, host/main.c), and the recent files Home lists (settings.rs `recent`, `RECENT`,
 * `opened`): both apps share one list.
 * Writing changes the field "recent" only: the file is parsed, that member replaced (or added),
 * and the whole written back canonical as serde_json writes Rust's (keys sorted bytewise, no
 * spaces; numbers as their text in the file, a repeated key once: its last value). A missing file
 * is created with that one field (and its folder); a file that does not read or is not a JSON
 * object is left alone: the list stays in memory and the log says why.
 * Files through cc_platform only (L5). The wasm module has no folders (pf->files 0): no list. */
#ifndef CCORE_SESSION_SETTINGS_FILE_H
#define CCORE_SESSION_SETTINGS_FILE_H
#include <stddef.h>
#include "buf.h"
#include "json_r.h"
#include "platform.h"

typedef struct cc_session cc_session;

#define CC_RECENT_MAX 12        /* settings.rs RECENT */
#define CC_SETTINGS_MAX_BYTES (1u << 20)

/* The "recent" list of the settings file `file`, as Rust reads it (settings.rs from_json: the
 * strings of the array, not empty, at most 4096 bytes, the first 12): paths[0..*n) malloc'd.
 * 0 (a missing file: *n 0), -1 when the file does not read or parse (*n 0). */
int cc_settings_recent_read(const cc_platform *pf, const char *file, char **paths, size_t *n);
/* Writes `paths` as the file's "recent" (above). 0 ok, -1 left alone (logged). */
int cc_settings_recent_write(const cc_platform *pf, const char *file, char *const *paths, size_t n);

/* ---- SET1: the other fields (settings.h) through the same writer ---- */
/* The "recent" list of a parsed settings value, as cc_settings_recent_read (paths malloc'd). */
void cc_settings_recent_of(const JrVal *v, char **paths, size_t *n);
/* A member written in place of the file's member of that key, or added where it sorts. */
typedef struct {
    const char *key;
    const char *json; /* its value, JSON text */
    size_t json_n;
} cc_settings_member;
/* `text` (a JSON object; NULL or n 0: an empty one) with `m[0..nm)` (sorted by key, bytewise) in
 * place of its members of those keys, the others kept: canonical, as above, into `out`.
 * 0, or -1 when `text` is not a JSON object (the reason in why[0..128)). */
int cc_settings_merge(const char *text, size_t n, const cc_settings_member *m, size_t nm, Buf *out, char *why);
/* The file with those members (a missing file: created with them alone, and its folder).
 * 0, -1 left alone (logged). */
int cc_settings_write(const cc_platform *pf, const char *file, const cc_settings_member *m, size_t nm);
/* ---- end SET1 ---- */

/* The session's list. `file` NULL: the list lives in memory only (tests, the dev host). */
void cc_recent_setup(cc_session *s, const char *file);
/* A file just opened or saved (settings.rs `opened`): first, once; at most 12. The list is read
 * from the file again first (the Rust app may have changed it), then written. Only an absolute
 * path of a carrier with files. */
void cc_recent_opened(cc_session *s, const char *path);
/* Drops `path` from the list (Home's "remove"). 0, or -1 when it is not in the list. */
int cc_recent_remove(cc_session *s, const char *path);
/* query recent_files: [{exists, folder, kind, mtime, name, opened_unix, path, schema}] newest
 * first (calls.rs:261 with `kind` by extension and `mtime` the file's, null when missing). */
void cc_recent_json(cc_session *s, Buf *out);
void cc_recent_free(cc_session *s);
#endif
