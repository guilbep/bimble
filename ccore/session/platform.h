/* cc_platform: what the session layer (L5) needs from the world, passed in by the carrier (L6)
 * that creates the session (docs/research/core-architecture.md §5). The session code calls
 * only these for files, time, randomness and logging; it has no #ifdef per platform.
 *   native: host/platform_native.c (cc_platform_native: POSIX files, clocks, /dev/urandom, stderr);
 *           on Windows host/platform_win.c (ticket WIN1: wide-character files, rand_s)
 *   wasm:   src/wasm_api.c (no files: the page stages the bytes an open reads, and the module
 *           keeps the last file written (Save, Save as, Export IFC) for the page to download;
 *           the wall clock is the page's (host_clock); the log is read back through host_log)
 * Every function takes the platform's own `ctx` first. */
#ifndef CCORE_SESSION_PLATFORM_H
#define CCORE_SESSION_PLATFORM_H
#include <stddef.h>
#include <stdint.h>

/* read_file's status */
enum { CC_PF_OK = 0, CC_PF_NOT_FOUND, CC_PF_TOO_LARGE, CC_PF_IO, CC_PF_NO_MEMORY };

/* ---- FS1: stat_path's answer ---- */
enum { CC_PF_FILE = 1, CC_PF_DIR, CC_PF_LINK, CC_PF_OTHER };
typedef struct {
    int kind;        /* CC_PF_FILE, CC_PF_DIR, CC_PF_LINK (only when not following), CC_PF_OTHER */
    uint64_t size;   /* bytes (a file), else 0 */
    int64_t mtime_s; /* last change, seconds since 1970-01-01 UTC */
} cc_pf_stat;
/* ---- end FS1 ---- */

typedef struct cc_platform {
    void *ctx;
    /* 1: paths name files of a file system; 0: none (the wasm module): a path is a file name for
     * the page, and the actions that would ask where (Save of a new model, Save as without a
     * path) write under the current name instead of refusing. */
    int files;
    /* The whole file at `path`, malloc'd and NUL-terminated (`*n` bytes before the NUL), at most
     * `max` bytes. Returns CC_PF_OK, or a status with *p NULL. */
    int (*read_file)(void *ctx, const char *path, size_t max, char **p, size_t *n);
    /* 1 if `path` exists. */
    int (*exists)(void *ctx, const char *path);
    /* Writes p[0..n) to `path` atomically (`<path>.tmp`, then rename). 0 ok, -1 failed. */
    int (*write_file)(void *ctx, const char *path, const char *p, size_t n);
    /* A monotonic clock in ms (0 without a clock). */
    double (*now_ms)(void *ctx);
    /* Wall-clock seconds since 1970-01-01 UTC (0 without a clock). */
    int64_t (*unix_s)(void *ctx);
    /* Fills p[0..n) with random bytes. 0 ok, -1 none to be had. */
    int (*random)(void *ctx, void *p, size_t n);
    /* One log line (text without the final newline; the platform adds it). */
    void (*log)(void *ctx, const char *line, size_t n);
    /* The entries of the folder at `path` (ticket H1, the history store): malloc'd, each one
     * "d<name>\0" (a folder) or "f<name>\0" (anything else), `*n` bytes in all, at most `max`
     * (else CC_PF_TOO_LARGE); "." and ".." left out, in no particular order. CC_PF_NOT_FOUND
     * when there is no such folder. The wasm module has no folders: NOT_FOUND always. NULL (a
     * carrier written before it) reads as no folders too. */
    int (*list_dir)(void *ctx, const char *path, size_t max, char **names, size_t *n);
    /* ---- FS1 (the file dialog in the page, recent files; NULL in a carrier without folders,
     * and in one written before them: callers check) ---- */
    /* What `path` is: lstat (follow 0: a symbolic link, a junction is CC_PF_LINK) or stat
     * (follow 1). CC_PF_OK, else CC_PF_NOT_FOUND. */
    int (*stat_path)(void *ctx, const char *path, int follow, cc_pf_stat *st);
    /* The absolute path of `path` with every link resolved (realpath; on Windows the final path,
     * with '/' separators), into out[0..n). 0 ok, -1 (no such path, too long). */
    int (*real_path)(void *ctx, const char *path, char *out, size_t n);
    /* Creates the folder `path` (one level). 0 when it was made or already is a folder, -1. */
    int (*make_dir)(void *ctx, const char *path);
    /* ---- end FS1 ---- */
} cc_platform;

#ifdef _WIN32
/* Windows (ticket WIN1, host/platform_win.c): the POSIX file calls the session file makes
 * (session.c), over the wide-character API: UTF-8 paths, binary mode, a rename that replaces its
 * target (also while an append handle of it is open). Same results as open, rename, unlink,
 * access(F_OK) == 0; cc_win_stamp: size and mtime in ns since 1970, 0 or -1. Not in the wasm
 * build. TODO(round 2): behind cc_platform members, so L5 calls no system function at all. */
int cc_win_open(const char *path, int flags);
int cc_win_rename(const char *from, const char *to);
int cc_win_unlink(const char *path);
int cc_win_exists(const char *path);
int cc_win_stamp(const char *path, uint64_t *size, uint64_t *mtime_ns);
int cc_win_random(void *p, size_t n);
#endif

/* printf-style log line through pf->log ("bimble-ccore: …"). At most 8 KiB, cut beyond. */
void cc_log(const cc_platform *pf, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
#endif
