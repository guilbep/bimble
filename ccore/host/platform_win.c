/* The native cc_platform on Windows (ticket WIN1; platform_native.c is the POSIX one, the same
 * cc_platform_native()): files through the wide-character API (paths are UTF-8, as everywhere in
 * the core), binary mode, a write that replaces its target (MoveFileExW: rename() does not
 * replace on Windows), QueryPerformanceCounter, rand_s (RtlGenRandom), stderr. Also the few POSIX
 * file calls of session.c (cc_win_*, session/platform.h) and the UTF-8 <-> UTF-16 helpers the
 * host uses (main.c, static.c). Empty on every other system: build.sh and test/run.sh compile it
 * everywhere. Built with clang for x86_64-w64-mingw32 (MSYS2 CLANG64, UCRT). */
#ifdef _WIN32
#ifndef _CRT_RAND_S
#define _CRT_RAND_S /* rand_s, before the first stdlib.h */
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "host.h"

wchar_t *cc_win_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
}

/* WC_ERR_INVALID_CHARS (FIXDOC): an unpaired surrogate fails (NULL) instead of becoming U+FFFD,
 * which would name another file. */
char *cc_win_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc((size_t)n);
    if (s && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, s, n, NULL, NULL) != n) {
        free(s);
        return NULL;
    }
    return s;
}

#define WIN_ENV_MAX 32
static struct {
    char *name, *value;
} win_env[WIN_ENV_MAX];

const char *cc_win_getenv(const char *name) {
    for (int i = 0; i < WIN_ENV_MAX && win_env[i].name; i++)
        if (!strcmp(win_env[i].name, name)) return win_env[i].value;
    wchar_t *wn = cc_win_wide(name);
    const wchar_t *wv = wn ? _wgetenv(wn) : NULL;
    char *v = wv ? cc_win_utf8(wv) : NULL;
    free(wn);
    for (int i = 0; i < WIN_ENV_MAX; i++)
        if (!win_env[i].name) {
            char *c = malloc(strlen(name) + 1);
            if (!c) break;
            strcpy(c, name);
            win_env[i].name = c, win_env[i].value = v;
            return v;
        }
    return v; /* the table is full: a new copy each call (never more than 32 names here) */
}

FILE *cc_win_fopen(const char *path, const char *mode) {
    wchar_t *w = cc_win_wide(path), wm[8];
    size_t i = 0;
    for (; mode[i] && i < 7; i++) wm[i] = (wchar_t)(unsigned char)mode[i];
    wm[i] = 0;
    FILE *f = w ? _wfopen(w, wm) : NULL;
    free(w);
    return f;
}

/* open(2) for session.c: O_WRONLY | O_CREAT | O_TRUNC or O_WRONLY | O_APPEND, always binary.
 * CreateFileW with FILE_SHARE_DELETE, so the session file stays replaceable (cc_win_rename) while
 * its append handle is open, as on POSIX. */
int cc_win_open(const char *path, int flags) {
    wchar_t *w = cc_win_wide(path);
    if (!w) {
        errno = ENOENT;
        return -1;
    }
    DWORD disp = (flags & O_CREAT) ? ((flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS)
                                   : ((flags & O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EACCES;
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)h, _O_BINARY | (flags & O_APPEND ? _O_APPEND : 0));
    if (fd < 0) CloseHandle(h);
    return fd;
}

/* rename(2) that replaces `to`. A scanner (Windows Defender) or an indexer may hold the target a
 * moment: a sharing or access error is tried again, 10 times 20 ms at most. */
int cc_win_rename(const char *from, const char *to) {
    wchar_t *a = cc_win_wide(from), *b = cc_win_wide(to);
    int ok = 0;
    for (int i = 0; a && b && i < 10; i++) {
        if ((ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)) break;
        DWORD e = GetLastError();
        if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED) break;
        Sleep(20);
    }
    free(a), free(b);
    return ok ? 0 : -1;
}

int cc_win_unlink(const char *path) {
    wchar_t *w = cc_win_wide(path);
    int ok = w && DeleteFileW(w);
    free(w);
    return ok ? 0 : -1;
}

int cc_win_exists(const char *path) {
    wchar_t *w = cc_win_wide(path);
    int ok = w && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
    free(w);
    return ok;
}

