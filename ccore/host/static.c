/* The built lab, read once at start (static.h). The directory is untrusted: every limit of
 * static.h holds, symbolic links are never followed below the root (fstatat and openat with
 * O_NOFOLLOW), the recursion is bounded by CC_STATIC_MAX_DEPTH, and every error is a message. */
#define _POSIX_C_SOURCE 200809L
#include "static.h"

#ifdef _WIN32
/* Windows (ticket WIN1): FindFirstFileW / FindNextFileW; a reparse point (symbolic link,
 * junction) below the root is never followed, as O_NOFOLLOW above. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include "host.h" /* cc_win_wide, cc_win_utf8 */
#else
#include <dirent.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct cc_static {
    cc_static_file *f;
    size_t n, cap, bytes;
};

static void fail(char *err, size_t errn, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (errn) vsnprintf(err, errn, fmt, ap);
    va_end(ap);
}

const char *cc_static_ctype(const char *name) {
    static const char *const map[][2] = {
        {"html", "text/html; charset=utf-8"},  {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"}, {"css", "text/css; charset=utf-8"},
        {"json", "application/json"},          {"map", "application/json"},
        {"wasm", "application/wasm"},          {"svg", "image/svg+xml"},
        {"png", "image/png"},                  {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},                {"gif", "image/gif"},
        {"webp", "image/webp"},                {"ico", "image/x-icon"},
        {"woff2", "font/woff2"},               {"woff", "font/woff"},
        {"ttf", "font/ttf"},                   {"otf", "font/otf"},
        {"txt", "text/plain; charset=utf-8"},  {"xml", "application/xml"},
        {"pdf", "application/pdf"},            {"webmanifest", "application/manifest+json"},
    };
    const char *slash = strrchr(name, '/');
    const char *dot = strrchr(slash ? slash : name, '.');
    if (dot && dot[1]) {
        char ext[16];
        size_t k = strlen(dot + 1);
        if (k < sizeof ext) {
            for (size_t i = 0; i <= k; i++) {
                char c = dot[1 + i];
                ext[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
            }
            for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
                if (!strcmp(ext, map[i][0])) return map[i][1];
        }
    }
    return "application/octet-stream";
}

/* A name the list can hold: no hidden file, nothing a request path could not carry as is. */
static int name_ok(const char *s) {
    if (!*s || *s == '.') return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c <= 0x20 || c == 0x7f || c == '%' || c == '?' || c == '#' || c == '\\' || c == '"') return 0;
    }
    return 1;
}

/* Reads one regular file (fd) of `size` bytes into the list as `path`. */
static int add_file(cc_static *s, int fd, off_t size, const char *path, char *err, size_t errn) {
    if (s->n >= CC_STATIC_MAX_FILES) {
        fail(err, errn, "more than %d files", CC_STATIC_MAX_FILES);
        return -1;
    }
    if (size < 0 || (unsigned long long)size > CC_STATIC_MAX_BYTES - s->bytes) {
        fail(err, errn, "more than %u bytes in all (at %s)", (unsigned)CC_STATIC_MAX_BYTES, path);
        return -1;
    }
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        cc_static_file *nf = realloc(s->f, cap * sizeof *nf);
        if (!nf) {
            fail(err, errn, "out of memory");
            return -1;
        }
        s->f = nf, s->cap = cap;
    }
    size_t n = (size_t)size, got = 0;
    char *data = malloc(n ? n : 1);
    char *p = strdup(path);
    if (!data || !p) {
        free(data), free(p);
        fail(err, errn, "out of memory");
        return -1;
    }
    while (got < n) { /* a file that shrank meanwhile ends at its end; one that grew is cut */
        ssize_t k = read(fd, data + got, n - got);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) {
            fail(err, errn, "%s: %s", path, strerror(errno));
            free(data), free(p);
            return -1;
        }
        if (k == 0) break;
        got += (size_t)k;
    }
    s->f[s->n++] = (cc_static_file){p, cc_static_ctype(p), data, got};
    s->bytes += got;
    return 0;
}

