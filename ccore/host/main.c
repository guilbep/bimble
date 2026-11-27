/* bimble-ccore: the C core's native host. Serves the twin protocol of
 * crates/bimble-viewer/src/serve.rs on 127.0.0.1 for the Vue lab (`?native=1&port=&token=`).
 *   bimble-ccore [--session[=file]] [--warm] [port] [model]
 *   bimble-ccore mcp [--read-only] <model>   an MCP server over stdio instead (mcp.h)
 *                             --warm: when idle after a load or a change, build the scene and the
 *                             /mesh bytes of the new rev before the page asks (dev.sh uses it)
 *                             --session: keep the session (model edits, undo steps, selection,
 *                             rev) in a file across restarts (session.h): <model>.ccore-session,
 *                             or `file`; also on when CCORE_SESSION names a file
 *                             port: argv[1], else BIMBLE_SERVE (as twin.rs:106-114)
 *                             model: argv[2], else CCORE_MODEL; a .bimble (v1 text or v2 binary),
 *                             a .json, or an .ifc (imported); none: the host answers as a viewer
 *                             with no model. The calls open, create, save and close models later
 *                             (`action new|open|save|save_as|export_ifc4|export_ifc2x3|close`,
 *                             session/calls.c file_action); the session file follows them
 *   bimble-ccore open [--port N] [--no-browser] [--web dir] [--session[=file]] [model]
 *                             the desktop (ADR 0069 rule 2, ticket R1): also serves the built lab
 *                             from <exe dir>/web/ (or --web; static.h), on a free port of
 *                             127.0.0.1 (or N), prints the lab's URL on stdout and starts xdg-open
 *                             (`open` on macOS)
 *                             on it (not with --no-browser). Always --warm.
 *   bimble-ccore --version    prints ccore/VERSION (build.sh passes it as CC_RELEASE_VERSION)
 *   FS1, in both modes:      --root <dir> (repeatable): a folder the page's file dialog may list
 *                             beside the home folder and the model's folder (fs.h, the path rule);
 *                             --config-dir <dir>: the folder of settings.json (the recent files,
 *                             session/settings_file.h); else BIMBLE_CONFIG_DIR; else, for `open`
 *                             only, Rust's folder (settings.rs `path`); else none: the recent files
 *                             live in memory (the tests and dev.sh never touch the user's file)
 *   BIMBLE_TOKEN              the token; random when unset or empty (twin.rs:115-118)
 *   BIMBLE_ORIGINS            extra allowed Origins, comma separated, beside the twin's two
 *                             (twin.rs:17), e.g. a Vite dev server on another port */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE /* realpath and the dyld header beside _POSIX_C_SOURCE */
#endif
#ifdef _WIN32
/* Windows (ticket WIN1): ShellExecuteW opens the browser, GetModuleFileNameW finds the program,
 * the arguments come as UTF-8 from GetCommandLineW. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif
#include <fcntl.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <sys/resource.h> /* LINKF: getrusage */
#endif
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "mcp.h"
#include "session.h"
#include "static.h"
#include "fs.h"            /* FS1 */
#include "settings_file.h" /* FS1 */
#include "link_cache.h"    /* LINKF: link-convert */

#ifdef __APPLE__
#include <mach-o/dyld.h> /* _NSGetExecutablePath: macOS has no /proc/self/exe */
#define BROWSER_OPENER "open"
#else
#define BROWSER_OPENER "xdg-open"
#endif

#ifndef CC_RELEASE_VERSION
#define CC_RELEASE_VERSION "dev" /* build.sh passes ccore/VERSION */
#endif

