/* The native cc_platform (session/platform.h): POSIX files, clocks, /dev/urandom, stderr.
 * Windows: host/platform_win.c (ticket WIN1); this file is empty there. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE /* realpath beside _POSIX_C_SOURCE (FS1) */
#elif !defined(__APPLE__)
#define _XOPEN_SOURCE 700 /* realpath (FS1): XSI */
#endif
#include "host.h"
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h> /* SIZE2: map_file */
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h> /* LINKF: spawn */

static int np_read_file(void *ctx, const char *path, size_t max, char **out, size_t *n) {
    (void)ctx;
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return CC_PF_NOT_FOUND;
    size_t cap = 1 << 16, len = 0;
    /* LINKF: a regular file's size first: refused before a byte is read when too large, else read
     * into one buffer of its size (doubling took 1 GiB and a copy for a 0.87 GB IFC) */
    struct stat sb;
    if (!fstat(fileno(f), &sb) && S_ISREG(sb.st_mode)) {
        if ((uint64_t)sb.st_size > (uint64_t)max) {
            fclose(f);
            return CC_PF_TOO_LARGE;
        }
        if ((uint64_t)sb.st_size + 1 > (uint64_t)cap) cap = (size_t)sb.st_size + 1; /* +1: EOF seen, no regrow */
    }
    char *p = malloc(cap + 1);
    while (p) {
        if (len == cap) {
            if (cap >= max) {
                free(p);
                fclose(f);
                return CC_PF_TOO_LARGE;
            }
            char *q = realloc(p, 2 * cap + 1);
            if (!q) break;
            p = q, cap *= 2;
        }
        size_t k = fread(p + len, 1, cap - len, f);
        len += k;
        if (k == 0) break;
    }
    int bad = ferror(f);
    fclose(f);
    if (!p || bad) {
        free(p);
        return bad ? CC_PF_IO : CC_PF_NO_MEMORY;
    }
    p[len] = 0;
    *out = p, *n = len;
    return CC_PF_OK;
}

static int np_exists(void *ctx, const char *path) { return (void)ctx, access(path, F_OK) == 0; }

static int np_write_file(void *ctx, const char *path, const char *p, size_t n) {
    (void)ctx;
    size_t k = 0;
    while (path[k]) k++;
    char *tmp = malloc(k + 5);
    if (!tmp) return -1;
    snprintf(tmp, k + 5, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(p, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    if (ok) ok = rename(tmp, path) == 0;
    else remove(tmp);
    free(tmp);
    return ok ? 0 : -1;
}

static double np_now_ms(void *ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int64_t np_unix_s(void *ctx) { return (void)ctx, (int64_t)time(NULL); }

static int np_random(void *ctx, void *p, size_t n) {
    (void)ctx;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(fd, (char *)p + got, n - got);
        if (k <= 0) break;
        got += (size_t)k;
    }
    close(fd);
    return got == n ? 0 : -1;
}

static void np_log(void *ctx, const char *line, size_t n) {
    (void)ctx;
    fwrite(line, 1, n, stderr);
    fputc('\n', stderr);
}

/* platform.h list_dir: readdir, "d"/"f" by the entry's type (lstat when the file system does
 * not say; a symbolic link is an "f": a history walk never follows one). */
static int np_list_dir(void *ctx, const char *path, size_t max, char **names, size_t *n) {
    (void)ctx;
    *names = NULL, *n = 0;
    DIR *d = opendir(path);
    if (!d) return CC_PF_NOT_FOUND;
    size_t cap = 256, len = 0;
    char *p = malloc(cap);
    if (!p) return closedir(d), CC_PF_NO_MEMORY;
    struct dirent *e;
    int st = CC_PF_OK;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        size_t k = strlen(nm);
        int dir = 0;
#ifdef DT_DIR
        if (e->d_type == DT_DIR) dir = 1;
        else if (e->d_type == DT_UNKNOWN)
#endif
        {
            char full[4096];
            struct stat sb;
            if ((size_t)snprintf(full, sizeof full, "%s/%s", path, nm) < sizeof full && !lstat(full, &sb))
                dir = S_ISDIR(sb.st_mode);
        }
        if (len + k + 2 > max) {
            st = CC_PF_TOO_LARGE;
            break;
        }
        if (len + k + 2 > cap) {
            while (len + k + 2 > cap) cap *= 2;
            char *q = realloc(p, cap);
            if (!q) {
                st = CC_PF_NO_MEMORY;
                break;
            }
            p = q;
        }
        p[len++] = dir ? 'd' : 'f';
        memcpy(p + len, nm, k + 1);
        len += k + 1;
    }
    closedir(d);
    if (st != CC_PF_OK) return free(p), st;
    *names = p, *n = len;
    return CC_PF_OK;
}

/* ---- FS1: stat, realpath, mkdir (platform.h) ---- */
static int np_stat_path(void *ctx, const char *path, int follow, cc_pf_stat *st) {
    (void)ctx;
    struct stat sb;
    if ((follow ? stat(path, &sb) : lstat(path, &sb)) != 0) return CC_PF_NOT_FOUND;
    st->kind = S_ISLNK(sb.st_mode) ? CC_PF_LINK : S_ISDIR(sb.st_mode) ? CC_PF_DIR : S_ISREG(sb.st_mode) ? CC_PF_FILE : CC_PF_OTHER;
    st->size = S_ISREG(sb.st_mode) ? (uint64_t)sb.st_size : 0;
    st->mtime_s = (int64_t)sb.st_mtime;
    return CC_PF_OK;
}

static int np_real_path(void *ctx, const char *path, char *out, size_t n) {
    (void)ctx;
    char buf[PATH_MAX];
    if (!realpath(path, buf) || strlen(buf) >= n) return -1;
    strcpy(out, buf);
    return 0;
}

static int np_make_dir(void *ctx, const char *path) {
    (void)ctx;
    struct stat sb;
    if (mkdir(path, 0700) == 0) return 0;
    return errno == EEXIST && stat(path, &sb) == 0 && S_ISDIR(sb.st_mode) ? 0 : -1;
}
/* ---- end FS1 ---- */

/* ---- LINKF (platform.h) ---- */
/* MemAvailable of /proc/meminfo (Linux); elsewhere unknown (0). BIMBLE_MEM_AVAILABLE (bytes)
 * overrides it: the tests refuse a file without needing a small machine. */
static uint64_t np_mem_available(void *ctx) {
    (void)ctx;
    const char *e = getenv("BIMBLE_MEM_AVAILABLE");
    if (e && *e) return strtoull(e, NULL, 10);
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    uint64_t kb = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "MemAvailable:", 13)) {
            kb = strtoull(line + 13, NULL, 10);
            break;
        }
    fclose(f);
    return kb * 1024;
}

