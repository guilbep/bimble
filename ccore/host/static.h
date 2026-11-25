/* The built lab served by the native host (L6, ticket R1): a list of files read once at start
 * from one directory (`<exe dir>/web/`, or `bimble-ccore open --web <dir>`). A request path is
 * matched against that list, never joined to a file system path: no `..`, no symlink walk, no
 * file that appeared after the start. */
#ifndef CCORE_HOST_STATIC_H
#define CCORE_HOST_STATIC_H
#include <stddef.h>

#define CC_STATIC_MAX_BYTES (64u << 20) /* all files together */
#define CC_STATIC_MAX_FILES 4096
#define CC_STATIC_MAX_DEPTH 8           /* directories below the root */
#define CC_STATIC_MAX_PATH 512          /* bytes of a served path, "/" included */

typedef struct {
    char *path;        /* "/index.html", "/assets/x.js": "/" and the path under the root */
    const char *ctype; /* by extension (cc_static_ctype) */
    char *data;
    size_t n;
} cc_static_file;

typedef struct cc_static cc_static;

/* Reads every regular file under `dir` (symbolic links, hidden names and other kinds are skipped).
 * Returns NULL with a message in err on an error: the directory cannot be read, a limit above is
 * passed, out of memory, or there is no index.html. */
cc_static *cc_static_load(const char *dir, char *err, size_t errn);

/* The file of a request path (the target before '?', not decoded), or NULL. "/" is
 * "/index.html". */
const cc_static_file *cc_static_find(const cc_static *s, const char *path, size_t n);

/* The number of files and their bytes. */
void cc_static_stats(const cc_static *s, size_t *files, size_t *bytes);

/* The Content-Type of a name, by its extension (application/octet-stream when unknown). */
const char *cc_static_ctype(const char *name);

void cc_static_free(cc_static *s);
#endif