/* 32 hex digits, as twin.rs `random_token` (twin.rs:88-99): /dev/urandom, else the clock. */
static void random_hex(char out[33]) {
    unsigned char b[16];
#ifdef _WIN32
    ssize_t k = cc_win_random(b, sizeof b) == 0 ? (ssize_t)sizeof b : -1; /* rand_s */
#else
    int fd = open("/dev/urandom", O_RDONLY);
    ssize_t k = fd >= 0 ? read(fd, b, sizeof b) : -1;
    if (fd >= 0) close(fd);
#endif
    if (k != (ssize_t)sizeof b) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned long long x = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
        x ^= (unsigned long long)getpid() << 32;
        for (int i = 0; i < 16; i++) {
            x ^= x << 13, x ^= x >> 7, x ^= x << 17; /* xorshift64 */
            b[i] = (unsigned char)x;
        }
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) out[2 * i] = hex[b[i] >> 4], out[2 * i + 1] = hex[b[i] & 15];
    out[32] = 0;
}

static int parse_port(const char *s, unsigned *out) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++; /* `trim()` before parse (twin.rs:108) */
    char *end;
    if (!*s || *s == '-') return 0;
    unsigned long v = strtoul(s, &end, 10);
    while (*end == ' ' || *end == '\t' || *end == '\n') end++;
    if (*end || v > 65535) return 0;
    *out = (unsigned)v;
    return 1;
}

/* `open`: the lab's URL once the port is known (cc_host_config.on_listen). */
typedef struct {
    const char *token;
    int browser;
} OpenCtx;

static void url_put_token(Buf *b, const char *t) { /* encodeURIComponent's unreserved set */
    static const char hex[] = "0123456789ABCDEF";
    for (; *t; t++) {
        unsigned char c = (unsigned char)*t;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.!~*'()", c))
            buf_putc(b, (char)c);
        else
            buf_putc(b, '%'), buf_putc(b, hex[c >> 4]), buf_putc(b, hex[c & 15]);
    }
}

static void on_listen(uint16_t port, void *ctx) {
    const OpenCtx *o = ctx;
    Buf url;
    buf_init(&url);
    buf_puts(&url, "http://127.0.0.1:");
    buf_u64(&url, port);
    buf_puts(&url, "/?native=1&gl=1&port=");
    buf_u64(&url, port);
    buf_puts(&url, "&token=");
    url_put_token(&url, o->token);
    const char *u = buf_cstr(&url);
    printf("%s\n", u);
    fflush(stdout);
#ifdef _WIN32
    if (o->browser && !url.oom) {
        wchar_t *w = cc_win_wide(u);
        /* ShellExecuteW: the default browser, not our child; > 32 is success. */
        if (!w || (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL) <= 32)
            fprintf(stderr, "bimble-ccore: cannot start the browser: open the address above\n");
        free(w);
    }
#else
    if (o->browser && !url.oom) {
        /* Two forks: the browser opener is not our child, so no zombie and no wait. */
        pid_t p = fork();
        if (p == 0) {
            if (fork() == 0) {
                setsid();
                int nul = open("/dev/null", O_RDWR);
                if (nul >= 0) dup2(nul, 0), dup2(nul, 1), dup2(nul, 2);
                /* The listening socket and the rest are not the browser's: a browser started
                 * by xdg-open would keep the port after the host stopped. */
                for (int fd = 3; fd < 1024; fd++) close(fd);
                execlp(BROWSER_OPENER, BROWSER_OPENER, u, (char *)NULL);
                _exit(127);
            }
            _exit(0);
        }
        if (p > 0) waitpid(p, NULL, 0);
        else fprintf(stderr, "bimble-ccore: cannot start " BROWSER_OPENER ": open the address above\n");
    }
#endif
    buf_free(&url);
}

/* <exe dir>/web, from /proc/self/exe (macOS: _NSGetExecutablePath, links resolved; else argv[0]);
 * 0 when it does not fit. */
