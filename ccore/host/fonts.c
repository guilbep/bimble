/* Where the native host finds the Roboto files of the PDF export (tickets D7, SIZE1,
 * session/sheet_svg.h cc_paper_pdf_query). The export is Helvetica by default (nothing embedded);
 * Roboto is an option, and its files (350 KB each) are not shipped: neither in the program, the
 * wasm nor the release archive. The host only names a folder; the session reads it when Roboto is
 * asked for (`pdf_fonts`, `sheet_pdf {"font":"roboto"}`), and finding nothing never fails:
 *   - $BIMBLE_FONTS when set (and nothing else: tests point it at a folder of their own);
 *   - else the repository's ccore/fonts when it holds Roboto-Regular.ttf (a dev
 *     build: the program is ccore/out/<mode>/bimble-ccore, three folders down);
 *   - else <the user's Bimble folder>/fonts, beside settings.json (cc_host_config_dir):
 *     $XDG_CONFIG_HOME/bimble or ~/.config/bimble on Linux, ~/Library/Application Support/Bimble
 *     on macOS, %APPDATA%\Bimble on Windows (named even when it does not exist yet: files put
 *     there are found on the next export). */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32 /* WIN2: the program's folder by GetModuleFileNameW */
#include <windows.h>
#endif

#include "host.h"
#include "sheet_svg.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* The program's folder (links resolved), or 0. */
static int exe_dir(char *out, size_t n) {
    char exe[PATH_MAX];
    exe[0] = 0;
#if defined(__APPLE__)
    char path[PATH_MAX];
    uint32_t pn = sizeof path;
    if (_NSGetExecutablePath(path, &pn) != 0 || !realpath(path, exe)) return 0;
#elif defined(__linux__)
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) return 0;
    exe[k] = 0;
#elif defined(_WIN32)
    /* WIN2: the program's path as UTF-8 (as host/main.c finds <exe dir>\web) */
    wchar_t wp[PATH_MAX];
    DWORD wn = GetModuleFileNameW(NULL, wp, PATH_MAX);
    if (wn == 0 || wn >= PATH_MAX) return 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, wp, -1, exe, (int)sizeof exe, NULL, NULL)) return 0;
#else
    return 0;
#endif
    char *slash = strrchr(exe, '/');
#ifdef _WIN32
    char *bs = strrchr(exe, '\\'); /* WIN2: either separator */
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    if (!slash) return 0;
    int w = snprintf(out, n, "%.*s", (int)(slash - exe), exe);
    return w > 0 && (size_t)w < n;
}

static int has_regular(const char *dir) {
    char f[PATH_MAX + 32];
    if (snprintf(f, sizeof f, "%s/Roboto-Regular.ttf", dir) >= (int)sizeof f) return 0;
#ifdef _WIN32
    FILE *fp = cc_win_fopen(f, "rb"); /* FIXDOC: a UTF-8 path, not the ANSI code page */
#else
    FILE *fp = fopen(f, "rb");
#endif
    if (fp) fclose(fp);
    return fp != NULL;
}

/* <the user's Bimble folder>/fonts into `out`, or 0 (no home): the settings folder of main.c
 * (FIXDOC: one spelling, settings.rs's; it was .../bimble/fonts beside .../Bimble on macOS and
 * Windows, two folders on a case-sensitive volume). */
static int user_dir(char *out, size_t n) {
    char base[PATH_MAX];
    if (!cc_host_config_dir(base, sizeof base)) return 0;
    int w = snprintf(out, n, "%s/fonts", base);
    return w > 0 && (size_t)w < n;
}

void cc_host_fonts(cc_session *s, const cc_static *web) {
    (void)web; /* SIZE1: the lab's files hold no font any more */
    const char *env = cc_host_getenv("BIMBLE_FONTS");
    if (env && *env) {
        cc_paper_set_font_dir(s, env);
        return;
    }
    char dir[PATH_MAX], cand[PATH_MAX + 64];
    if (exe_dir(dir, sizeof dir) && snprintf(cand, sizeof cand, "%s/../../fonts", dir) < (int)sizeof cand &&
        has_regular(cand)) {
        cc_paper_set_font_dir(s, cand);
        return;
    }
    if (user_dir(cand, sizeof cand)) cc_paper_set_font_dir(s, cand);
}