/* Size and last write time (ns since 1970-01-01 UTC, 100 ns steps) of a file. */
int cc_win_stamp(const char *path, uint64_t *size, uint64_t *mtime_ns) {
    wchar_t *w = cc_win_wide(path);
    WIN32_FILE_ATTRIBUTE_DATA a;
    int ok = w && GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    free(w);
    if (!ok) return -1;
    *size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    uint64_t t = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    const uint64_t EPOCH = 116444736000000000ull; /* 1601 to 1970 in 100 ns */
    *mtime_ns = t > EPOCH ? (t - EPOCH) * 100 : 0;
    return 0;
}

int cc_win_random(void *p, size_t n) {
    unsigned char *b = p;
    for (size_t i = 0; i < n; i += 4) {
        unsigned int r;
        if (rand_s(&r) != 0) return -1;
        for (size_t j = 0; j < 4 && i + j < n; j++) b[i + j] = (unsigned char)(r >> (8 * j));
    }
    return 0;
}

static int wp_read_file(void *ctx, const char *path, size_t max, char **out, size_t *n) {
    (void)ctx;
    *out = NULL;
    FILE *f = cc_win_fopen(path, "rb");
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

static int wp_exists(void *ctx, const char *path) { return (void)ctx, cc_win_exists(path); }

static int wp_write_file(void *ctx, const char *path, const char *p, size_t n) {
    (void)ctx;
    size_t k = strlen(path);
    char *tmp = malloc(k + 5);
    if (!tmp) return -1;
    snprintf(tmp, k + 5, "%s.tmp", path);
    FILE *f = cc_win_fopen(tmp, "wb");
    int ok = f && fwrite(p, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    if (ok) ok = cc_win_rename(tmp, path) == 0;
    if (!ok) cc_win_unlink(tmp);
    free(tmp);
    return ok ? 0 : -1;
}

static double wp_now_ms(void *ctx) {
    (void)ctx;
    LARGE_INTEGER f, c;
    if (!QueryPerformanceFrequency(&f) || !QueryPerformanceCounter(&c) || f.QuadPart <= 0) return 0;
    return (double)c.QuadPart * 1e3 / (double)f.QuadPart;
}

static int64_t wp_unix_s(void *ctx) { return (void)ctx, (int64_t)time(NULL); }

static int wp_random(void *ctx, void *p, size_t n) { return (void)ctx, cc_win_random(p, n); }

static void wp_log(void *ctx, const char *line, size_t n) {
    (void)ctx;
    fwrite(line, 1, n, stderr);
    fputc('\n', stderr);
}

/* platform.h list_dir (ticket H1): FindFirstFileW / FindNextFileW, "d" for a folder, "f" for
 * anything else; a reparse point (symbolic link, junction) is an "f", as lstat's link on POSIX:
 * a history walk never follows one. Names as UTF-8, in the system's order (no sort, as readdir). */
static int wp_list_dir(void *ctx, const char *path, size_t max, char **names, size_t *n) {
    (void)ctx;
    *names = NULL, *n = 0;
    size_t pn = strlen(path);
    char *pat = malloc(pn + 3);
    if (!pat) return CC_PF_NO_MEMORY;
    snprintf(pat, pn + 3, "%s%s", path, pn && (path[pn - 1] == '/' || path[pn - 1] == '\\') ? "*" : "/*");
    wchar_t *w = cc_win_wide(pat);
    free(pat);
    if (!w) return CC_PF_NOT_FOUND;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return CC_PF_NOT_FOUND; /* a folder always holds "." and ".." */
    size_t cap = 256, len = 0;
    char *p = malloc(cap);
    if (!p) return FindClose(h), CC_PF_NO_MEMORY;
    int st = CC_PF_OK;
    do {
        char *nm = cc_win_utf8(fd.cFileName);
        if (!nm) continue; /* not valid UTF-16: no name the core could ask for */
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) {
            free(nm);
            continue;
        }
        size_t k = strlen(nm);
        int dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        if (len + k + 2 > max) st = CC_PF_TOO_LARGE;
        else if (len + k + 2 > cap) {
            while (len + k + 2 > cap) cap *= 2;
            char *q = realloc(p, cap);
            if (!q) st = CC_PF_NO_MEMORY;
            else p = q;
        }
        if (st == CC_PF_OK) {
            p[len++] = dir ? 'd' : 'f';
            memcpy(p + len, nm, k + 1);
            len += k + 1;
        }
        free(nm);
    } while (st == CC_PF_OK && FindNextFileW(h, &fd));
    FindClose(h);
    if (st != CC_PF_OK) return free(p), st;
    *names = p, *n = len;
    return CC_PF_OK;
}

/* ---- FS1: stat, the final path, mkdir (platform.h), over the wide-character API ---- */
static int64_t wp_unix_of(FILETIME ft) {
    uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    const uint64_t EPOCH = 116444736000000000ull; /* 1601 to 1970 in 100 ns */
    return t > EPOCH ? (int64_t)((t - EPOCH) / 10000000ull) : 0;
}

/* A reparse point (symbolic link, junction) is CC_PF_LINK when not following, as lstat. */
static int wp_stat_path(void *ctx, const char *path, int follow, cc_pf_stat *st) {
    (void)ctx;
    wchar_t *w = cc_win_wide(path);
    if (!w) return CC_PF_NOT_FOUND;
    WIN32_FILE_ATTRIBUTE_DATA a;
    int ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a) != 0;
    if (ok && follow && (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        /* the target's attributes: open it (links followed) and ask the handle */
        HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, NULL);
        BY_HANDLE_FILE_INFORMATION bi;
        ok = h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &bi);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (ok) {
            a.dwFileAttributes = bi.dwFileAttributes & ~(DWORD)FILE_ATTRIBUTE_REPARSE_POINT;
            a.nFileSizeHigh = bi.nFileSizeHigh, a.nFileSizeLow = bi.nFileSizeLow;
            a.ftLastWriteTime = bi.ftLastWriteTime;
        }
    }
    free(w);
    if (!ok) return CC_PF_NOT_FOUND;
    DWORD at = a.dwFileAttributes;
    st->kind = (at & FILE_ATTRIBUTE_REPARSE_POINT) ? CC_PF_LINK : (at & FILE_ATTRIBUTE_DIRECTORY) ? CC_PF_DIR : CC_PF_FILE;
    st->size = st->kind == CC_PF_FILE ? (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow : 0;
    st->mtime_s = wp_unix_of(a.ftLastWriteTime);
    return CC_PF_OK;
}