static int default_web(const char *argv0, char *out, size_t n) {
    char exe[PATH_MAX];
#if defined(_WIN32)
    ssize_t k = -1;
    wchar_t wexe[32768];
    DWORD wn = GetModuleFileNameW(NULL, wexe, sizeof wexe / sizeof wexe[0]);
    if (wn > 0 && wn < sizeof wexe / sizeof wexe[0]) {
        char *u = cc_win_utf8(wexe);
        if (u && strlen(u) < sizeof exe) strcpy(exe, u), k = (ssize_t)strlen(exe);
        free(u);
    }
#elif defined(__APPLE__)
    char path[PATH_MAX];
    uint32_t pn = sizeof path;
    ssize_t k = -1;
    if (_NSGetExecutablePath(path, &pn) == 0 && realpath(path, exe)) k = (ssize_t)strlen(exe);
#else
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k > 0) exe[k] = 0;
#endif
    if (k <= 0) {
        if (strlen(argv0) >= sizeof exe) return 0;
        strcpy(exe, argv0);
    }
    char *slash = strrchr(exe, '/');
#ifdef _WIN32
    char *bs = strrchr(exe, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    int w = slash ? snprintf(out, n, "%.*s/web", (int)(slash - exe), exe) : snprintf(out, n, "web");
    return w > 0 && (size_t)w < n;
}

typedef struct {
    const char *model, *session;
    int want_session, warm;
    const cc_static *web;
    OpenCtx *open;
    /* ---- FS1 ---- */
    const char *roots[FS_MAX_ROOTS];
    int nroots;
    const char *config_dir; /* --config-dir */
    int default_config;     /* `open`: Rust's settings folder when nothing else is given */
    /* ---- end FS1 ---- */
} Serve;

/* ---- FS1: the settings folder (settings.rs `path`) and the dialog's roots ---- */
int cc_host_config_dir(char *buf, size_t n) {
    int w;
#if defined(__APPLE__)
    const char *home = cc_host_getenv("HOME");
    if (!home || !*home) return 0;
    w = snprintf(buf, n, "%s/Library/Application Support/Bimble", home);
#elif defined(_WIN32)
    const char *ad = cc_host_getenv("APPDATA");
    if (!ad || !*ad) return 0;
    w = snprintf(buf, n, "%s/Bimble", ad);
#else
    const char *x = cc_host_getenv("XDG_CONFIG_HOME"), *home = cc_host_getenv("HOME");
    if (x && *x) w = snprintf(buf, n, "%s/bimble", x);
    else if (home && *home) w = snprintf(buf, n, "%s/.config/bimble", home);
    else return 0;
#endif
    return w > 0 && (size_t)w < n;
}

static const char *settings_dir(const Serve *sv, char *buf, size_t n) {
    if (sv->config_dir && *sv->config_dir) return sv->config_dir;
    const char *e = cc_host_getenv("BIMBLE_CONFIG_DIR");
    if (e && *e) return e;
    if (!sv->default_config) return NULL;
    return cc_host_config_dir(buf, n) ? buf : NULL;
}

static void files_setup(cc_session *ses, const Serve *sv) {
    cc_host_fs_init();
    for (int i = 0; i < sv->nroots; i++)
        if (cc_host_fs_add_root(sv->roots[i])) fprintf(stderr, "bimble-ccore: --root %s: not a folder\n", sv->roots[i]);
    char dbuf[4200], abs[4200];
    const char *dir = settings_dir(sv, dbuf, sizeof dbuf);
    if (dir) {
        char f[4300];
        snprintf(f, sizeof f, "%s/settings.json", dir);
        cc_recent_setup(ses, f);
    }
    const cc_platform *pf = cc_platform_native();
    if (sv->model && *sv->model && !pf->real_path(pf->ctx, sv->model, abs, sizeof abs)) {
        cc_recent_opened(ses, abs); /* settings.rs `opened`: the model given at start too */
        char *sl = strrchr(abs, '/');
        if (sl) {
            if (sl == abs) sl[1] = 0;
            else *sl = 0;
            cc_host_fs_add_root(abs); /* the model's folder */
        }
    }
}

/* "--root <dir>" / "--config-dir <dir>" at argv[*i]: 1 when taken (*i on its value), 0 not one,
 * -1 an error (printed). */
