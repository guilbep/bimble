/* The native app's local session log (ticket NUI2, the owner's "put the log somewhere so that when I try the app you have
 * observability"): always on in a window (the setting session_log, native.json; off: nothing written), on offscreen only
 * when BIMBLE_LOG_DIR is set. Local only: nothing leaves the machine.
 *
 *   $BIMBLE_LOG_DIR | $XDG_STATE_HOME/bimble/sessions | ~/.local/state/bimble/sessions / <UTC time>-<pid>/
 *   (macOS, MACAPP: $BIMBLE_LOG_DIR | ~/Library/Logs/bimble/sessions / <UTC time>-<pid>/; crash addresses: atos)
 *     session.log   the app's version, SDL, GPU, display scale, window and swapchain sizes at each change, the settings
 *                   in force, every file and link opened with its times, SDL's errors and assertions, warnings
 *     frames.csv    one row a second: frames, frame ms median / p95 / max, input -> frame median / max, triangles, draws,
 *                   tiles, GPU MB, RSS MB, moving or still
 *     inputs.csv    inputs answered in more than 100 ms
 *     report.txt    the Performance report, every 10 s and at exit
 *   On SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE: a last line and the stack's raw addresses (async-signal-safe: write(2),
 *   backtrace_symbols_fd), the executable's base address logged at start to place them. Each line flushed (a kill loses
 *   at most the second being aggregated); the last 20 sessions kept; a session's files capped at 20 MB together. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#ifdef __APPLE__
#include "mac.h" /* MACAPP: the load address and LC_UUID, ~/Library/Logs */
#else
#include <link.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ui.h"

#define KEEP_SESSIONS 20
#define CAP_BYTES (20u << 20)

static char dir[1024];
static FILE *f_log, *f_frames, *f_inputs;
static int crash_fd = -1;
static uint64_t written;
static double sec_t0, next_report;
static float sec_ms[4096], sec_in[512];
static int sec_n, sec_n3d, sec_nin;
static SDL_LogOutputFunction sdl_default_log;
static void *sdl_default_ud;

const char *slog_dir(void) { return dir; }

static int base_dir(char *out, size_t n) {
    const char *e = getenv("BIMBLE_LOG_DIR");
    if (e && *e) return snprintf(out, n, "%s", e) > 0;
#ifdef __APPLE__
    return mac_library(out, n, "Logs/bimble/sessions");
#endif
    const char *x = getenv("XDG_STATE_HOME"), *h = getenv("HOME");
    if (x && *x) return snprintf(out, n, "%s/bimble/sessions", x) > 0;
    if (h && *h) return snprintf(out, n, "%s/.local/state/bimble/sessions", h) > 0;
    return 0;
}

static void mkdirs(const char *p) {
    char d[1100];
    snprintf(d, sizeof d, "%s", p);
    for (char *q = d + 1; *q; q++)
        if (*q == '/') *q = 0, mkdir(d, 0700), *q = '/';
    mkdir(d, 0700);
}

static int cmps(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }

/* The session folders under `base`, sorted (their names start with the UTC time): n, names malloc'd. */
static int sessions(const char *base, char ***names) {
    DIR *d = opendir(base);
    *names = NULL;
    if (!d) return 0;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9' || strlen(e->d_name) < 16) continue;
        if (n == cap) {
            char **nn = realloc(*names, (size_t)(cap = cap ? cap * 2 : 32) * sizeof *nn);
            if (!nn) break;
            *names = nn;
        }
        (*names)[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n) qsort(*names, (size_t)n, sizeof **names, cmps);
    return n;
}

static void rm_session(const char *base, const char *name) {
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", base, name);
    DIR *d = opendir(p);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char f[1700];
        snprintf(f, sizeof f, "%s/%s", p, e->d_name);
        unlink(f);
    }
    closedir(d);
    rmdir(p);
}

int slog_last(void) {
    char base[1024];
    if (!base_dir(base, sizeof base)) return 1;
    char **names;
    int n = sessions(base, &names);
    if (n) printf("%s/%s\n", base, names[n - 1]);
    else fprintf(stderr, "bimble: no session log in %s\n", base);
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
    return n ? 0 : 1;
}