/* GetFinalPathNameByHandleW: links resolved, "\\?\" dropped, '/' separators. */
static int wp_real_path(void *ctx, const char *path, char *out, size_t n) {
    (void)ctx;
    wchar_t *w = cc_win_wide(path);
    if (!w) return -1;
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    wchar_t fin[32768];
    DWORD k = GetFinalPathNameByHandleW(h, fin, sizeof fin / sizeof fin[0], FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (k == 0 || k >= sizeof fin / sizeof fin[0]) return -1;
    const wchar_t *f = fin;
    if (!wcsncmp(f, L"\\\\?\\UNC\\", 8)) f += 6, fin[6] = L'\\'; /* \\?\UNC\srv -> \\srv */
    else if (!wcsncmp(f, L"\\\\?\\", 4)) f += 4;
    char *u = cc_win_utf8(f);
    if (!u) return -1;
    size_t un = strlen(u);
    int rc = un < n ? 0 : -1;
    if (!rc) {
        for (size_t i = 0; i <= un; i++) out[i] = u[i] == '\\' ? '/' : u[i];
    }
    free(u);
    return rc;
}

static int wp_make_dir(void *ctx, const char *path) {
    (void)ctx;
    wchar_t *w = cc_win_wide(path);
    if (!w) return -1;
    int ok = CreateDirectoryW(w, NULL) != 0;
    if (!ok && GetLastError() == ERROR_ALREADY_EXISTS) {
        DWORD at = GetFileAttributesW(w);
        ok = at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
    }
    free(w);
    return ok ? 0 : -1;
}
/* ---- end FS1 ---- */

static const cc_platform NATIVE = {
    NULL, 1, wp_read_file, wp_exists, wp_write_file, wp_now_ms, wp_unix_s, wp_random, wp_log, wp_list_dir,
    wp_stat_path, wp_real_path, wp_make_dir, /* FS1 */
};

int (*cc_host_path_rule)(const char *path); /* LINKF: host.h (not asked here: no path_allowed yet) */
const cc_platform *cc_platform_native(void) { return &NATIVE; }
#else
typedef int cc_platform_win_empty; /* ISO C wants a declaration in every translation unit */
#endif
