/* Where the native host finds the Roboto files of the PDF export (tickets D7, SIZE1,
 * session/sheet_svg.h cc_paper_pdf_query). The export is Helvetica by default (nothing embedded);
 * Roboto is an option, and its files (350 KB each) are not shipped: neither in the program, the
 * wasm nor the release archive. The host only names a folder; the session reads it when Roboto is
 * asked for (`pdf_fonts`, `sheet_pdf {"font":"roboto"}`), and finding nothing never fails:
 *   - $BIMBLE_FONTS when set (and nothing else: tests point it at a folder of their own);
 *   - else the repository's ccore/fonts when it holds Roboto-Regular.ttf (a dev
 *     build: the program is ccore/out/<mode>/bimble-ccore, three folders down);
 *   - else the user's folder <config dir>/bimble/fonts: $XDG_CONFIG_HOME or ~/.config on Linux,
 *     ~/Library/Application Support on macOS, %APPDATA% on Windows (named even when it does not
 *     exist yet: files put there are found on the next export). */
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
#else
    return 0;
#endif
    char *slash = strrchr(exe, '/');
    if (!slash) return 0;
    int w = snprintf(out, n, "%.*s", (int)(slash - exe), exe);
    return w > 0 && (size_t)w < n;
}

static int has_regular(const char *dir) {
    char f[PATH_MAX + 32];
    if (snprintf(f, sizeof f, "%s/Roboto-Regular.ttf", dir) >= (int)sizeof f) return 0;
    FILE *fp = fopen(f, "rb");
    if (fp) fclose(fp);
    return fp != NULL;
}

/* <config dir>/bimble/fonts into `out`, or 0 (no home). */
static int user_dir(char *out, size_t n) {
    const char *base = NULL, *tail = "/bimble/fonts";
    char tmp[PATH_MAX];
#if defined(_WIN32)
    base = getenv("APPDATA"), tail = "\\bimble\\fonts";
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (home && *home && snprintf(tmp, sizeof tmp, "%s/Library/Application Support", home) < (int)sizeof tmp) base = tmp;
#else
    base = getenv("XDG_CONFIG_HOME");
    if (!base || !*base) {
        const char *home = getenv("HOME");
        base = NULL;
        if (home && *home && snprintf(tmp, sizeof tmp, "%s/.config", home) < (int)sizeof tmp) base = tmp;
    }
#endif
    if (!base || !*base) return 0;
    int w = snprintf(out, n, "%s%s", base, tail);
    return w > 0 && (size_t)w < n;
}

void cc_host_fonts(cc_session *s, const cc_static *web) {
    (void)web; /* SIZE1: the lab's files hold no font any more */
    const char *env = getenv("BIMBLE_FONTS");
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