#ifdef _WIN32
/* Walks the directory `dir`, whose served path is `prefix` ("" for the root). */
static int walk(cc_static *s, const wchar_t *dir, const char *prefix, int depth, char *err, size_t errn) {
    size_t dn = wcslen(dir);
    wchar_t *pat = malloc((dn + 3) * sizeof *pat);
    if (!pat) {
        fail(err, errn, "out of memory");
        return -1;
    }
    memcpy(pat, dir, dn * sizeof *pat);
    pat[dn] = L'\\', pat[dn + 1] = L'*', pat[dn + 2] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) {
        fail(err, errn, "%s/: cannot read the folder (error %lu)", prefix, (unsigned long)GetLastError());
        return -1;
    }
    int rc = 0;
    char path[CC_STATIC_MAX_PATH];
    do {
        char *name = cc_win_utf8(fd.cFileName);
        if (!name) {
            fail(err, errn, "%s/: a name that is not valid UTF-16", prefix);
            rc = -1;
            break;
        }
        int ok = name_ok(name);
        int pn = ok ? snprintf(path, sizeof path, "%s/%s", prefix, name) : 0;
        free(name);
        if (!ok) continue;
        if (pn < 0 || (size_t)pn >= sizeof path) {
            fail(err, errn, "a path longer than %d bytes under %s/", CC_STATIC_MAX_PATH - 1, prefix);
            rc = -1;
            break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue; /* links: not served */
        size_t fn = wcslen(fd.cFileName);
        wchar_t *full = malloc((dn + fn + 2) * sizeof *full);
        if (!full) {
            fail(err, errn, "out of memory");
            rc = -1;
            break;
        }
        memcpy(full, dir, dn * sizeof *full);
        full[dn] = L'\\';
        memcpy(full + dn + 1, fd.cFileName, (fn + 1) * sizeof *full);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth >= CC_STATIC_MAX_DEPTH) {
                fail(err, errn, "%s/: deeper than %d directories", path, CC_STATIC_MAX_DEPTH);
                rc = -1;
            } else {
                rc = walk(s, full, path, depth + 1, err, errn);
            }
        } else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DEVICE)) {
            HANDLE fh = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
            LARGE_INTEGER sz;
            if (fh != INVALID_HANDLE_VALUE && GetFileSizeEx(fh, &sz)) {
                int cfd = _open_osfhandle((intptr_t)fh, _O_RDONLY | _O_BINARY);
                if (cfd >= 0) {
                    /* off_t is 32 bits here: anything over the limit is "over the limit". */
                    off_t size = sz.QuadPart > (LONGLONG)CC_STATIC_MAX_BYTES ? (off_t)CC_STATIC_MAX_BYTES + 1 : (off_t)sz.QuadPart;
                    rc = add_file(s, cfd, size, path, err, errn);
                    _close(cfd);
                } else {
                    CloseHandle(fh);
                }
            } else if (fh != INVALID_HANDLE_VALUE) {
                CloseHandle(fh);
            }
        }
        free(full);
    } while (rc == 0 && FindNextFileW(h, &fd));
    if (rc == 0 && GetLastError() != ERROR_NO_MORE_FILES) {
        fail(err, errn, "%s/: cannot read the folder (error %lu)", prefix, (unsigned long)GetLastError());
        rc = -1;
    }
    FindClose(h);
    return rc;
}
#else
/* Walks the directory `dfd` (closed here), whose served path is `prefix` ("" for the root). */
static int walk(cc_static *s, int dfd, const char *prefix, int depth, char *err, size_t errn) {
    DIR *d = fdopendir(dfd);
    if (!d) {
        fail(err, errn, "%s/: %s", prefix, strerror(errno));
        close(dfd);
        return -1;
    }
    int rc = 0;
    struct dirent *e;
    char path[CC_STATIC_MAX_PATH];
    while (rc == 0 && (errno = 0, e = readdir(d))) {
        if (!name_ok(e->d_name)) continue;
        int pn = snprintf(path, sizeof path, "%s/%s", prefix, e->d_name);
        if (pn < 0 || (size_t)pn >= sizeof path) {
            fail(err, errn, "a path longer than %d bytes under %s/", CC_STATIC_MAX_PATH - 1, prefix);
            rc = -1;
            break;
        }
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) continue; /* gone meanwhile */
        if (S_ISDIR(st.st_mode)) {
            if (depth >= CC_STATIC_MAX_DEPTH) {
                fail(err, errn, "%s/: deeper than %d directories", path, CC_STATIC_MAX_DEPTH);
                rc = -1;
                break;
            }
            int sub = openat(dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sub < 0) continue; /* replaced by a link meanwhile, or unreadable */
            rc = walk(s, sub, path, depth + 1, err, errn);
        } else if (S_ISREG(st.st_mode)) {
            int fd = openat(dirfd(d), e->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) continue;
            struct stat fs;
            if (fstat(fd, &fs) == 0 && S_ISREG(fs.st_mode)) rc = add_file(s, fd, fs.st_size, path, err, errn);
            close(fd);
        }
        /* symbolic links, devices, sockets, fifos: not served */
    }
    if (rc == 0 && errno) {
        fail(err, errn, "%s/: %s", prefix, strerror(errno));
        rc = -1;
    }
    closedir(d);
    return rc;
}
#endif