static void stamp(char *o, size_t n) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm g;
    gmtime_r(&ts.tv_sec, &g);
    size_t k = strftime(o, n, "%Y-%m-%dT%H:%M:%S", &g);
    snprintf(o + k, n - k, ".%03ldZ", ts.tv_nsec / 1000000);
}

static void out(FILE *f, const char *s, size_t n) {
    if (!f) return;
    if (written + n > CAP_BYTES) {
        if (written <= CAP_BYTES && f_log) fputs("(the session's log reached 20 MB: nothing more written)\n", f_log), fflush(f_log);
        written = CAP_BYTES + 1;
        return;
    }
    fwrite(s, 1, n, f);
    fflush(f);
    written += n;
}

void slog(const char *fmt, ...) {
    if (!f_log) return;
    char line[2048], t[40];
    stamp(t, sizeof t);
    int k = snprintf(line, sizeof line, "%s ", t);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + k, sizeof line - (size_t)k - 2, fmt, ap);
    va_end(ap);
    size_t len = (size_t)k + (m < 0 ? 0 : (size_t)m < sizeof line - (size_t)k - 2 ? (size_t)m : sizeof line - (size_t)k - 3);
    if (len && line[len - 1] != '\n') line[len++] = '\n';
    out(f_log, line, len);
}

/* ---- SDL's own messages and assertions ---- */
static void sdl_log(void *ud, int cat, SDL_LogPriority pri, const char *msg) {
    (void)ud;
    if (pri >= SDL_LOG_PRIORITY_WARN) slog("SDL %s (category %d): %s", pri >= SDL_LOG_PRIORITY_ERROR ? "error" : "warning", cat, msg);
    if (sdl_default_log) sdl_default_log(sdl_default_ud, cat, pri, msg);
}

static SDL_AssertState SDLCALL on_assert(const SDL_AssertData *d, void *ud) {
    App *a = ud;
    a->sdl_asserts++;
    fprintf(stderr, "bimble: SDL assertion '%s' at %s (%s:%d), seen %u times: logged, the session goes on\n", d->condition, d->function,
            d->filename, d->linenum, d->trigger_count + 1);
    if (d->trigger_count < 20) slog("SDL assertion '%s' at %s (%s:%d), seen %u times", d->condition, d->function, d->filename, d->linenum, d->trigger_count + 1);
    return SDL_ASSERTION_IGNORE; /* never abort a user's session */
}

void slog_sdl_hooks(App *a) {
    SDL_SetAssertionHandler(on_assert, a);
    SDL_GetLogOutputFunction(&sdl_default_log, &sdl_default_ud);
    SDL_SetLogOutputFunction(sdl_log, NULL);
}

