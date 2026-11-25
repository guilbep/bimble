/* cc_platform: what the session layer (L5) needs from the world, passed in by the carrier (L6)
 * that creates the session (docs/research/core-architecture.md §5). The session code calls
 * only these for files, time, randomness and logging; it has no #ifdef per platform.
 *   native: host/platform_native.c (cc_platform_native: POSIX files, clocks, /dev/urandom, stderr)
 *   wasm:   src/wasm_api.c (no files: the model text is handed in and Save keeps the text; no
 *           clock; the log is read back by the page through the export host_log)
 * Every function takes the platform's own `ctx` first. */
#ifndef CCORE_SESSION_PLATFORM_H
#define CCORE_SESSION_PLATFORM_H
#include <stddef.h>
#include <stdint.h>

/* read_file's status */
enum { CC_PF_OK = 0, CC_PF_NOT_FOUND, CC_PF_TOO_LARGE, CC_PF_IO, CC_PF_NO_MEMORY };

typedef struct cc_platform {
    void *ctx;
    /* 1: paths name files of a file system; 0: none (the wasm module). Without files, exporting
     * IFC from an action only logs (the page calls the export itself). */
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
} cc_platform;

/* printf-style log line through pf->log ("bimble-ccore: …"). At most 8 KiB, cut beyond. */
void cc_log(const cc_platform *pf, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
#endif