static int files_flag(Serve *sv, int argc, char **argv, int *i) {
    const char *a = argv[*i];
    int root = !strcmp(a, "--root"), conf = !strcmp(a, "--config-dir");
    if (!root && !conf) return 0;
    if (*i + 1 >= argc || !*argv[*i + 1]) {
        fprintf(stderr, "bimble-ccore: %s needs a folder\n", a);
        return -1;
    }
    const char *v = argv[++*i];
    if (conf) sv->config_dir = v;
    else if (sv->nroots < FS_MAX_ROOTS) sv->roots[sv->nroots++] = v;
    else {
        fprintf(stderr, "bimble-ccore: at most %d --root\n", FS_MAX_ROOTS);
        return -1;
    }
    return 1;
}
/* ---- end FS1 ---- */

/* Opens the model (if any) and serves until SIGTERM or SIGINT. */
static int serve(unsigned port, Serve *sv) {
    const char *session = sv->session;
    int want_session = sv->want_session;
    /* The host's one session (a handle: the process could hold more). */
    cc_session *ses = cc_session_new(cc_platform_native());
    if (!ses) {
        fprintf(stderr, "bimble-ccore: out of memory\n");
        return 1;
    }
    { /* LINKF: BIMBLE_LINK_CACHE_MIN (bytes): IFC links from this size get a cache and a thread (links.h) */
        const char *cm = cc_host_getenv("BIMBLE_LINK_CACHE_MIN");
        if (cm && *cm) ses->link_cache_min = strtoull(cm, NULL, 10);
        const char *cb = cc_host_getenv("BIMBLE_LINK_CACHE_BESIDE"); /* "0": only the cache folder */
        ses->link_no_beside = cb && !strcmp(cb, "0");
        const char *mb = cc_host_getenv("BIMBLE_LINK_MESH_BUDGET_MB"); /* SIZE3: decoded link meshes kept at rest */
        if (mb && *mb) ses->link_mesh_budget = strtoull(mb, NULL, 10) << 20;
        const char *km = cc_host_getenv("BIMBLE_LINK_KEEP_MS"); /* SIZE3: tests */
        if (km && *km) ses->link_keep_ms = strtod(km, NULL);
        const char *id = cc_host_getenv("BIMBLE_LINK_INDEX_DELAY_MS"); /* SIZE3 (E): tests */
        if (id && *id) ses->link_index_delay_ms = strtod(id, NULL);
        const char *tl = cc_host_getenv("BIMBLE_TILES"); /* TILE1: the pyramids' bake settings ("leaf=40000 occ=64 …") */
        if (tl && *tl) cc_tiles_opts_parse(&ses->tiles_opts, tl);
    }
    const char *model = sv->model;
    if (model && *model) {
        cc_error err;
        static char def[4200];
        if (want_session && !session) {
            snprintf(def, sizeof def, "%s.ccore-session", model);
            session = def;
        }
        if (want_session ? cc_session_open_persisted(ses, model, session, &err) : cc_session_open(ses, model, &err)) {
            fprintf(stderr, "bimble-ccore: %s: %s\n", model, err.msg);
            return 1;
        }
    } else if (want_session) {
        fprintf(stderr, "bimble-ccore: --session needs a model\n");
        return 2;
    }
    cc_host_fonts(ses, sv->web); /* D7: the PDF export's Roboto files (fonts.c) */
    files_setup(ses, sv);         /* FS1 */
    static char token[33], epoch[33];
    const char *t = cc_host_getenv("BIMBLE_TOKEN");
    if (!t || !*t) {
        random_hex(token);
        t = token;
    }
    random_hex(epoch);

    cc_host_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.port = (uint16_t)port;
    cfg.warm = sv->warm;
    cfg.session = ses;
    cfg.token = t;
    cfg.epoch = epoch;
    cfg.web = sv->web;
    if (sv->open) {
        sv->open->token = t;
        cfg.on_listen = on_listen;
        cfg.on_listen_ctx = sv->open;
    }
    cfg.origins[cfg.n_origins++] = "http://localhost:5173";
    cfg.origins[cfg.n_origins++] = "http://127.0.0.1:5173";
    static char extra[1024];
    const char *e = cc_host_getenv("BIMBLE_ORIGINS");
    if (e && strlen(e) < sizeof extra) {
        strcpy(extra, e);
        for (char *tok = strtok(extra, ","); tok && cfg.n_origins < CC_HOST_MAX_ORIGINS; tok = strtok(NULL, ","))
            if (*tok && strlen(tok) < 256) cfg.origins[cfg.n_origins++] = tok;
    }
    int rc = cc_host_run(&cfg);
    cc_host_stage_cleanup(); /* FS1: the dropped files' copies */
    cc_session_persist_now(ses); /* SIGTERM or SIGINT: the last change, if not written yet */
    cc_session_free(ses);
    return rc;
}