static int np_read_range(void *ctx, const char *path, uint64_t off, size_t n, char *p, size_t *got) {
    (void)ctx;
    *got = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? CC_PF_NOT_FOUND : CC_PF_IO;
    while (*got < n) {
        ssize_t k = pread(fd, p + *got, n - *got, (off_t)(off + *got));
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        *got += (size_t)k;
    }
    close(fd);
    return CC_PF_OK;
}

static int np_cache_dir(void *ctx, char *out, size_t n) {
    const char *e = getenv("BIMBLE_CACHE_DIR"), *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    int w;
    if (e && *e) w = snprintf(out, n, "%s", e);
    else if (x && *x == '/') w = snprintf(out, n, "%s/bimble", x);
    else if (h && *h) {
        char c[4096];
        if ((size_t)snprintf(c, sizeof c, "%s/.cache", h) < sizeof c) np_make_dir(ctx, c);
        w = snprintf(out, n, "%s/.cache/bimble", h);
    } else return -1;
    if (w < 0 || (size_t)w >= n) return -1;
    return np_make_dir(ctx, out);
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} np_thread_arg;

static void *np_thread(void *a) {
    np_thread_arg t = *(np_thread_arg *)a;
    free(a);
    t.fn(t.arg);
    return NULL;
}

static int np_spawn(void *ctx, void (*fn)(void *), void *arg) {
    (void)ctx;
    np_thread_arg *a = malloc(sizeof *a);
    if (!a) return -1;
    a->fn = fn, a->arg = arg;
    pthread_attr_t at;
    pthread_t t;
    if (pthread_attr_init(&at)) return free(a), -1;
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, (size_t)8 << 20);
    int rc = pthread_create(&t, &at, np_thread, a);
    pthread_attr_destroy(&at);
    if (rc) return free(a), -1;
    return 0;
}
/* ---- SIZE2: a file mapped instead of copied (platform.h map_file): the IFC's text is the page cache's ---- */
typedef struct {
    void *base;
    size_t len;
} np_map;
static int np_map_file(void *ctx, const char *path, size_t max, const char **p, size_t *n, void **handle) {
    (void)ctx;
    *p = NULL, *n = 0, *handle = NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? CC_PF_NOT_FOUND : CC_PF_IO;
    struct stat sb;
    long pg = sysconf(_SC_PAGESIZE);
    if (fstat(fd, &sb) || !S_ISREG(sb.st_mode) || sb.st_size <= 0 || pg <= 0 || (uint64_t)sb.st_size % (uint64_t)pg == 0) {
        close(fd); /* empty, not a file, or no zero byte after its end within its last page: read it instead */
        return CC_PF_IO;
    }
    if ((uint64_t)sb.st_size > (uint64_t)max) return close(fd), CC_PF_TOO_LARGE;
    void *m = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return CC_PF_IO;
    np_map *h = malloc(sizeof *h);
    if (!h) return munmap(m, (size_t)sb.st_size), CC_PF_NO_MEMORY;
    posix_madvise(m, (size_t)sb.st_size, POSIX_MADV_SEQUENTIAL);
    h->base = m, h->len = (size_t)sb.st_size;
    *p = m, *n = (size_t)sb.st_size, *handle = h; /* the bytes after the end of the last page are zeros (mmap) */
    return CC_PF_OK;
}
static void np_unmap_file(void *ctx, void *handle) {
    (void)ctx;
    np_map *h = handle;
    if (!h) return;
    munmap(h->base, h->len);
    free(h);
}
/* ---- end SIZE2 ---- */

/* The dialog's path rule, set by fs.c when the host lists folders (cc_host_fs_init); none: all. */
int (*cc_host_path_rule)(const char *path);
static int np_path_allowed(void *ctx, const char *path) { return (void)ctx, cc_host_path_rule ? cc_host_path_rule(path) : 1; }
/* ---- end LINKF ---- */

static const cc_platform NATIVE = {
    NULL, 1, np_read_file, np_exists, np_write_file, np_now_ms, np_unix_s, np_random, np_log, np_list_dir,
    np_stat_path, np_real_path, np_make_dir, /* FS1 */
    np_mem_available, np_read_range, np_cache_dir, np_spawn, np_path_allowed, /* LINKF */
    np_map_file, np_unmap_file, /* SIZE2 */
};

const cc_platform *cc_platform_native(void) { return &NATIVE; }
#else
typedef int cc_platform_native_empty; /* Windows: platform_win.c */
#endif
