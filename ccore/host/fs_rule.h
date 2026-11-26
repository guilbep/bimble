/* The file dialog's path rule, its lexical part (ticket FIXDOC; host/fs.h has the whole rule).
 * Pure string functions, header only: fs.c uses them with the rules of the system it is built
 * for, test/fs_rule_test.c with both (the Windows forms are tested on every system).
 *
 * fs_list checks a path in this order, so that a path outside the roots never reaches the file
 * system (on Windows, opening "\\server\share" contacts the server; "\\.\…" names a device):
 *   1. fsr_normalise: absolute, no NUL, at most FSR_MAX bytes; separators made '/', empty and "."
 *      segments dropped, ".." resolved by the text alone (one above the root is refused). Windows:
 *      "X:/" or "X:\" (a drive), else a network path ("\\server\share", "//server/share": FSR_UNC)
 *      or a device or NT path ("\\.\", "\\?\", "\??\": FSR_DEVICE, always refused); "X:" without a
 *      separator, "\x" without a drive, a ':' after the drive (a stream) and a segment that ends
 *      with '.' or ' ' (Windows drops those characters) are refused (FSR_BAD).
 *   2. fsr_under: the normalised path is a root or under one (Windows: ASCII letters compare
 *      without case). The host keeps each root as given (normalised) and as resolved.
 *   3. Only then the file system: realpath, and the same rule on the resolved path (fs.c). */
#ifndef CCORE_HOST_FS_RULE_H
#define CCORE_HOST_FS_RULE_H
#include <stddef.h>
#include <string.h>

#define FSR_MAX 4096
enum { FSR_OK = 0, FSR_BAD = 1, FSR_UNC = 2, FSR_DEVICE = 3 };

static inline int fsr_sep(char c, int win) { return c == '/' || (win && c == '\\'); }

/* `p` normalised into `out` (at least strlen(p) + 2 bytes, FSR_MAX + 2 is always enough).
 * Returns FSR_OK, or FSR_UNC with `out` normalised ("//server/share/…"), or FSR_BAD / FSR_DEVICE
 * (out undefined). `win`: the rules of Windows paths. */
static inline int fsr_normalise(const char *p, int win, char *out, size_t cap) {
    size_t n = p ? strlen(p) : 0, o = 0, i = 0, base;
    if (!n || n > FSR_MAX || cap < n + 2) return FSR_BAD;
    int unc = 0;
    if (win) {
        if (fsr_sep(p[0], 1) && fsr_sep(p[1], 1)) {
            /* "\\.\x", "\\?\x" (and with '/'): a device or the raw NT namespace */
            if ((p[2] == '.' || p[2] == '?') && (fsr_sep(p[3], 1) || !p[3])) return FSR_DEVICE;
            unc = 1;
            out[o++] = '/', out[o++] = '/';
            i = 2;
        } else if (fsr_sep(p[0], 1) && p[1] == '?' && p[2] == '?' && fsr_sep(p[3], 1)) {
            return FSR_DEVICE; /* "\??\C:\…" */
        } else if (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':' && fsr_sep(p[2], 1)) {
            out[o++] = p[0], out[o++] = ':', out[o++] = '/';
            i = 3;
        } else {
            return FSR_BAD; /* relative, "C:x", or "\x" on the current drive */
        }
        for (size_t j = i; j < n; j++)
            if (p[j] == ':') return FSR_BAD; /* "file:stream" */
    } else {
        if (p[0] != '/') return FSR_BAD;
        out[o++] = '/';
        i = 1;
    }
    base = o; /* never popped below: the root, "X:/" or "//" */
    int segs = 0;
    while (i < n) {
        while (i < n && fsr_sep(p[i], win)) i++;
        size_t s = i;
        while (i < n && !fsr_sep(p[i], win)) i++;
        size_t k = i - s;
        if (!k || (k == 1 && p[s] == '.')) continue;
        if (k == 2 && p[s] == '.' && p[s + 1] == '.') {
            if (unc && segs <= 2) return FSR_BAD; /* above "//server/share" */
            if (o == base) return FSR_BAD;        /* above the root */
            o--;                                  /* the '/' after the last segment */
            while (o > base && out[o - 1] != '/') o--;
            segs--;
            continue;
        }
        if (win && (p[i - 1] == '.' || p[i - 1] == ' ')) return FSR_BAD;
        memcpy(out + o, p + s, k);
        o += k;
        out[o++] = '/';
        segs++;
    }
    if (unc && segs < 2) return FSR_BAD; /* "\\server" alone: no share */
    if (o > base) o--;                    /* no trailing '/' but on the root */
    out[o] = 0;
    return unc ? FSR_UNC : FSR_OK;
}

static inline int fsr_ch_eq(char a, char b, int win) {
    if (win) {
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    }
    return a == b;
}

/* `p` is `root` or under it; both normalised ('/' separators, as fsr_normalise or realpath give). */
static inline int fsr_under(const char *p, const char *root, int win) {
    size_t k = strlen(root);
    while (k > 1 && root[k - 1] == '/') k--; /* "/" stays "/"; "C:/" -> "C:" */
    for (size_t i = 0; i < k; i++)
        if (!p[i] || !fsr_ch_eq(p[i], root[i], win)) return 0;
    return p[k] == 0 || p[k] == '/' || (k == 1 && root[0] == '/');
}
#endif