static const char OPEN_USAGE[] =
    "usage: bimble-ccore open [--port N] [--no-browser] [--web dir] [--session[=file]] [--root dir]... [--config-dir dir] [model]\n";

/* bimble-ccore open ... (see the header). argv[0] is "open". */
static int open_main(const char *argv0, int argc, char **argv) {
    Serve sv;
    memset(&sv, 0, sizeof sv);
    sv.warm = 1;
    sv.default_config = 1; /* FS1: the desktop keeps Rust's settings file */
    OpenCtx oc = {NULL, 1};
    unsigned port = 0;
    const char *web = NULL;
    int i = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
        const char *a = argv[i];
        int ff = files_flag(&sv, argc, argv, &i); /* FS1 */
        if (ff < 0) return 2;
        if (ff) continue;
        if (!strcmp(a, "--no-browser")) oc.browser = 0;
        else if (!strcmp(a, "--session")) sv.want_session = 1;
        else if (!strncmp(a, "--session=", 10) && a[10]) sv.want_session = 1, sv.session = a + 10;
        else if (!strcmp(a, "--port") && i + 1 < argc) {
            if (!parse_port(argv[++i], &port)) {
                fprintf(stderr, "bimble-ccore: \"%s\" is not a port\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "--web") && i + 1 < argc) web = argv[++i];
        else if (!strcmp(a, "--help")) {
            fputs(OPEN_USAGE, stdout);
            return 0;
        } else {
            fprintf(stderr, "bimble-ccore: open: unknown flag %s\n%s", a, OPEN_USAGE);
            return 2;
        }
    }
    if (argc - i > 1) {
        fputs(OPEN_USAGE, stderr);
        return 2;
    }
    sv.model = i < argc ? argv[i] : NULL;
    static char defweb[PATH_MAX + 8];
    if (!web) {
        if (!default_web(argv0, defweb, sizeof defweb)) {
            fprintf(stderr, "bimble-ccore: cannot find the web/ folder next to the program: use --web <dir>\n");
            return 1;
        }
        web = defweb;
    }
    char err[600];
    cc_static *st = cc_static_load(web, err, sizeof err);
    if (!st) {
        fprintf(stderr, "bimble-ccore: the lab's files: %s\n", err);
        return 1;
    }
    size_t nf, nb;
    cc_static_stats(st, &nf, &nb);
    fprintf(stderr, "bimble-ccore: serving %zu files (%zu bytes) from %s\n", nf, nb, web);
    sv.web = st;
    sv.open = &oc;
    int rc = serve(port, &sv);
    cc_static_free(st);
    return rc;
}

#ifdef _WIN32
/* The arguments as UTF-8 (main's argv is in the ANSI code page): from GetCommandLineW. NULL on
 * a failure (then main's argv as is). Never freed: they live as long as the process. */
static char **utf8_argv(int *argc) {
    int n;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return NULL;
    char **a = calloc((size_t)n + 1, sizeof *a);
    for (int i = 0; a && i < n; i++)
        if (!(a[i] = cc_win_utf8(w[i]))) a = NULL; /* leaked: once, at start */
    LocalFree(w);
    if (a) *argc = n;
    return a;
}
#endif

/* LINKF: bimble-ccore link-convert [--cache-dir DIR] [--keep] [--index|--reindex] FILE...: each file through a link's
 * conversion (session/link_cache.h), one at a time, in the order given; with --cache-dir the
 * caches go only there (nothing written beside the files). One line per file: bytes, ms, elements,
 * shapes, triangles, cache bytes, from the cache or read. --keep: every scene kept until the end,
 * then the totals (all of them loaded together). TILE1: --index runs the indexing job (proxies, tile pyramid) after a
 * conversion, --reindex also on a complete cache (its time and the process's peak alone). Exit 1 when one failed. */
static int link_convert_main(int argc, char **argv) {
    const cc_platform *pf = cc_platform_native();
    int keep = 0, no_beside = 0, bad = 0, index = 0;
    int i = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
        if (!strcmp(argv[i], "--keep")) keep = 1;
        else if (!strcmp(argv[i], "--index")) index = 1; /* TILE1 */
        else if (!strcmp(argv[i], "--reindex")) index = 2; /* TILE1: a complete cache indexed again (timing, peak) */
        else if (!strcmp(argv[i], "--cache-dir") && i + 1 < argc) {
#ifndef _WIN32
            setenv("BIMBLE_CACHE_DIR", argv[++i], 1);
#else
            _putenv_s("BIMBLE_CACHE_DIR", argv[++i]);
#endif
            no_beside = 1;
        } else {
            fprintf(stderr, "usage: bimble-ccore link-convert [--cache-dir DIR] [--keep] [--index|--reindex] FILE...\n");
            return 2;
        }
    }
    cc_link_job **kept = calloc((size_t)argc, sizeof *kept);
    if (!kept) return 1;
    cc_tiles_opts topts; /* TILE1: BIMBLE_TILES, else the defaults */
    cc_tiles_opts_parse(&topts, cc_host_getenv("BIMBLE_TILES"));
    double t0 = pf->now_ms(pf->ctx);
    uint64_t te = 0, tt = 0, ts = 0;
    for (int k = 0; i < argc; i++, k++) {
        cc_link_job *j = cc_link_job_new(pf, argv[i]);
        if (!j) return 1;
        j->topts = topts;
        j->cache_min = 0, j->no_beside = no_beside;
        cc_link_job_run(j);
        if (j->rc) {
            printf("file %d: %s\n", k + 1, j->rc == CC_JOB_REFUSED ? "refused" : "failed"), bad = 1;
            fprintf(stderr, "%s\n", j->err.msg);
            cc_link_job_free(j);
            continue;
        }
        size_t ne = j->vf ? j->vf->nelements : j->ifc->scene.nelements, ns = j->vf ? j->vf->nshapes : j->ifc->scene.nshapes;
        uint64_t nt = 0; /* drawn: each copy counted */
        for (size_t e = 0; j->vf && e < ne; e++) nt += cc_viewfile_local_mesh(j->vf, e)->ni / 3;
        if (!j->vf) nt = j->ifc->triangles;
        printf("file %d: bytes %llu ms %.0f elements %zu shapes %zu triangles %llu cache_bytes %llu %s\n", k + 1,
               (unsigned long long)j->bytes, j->ms, ne, ns, (unsigned long long)nt, (unsigned long long)j->cache_bytes,
               j->from_cache ? "from-cache" : "read");
        fflush(stdout);
        if (index && (j->incomplete || index == 2) && j->cache[0]) { /* TILE1: the indexing job (proxies, tiles) run here, timed */
            cc_link_job *x = cc_link_job_new(pf, argv[i]);
            if (!x) return 1;
            x->topts = topts;
            x->index_only = 1;
            snprintf(x->cache, sizeof x->cache, "%s", j->cache);
            snprintf(x->log, sizeof x->log, "%s", j->log);
            cc_link_job_free(j); /* its mapping goes before the cache is rewritten */
            j = NULL;
            cc_link_job_run(x);
            long pk = 0;
#ifndef _WIN32
            struct rusage ru2;
            if (!getrusage(RUSAGE_SELF, &ru2)) pk = (long)ru2.ru_maxrss;
#endif
            printf("file %d: indexed %s ms %.0f hlod_ms %.0f tiles %llu levels %llu proxy_tris %llu tiles_bytes %llu tiles_ms %.0f cache_bytes %llu peak_rss_kB %ld%s%s\n",
                   k + 1, x->rc ? "failed" : "ok", x->ms, x->hlod_ms, (unsigned long long)x->tiles_count, (unsigned long long)x->tiles_levels,
                   (unsigned long long)x->tiles_proxy_tris, (unsigned long long)x->tiles_bytes, x->tiles_ms, (unsigned long long)x->cache_bytes, pk,
                   x->tiles_why[0] ? " " : "", x->tiles_why);
            fflush(stdout);
            if (x->rc) bad = 1;
            if (keep) kept[k] = x;
            else cc_link_job_free(x);
            te += ne, ts += ns, tt += nt;
            continue;
        }
        te += ne, ts += ns, tt += nt;
        if (keep) kept[k] = j;
        else cc_link_job_free(j);
    }
    if (keep)
        printf("all: ms %.0f elements %llu shapes %llu triangles %llu\n", pf->now_ms(pf->ctx) - t0, (unsigned long long)te,
               (unsigned long long)ts, (unsigned long long)tt);
#ifndef _WIN32
    struct rusage ru; /* the peak resident memory of this process (kB on Linux) */
    if (!getrusage(RUSAGE_SELF, &ru)) printf("peak_rss_kB %ld\n", (long)ru.ru_maxrss);
#endif
    for (int k = 0; k < argc; k++) cc_link_job_free(kept[k]);
    free(kept);
    return bad;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    {
        char **u = utf8_argv(&argc);
        if (u) argv = u;
    }
#endif
    if (argc > 1 && !strcmp(argv[1], "mcp")) return cc_mcp_main(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        printf("bimble-ccore %s\n", CC_RELEASE_VERSION);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "open")) return open_main(argv[0], argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "link-convert")) return link_convert_main(argc - 1, argv + 1); /* LINKF */
    /* Flags first, then the positional port and model. */
    Serve sv;
    memset(&sv, 0, sizeof sv);
    const char *session = cc_host_getenv("CCORE_SESSION");
    sv.want_session = session && *session;
    sv.session = sv.want_session ? session : NULL;
    while (argc > 1 && !strncmp(argv[1], "--", 2)) {
        int fi = 1, ff = files_flag(&sv, argc, argv, &fi); /* FS1: --root, --config-dir and a value */
        if (ff < 0) return 2;
        if (ff) {
            argv[2] = argv[0], argv += 2, argc -= 2;
            continue;
        }
        if (!strcmp(argv[1], "--session")) sv.want_session = 1;
        else if (!strcmp(argv[1], "--warm")) sv.warm = 1;
        else if (!strncmp(argv[1], "--session=", 10) && argv[1][10]) sv.want_session = 1, sv.session = argv[1] + 10;
        else {
            fprintf(stderr, "bimble-ccore: unknown flag %s\n", argv[1]);
            return 2;
        }
        argv[1] = argv[0], argv++, argc--;
    }
    const char *ps = argc > 1 ? argv[1] : cc_host_getenv("BIMBLE_SERVE");
    unsigned port;
    if (!ps) {
        fprintf(stderr, "usage: bimble-ccore <port> [model] (or BIMBLE_SERVE=<port>, CCORE_MODEL=<model>)\n"
                        "       bimble-ccore open [--port N] [--no-browser] [model]\n");
        return 2;
    }
    if (!parse_port(ps, &port)) {
        fprintf(stderr, "bimble-ccore: \"%s\" is not a port\n", ps);
        return 2;
    }
    sv.model = argc > 2 ? argv[2] : cc_host_getenv("CCORE_MODEL");
    return serve(port, &sv);
}