static int by_path(const void *a, const void *b) {
    return strcmp(((const cc_static_file *)a)->path, ((const cc_static_file *)b)->path);
}

cc_static *cc_static_load(const char *dir, char *err, size_t errn) {
    if (errn) err[0] = 0;
    cc_static *s = calloc(1, sizeof *s);
    if (!s) {
        fail(err, errn, "out of memory");
        return NULL;
    }
    /* The root itself may be a link (an installed tree); nothing below it is followed. */
#ifdef _WIN32
    wchar_t *wdir = cc_win_wide(dir);
    DWORD at = wdir ? GetFileAttributesW(wdir) : INVALID_FILE_ATTRIBUTES;
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) {
        fail(err, errn, "%s: not a folder", dir);
        free(wdir), free(s);
        return NULL;
    }
    int wrc = walk(s, wdir, "", 0, err, errn);
    free(wdir);
    if (wrc < 0) {
#else
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        fail(err, errn, "%s: %s", dir, strerror(errno));
        free(s);
        return NULL;
    }
    if (walk(s, dfd, "", 0, err, errn) < 0) {
#endif
        cc_static_free(s);
        return NULL;
    }
    if (s->n) qsort(s->f, s->n, sizeof *s->f, by_path);
    if (!cc_static_find(s, "/", 1)) {
        fail(err, errn, "%s: no index.html", dir);
        cc_static_free(s);
        return NULL;
    }
    return s;
}

const cc_static_file *cc_static_find(const cc_static *s, const char *path, size_t n) {
    if (!s) return NULL;
    if (n == 1 && path[0] == '/') path = "/index.html", n = 11;
    size_t lo = 0, hi = s->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *q = s->f[mid].path;
        size_t qn = strlen(q);
        int c = memcmp(q, path, qn < n ? qn : n);
        if (c == 0) c = qn < n ? -1 : qn > n ? 1 : 0;
        if (c == 0) return &s->f[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

void cc_static_stats(const cc_static *s, size_t *files, size_t *bytes) {
    *files = s ? s->n : 0;
    *bytes = s ? s->bytes : 0;
}

void cc_static_free(cc_static *s) {
    if (!s) return;
    for (size_t i = 0; i < s->n; i++) free(s->f[i].path), free(s->f[i].data);
    free(s->f);
    free(s);
}
