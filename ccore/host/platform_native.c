/* The native cc_platform (session/platform.h): POSIX files, clocks, /dev/urandom, stderr.
 * Windows: host/platform_win.c (ticket WIN1); this file is empty there. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#include "host.h"
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int np_read_file(void *ctx, const char *path, size_t max, char **out, size_t *n) {
    (void)ctx;
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return CC_PF_NOT_FOUND;
    size_t cap = 1 << 16, len = 0;
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

static const cc_platform NATIVE = {
    NULL, 1, np_read_file, np_exists, np_write_file, np_now_ms, np_unix_s, np_random, np_log, np_list_dir,
};

const cc_platform *cc_platform_native(void) { return &NATIVE; }
#else
typedef int cc_platform_native_empty; /* Windows: platform_win.c */
#endif