/* ---- a crash ---- */
static void wr(const char *s) {
    if (crash_fd >= 0) {
        ssize_t r = write(crash_fd, s, strlen(s));
        (void)r;
    }
    ssize_t r = write(2, s, strlen(s));
    (void)r;
}
static void on_signal(int sig) {
    static volatile sig_atomic_t in;
    if (!in) {
        in = 1;
        char m[96] = "bimble: crash: signal ";
        size_t k = strlen(m);
        int s = sig;
        char d[8];
        int nd = 0;
        do d[nd++] = (char)('0' + s % 10), s /= 10;
        while (s && nd < 7);
        while (nd) m[k++] = d[--nd];
        m[k] = 0;
        strcat(m, sig == SIGSEGV ? " (SIGSEGV)" : sig == SIGABRT ? " (SIGABRT)" : sig == SIGBUS ? " (SIGBUS)" : sig == SIGILL ? " (SIGILL)" : " (SIGFPE)");
        strcat(m, "; the stack's raw addresses:\n");
        wr(m);
        void *bt[64];
        int n = backtrace(bt, 64);
        if (crash_fd >= 0) backtrace_symbols_fd(bt, n, crash_fd);
        backtrace_symbols_fd(bt, n, 2);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static char build_id[48];
#ifndef __APPLE__
static int exe_base(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    if (!info->dlpi_name || !info->dlpi_name[0]) { /* the executable: its base and its GNU build id (native/symbolize.sh) */
        *(uintptr_t *)data = (uintptr_t)info->dlpi_addr;
        for (int i = 0; i < info->dlpi_phnum; i++) {
            const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
            if (ph->p_type != PT_NOTE) continue;
            const uint8_t *p = (const uint8_t *)(info->dlpi_addr + ph->p_vaddr), *end = p + ph->p_memsz;
            while (p + 12 <= end) {
                uint32_t nn, dn, ty;
                memcpy(&nn, p, 4), memcpy(&dn, p + 4, 4), memcpy(&ty, p + 8, 4);
                const uint8_t *name = p + 12, *desc = name + ((nn + 3) & ~3u);
                if (desc + dn > end) break;
                if (ty == 3 && nn == 4 && !memcmp(name, "GNU", 4))
                    for (uint32_t k = 0; k < dn && k < 20; k++) snprintf(build_id + 2 * k, 3, "%02x", desc[k]);
                p = desc + ((dn + 3) & ~3u);
            }
        }
        return 1;
    }
    return 0;
}
#endif

int slog_open(App *a) {
    if (!a->session_log) return 0;
    char base[1024];
    if (!base_dir(base, sizeof base)) return 0;
    mkdirs(base);
    char **names;
    int n = sessions(base, &names); /* the oldest beyond the last 19 go: with this one, 20 */
    for (int i = 0; i + KEEP_SESSIONS - 1 < n; i++) rm_session(base, names[i]);
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
    time_t now = time(NULL);
    struct tm g;
    gmtime_r(&now, &g);
    char t[32];
    strftime(t, sizeof t, "%Y%m%dT%H%M%SZ", &g);
    snprintf(dir, sizeof dir, "%s/%s-%d", base, t, (int)getpid());
    if (mkdir(dir, 0700) && errno != EEXIST) { dir[0] = 0; return 0; }
    char p[1200];
    snprintf(p, sizeof p, "%s/session.log", dir), f_log = fopen(p, "w");
    snprintf(p, sizeof p, "%s/frames.csv", dir), f_frames = fopen(p, "w");
    snprintf(p, sizeof p, "%s/inputs.csv", dir), f_inputs = fopen(p, "w");
    if (!f_log) { dir[0] = 0; return 0; }
    crash_fd = fileno(f_log);
    void *warm[2];
    backtrace(warm, 2); /* loads libgcc's unwinder now, not in the handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
    static const char FH[] = "t_s,frames,frames_3d,frame_ms_median,frame_ms_p95,frame_ms_max,input_ms_median,input_ms_max,triangles,draws,tiles,gpu_mb,rss_mb,state\n",
                      IH[] = "t_s,what,app_ms,frame_ms\n";
    if (f_frames) out(f_frames, FH, sizeof FH - 1); /* the header's newline (it was cut: the first row glued to it) */
    if (f_inputs) out(f_inputs, IH, sizeof IH - 1);
    uintptr_t eb = 0;
#ifdef __APPLE__
    eb = mac_exe_base(build_id, sizeof build_id); /* the Mach-O header's address; the build id is the LC_UUID (the .dSYM's) */
#else
    dl_iterate_phdr(exe_base, &eb);
#endif
    int sv = SDL_GetVersion();
    slog("bimble %s (native), build id %s, pid %d, executable base 0x%lx (crash addresses are relative to it: native/symbolize.sh); SDL %d.%d.%d (%s)",
         CC_RELEASE_VERSION, build_id[0] ? build_id : "none", (int)getpid(), (unsigned long)eb, SDL_VERSIONNUM_MAJOR(sv), SDL_VERSIONNUM_MINOR(sv), SDL_VERSIONNUM_MICRO(sv), SDL_GetRevision());
    slog("this log stays on this machine (%s); it names the files opened", dir);
    sec_t0 = app_now(), next_report = sec_t0 + 10000;
    return 1;
}

static int cmpfl(const void *x, const void *y) { float a = *(const float *)x, b = *(const float *)y; return a < b ? -1 : a > b; }
static void pct(float *v, int n, double *med, double *p95, double *mx) {
    *med = *p95 = *mx = 0;
    if (!n) return;
    qsort(v, (size_t)n, sizeof *v, cmpfl);
    *med = v[n / 2], *p95 = v[(int)(n * 0.95) < n ? (int)(n * 0.95) : n - 1], *mx = v[n - 1];
}

void slog_report(App *a, const char *name) {
    if (!dir[0] || !a->r) return;
    Buf b;
    buf_init(&b);
    perf_report(a, &b);
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (f && written + b.len <= CAP_BYTES) fwrite(b.p, 1, b.len, f), written += b.len;
    if (f) fclose(f);
    buf_free(&b);
}

void slog_frame(App *a, double now, int drew3d, double frame_ms) {
    if (!f_log) return;
    if (sec_n < 4096) sec_ms[sec_n] = (float)frame_ms;
    sec_n++, sec_n3d += drew3d;
    if (now - sec_t0 >= 1000) {
        int n = sec_n < 4096 ? sec_n : 4096;
        double m, p, x, im, ip, ix;
        pct(sec_ms, n, &m, &p, &x), pct(sec_in, sec_nin < 512 ? sec_nin : 512, &im, &ip, &ix);
        nv_render_stats st = nv_render_get_stats(a->r);
        uint64_t rss = app_rss(NULL);
        char row[400];
        int k = snprintf(row, sizeof row, "%.0f,%d,%d,%.2f,%.2f,%.2f,%.1f,%.1f,%llu,%u,%u,%.1f,%.1f,%s\n", (now - a->t_start) / 1000, sec_n, sec_n3d, m, p, x, im, ix,
                         (unsigned long long)st.tris, st.draws, st.tiles, st.gpu_bytes / 1048576.0, rss / 1048576.0, a->moving ? "moving" : "still");
        out(f_frames, row, (size_t)k);
        sec_n = sec_n3d = sec_nin = 0, sec_t0 = now;
    }
    if (now >= next_report) next_report = now + 10000, slog_report(a, "report.txt");
}

void slog_input(App *a, const char *what, double app_ms, double ms) {
    if (!f_log) return;
    if (sec_nin < 512) sec_in[sec_nin++] = (float)ms;
    if (ms > 100 && f_inputs) {
        char row[200];
        int k = snprintf(row, sizeof row, "%.1f,%s,%.1f,%.1f\n", (app_now() - a->t_start) / 1000, what, app_ms, ms);
        out(f_inputs, row, (size_t)k);
    }
}

void slog_close(App *a) {
    if (!f_log) return;
    slog_report(a, "report.txt");
    slog("exit: %llu frames, %u SDL assertions", (unsigned long long)a->frames, a->sdl_asserts);
    if (f_frames) fclose(f_frames);
    if (f_inputs) fclose(f_inputs);
    crash_fd = -1;
    fclose(f_log);
    f_log = f_frames = f_inputs = NULL;
}

/* The report as a file of its own: in the session's folder, else beside the sessions (reports/). 0 ok, path in out. */
int slog_save_report(App *a, char *path, size_t n) {
    char d[1100];
    if (dir[0]) snprintf(d, sizeof d, "%s", dir);
    else {
        char base[1024];
        if (!base_dir(base, sizeof base)) return -1;
        snprintf(d, sizeof d, "%s/../reports", base);
        mkdirs(d);
    }
    time_t now = time(NULL);
    struct tm g;
    gmtime_r(&now, &g);
    char t[32];
    strftime(t, sizeof t, "%Y%m%dT%H%M%SZ", &g);
    snprintf(path, n, "%s/report-%s.txt", d, t);
    Buf b;
    buf_init(&b);
    perf_report(a, &b);
    FILE *f = fopen(path, "w");
    int ok = f && fwrite(b.p, 1, b.len, f) == b.len;
    if (f) ok = fclose(f) == 0 && ok;
    buf_free(&b);
    return ok ? 0 : -1;
}
